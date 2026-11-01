// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Browser frontend for the WebAssembly build. Linked with PROXY_TO_PTHREAD, so main()
// runs on a worker and the browser main thread only runs the page's JavaScript, which
// talks to us through the pcsx2web_* exports at the bottom of this file. Those exports
// never block: they only touch atomics and a lock-free key queue. The BIOS arrives through
// MEMFS. Frames reach the page in one of two ways: with WebGPU, the GS device posts each
// frame as an ImageBitmap over its frame port (pcsx2/GS/Renderers/WebGPU/WebGPUHost.h);
// otherwise GSDeviceNull's frames go through a shared descriptor (see PresentFrame()).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/webaudio.h>

#include <SDL3/SDL_hints.h>

#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"
#include "common/Timer.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/CDVD/Emscripten/WebDisc.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/Renderers/Null/GSDeviceNull.h"
#include "pcsx2/GS/Renderers/WebGPU/WebGPUHost.h"
#include "pcsx2/GameDatabase.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/Host/AudioStream.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/MTVU.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/SIO/Pad/PadBase.h"
#include "pcsx2/SIO/Sio.h"
#include "pcsx2/USB/USB.h"
#include "pcsx2/USB/usb-pad/usb-pad.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/wasm/PerfCounters.h"
#include "pcsx2/wasm/EEJit.h"
#include "pcsx2/wasm/IopJit.h"
#include "pcsx2/ps2/BiosTools.h"
#include "pcsx2/wasm/VU1Jit.h"
#include "pcsx2/wasm/VU1JitFuzz.h"

namespace
{
	// Values of pcsx2web_state().
	enum WebState : int
	{
		WEB_STATE_WAITING = 0,
		WEB_STATE_STARTING = 1,
		WEB_STATE_RUNNING = 2,
		WEB_STATE_STOPPED = 3,
		WEB_STATE_FAILED = 4,
	};

	// Frame descriptor words, read by the page.
	enum FrameDescriptor : u32
	{
		FRAME_SEQ,
		FRAME_WIDTH,
		FRAME_HEIGHT,
		FRAME_PIXELS,
		FRAME_VSYNC, // the vsync (counted since boot) whose picture this is, see CurrentFrameNumber()
		FRAME_DESCRIPTOR_SIZE,
	};
} // namespace

// GSDeviceNull's fixed virtual window, which the page scales (tests can change it with -surface).
static u32 s_window_width = 640;
static u32 s_window_height = 480;
static constexpr const char* BIOS_DIRECTORY = "/bios";
static constexpr const char* SNAPSHOT_DIRECTORY = "/snapshots";

static MemorySettingsInterface s_settings_interface;
static std::vector<u8> s_roboto_font_data;

static std::atomic<int> s_state{WEB_STATE_WAITING};
static std::atomic<bool> s_fatal_error{false};
static std::atomic<bool> s_stop_requested{false};
// The VU1 JIT for the next boots: on by default, off with the page's ?vu1jit=0.
static std::atomic<bool> s_vu1jit_enabled{true};
// CPUThreadInitialize has finished (it sets up VU memory, which the VU1 JIT fuzzer must not race).
static std::atomic<bool> s_cpu_thread_ready{false};
static std::atomic<float> s_speed{0.0f};
static std::atomic<float> s_vps{0.0f};
// The per-vsync recorder (pcsx2web_perf_ring()): one record per vsync in a ring the page reads.
// Each record is PERF_FIELDS doubles: vsync number, wall time (ms), cumulative busy time (ms) of the
// EE, GS and VU1 threads, cumulative JIT compile time (ms), cumulative time waiting for GPU pipelines
// (ms) and their count, the emulated vsync rate (Hz) the record's speed is relative to, cumulative time
// the EE JIT's code generation thread spent building modules (ms), and the cumulative counts of EE JIT
// modules installed and of those built or compiled ahead from a code list.
static constexpr u32 PERF_RECORDS = 8192;
static constexpr u32 PERF_FIELDS = 12;
alignas(8) static double s_perf_ring[PERF_RECORDS * PERF_FIELDS];
static std::atomic<u32> s_perf_written{0};

// EE, GS and VU thread usage in percent (busy wall time; see Threading::ScopedWaitTimer).
static std::atomic<float> s_thread_usage[3] = {0.0f, 0.0f, 0.0f};

// Boot request: the page writes the name, then sets the futex word to 1 and wakes the CPU
// thread. Only one request can be in flight (the state CAS to STARTING guards the buffer).
static std::atomic<u32> s_boot_request{0};
static char s_boot_bios_name[256];

// Frames: a ring of three buffers, so a page that saw seq N can copy buffer N while N+1 is
// published; buffer N is only rewritten for frame N+3, after seq reached N+2.
static constexpr u32 NUM_FRAME_BUFFERS = 3;
static std::array<std::vector<u32>, NUM_FRAME_BUFFERS> s_frame_buffers;
static u32 s_frame_write_index = 0; // GS thread only
alignas(16) static std::atomic<u32> s_frame_descriptor[FRAME_DESCRIPTOR_SIZE] = {};
static_assert(sizeof(std::atomic<u32>) == sizeof(u32) && std::atomic<u32>::is_always_lock_free);

// Vsyncs since the module started (CPU thread writes, the page reads).
static std::atomic<u32> s_vsync_count{0};

// Crisp: nearest-neighbour presentation (WebGPU path). -1 = unchanged since the last apply.
static std::atomic<int> s_crisp_request{-1};
// The page's picture filter (PCSX2's TV shader, GSConfig.TVShader: 0 none, 5 Lottes CRT...), -1: no request.
static std::atomic<int> s_tv_shader_request{-1};
static int s_tv_shader = 0;
static bool s_crisp = false; // CPU thread
// The page's internal resolution (GSConfig.UpscaleMultiplier: 1 native, 2 twice the PS2's lines), 0: no request.
static std::atomic<int> s_upscale_request{0};
static int s_upscale = 1; // CPU thread
// The EE cycle rate the page asked for (EmuConfig.Speedhacks.EECycleRate: 0, or -1 to -3 to underclock the EE),
// kCycleRateNone when there is no new request.
static constexpr int kCycleRateNone = -128;
static std::atomic<int> s_cycle_rate_request{kCycleRateNone};
static int s_cycle_rate = 0; // CPU thread

// Test hooks (README "Automated runs"): a screenshot at a vsync, and copies of presented 2D-path
// frames by vsync number. The capture slots are written once by the GS thread and published
// through their frame word.
static std::atomic<u32> s_snapshot_vsync{0};
static constexpr u32 MAX_CAPTURES = 32;
struct FrameCapture
{
	std::atomic<u32> frame; // 0 = not yet; then the vsync number of the frame copied
	u32 wanted; // the first presented frame at this vsync number or later is copied
	u32 width;
	u32 height;
	std::vector<u32> pixels;
};
static FrameCapture s_captures[MAX_CAPTURES];
static u32 s_capture_header[4]; // frame, width, height, pixels (read by the page after publication)

// GS thread: the number of the vsync being presented, counted from boot. Frames are identified by
// it rather than by how many were presented, because how many are presented depends on host
// timing: SkipDuplicateFrames skips presenting frames that PerformanceMetrics' internal-FPS
// detector, which works on wall-clock windows, considers repeats. Both display paths and the
// WebGPU device use this number (see GSDeviceWebGPU::EndPresent()).
static u32 CurrentFrameNumber()
{
	// GSRenderer::VSync() counts the vsync after presenting it.
	return static_cast<u32>(PerformanceMetrics::GetFrameNumber()) + 1;
}

// -set Section/Key=Value overrides from the page's ?set= (tests), applied after the defaults.
struct SettingOverride
{
	std::string section;
	std::string key;
	std::string value;
};
static std::vector<SettingOverride> s_setting_overrides;

// Keyboard events: single producer (browser main thread), single consumer (CPU thread).
// Each entry is a key code with bit 31 set for a press.
static constexpr u32 KEY_QUEUE_SIZE = 256;
static constexpr u32 KEY_PRESSED_BIT = 0x80000000u;
static u32 s_key_queue[KEY_QUEUE_SIZE];
static std::atomic<u32> s_key_queue_head{0}; // written by the producer
static std::atomic<u32> s_key_queue_tail{0}; // written by the consumer
static_assert((KEY_QUEUE_SIZE & (KEY_QUEUE_SIZE - 1)) == 0);

// Work for the CPU thread from other emulator threads (never from the browser main thread).
static std::mutex s_cpu_thread_queue_lock;
static std::condition_variable s_cpu_thread_queue_cv;
static std::deque<std::function<void()>> s_cpu_thread_queue;
static std::thread::id s_cpu_thread_id;

// Host key names; the key code is the index + 1 (0 would make an all-zero InputBindingKey).
static constexpr const char* s_key_names[] = {
	"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W",
	"X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8",
	"F9", "F10", "F11", "F12", "Up", "Down", "Left", "Right", "Return", "Backspace", "Escape", "Space", "Tab", "Shift",
	"Control", "Alt", "Insert", "Delete", "Home", "End", "PageUp", "PageDown"};
static constexpr u32 NUM_KEYS = static_cast<u32>(std::size(s_key_names));

// A quick tap can put a press and its release in the same drain. Applied back to back, the
// game never samples the button as held (menus miss D-pad taps). So each key keeps at least
// this many polls (vsyncs) between its applied edges, down and up. Pending edges are tracked
// per key, so one held-back key never delays another, and the ring always drains fully.
static constexpr u32 MIN_KEY_HOLD_POLLS = 2;
// More than this many pending edges per key drops whole taps, so mashing at low emulation
// speed does not replay presses for seconds after the user stops.
static constexpr u8 MAX_PENDING_KEY_EDGES = 3;
struct KeySlot
{
	bool down;      // state applied to the emulator
	u8 pending;     // edges (toggles) still to apply
	u32 changed_at; // poll of the last applied edge, 0 = never
};
static u32 s_key_poll_count = 0; // CPU thread only
static KeySlot s_keys[NUM_KEYS + 1] = {}; // CPU thread only

// Pad events: the page sends the state of one PS2 input of one port (port 1 or 2) as a value from 0
// to 1: the same single producer (browser main thread) and single consumer (CPU thread) as the
// keys. The keyboard's pad keys and the gamepads all arrive here, already routed to a port by the
// page (the Players choice in the Keys drawer), so the core has no keyboard bindings for the pads.
// The CPU thread hands each value to the pad (Pad::SetControllerState, what a PCSX2 binding does).
// Each entry: value 0..65535 in bits 0-15, the input in bits 16-20, the port in bit 21.
static constexpr u32 PAD_QUEUE_SIZE = 512;
static constexpr u32 PAD_EVENT_INPUT_SHIFT = 16;
static constexpr u32 PAD_EVENT_PORT_SHIFT = 21;
static u32 s_pad_queue[PAD_QUEUE_SIZE];
static std::atomic<u32> s_pad_queue_head{0}; // written by the producer
static std::atomic<u32> s_pad_queue_tail{0}; // written by the consumer
static_assert((PAD_QUEUE_SIZE & (PAD_QUEUE_SIZE - 1)) == 0);

// The inputs of a DualShock 2, by the core's binding names. The first NUM_PAD_BUTTONS are buttons
// (L2 and R2 take a pressure), the rest the eight halves of the two sticks (a value 0 to 1 each).
static constexpr const char* s_pad_input_names[] = {"Up", "Right", "Down", "Left", "Triangle", "Circle", "Cross",
	"Square", "Select", "Start", "L1", "L2", "R1", "R2", "L3", "R3", "Analog", "LUp", "LRight", "LDown", "LLeft",
	"RUp", "RRight", "RDown", "RLeft"};
static constexpr u32 NUM_PAD_INPUTS = static_cast<u32>(std::size(s_pad_input_names));
static constexpr u32 NUM_PAD_BUTTONS = 17;
static constexpr u32 NUM_PAD_PORTS = 2;
static_assert(NUM_PAD_INPUTS <= 32);

// A button keeps the key rules above (a tap lasts at least MIN_KEY_HOLD_POLLS, mashing drops taps);
// a stick half applies its latest value. A gamepad's edges can arrive in bursts (Firefox can hand the
// page a few taps at once), so a button keeps two whole taps pending before it drops one.
static constexpr u8 MAX_PENDING_PAD_EDGES = 5;
struct PadSlot
{
	float target; // the last value (of a button: the last pressed value)
	float applied; // what the pad was given
	bool down; // button state applied
	u8 pending; // button edges (toggles) still to apply
	u32 changed_at; // poll of the last applied button edge, 0 = never
};
static PadSlot s_pad_slots[NUM_PAD_PORTS][NUM_PAD_INPUTS] = {}; // CPU thread only
static u8 s_pad_bind_index[NUM_PAD_INPUTS] = {}; // the core's bind index of each input
static bool s_pad_bind_index_ready = false;

// What each port's pad holds, for tests (pcsx2web_test_pad_state): the raw value (0..255) of every
// input in a byte, then a word with bit 0 set while the pad is connected and bit 1 while its analog
// light is on.
static constexpr u32 PAD_SNAPSHOT_WORDS = (NUM_PAD_INPUTS + 3) / 4 + 1;
static std::atomic<u32> s_pad_snapshot[NUM_PAD_PORTS][PAD_SNAPSHOT_WORDS];

// Racing wheels: each of the PS2's two USB ports can hold one of PCSX2's emulated Logitech wheels (USB/usb-pad:
// Driving Force, Driving Force Pro, Driving Force Pro rev 11.02, GT Force; the core's subtype numbers), picked by the
// page with pcsx2web_set_wheel(), next to the two pads. It is for a real Logitech wheel that understands the classic
// force feedback commands: the game's commands go out raw through the FFB ring (pcsx2web_ffb_ring) and the page sends
// them to that port's wheel with WebHID; the wheel's steering, pedals and buttons come back through pcsx2web_wheel().
static constexpr u32 NUM_WHEEL_PORTS = USB::NUM_PORTS;
static constexpr int WHEEL_NONE = -1;
static constexpr int WHEEL_NO_REQUEST = -2;
static constexpr int NUM_WHEEL_TYPES = 4;
static std::atomic<int> s_wheel_request[NUM_WHEEL_PORTS] = {WHEEL_NO_REQUEST, WHEEL_NO_REQUEST};
static int s_wheel_type[NUM_WHEEL_PORTS] = {WHEEL_NONE, WHEEL_NONE}; // CPU thread

// The FFB ring: one producer (the CPU thread, in the wheel's USB output handler), one consumer (the page). Words: 0
// head (commands written), 1 tail (commands the page took), 2 commands dropped because the ring was full, 3 the
// ring's size in commands, then two words per command: its first 7 bytes, and the USB port in the 8th.
static constexpr u32 FFB_RING_SIZE = 256;
static constexpr u32 FFB_RING_HEADER_WORDS = 4;
alignas(16) static std::atomic<u32> s_ffb_ring[FFB_RING_HEADER_WORDS + FFB_RING_SIZE * 2] = {};
static_assert((FFB_RING_SIZE & (FFB_RING_SIZE - 1)) == 0);

// Wheel input: the same single producer (browser main thread) and consumer (CPU thread) as the pads. Each entry:
// value 0..65535 in bits 0-15, the wheel's bind index (usb_pad::ControlID) in bits 16-23, the USB port in bit 24.
static constexpr u32 WHEEL_QUEUE_SIZE = 512;
static constexpr u32 WHEEL_EVENT_BIND_SHIFT = 16;
static constexpr u32 WHEEL_EVENT_PORT_SHIFT = 24;
static u32 s_wheel_queue[WHEEL_QUEUE_SIZE];
static std::atomic<u32> s_wheel_queue_head{0}; // written by the producer
static std::atomic<u32> s_wheel_queue_tail{0}; // written by the consumer
static_assert((WHEEL_QUEUE_SIZE & (WHEEL_QUEUE_SIZE - 1)) == 0);

// A USB port: nothing, or the core's wheel device ("Pad") with the given subtype. No bindings: the page feeds the
// wheel (ProcessWheelEvents), and no force feedback device: the page passes the commands to the real wheel.
static void ConfigureWheel(SettingsInterface& si, u32 port, int type)
{
	USB::ClearPortBindings(si, port); // the old device's
	if (type == WHEEL_NONE)
	{
		USB::SetConfigDevice(si, port, "None");
		return;
	}

	USB::SetConfigDevice(si, port, "Pad");
	USB::SetConfigSubType(si, port, "Pad", static_cast<u32>(type));
	USB::ClearPortBindings(si, port); // the new one's
	si.DeleteValue(USB::GetConfigSection(port).c_str(), USB::GetConfigSubKey("Pad", "FFDevice").c_str());
}

// CPU thread (usb_pad::SetRawFFBHandler): queues a force feedback command the game sent to the emulated wheel. A
// full ring drops the command and counts it; the page drains the ring every few milliseconds.
static void QueueWheelFFBCommand(u32 port, const u8* data, int len)
{
	if (len <= 0)
		return;

	const u32 head = s_ffb_ring[0].load(std::memory_order_relaxed);
	const u32 tail = s_ffb_ring[1].load(std::memory_order_acquire);
	if (head - tail >= FFB_RING_SIZE)
	{
		s_ffb_ring[2].fetch_add(1, std::memory_order_relaxed);
		return;
	}

	u8 bytes[8] = {};
	std::memcpy(bytes, data, std::min(len, 7));
	bytes[7] = static_cast<u8>(port);
	u32 words[2];
	std::memcpy(words, bytes, sizeof(words));
	const u32 slot = FFB_RING_HEADER_WORDS + (head % FFB_RING_SIZE) * 2;
	s_ffb_ring[slot].store(words[0], std::memory_order_relaxed);
	s_ffb_ring[slot + 1].store(words[1], std::memory_order_relaxed);
	s_ffb_ring[0].store(head + 1, std::memory_order_release);
}

//////////////////////////////////////////////////////////////////////////
// Discs, game details and memory cards
//////////////////////////////////////////////////////////////////////////

// Discs: the page opens the user's file (or a URL) in its disc worker (web/disc-worker.js), which
// serves the request block pcsx2web_disc_block() returns (pcsx2/CDVD/Emscripten/WebDisc.h). It writes
// a 0-byte placeholder at /disc/<name> and boots that path with pcsx2web_boot_disc(); the ISO reader
// then reads the disc through WebDiscFileReader (docs/web-port/DISC-DESIGN.md).
//
// Memory cards: both slots start empty. The page keeps a library of cards in IndexedDB, writes the ones it
// plugs in to /memcards (MEMFS) and names them with pcsx2web_memcard(); it copies a card back to IndexedDB
// after the game writes to it (app.js, "memory cards"). The CPU thread applies a change at the next vsync, or
// at boot: PCSX2 then reports the slot empty for a moment (AutoEject), so the game sees the card change.
//
// ELFs (developer runs, ?elf=): the page writes the file to /elf/<name> and boots it with
// pcsx2web_boot_elf(), with no disc and fast boot, as PCSX2 boots an ELF on the desktop.

static constexpr const char* DISC_DIRECTORY = "/disc";
static constexpr const char* ELF_DIRECTORY = "/elf";
static constexpr const char* MEMCARD_DIRECTORY = "/memcards";
// Developer save states (?threads=1, ?state=): files the page writes or reads directly inside /states.
static constexpr const char* STATE_DIRECTORY = "/states";

// pcsx2web_boot_disc() flags.
static constexpr int BOOT_FLAG_FULL_BOOT = 1; // run the BIOS intro instead of fast boot
static constexpr int BOOT_FLAG_ELF = 2; // internal: s_boot_disc_path is an ELF (pcsx2web_boot_elf())

// The disc (or ELF) for the boot request in flight; written by pcsx2web_boot_disc() or
// pcsx2web_boot_elf() between its claim of the STARTING state and the request, taken by RunVM() on
// the CPU thread.
static char s_boot_disc_path[512];
// The state the next boot loads (pcsx2web_set_boot_state()); empty for none. Written by the page while no VM starts
// or runs, read by the CPU thread at the start of RunVM().
static char s_boot_state_path[512];
// Save state results for the page: (sequence << 1) | ok, the sequence counting finished saves and loads.
static std::atomic<unsigned int> s_state_result{0};
// (sequence << 1) | ok of the last finished pcsx2web_change_disc(), as s_state_result.
static std::atomic<unsigned int> s_disc_change_result{0};
static std::atomic<int> s_boot_disc_flags{0};
static std::atomic<bool> s_boot_disc_requested{false};

struct BootDisc
{
	std::string path; // empty: the BIOS alone
	std::string elf; // an ELF to boot instead of a disc
	bool full_boot = false;
};

// Game details for the page, as a seqlock: the CPU thread makes seq odd, writes, then makes it even;
// the page copies the fields and keeps them only if seq was even and unchanged.
struct GameDetails
{
	std::atomic<u32> seq;
	u32 disc_crc;
	u32 elf_crc;
	char serial[32];
	char title[256];
};
static GameDetails s_game_details = {};

// One request per slot, handed from the page to the CPU thread without a lock: the page writes only a FREE or
// READY request (one the CPU thread has not taken yet is replaced), the CPU thread takes only a READY one.
static constexpr int NUM_MEMCARD_SLOTS = 2;
enum : int
{
	MEMCARD_REQUEST_FREE,
	MEMCARD_REQUEST_WRITING, // the page is writing it
	MEMCARD_REQUEST_READY,
	MEMCARD_REQUEST_TAKING, // the CPU thread is reading it
};
struct MemcardRequest
{
	std::atomic<int> state{MEMCARD_REQUEST_FREE};
	u32 id = 0;
	char file[64] = {}; // a name inside /memcards; empty: unplug
};
static MemcardRequest s_memcard_requests[NUM_MEMCARD_SLOTS];

// For pcsx2web_memcard_status(): {flags, the id of the last request applied in slot 1, in slot 2}. Flag 1: the
// game wrote to a card in the last 300 vsyncs (MemcardBusy), so pulling a card now could cut a save short.
static constexpr u32 MEMCARD_STATUS_BUSY = 1;
static std::atomic<u32> s_memcard_status[1 + NUM_MEMCARD_SLOTS];

// The VU1 JIT fuzzer (pcsx2web_vu1jit_fuzz) uses VU1 itself: no boot while it runs.
static bool VU1JitFuzzRunning()
{
	return VU1Jit::GetFuzzStatus().state.load(std::memory_order_acquire) == 1;
}

static void ConfigureMemoryCards(SettingsInterface& si)
{
	si.SetStringValue("Folders", "MemoryCards", MEMCARD_DIRECTORY);
	for (int slot = 0; slot < NUM_MEMCARD_SLOTS; slot++)
	{
		si.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", slot + 1).c_str(), false);
		si.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", slot + 1).c_str(), "");
	}
}

// CPU thread: takes the page's card requests into the settings, applied now (a running VM ejects and reopens the
// cards that changed) or by the caller (a boot).
static void CheckMemcardRequests(bool apply_now)
{
	u32 applied[NUM_MEMCARD_SLOTS] = {};
	for (int slot = 0; slot < NUM_MEMCARD_SLOTS; slot++)
	{
		MemcardRequest& r = s_memcard_requests[slot];
		int expected = MEMCARD_REQUEST_READY;
		if (!r.state.compare_exchange_strong(expected, MEMCARD_REQUEST_TAKING, std::memory_order_acquire))
			continue;
		const u32 id = r.id;
		const std::string file = r.file;
		r.state.store(MEMCARD_REQUEST_FREE, std::memory_order_release);

		auto lock = Host::GetSettingsLock();
		s_settings_interface.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", slot + 1).c_str(), !file.empty());
		s_settings_interface.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", slot + 1).c_str(), file.c_str());
		applied[slot] = id;
		Console.WriteLnFmt("pcsx2-wasm: memory card slot {}: {}", slot + 1, file.empty() ? "empty" : file);
	}

	bool changed = false;
	for (u32 id : applied)
		changed |= (id != 0);
	if (changed && apply_now && VMManager::HasValidVM())
		VMManager::ApplySettings();
	for (int slot = 0; slot < NUM_MEMCARD_SLOTS; slot++)
	{
		if (applied[slot] != 0)
			s_memcard_status[1 + slot].store(applied[slot], std::memory_order_release);
	}
}

static bool EnsureDiscAndMemoryCardFolders()
{
	// The page creates both (and mounts IndexedDB on /memcards); without them the cards would fail.
	Error error;
	for (const char* dir : {DISC_DIRECTORY, MEMCARD_DIRECTORY})
	{
		if (!FileSystem::EnsureDirectoryExists(dir, false, &error))
		{
			Console.ErrorFmt("Failed to create {}: {}", dir, error.GetDescription());
			return false;
		}
	}
	return true;
}

// CPU thread, at the start of every RunVM(): the disc of this request, if it is a disc boot.
static BootDisc TakeBootDisc()
{
	BootDisc disc;
	if (s_boot_disc_requested.exchange(false, std::memory_order_acq_rel))
	{
		const int flags = s_boot_disc_flags.load(std::memory_order_relaxed);
		if (flags & BOOT_FLAG_ELF)
			disc.elf = s_boot_disc_path;
		else
			disc.path = s_boot_disc_path;
		disc.full_boot = (flags & BOOT_FLAG_FULL_BOOT) != 0;
	}
	return disc;
}

// RunVM() has set up a BIOS boot (no disc, no fast boot); a disc or ELF boot changes that.
static void MakeBootParameters(const std::string& bios_name, const BootDisc& disc, VMBootParameters* params)
{
	if (s_boot_state_path[0])
	{
		params->save_state = s_boot_state_path;
		Console.WriteLnFmt("pcsx2-wasm: the boot loads the state '{}'", s_boot_state_path);
	}
	if (!disc.elf.empty())
	{
		// PCSX2 always fast boots an ELF: the BIOS starts it instead of its menu.
		params->elf_override = disc.elf;
		params->fast_boot = true;
		Console.WriteLnFmt("pcsx2-wasm: booting ELF '{}' with BIOS '{}'", disc.elf, bios_name);
		return;
	}

	if (disc.path.empty())
	{
		Console.WriteLnFmt("pcsx2-wasm: booting BIOS '{}'", bios_name);
		return;
	}

	// Fast boot by default, as PCSX2's desktop frontend: the BIOS intro is skipped and the game starts.
	params->filename = disc.path;
	params->source_type = CDVD_SourceType::Iso;
	params->fast_boot = !disc.full_boot;
	Console.WriteLnFmt("pcsx2-wasm: booting '{}' with BIOS '{}' ({})", disc.path, bios_name,
		disc.full_boot ? "full boot" : "fast boot");
}

// CPU thread (Host::OnGameChanged).
static void PublishGameDetails(const std::string& title, const std::string& serial, u32 disc_crc, u32 elf_crc)
{
	GameDetails& d = s_game_details;
	const u32 seq = d.seq.load(std::memory_order_relaxed);
	d.seq.store(seq + 1, std::memory_order_relaxed);
	std::atomic_thread_fence(std::memory_order_seq_cst);
	StringUtil::Strlcpy(d.title, title, sizeof(d.title));
	StringUtil::Strlcpy(d.serial, serial, sizeof(d.serial));
	d.disc_crc = disc_crc;
	d.elf_crc = elf_crc;
	std::atomic_thread_fence(std::memory_order_seq_cst);
	d.seq.store(seq + 2, std::memory_order_release);
}

extern "C" {

/// Boots the disc at disc_path ("/disc/<name>", served by the page's disc worker) with the BIOS named
/// bios_filename inside /bios. flags: 1 = full boot (the BIOS intro), otherwise fast boot. 0 if
/// accepted; -1 as pcsx2web_boot(), or if disc_path is not a disc path.
EMSCRIPTEN_KEEPALIVE int pcsx2web_boot_disc(const char* bios_filename, const char* disc_path, int flags)
{
	if (!bios_filename || !disc_path || s_fatal_error.load(std::memory_order_acquire) || VU1JitFuzzRunning())
		return -1;

	const size_t bios_length = std::strlen(bios_filename);
	const size_t disc_length = std::strlen(disc_path);
	if (bios_length == 0 || bios_length >= sizeof(s_boot_bios_name) || std::strchr(bios_filename, '/') ||
		std::strcmp(bios_filename, ".") == 0 || std::strcmp(bios_filename, "..") == 0 ||
		disc_length >= sizeof(s_boot_disc_path) || !WebDisc::IsDiscPath(disc_path))
	{
		return -1;
	}

	int state = s_state.load(std::memory_order_acquire);
	do
	{
		if (state == WEB_STATE_STARTING || state == WEB_STATE_RUNNING)
			return -1;
	} while (!s_state.compare_exchange_weak(state, WEB_STATE_STARTING, std::memory_order_acq_rel));

	std::memcpy(s_boot_bios_name, bios_filename, bios_length + 1);
	std::memcpy(s_boot_disc_path, disc_path, disc_length + 1);
	s_boot_disc_flags.store(flags & BOOT_FLAG_FULL_BOOT, std::memory_order_relaxed);
	s_boot_disc_requested.store(true, std::memory_order_release);
	s_stop_requested.store(false, std::memory_order_release);
	// The last Stop cancelled disc reads; this boot reads again. Here, before the request, and on the page's
	// thread like pcsx2web_stop(): a Stop that follows this call always cancels this boot's reads.
	WebDisc::ResumeReads();
	s_boot_request.store(1, std::memory_order_release);
	emscripten_futex_wake(&s_boot_request, 1);
	return 0;
}

/// Developer runs (?elf=): boots the PS2 executable at elf_path ("/elf/<name>", which the page wrote)
/// with the BIOS named bios_filename inside /bios, without a disc. 0 if accepted; -1 as
/// pcsx2web_boot(), or if elf_path is not a file directly inside /elf.
EMSCRIPTEN_KEEPALIVE int pcsx2web_boot_elf(const char* bios_filename, const char* elf_path)
{
	if (!bios_filename || !elf_path || s_fatal_error.load(std::memory_order_acquire) || VU1JitFuzzRunning())
		return -1;

	const size_t bios_length = std::strlen(bios_filename);
	const size_t elf_length = std::strlen(elf_path);
	const size_t dir_length = std::strlen(ELF_DIRECTORY);
	const char* elf_name = (elf_length > dir_length) ? elf_path + dir_length + 1 : elf_path;
	if (bios_length == 0 || bios_length >= sizeof(s_boot_bios_name) || std::strchr(bios_filename, '/') ||
		std::strcmp(bios_filename, ".") == 0 || std::strcmp(bios_filename, "..") == 0 ||
		elf_length >= sizeof(s_boot_disc_path) || elf_length <= dir_length + 1 ||
		std::strncmp(elf_path, ELF_DIRECTORY, dir_length) != 0 || elf_path[dir_length] != '/' ||
		std::strchr(elf_name, '/') || std::strcmp(elf_name, ".") == 0 || std::strcmp(elf_name, "..") == 0)
	{
		return -1;
	}

	int state = s_state.load(std::memory_order_acquire);
	do
	{
		if (state == WEB_STATE_STARTING || state == WEB_STATE_RUNNING)
			return -1;
	} while (!s_state.compare_exchange_weak(state, WEB_STATE_STARTING, std::memory_order_acq_rel));

	std::memcpy(s_boot_bios_name, bios_filename, bios_length + 1);
	std::memcpy(s_boot_disc_path, elf_path, elf_length + 1);
	s_boot_disc_flags.store(BOOT_FLAG_ELF, std::memory_order_relaxed);
	s_boot_disc_requested.store(true, std::memory_order_release);
	s_stop_requested.store(false, std::memory_order_release);
	s_boot_request.store(1, std::memory_order_release);
	emscripten_futex_wake(&s_boot_request, 1);
	return 0;
}

// A save state path the page may use: a plain file name directly inside /states.
static bool IsStatePath(const char* path)
{
	const size_t length = std::strlen(path);
	const size_t dir_length = std::strlen(STATE_DIRECTORY);
	if (length >= sizeof(s_boot_state_path) || length <= dir_length + 1 ||
		std::strncmp(path, STATE_DIRECTORY, dir_length) != 0 || path[dir_length] != '/')
		return false;
	const char* name = path + dir_length + 1;
	return !std::strchr(name, '/') && std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0;
}

/// Developer runs (?state=): the save state the next boot loads once the game started (a path the page wrote
/// directly inside /states), or "" for none. Only while no VM starts or runs. 0 if accepted, -1 otherwise.
EMSCRIPTEN_KEEPALIVE int pcsx2web_set_boot_state(const char* path)
{
	if (!path || (path[0] && !IsStatePath(path)))
		return -1;
	const int state = s_state.load(std::memory_order_acquire);
	if (state == WEB_STATE_STARTING || state == WEB_STATE_RUNNING)
		return -1;
	StringUtil::Strlcpy(s_boot_state_path, path, sizeof(s_boot_state_path));
	return 0;
}

// Queues a save or load of the state at path on the CPU thread, which publishes the result in s_state_result.
static int QueueStateOperation(const char* path, bool save)
{
	if (!path || !IsStatePath(path) || s_state.load(std::memory_order_acquire) != WEB_STATE_RUNNING)
		return -1;
	Host::RunOnCPUThread([path = std::string(path), save]() {
		bool ok = VMManager::HasValidVM();
		if (ok && save)
		{
			Error error;
			FileSystem::EnsureDirectoryExists(STATE_DIRECTORY, false, &error);
			// On this thread, not a compression thread of its own: the page reads the file once this returns.
			VMManager::SaveState(path.c_str(), false, false, [&ok](const std::string& message) {
				Console.ErrorFmt("pcsx2-wasm: saving the state failed: {}", message);
				ok = false;
			});
		}
		else if (ok)
		{
			Error error;
			ok = VMManager::LoadState(path.c_str(), &error);
			if (!ok)
				Console.ErrorFmt("pcsx2-wasm: loading the state failed: {}", error.GetDescription());
		}
		const unsigned int seq = (s_state_result.load(std::memory_order_relaxed) >> 1) + 1;
		s_state_result.store((seq << 1) | (ok ? 1u : 0u), std::memory_order_release);
	});
	return 0;
}

/// Developer runs (?threads=1): saves the running VM's state to path (directly inside /states) at the CPU
/// thread's next pause point. 0 if queued, -1 otherwise; pcsx2web_state_result() changes when it is done.
EMSCRIPTEN_KEEPALIVE int pcsx2web_save_state(const char* path)
{
	return QueueStateOperation(path, true);
}

/// Developer runs: loads the state at path (directly inside /states) into the running VM. As
/// pcsx2web_save_state().
EMSCRIPTEN_KEEPALIVE int pcsx2web_load_state(const char* path)
{
	return QueueStateOperation(path, false);
}

/// (sequence << 1) | ok of the last finished pcsx2web_save_state() or pcsx2web_load_state().
EMSCRIPTEN_KEEPALIVE unsigned int pcsx2web_state_result(void)
{
	return s_state_result.load(std::memory_order_acquire);
}

/// Disc swap while the game runs, in two steps as on the console: "" ejects the disc (the tray opens and
/// PCSX2 closes the old reader), then the page opens the new file in its disc worker and passes its path
/// ("/disc/<name>") to insert it. Ejecting first means nothing reads the old disc once the worker has
/// replaced it, and PCSX2's fallback when an insert fails is "no disc", never the old file. Runs on the CPU
/// thread at its next pause point. 0 if queued, -1 otherwise; pcsx2web_disc_change_result() changes when it
/// is done.
EMSCRIPTEN_KEEPALIVE int pcsx2web_change_disc(const char* path)
{
	if (!path || (path[0] && (std::strlen(path) >= sizeof(s_boot_disc_path) || !WebDisc::IsDiscPath(path))) ||
		s_state.load(std::memory_order_acquire) != WEB_STATE_RUNNING)
		return -1;
	Host::RunOnCPUThread([path = std::string(path)]() {
		bool ok = VMManager::HasValidVM();
		if (ok)
			ok = VMManager::ChangeDisc(path.empty() ? CDVD_SourceType::NoDisc : CDVD_SourceType::Iso, path);
		if (!ok)
			Console.ErrorFmt("pcsx2-wasm: {} failed", path.empty() ? "ejecting the disc" : "inserting " + path);
		const unsigned int seq = (s_disc_change_result.load(std::memory_order_relaxed) >> 1) + 1;
		s_disc_change_result.store((seq << 1) | (ok ? 1u : 0u), std::memory_order_release);
	});
	return 0;
}

/// (sequence << 1) | ok of the last finished pcsx2web_change_disc().
EMSCRIPTEN_KEEPALIVE unsigned int pcsx2web_disc_change_result(void)
{
	return s_disc_change_result.load(std::memory_order_acquire);
}

/// Each pad port's vibration, two words (large motor << 8 | small motor, 0-255 each) that PCSX2 updates on every pad
/// poll; the page plays them on that player's gamepads (UI-SPEC.md §17). Read with Atomics; never blocks.
EMSCRIPTEN_KEEPALIVE const void* pcsx2web_pad_vibration(void)
{
	return InputManager::GetWebPadVibration();
}

/// The disc request block the page's disc worker serves (docs/web-port/DISC-DESIGN.md, 4.1).
EMSCRIPTEN_KEEPALIVE const void* pcsx2web_disc_block(void)
{
	return WebDisc::GetBlock();
}

/// The running game, as {seq, disc CRC, ELF CRC, serial[32], title[256]} (UTF-8, NUL-terminated).
/// seq is odd while the CPU thread writes; copy the fields, then keep them only if seq was even and
/// has not changed.
EMSCRIPTEN_KEEPALIVE const void* pcsx2web_game(void)
{
	return &s_game_details;
}

/// Plugs the card file (a plain name inside /memcards) into slot (0 or 1), or empties the slot when file is "".
/// Applied by the CPU thread at the next vsync, or at the next boot; id (not 0) then shows in
/// pcsx2web_memcard_status(). A request the CPU thread has not taken yet is replaced. 0 if accepted; 1 if the CPU
/// thread is reading the last one (call again); -1 if an argument is wrong.
EMSCRIPTEN_KEEPALIVE int pcsx2web_memcard(int slot, const char* file, unsigned int id)
{
	if (slot < 0 || slot >= NUM_MEMCARD_SLOTS || !file || id == 0)
		return -1;
	MemcardRequest& r = s_memcard_requests[slot];
	const size_t length = std::strlen(file);
	if (length >= sizeof(r.file) || std::strchr(file, '/') || std::strcmp(file, ".") == 0 || std::strcmp(file, "..") == 0)
		return -1;

	int state = r.state.load(std::memory_order_acquire);
	do
	{
		if (state != MEMCARD_REQUEST_FREE && state != MEMCARD_REQUEST_READY)
			return 1;
	} while (!r.state.compare_exchange_weak(state, MEMCARD_REQUEST_WRITING, std::memory_order_acquire));
	r.id = id;
	std::memcpy(r.file, file, length + 1);
	r.state.store(MEMCARD_REQUEST_READY, std::memory_order_release);
	return 0;
}

/// {flags, id applied in slot 1, id applied in slot 2}; flags bit 0: the game wrote to a card in the last
/// 300 vsyncs (cleared when the VM stops).
EMSCRIPTEN_KEEPALIVE const unsigned int* pcsx2web_memcard_status(void)
{
	static_assert(sizeof(s_memcard_status) == (1 + NUM_MEMCARD_SLOTS) * sizeof(u32));
	return reinterpret_cast<const unsigned int*>(s_memcard_status);
}

} // extern "C"

//////////////////////////////////////////////////////////////////////////
// Audio
//////////////////////////////////////////////////////////////////////////

// Sound plays through an Emscripten Wasm Audio Worklet (docs/web-port/AUDIO-DESIGN.md). The page makes an
// AudioContext in a user gesture and calls pcsx2web_audio_start(), which loads this module into the context's
// worklet scope and creates one processor node for the page's lifetime. Its callback, AudioRender(), runs on the
// browser's audio rendering thread and reads the attached WebAudioStream with AudioStream::ReadFrames(), one render
// quantum (128 frames) at a time.
//
// SPU2 writes the stream on the CPU thread, time-stretching included. The CPU thread alone attaches and detaches the
// stream, once per vsync (UpdateAudioOutput()): attached while the worklet's heartbeat moves, otherwise detached and
// drained at the real-time rate, so a context that does not run (autoplay, after Stop, an interruption) leaves no
// backlog. ReadFrames() so always has exactly one caller. Nothing on the audio thread may block or allocate. Below
// full speed the stream's target buffer steps up, in place, so that time-stretching keeps the sound continuous.

namespace
{
	// Words of the audio block, pcsx2web_audio() (AUDIO-DESIGN.md 4.5).
	enum AudioBlockWord : u32
	{
		AUDIO_MAGIC,
		AUDIO_VERSION,
		AUDIO_WORKLET, // AudioWorkletState, written on the page's thread
		AUDIO_STREAM_RATE, // the stream's rate (48000, 44100 in PS1 mode), 0 while there is no stream
		AUDIO_ATTACHED, // 1 while the worklet reads the stream
		AUDIO_HEARTBEAT, // render quanta seen (audio thread)
		AUDIO_FRAMES, // frames the worklet read from streams
		AUDIO_SHORT_QUANTA, // quanta that could not be filled from the stream (an underrun, or refilling after one)
		AUDIO_DRAINED, // frames read and discarded while detached
		AUDIO_CONTEXT_RATE, // the AudioContext's rate
		AUDIO_BUFFER_MS, // the stream's BufferMS (the slow-scene step)
		AUDIO_FILL, // frames buffered in the stream at the last vsync
		AUDIO_ATTACHES, // times a stream was attached
		AUDIO_STEPS, // BufferMS steps applied
		AUDIO_STREAMS, // streams created
		AUDIO_RESERVED,
		AUDIO_BLOCK_WORDS,
	};

	enum AudioWorkletState : u32
	{
		AUDIO_WORKLET_NONE,
		AUDIO_WORKLET_STARTING,
		AUDIO_WORKLET_RUNNING,
		AUDIO_WORKLET_FAILED,
	};

	class WebAudioStream;
} // namespace

static constexpr u32 AUDIO_BLOCK_MAGIC = 0x44554154u; // "TAUD"
static constexpr u32 AUDIO_BLOCK_VERSION = 1;
static constexpr const char* AUDIO_PROCESSOR_NAME = "tabstationtoo-audio";
// Frames per render quantum the callback serves (Web Audio 1.0 renders 128), and the resampling ratio it accepts:
// together they bound the frames one ReadFrames() call reads, and so its alloca() on the worklet's 64 KB stack. The
// ratio is the stream's rate over the context's: 48 kHz plays into contexts from 24 kHz to 384 kHz.
static constexpr u32 AUDIO_MAX_QUANTUM = 512;
static constexpr double AUDIO_MIN_STEP = 0.125;
static constexpr double AUDIO_MAX_STEP = 2.0;
static constexpr u32 AUDIO_DRAIN_CHUNK = 1024;
static constexpr double AUDIO_HEARTBEAT_TIMEOUT = 0.1; // s without a render quantum: detach and drain
static constexpr double AUDIO_MAX_DRAIN = 1.0; // s drained at most at one vsync
static constexpr double AUDIO_DETACH_WARN = 1.0; // s waiting for the audio thread to leave its callback: logged

alignas(64) static std::atomic<u32> s_audio_block[AUDIO_BLOCK_WORDS] = {AUDIO_BLOCK_MAGIC, AUDIO_BLOCK_VERSION};
alignas(16) static u8 s_audio_worklet_stack[64 * 1024];

// The gate (AUDIO-DESIGN.md 4.2): the stream the worklet reads, or null. Only the CPU thread changes it. Both sides
// use seq_cst with s_audio_in_callback, as in Dekker's algorithm: once WebAudioStream::Detach() returns, the worklet is
// out of ReadFrames() and will not enter it again.
static std::atomic<WebAudioStream*> s_audio_reader{nullptr};
static std::atomic<bool> s_audio_in_callback{false};

// CPU thread.
static WebAudioStream* s_audio_stream = nullptr; // the stream SPU2 writes
static u32 s_audio_heartbeat_seen = 0;
static Common::Timer::Value s_audio_heartbeat_moved = 0; // 0: never
static bool s_audio_null = false; // -audio-null: PCSX2's Null backend (for cost comparisons)

// The buffer for slow scenes (AUDIO-DESIGN.md 4.4): stretched sound is continuous once the target buffer x speed is
// about 50-60 ms. Each step is for speeds (percent) from its minimum up, which keeps that product at 50 ms or more
// below full speed. Every stream is made with the largest step's buffer and aims for the current step's
// (AudioStream::SetTargetBufferMS()), so a step keeps everything buffered. A bigger step applies at once, once the
// speed has called for it for 1 s. A smaller one needs the speed well beyond its boundary (AudioStepDownSpeed()) for
// 10 s, since games swing between fast and slow scenes within seconds, and is then reached gradually: the target
// falls by AUDIO_STEP_DOWN_RATE, so the stretcher plays the excess out a little faster (a tempo of about 1.1)
// instead of skipping it or leaving the smaller buffer short.
static constexpr std::array<u16, 5> AUDIO_BUFFER_STEPS = {50, 100, 200, 400, 800};
static constexpr std::array<float, 5> AUDIO_STEP_MIN_SPEED = {75.0f, 50.0f, 28.0f, 14.0f, 0.0f};
static constexpr double AUDIO_STEP_UP_AFTER = 1.0; // s
static constexpr double AUDIO_STEP_DOWN_AFTER = 10.0; // s
static constexpr double AUDIO_STEP_DOWN_RATE = 100.0; // ms of target per s
// Near full speed, frame time spikes (a slow frame, a disc read, a GPU stall) can empty the 50 ms buffer: the
// stretcher stays at tempo 1 within 20 % of the target (AudioStream::UpdateStretchTempo()), so it does not slow the
// sound down for a scene at 90-99 % speed either. An underrun at the first step therefore moves to the second at
// once, and it steps back down only after AUDIO_UNDERRUN_HOLD without one.
static constexpr double AUDIO_UNDERRUN_HOLD = 30.0; // s
static constexpr double AUDIO_UNDERRUN_GRACE = 3.0; // s after a stream attaches: underruns there are the start's
static u32 s_audio_step = 0; // CPU thread
static double s_audio_target_ms = AUDIO_BUFFER_STEPS[0]; // what the stream aims for: the step's, or above it
static Common::Timer::Value s_audio_step_up_since = 0;
static Common::Timer::Value s_audio_step_down_since = 0;
static Common::Timer::Value s_audio_target_updated = 0; // 0: not while running
static u32 s_audio_short_seen = 0; // AUDIO_SHORT_QUANTA at the last look
static Common::Timer::Value s_audio_underrun_at = 0; // the last underrun noticed while running, 0: none
static Common::Timer::Value s_audio_running_since = 0; // attached and running since, 0: not

static bool AudioRender(int num_inputs, const AudioSampleFrame* inputs, int num_outputs, AudioSampleFrame* outputs,
	int num_params, const AudioParamFrame* params, void* user_data);

// The worklet scope evaluates this module's JS too; these come from Emscripten's library (libwebaudio.js).
EM_JS_DEPS(pcsx2web_audio, "$emscriptenRegisterAudioObject,$emscriptenGetAudioObject");

namespace
{
	class WebAudioStream final : public AudioStream
	{
	public:
		WebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
		~WebAudioStream() override;

		/// AudioBackend::Frontend's factory (AudioStream::SetFrontendStreamFactory()), on the CPU thread.
		static std::unique_ptr<AudioStream> Create(u32 sample_rate, const AudioStreamParameters& parameters,
			bool stretch_enabled, Error* error);

		/// A paused device stops pulling: detach, and do not drain.
		void SetPaused(bool paused) override;

		/// CPU thread, once per vsync: attached while the worklet runs, otherwise played to nowhere at the
		/// real-time rate, as a device would, so the stream's fill and time-stretching stay what playback leaves.
		void Service(Common::Timer::Value now, bool worklet_alive);

		/// Audio thread, through the gate: one render quantum, planar.
		void Render(float* left, float* right, u32 frames);

	private:
		void Attach();
		void Detach();
		void Drain(Common::Timer::Value now);
		void RenderResampled(float* left, float* right, u32 frames);

		// Interleaved frames for Render(): a quantum at the widest step, plus the frames carried between quanta.
		static constexpr u32 RENDER_BUFFER_FRAMES = static_cast<u32>(AUDIO_MAX_QUANTUM * AUDIO_MAX_STEP) + 4;

		std::unique_ptr<SampleType[]> m_render_buffer;
		std::unique_ptr<SampleType[]> m_drain_buffer;
		double m_drain_time = 0.0; // s, Common::Timer: the stream has been drained up to here

		// Linear resampling when the stream's rate is not the context's (PS1 mode: 44.1 kHz into 48 kHz). The
		// buffer starts with m_carry frames already read; the next output frame sits m_position frames after its
		// first one. m_skip frames are still to be dropped before the next read (only for steps above 1).
		double m_step = 1.0; // stream frames per output frame
		double m_position = 0.0;
		u32 m_carry = 0;
		u32 m_skip = 0;
	};

	WebAudioStream::WebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
		: AudioStream(sample_rate, parameters)
		, m_render_buffer(std::make_unique<SampleType[]>(RENDER_BUFFER_FRAMES * NUM_INPUT_CHANNELS))
		, m_drain_buffer(std::make_unique<SampleType[]>(AUDIO_DRAIN_CHUNK * NUM_INPUT_CHANNELS))
	{
	}

	WebAudioStream::~WebAudioStream()
	{
		if (s_audio_reader.load(std::memory_order_relaxed) == this)
			Detach();
		if (s_audio_stream == this)
		{
			s_audio_stream = nullptr;
			s_audio_block[AUDIO_STREAM_RATE].store(0, std::memory_order_relaxed);
			s_audio_block[AUDIO_FILL].store(0, std::memory_order_relaxed);
		}
	}

	void WebAudioStream::SetPaused(bool paused)
	{
		m_paused = paused;
		if (paused)
		{
			if (s_audio_reader.load(std::memory_order_relaxed) == this)
				Detach();
		}
		else
		{
			// The next vsync attaches, or drains from now on.
			m_drain_time = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue());
		}
	}

	void WebAudioStream::Attach()
	{
		const u32 context_rate = s_audio_block[AUDIO_CONTEXT_RATE].load(std::memory_order_relaxed);
		m_step = (context_rate == 0 || context_rate == m_sample_rate) ?
					 1.0 :
					 std::clamp(static_cast<double>(m_sample_rate) / context_rate, AUDIO_MIN_STEP, AUDIO_MAX_STEP);
		m_position = 0.0;
		m_carry = 1; // starts from a silent frame
		m_skip = 0;
		std::fill_n(m_render_buffer.get(), NUM_INPUT_CHANNELS, 0.0f);

		// Publishes the fields above to the audio thread.
		s_audio_reader.store(this, std::memory_order_seq_cst);
		s_audio_block[AUDIO_ATTACHED].store(1, std::memory_order_relaxed);
		s_audio_block[AUDIO_ATTACHES].fetch_add(1, std::memory_order_relaxed);
	}

	void WebAudioStream::Detach()
	{
		s_audio_reader.store(nullptr, std::memory_order_seq_cst);
		// At most the rest of one quantum's work: microseconds, unless the audio thread is preempted. A callback that
		// trapped never clears its flag; the page then marks the worklet failed (its processorerror event), and the
		// audio thread never runs again. Until then this must wait: the stream may be freed next.
		const Common::Timer::Value start = Common::Timer::GetCurrentValue();
		bool warned = false;
		while (s_audio_in_callback.load(std::memory_order_seq_cst))
		{
			if (s_audio_block[AUDIO_WORKLET].load(std::memory_order_acquire) == AUDIO_WORKLET_FAILED)
				break;
			if (!warned &&
				Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue() - start) >= AUDIO_DETACH_WARN)
			{
				warned = true;
				Console.Warning("pcsx2-wasm: still waiting for the audio thread to leave its callback");
			}
			std::this_thread::yield();
		}
		s_audio_block[AUDIO_ATTACHED].store(0, std::memory_order_relaxed);
	}

	void WebAudioStream::Service(Common::Timer::Value now, bool worklet_alive)
	{
		if (m_paused)
			return;

		const bool attached = (s_audio_reader.load(std::memory_order_relaxed) == this);
		if (worklet_alive)
		{
			if (!attached)
				Attach();
		}
		else
		{
			if (attached)
			{
				Detach();
				m_drain_time = Common::Timer::ConvertValueToSeconds(now);
			}
			Drain(now);
		}
		s_audio_block[AUDIO_FILL].store(GetBufferedFramesRelaxed(), std::memory_order_relaxed);
	}

	void WebAudioStream::Drain(Common::Timer::Value now)
	{
		const double t = Common::Timer::ConvertValueToSeconds(now);
		u32 frames;
		if (t - m_drain_time > AUDIO_MAX_DRAIN)
		{
			// The CPU thread was held up: what is older than that is not played.
			frames = static_cast<u32>(AUDIO_MAX_DRAIN * m_sample_rate);
			m_drain_time = t;
		}
		else
		{
			frames = static_cast<u32>(std::max(0.0, t - m_drain_time) * m_sample_rate);
			m_drain_time += static_cast<double>(frames) / m_sample_rate;
		}

		s_audio_block[AUDIO_DRAINED].fetch_add(frames, std::memory_order_relaxed);
		while (frames > 0)
		{
			const u32 count = std::min(frames, AUDIO_DRAIN_CHUNK);
			ReadFrames(m_drain_buffer.get(), count);
			frames -= count;
		}
	}

	void WebAudioStream::Render(float* left, float* right, u32 frames)
	{
		if (m_step == 1.0)
		{
			SampleType* const buffer = m_render_buffer.get();
			ReadFrames(buffer, frames);
			for (u32 i = 0; i < frames; i++)
			{
				left[i] = buffer[i * 2];
				right[i] = buffer[i * 2 + 1];
			}
			s_audio_block[AUDIO_FRAMES].fetch_add(frames, std::memory_order_relaxed);
		}
		else
		{
			RenderResampled(left, right, frames);
		}

		// ReadFrames() stays in its filling state from an underrun until the buffer holds enough again; meanwhile
		// it plays silence, or what little it has spread over the quantum.
		if (m_filling)
			s_audio_block[AUDIO_SHORT_QUANTA].fetch_add(1, std::memory_order_relaxed);
	}

	void WebAudioStream::RenderResampled(float* left, float* right, u32 frames)
	{
		SampleType* const buffer = m_render_buffer.get();
		const bool was_filling = m_filling;
		u32 read = 0;
		if (m_skip > 0)
		{
			// m_carry is 0: the skipped frames land where the read below overwrites them.
			ReadFrames(buffer, m_skip);
			read += m_skip;
			m_skip = 0;
		}

		// Output frame i sits at m_position + i * m_step in the buffer, between two frames read.
		const double last = m_position + (frames - 1) * m_step;
		const u32 total = static_cast<u32>(last) + 2;
		ReadFrames(buffer + m_carry * NUM_INPUT_CHANNELS, total - m_carry);
		read += total - m_carry;
		if (was_filling && m_filling)
		{
			// Refilling after an underrun: ReadFrames() gave only silence. The frames carried from before the
			// underrun go too, so the quantum is all silence and SmoothAudioEdges() ramps out of the sound, instead of
			// the carried frames stepping down to zero within the quantum. The sound comes back faded in.
			std::fill_n(buffer, total * NUM_INPUT_CHANNELS, 0.0f);
		}
		for (u32 i = 0; i < frames; i++)
		{
			const double x = m_position + i * m_step;
			const u32 index = static_cast<u32>(x);
			const float frac = static_cast<float>(x - index);
			const SampleType* const a = buffer + index * NUM_INPUT_CHANNELS;
			left[i] = a[0] + (a[2] - a[0]) * frac;
			right[i] = a[1] + (a[3] - a[1]) * frac;
		}

		const double next = m_position + frames * m_step;
		const u32 first = static_cast<u32>(next);
		m_position = next - first;
		if (first < total)
		{
			m_carry = total - first;
			std::memmove(buffer, buffer + first * NUM_INPUT_CHANNELS, m_carry * NUM_INPUT_CHANNELS * sizeof(SampleType));
		}
		else
		{
			m_carry = 0;
			m_skip = first - total;
		}
		s_audio_block[AUDIO_FRAMES].fetch_add(read, std::memory_order_relaxed);
	}
} // namespace

// CPU thread: whether the worklet rendered a quantum in the last AUDIO_HEARTBEAT_TIMEOUT (as far as the CPU thread's
// looks can tell: at 15 % speed it looks every 111 ms).
static bool IsAudioWorkletAlive(Common::Timer::Value now)
{
	const u32 heartbeat = s_audio_block[AUDIO_HEARTBEAT].load(std::memory_order_relaxed);
	if (heartbeat != s_audio_heartbeat_seen)
	{
		s_audio_heartbeat_seen = heartbeat;
		s_audio_heartbeat_moved = now;
		return true;
	}
	return s_audio_heartbeat_moved != 0 &&
		   Common::Timer::ConvertValueToSeconds(now - s_audio_heartbeat_moved) < AUDIO_HEARTBEAT_TIMEOUT;
}

std::unique_ptr<AudioStream> WebAudioStream::Create(u32 sample_rate, const AudioStreamParameters& parameters,
	bool stretch_enabled, Error* error)
{
	// Stereo only, what SPU2 mixes: no surround expansion (AUDIO-DESIGN.md 5).
	AudioStreamParameters params = parameters;
	params.expansion_mode = AudioExpansionMode::Disabled;

	// The largest step's buffer, aiming for the current target (the buffer steps never recreate the stream).
	params.buffer_ms = AUDIO_BUFFER_STEPS.back();

	std::unique_ptr<WebAudioStream> stream = std::make_unique<WebAudioStream>(sample_rate, params);
	stream->BaseInitialize(&StereoSampleReaderImpl, stretch_enabled);
	const u32 target_ms = static_cast<u32>(std::lround(s_audio_target_ms));
	stream->SetTargetBufferMS(target_ms);
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	stream->m_drain_time = Common::Timer::ConvertValueToSeconds(now);
	s_audio_stream = stream.get();
	s_audio_block[AUDIO_STREAM_RATE].store(sample_rate, std::memory_order_relaxed);
	s_audio_block[AUDIO_BUFFER_MS].store(target_ms, std::memory_order_relaxed);
	s_audio_block[AUDIO_STREAMS].fetch_add(1, std::memory_order_relaxed);
	// A stream that replaces another (PS1 mode) plays at once.
	if (IsAudioWorkletAlive(now))
		stream->Attach();
	return stream;
}

// Audio thread: the output's edges into and out of digital silence (no stream, an underrun's silence, a stream that
// starts) become 64-frame ramps instead of steps, which would click.
static constexpr u32 AUDIO_RAMP_FRAMES = 64;
static float s_audio_last_output[2] = {}; // the last frame played
static bool s_audio_output_silent = true; // the last quantum was all zeros

static void SmoothAudioEdges(float* left, float* right, u32 frames)
{
	bool silent = true;
	for (u32 i = 0; i < frames && silent; i++)
		silent = (left[i] == 0.0f && right[i] == 0.0f);

	const u32 ramp = std::min(frames, AUDIO_RAMP_FRAMES);
	if (silent)
	{
		// Out of the sound: from the last frame played down to zero.
		const float l = s_audio_last_output[0], r = s_audio_last_output[1];
		if (l != 0.0f || r != 0.0f)
		{
			for (u32 i = 0; i < ramp; i++)
			{
				const float f = static_cast<float>(ramp - 1 - i) / ramp;
				left[i] = l * f;
				right[i] = r * f;
			}
		}
	}
	else if (s_audio_output_silent)
	{
		// Into the sound: faded in.
		for (u32 i = 0; i < ramp; i++)
		{
			const float f = static_cast<float>(i) / ramp;
			left[i] *= f;
			right[i] *= f;
		}
	}
	s_audio_output_silent = silent;
	s_audio_last_output[0] = left[frames - 1];
	s_audio_last_output[1] = right[frames - 1];
}

// Audio thread, every render quantum.
static bool AudioRender(int num_inputs, const AudioSampleFrame* inputs, int num_outputs, AudioSampleFrame* outputs,
	int num_params, const AudioParamFrame* params, void* user_data)
{
	s_audio_block[AUDIO_HEARTBEAT].fetch_add(1, std::memory_order_relaxed);
	if (num_outputs < 1)
		return true;

	AudioSampleFrame& out = outputs[0];
	const u32 frames = static_cast<u32>(out.samplesPerChannel);
	const u32 channels = static_cast<u32>(out.numberOfChannels);
	s_audio_in_callback.store(true, std::memory_order_seq_cst);
	WebAudioStream* const stream = s_audio_reader.load(std::memory_order_seq_cst);
	if (stream && channels >= 2 && frames <= AUDIO_MAX_QUANTUM)
	{
		stream->Render(out.data, out.data + frames, frames);
		if (channels > 2)
			std::memset(out.data + frames * 2, 0, (channels - 2) * frames * sizeof(float));
	}
	else
	{
		std::memset(out.data, 0, channels * frames * sizeof(float));
	}
	s_audio_in_callback.store(false, std::memory_order_release);
	if (channels >= 2 && frames > 0)
		SmoothAudioEdges(out.data, out.data + frames, frames);
	return true; // keeps the processor alive
}

// Page thread (the worklet's start callbacks): hands the node to the page, or null when sound cannot start.
static void NotifyAudioReady(EMSCRIPTEN_WEBAUDIO_T node)
{
	// clang-format off
	EM_ASM({
		const ready = Module['pcsx2webAudioReady'];
		if (ready)
			ready($0 ? emscriptenGetAudioObject($0) : null);
	}, node);
	// clang-format on
}

static void OnAudioProcessorCreated(EMSCRIPTEN_WEBAUDIO_T context, bool success, void* user_data)
{
	EMSCRIPTEN_WEBAUDIO_T node = 0;
	if (success)
	{
		static int output_channels[1] = {2};
		EmscriptenAudioWorkletNodeCreateOptions options = {};
		options.numberOfInputs = 0;
		options.numberOfOutputs = 1;
		options.outputChannelCounts = output_channels;
		node = emscripten_create_wasm_audio_worklet_node(context, AUDIO_PROCESSOR_NAME, &options, &AudioRender, nullptr);
	}
	s_audio_block[AUDIO_WORKLET].store(node ? AUDIO_WORKLET_RUNNING : AUDIO_WORKLET_FAILED, std::memory_order_release);
	NotifyAudioReady(node);
}

static void OnAudioWorkletStarted(EMSCRIPTEN_WEBAUDIO_T context, bool success, void* user_data)
{
	if (!success)
	{
		s_audio_block[AUDIO_WORKLET].store(AUDIO_WORKLET_FAILED, std::memory_order_release);
		NotifyAudioReady(0);
		return;
	}

	WebAudioWorkletProcessorCreateOptions options = {};
	options.name = AUDIO_PROCESSOR_NAME;
	emscripten_create_wasm_audio_worklet_processor_async(context, &options, &OnAudioProcessorCreated, nullptr);
}

// main(), before InitializeConfig(). -audio-null (Module.arguments; the page's ?audio=null test switch) keeps PCSX2's
// Null backend, so the cost of sound can be measured against no sound.
static void ParseAudioArguments(int argc, char* argv[])
{
	for (int i = 1; i < argc; i++)
	{
		if (std::strcmp(argv[i], "-audio-null") == 0)
			s_audio_null = true;
	}
}

// InitializeConfig() (AUDIO-DESIGN.md 5), and the factory, before the CPU thread starts.
// Two DualShock 2 pads, ports 1 and 2, fed by pcsx2web_pad (the keyboard and the gamepads, routed by
// the page). The core's default keyboard bindings for port 1 are removed: the keyboard goes through
// the page's Players choice like a gamepad. Sticks use the core's diagonal scale correction ("circular
// pickup": a modern controller's round gate reaches the corners) at sensitivity 100%, as the core
// recommends for it.
static void ConfigurePads(SettingsInterface& si)
{
	const Pad::ControllerInfo* info = Pad::GetControllerInfo(Pad::ControllerType::DualShock2);
	for (u32 port = 0; port < NUM_PAD_PORTS; port++)
	{
		const std::string section = Pad::GetConfigSection(port);
		si.SetStringValue(section.c_str(), "Type", info->name);
		si.SetBoolValue(section.c_str(), "UseDiagonalScaleCorrection", true);
		si.SetFloatValue(section.c_str(), "AxisScale", 1.0f);
		for (const InputBindingInfo& bi : info->bindings)
			si.DeleteValue(section.c_str(), bi.name);
	}
}

static void ConfigureAudio(SettingsInterface& si)
{
	AudioStream::SetFrontendStreamFactory(&WebAudioStream::Create);

	// The legacy "OutputModule" key is no longer read; without Backend, Cubeb (not built here) would fail and fall back.
	si.SetStringValue("SPU2/Output", "Backend",
		AudioStream::GetBackendName(s_audio_null ? AudioBackend::Null : AudioBackend::Frontend));
	si.SetStringValue("SPU2/Output", "SyncMode",
		Pcsx2Config::SPU2Options::GetSyncModeName(Pcsx2Config::SPU2Options::SPU2SyncMode::TimeStretch));
	si.SetBoolValue("SPU2/Output", "StretchUseQuickSeek", true);
	si.SetStringValue("SPU2/Output", "ExpansionMode", AudioStream::GetExpansionModeName(AudioExpansionMode::Disabled));
	si.SetIntValue("SPU2/Output", "BufferMS", AUDIO_BUFFER_STEPS[0]); // WebAudioStream::Create() sizes its own
	if (s_audio_null)
		Console.WriteLn("pcsx2-wasm: -audio-null: no sound (PCSX2's Null audio backend)");
}

// RunVM(), at every boot: it starts at the full-speed buffer.
static void ResetAudioBufferStep()
{
	s_audio_step = 0;
	s_audio_target_ms = AUDIO_BUFFER_STEPS[0];
	s_audio_step_up_since = 0;
	s_audio_step_down_since = 0;
	s_audio_target_updated = 0;
	s_audio_underrun_at = 0;
	s_audio_running_since = 0;
}

// The speed it takes to step down to a step: half as fast again as its minimum, at most 15 points more (90 % for the
// full-speed step).
static float AudioStepDownSpeed(float min_speed)
{
	return std::min(min_speed * 1.5f, min_speed + 15.0f);
}

static u32 AudioStepForSpeed(float speed, bool stepping_down)
{
	for (u32 i = 0; i < AUDIO_STEP_MIN_SPEED.size(); i++)
	{
		if (speed >= (stepping_down ? AudioStepDownSpeed(AUDIO_STEP_MIN_SPEED[i]) : AUDIO_STEP_MIN_SPEED[i]))
			return i;
	}
	return static_cast<u32>(AUDIO_STEP_MIN_SPEED.size() - 1);
}

// The stream's target, in place (no new stream, no settings change: PCSX2's BufferMS setting stays at the first step).
static void SetAudioTarget(double target_ms)
{
	s_audio_target_ms = target_ms;
	const u32 ms = static_cast<u32>(std::lround(target_ms));
	if (s_audio_stream)
		s_audio_stream->SetTargetBufferMS(ms);
	s_audio_block[AUDIO_BUFFER_MS].store(ms, std::memory_order_relaxed);
}

// A bigger step applies at once; a smaller one is reached by UpdateAudioTarget().
static void ApplyAudioBufferStep(u32 step, float speed, const char* why = "")
{
	const u16 buffer_ms = AUDIO_BUFFER_STEPS[step];
	s_audio_step = step;
	s_audio_step_up_since = 0;
	s_audio_step_down_since = 0;
	if (buffer_ms > s_audio_target_ms)
		SetAudioTarget(buffer_ms);
	s_audio_block[AUDIO_STEPS].fetch_add(1, std::memory_order_relaxed);
	Console.WriteLnFmt("pcsx2-wasm: audio buffer {} ms at {:.0f}% speed{}", buffer_ms, speed, why);
}

// Down towards the step's buffer at AUDIO_STEP_DOWN_RATE.
static void UpdateAudioTarget(Common::Timer::Value now)
{
	const double wanted = AUDIO_BUFFER_STEPS[s_audio_step];
	if (s_audio_target_ms > wanted && s_audio_target_updated != 0)
	{
		const double elapsed = std::min(Common::Timer::ConvertValueToSeconds(now - s_audio_target_updated), 0.1);
		const double target = std::max(wanted, s_audio_target_ms - AUDIO_STEP_DOWN_RATE * elapsed);
		if (std::lround(target) != std::lround(s_audio_target_ms))
			SetAudioTarget(target);
		else
			s_audio_target_ms = target;
	}
	s_audio_target_updated = now;
}

static void UpdateAudioBufferStep(Common::Timer::Value now)
{
	const float speed = s_speed.load(std::memory_order_relaxed);
	const u32 short_quanta = s_audio_block[AUDIO_SHORT_QUANTA].load(std::memory_order_relaxed);
	const bool underrun = (short_quanta != s_audio_short_seen);
	s_audio_short_seen = short_quanta;
	if (!s_audio_stream || speed <= 0.0f || VMManager::GetState() != VMState::Running)
	{
		s_audio_step_up_since = 0;
		s_audio_step_down_since = 0;
		s_audio_target_updated = 0;
		s_audio_running_since = 0;
		return;
	}
	if (s_audio_block[AUDIO_ATTACHED].load(std::memory_order_relaxed) == 0)
		s_audio_running_since = 0;
	else if (s_audio_running_since == 0)
		s_audio_running_since = now;
	if (underrun && s_audio_running_since != 0 &&
		Common::Timer::ConvertValueToSeconds(now - s_audio_running_since) >= AUDIO_UNDERRUN_GRACE)
	{
		s_audio_underrun_at = now;
		if (s_audio_step == 0)
			ApplyAudioBufferStep(1, speed, " after an underrun");
	}
	const bool underrun_recently = s_audio_underrun_at != 0 &&
								   Common::Timer::ConvertValueToSeconds(now - s_audio_underrun_at) < AUDIO_UNDERRUN_HOLD;

	const auto held_for = [now](Common::Timer::Value& since) {
		if (since == 0)
			since = now;
		return Common::Timer::ConvertValueToSeconds(now - since);
	};
	const u32 slower = AudioStepForSpeed(speed, false);
	const u32 faster = std::max(AudioStepForSpeed(speed, true), underrun_recently ? 1u : 0u);
	if (slower > s_audio_step)
	{
		s_audio_step_down_since = 0;
		if (held_for(s_audio_step_up_since) >= AUDIO_STEP_UP_AFTER)
			ApplyAudioBufferStep(slower, speed);
	}
	else if (faster < s_audio_step)
	{
		s_audio_step_up_since = 0;
		if (held_for(s_audio_step_down_since) >= AUDIO_STEP_DOWN_AFTER)
			ApplyAudioBufferStep(faster, speed);
	}
	else
	{
		s_audio_step_up_since = 0;
		s_audio_step_down_since = 0;
	}
	UpdateAudioTarget(now);
}

// CPU thread, once per vsync (and every 10 ms while paused).
static void UpdateAudioOutput()
{
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	const bool alive = IsAudioWorkletAlive(now);
	if (s_audio_stream)
		s_audio_stream->Service(now, alive);
	UpdateAudioBufferStep(now);
}

extern "C" {

/// Starts the page's sound, on the page's thread (AUDIO-DESIGN.md 4.2): loads this module into the worklet scope of
/// Module.pcsx2webAudioContext (an AudioContext the page made) and creates the processor node, then calls
/// Module.pcsx2webAudioReady(node), or Module.pcsx2webAudioReady(null) when that failed. 0 if started; -1 if a start
/// already ran or there is no context. Never blocks.
EMSCRIPTEN_KEEPALIVE int pcsx2web_audio_start(void)
{
	u32 expected = AUDIO_WORKLET_NONE;
	if (!s_audio_block[AUDIO_WORKLET].compare_exchange_strong(expected, AUDIO_WORKLET_STARTING, std::memory_order_acq_rel))
		return -1;

	// clang-format off
	const EMSCRIPTEN_WEBAUDIO_T context = EM_ASM_INT({
		const ctx = Module['pcsx2webAudioContext'];
		return ctx ? emscriptenRegisterAudioObject(ctx) : 0;
	});
	// clang-format on
	if (!context)
	{
		s_audio_block[AUDIO_WORKLET].store(AUDIO_WORKLET_FAILED, std::memory_order_release);
		return -1;
	}

	s_audio_block[AUDIO_CONTEXT_RATE].store(static_cast<u32>(emscripten_audio_context_sample_rate(context)),
		std::memory_order_relaxed);
	emscripten_start_wasm_audio_worklet_thread_async(context, s_audio_worklet_stack, sizeof(s_audio_worklet_stack),
		&OnAudioWorkletStarted, nullptr);
	return 0;
}

/// The audio block: 16 little-endian u32 words (AUDIO-DESIGN.md 4.5). Read them with Atomics.
EMSCRIPTEN_KEEPALIVE const void* pcsx2web_audio(void)
{
	return s_audio_block;
}

} // extern "C"

static bool InitializeConfig()
{
	// Resources are preloaded at /resources (see CMakeLists.txt); the data root is in MEMFS.
	EmuFolders::SetAppRoot();
	EmuFolders::AppRoot = "/";
	EmuConfig.CustomDataPath = "/pcsx2-data";

	// The data root is CustomDataPath/PCSX2, which SetDataDirectory() creates non-recursively.
	Error error;
	if (!FileSystem::EnsureDirectoryExists(EmuConfig.CustomDataPath.c_str(), false, &error) ||
		!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(&error))
	{
		Console.ErrorFmt("Failed to set up folders: {}", error.GetDescription());
		return false;
	}

	// The page writes the BIOS here, but create it in case it has not yet.
	if (!FileSystem::EnsureDirectoryExists(BIOS_DIRECTORY, false, &error))
	{
		Console.ErrorFmt("Failed to create {}: {}", BIOS_DIRECTORY, error.GetDescription());
		return false;
	}
	if (!EnsureDiscAndMemoryCardFolders())
		return false;

	const char* hw_error;
	if (!VMManager::PerformEarlyHardwareChecks(&hw_error))
	{
		Console.ErrorFmt("Hardware check failed: {}", hw_error);
		return false;
	}

	// The OSD/ImGui layer needs a font even when nothing is displayed. Read rather than
	// mapped: mmap of a MEMFS file is emulated by a copy anyway.
	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		std::optional<std::vector<u8>> roboto_data = FileSystem::ReadBinaryFile(roboto_path.c_str());
		if (!roboto_data.has_value() || roboto_data->empty())
		{
			Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
			return false;
		}
		s_roboto_font_data = std::move(roboto_data.value());

		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = s_roboto_font_data;
		fi.exclude_ranges = {};
		fi.face_name = nullptr;
		fi.is_emoji_font = false;
		fonts.push_back(fi);
		ImGuiManager::SetFonts(std::move(fonts));
	}

	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);
	VMManager::SetDefaultSettings(si, true, true, true, true, true);

	// The VU1 JIT is on (docs/web-port/VU1-JIT-DESIGN.md); RunVM applies the page's ?vu1jit=0
	// (pcsx2web_vu1jit_enable).
	si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", true);
	// The VU0 micro-mode JIT (pcsx2/wasm/VU0Jit.h) is on too: GTA San Andreas runs VU0 programs for an eighth
	// of the EE thread on the interpreter (docs/web-port/PERFORMANCE.md).
	si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", true);

	// The hardware renderer on the WebGPU device (without WebGPU the GS falls back to the software
	// renderer into GSDeviceNull; the page's ?renderer=sw picks the software renderer), sound through
	// the page's Web Audio worklet, two DualShock 2 pads fed by the page (ConfigurePads), two memory
	// card slots that start empty (the page plugs cards in). Hotkeys are cleared: most of them (pause menu, save states, screenshots) need host support that is not
	// there yet.
	si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::WebGPU));
	// The software renderer's rasterizer threads limit 3D scenes with PCSX2's default of 2 (BT3 fight, 16
	// threads: 30 VPS with 2, 38 with 4, 41 with 6; output identical). Leave about four threads to the EE, VU1,
	// GS threads and the browser. The thread pool (CMakeLists.txt) covers 6.
	si.SetIntValue("EmuCore/GS", "extrathreads", std::clamp(emscripten_num_logical_cores() - 4, 2, 6));
	ConfigureAudio(si);
	si.SetBoolValue("InputSources", "SDL", false);
	si.SetBoolValue("InputSources", "XInput", false);
	ConfigurePads(si);
	for (u32 port = 0; port < NUM_WHEEL_PORTS; port++)
		ConfigureWheel(si, port, WHEEL_NONE);
	s_ffb_ring[3].store(FFB_RING_SIZE, std::memory_order_relaxed);
	usb_pad::SetRawFFBHandler(&QueueWheelFFBCommand);
	si.ClearSection("Hotkeys");
	ConfigureMemoryCards(si);

	// No OSD messages in the picture: the page has its own status line, and the 2D path never
	// showed them (GSDeviceNull draws no ImGui). Tests turn them back on with -set.
	si.SetIntValue("EmuCore/GS", "OsdMessagesPos", static_cast<int>(OsdOverlayPos::None));

	// Show what the PS2 itself prints (it reaches the page through Module.print).
	si.SetBoolValue("Logging", "EnableSystemConsole", true);
	si.SetBoolValue("Logging", "EnableFileLogging", false);
	si.SetBoolValue("Logging", "EnableTimestamps", true);
	si.SetBoolValue("Logging", "EnableEEConsole", true);
	si.SetBoolValue("Logging", "EnableIOPConsole", true);

	si.SetStringValue("Folders", "Bios", BIOS_DIRECTORY);

	// The settings interface stores strings, so ints, floats and bools ("true"/"false") all parse back.
	for (const SettingOverride& ov : s_setting_overrides)
	{
		INFO_LOG("pcsx2-wasm: setting [{}] {} = {}", ov.section, ov.key, ov.value);
		si.SetStringValue(ov.section.c_str(), ov.key.c_str(), ov.value.c_str());
	}

	VMManager::Internal::LoadStartupSettings();
	return true;
}

// The page's switches, passed as main()'s arguments (Module.arguments): -gpu 0|1 and, for tests,
// -set Section/Key=Value, -surface WxH, -gs-idle block, -gs-pipelines wait and -capture N,M,... (vsync
// numbers of the 2D path's frames to keep a copy of).
// Benchmarks (?unlimited=1): boot with the frame limiter off, as pcsx2-headless -unlimited.
static bool s_start_unlimited = false;

static void ParseArguments(int argc, char* argv[])
{
	for (int i = 1; i < argc; i++)
	{
		const std::string_view arg = argv[i];
		const bool has_value = (i + 1 < argc);
		if (arg == "-gpu" && has_value)
		{
			WebGPUHost::SetEnabled(std::string_view(argv[++i]) != "0");
		}
		else if (arg == "-eejit" && has_value)
		{
			// Tests: "0" runs the EE interpreter instead of the EE JIT.
			EEJit::g_mode = (std::string_view(argv[++i]) != "0") ? 1 : 0;
		}
		else if (arg == "-iopjit" && has_value)
		{
			// Tests: "0" runs the IOP interpreter instead of the IOP JIT.
			IopJit::g_mode = (std::string_view(argv[++i]) != "0") ? 1 : 0;
		}
		else if (arg == "-unlimited")
		{
			s_start_unlimited = true;
		}
		else if (arg == "-gs-idle" && has_value)
		{
			// Tests: "block" keeps the GS thread's idle wait blocking with WebGPU, to exercise the
			// device's bounded event-loop turn on its own.
			WebGPUHost::SetIdleWaitsSuspend(std::string_view(argv[++i]) != "block");
		}
		else if (arg == "-gs-pipelines" && has_value)
		{
			// Tests: "wait" makes the HW renderer wait for every new TFX pipeline instead of skipping
			// the draws that need one still being created (frames identical to a run with no skips).
			WebGPUHost::SetPipelinesWait(std::string_view(argv[++i]) == "wait");
		}
		else if (arg == "-set" && has_value)
		{
			const std::string_view setting = argv[++i];
			const std::string_view::size_type eq = setting.find('=');
			const std::string_view path = setting.substr(0, eq);
			const std::string_view::size_type slash = path.rfind('/');
			if (eq == std::string_view::npos || slash == std::string_view::npos || slash == 0 || slash + 1 == path.size())
			{
				Console.ErrorFmt("pcsx2-wasm: bad -set '{}' (use Section/Key=Value)", setting);
				continue;
			}
			s_setting_overrides.push_back({std::string(path.substr(0, slash)), std::string(path.substr(slash + 1)),
				std::string(setting.substr(eq + 1))});
		}
		else if (arg == "-surface" && has_value)
		{
			// Tests: GSDeviceNull presents at this size too, so both paths can be compared 1:1.
			const std::string_view size = argv[++i];
			const std::string_view::size_type x = size.find('x');
			const u32 w = StringUtil::FromChars<u32>(size.substr(0, x)).value_or(0);
			const u32 h = (x != std::string_view::npos) ? StringUtil::FromChars<u32>(size.substr(x + 1)).value_or(0) : 0;
			if (w > 0 && h > 0 && w <= 8192 && h <= 8192)
			{
				s_window_width = w;
				s_window_height = h;
			}
		}
		else if (arg == "-capture" && has_value)
		{
			u32 slot = 0;
			for (const std::string_view n : StringUtil::SplitString(argv[++i], ','))
			{
				if (slot < MAX_CAPTURES)
					s_captures[slot++].wanted = StringUtil::FromChars<u32>(n).value_or(0);
			}
		}
		else
		{
			Console.WarningFmt("pcsx2-wasm: ignoring argument '{}'", arg);
		}
	}
}

// GS thread, once per presented frame.
static void PresentFrame(const u32* pixels, u32 width, u32 height)
{
	const size_t count = static_cast<size_t>(width) * height;
	std::vector<u32>& buffer = s_frame_buffers[s_frame_write_index];
	if (buffer.size() != count)
		buffer = std::vector<u32>(count);
	std::memcpy(buffer.data(), pixels, count * sizeof(u32));

	s_frame_descriptor[FRAME_WIDTH].store(width, std::memory_order_relaxed);
	s_frame_descriptor[FRAME_HEIGHT].store(height, std::memory_order_relaxed);
	s_frame_descriptor[FRAME_PIXELS].store(static_cast<u32>(reinterpret_cast<uintptr_t>(buffer.data())),
		std::memory_order_relaxed);
	const u32 vsync = CurrentFrameNumber();
	s_frame_descriptor[FRAME_VSYNC].store(vsync, std::memory_order_relaxed);
	s_frame_descriptor[FRAME_SEQ].fetch_add(1, std::memory_order_release);
	s_frame_write_index = (s_frame_write_index + 1) % NUM_FRAME_BUFFERS;

	for (FrameCapture& cap : s_captures)
	{
		if (cap.wanted == 0 || vsync < cap.wanted || cap.frame.load(std::memory_order_relaxed) != 0)
			continue;
		cap.width = width;
		cap.height = height;
		cap.pixels.assign(pixels, pixels + count);
		cap.frame.store(vsync, std::memory_order_release);
	}
}

// Applies Crisp (nearest-neighbour presentation) to the GS settings. CPU thread.
static void ApplyCrispSetting(bool crisp, bool apply_now)
{
	s_crisp = crisp;
	{
		auto lock = Host::GetSettingsLock();
		s_settings_interface.SetIntValue("EmuCore/GS", "linear_present_mode",
			static_cast<int>(crisp ? GSPostBilinearMode::Off : GSPostBilinearMode::BilinearSmooth));
	}
	if (apply_now && VMManager::HasValidVM())
		VMManager::ApplySettings();
}

static void CheckCrispRequest(bool apply_now)
{
	const int request = s_crisp_request.exchange(-1, std::memory_order_acq_rel);
	if (request >= 0 && (request != 0) != s_crisp)
		ApplyCrispSetting(request != 0, apply_now);
}

// Plugs the page's wheel choices into the USB ports (a running game sees the old device unplugged and the new one
// plugged in). CPU thread.
static void CheckWheelRequest(bool apply_now)
{
	bool changed = false;
	for (u32 port = 0; port < NUM_WHEEL_PORTS; port++)
	{
		const int request = s_wheel_request[port].exchange(WHEEL_NO_REQUEST, std::memory_order_acq_rel);
		if (request == WHEEL_NO_REQUEST || request == s_wheel_type[port])
			continue;

		s_wheel_type[port] = request;
		{
			auto lock = Host::GetSettingsLock();
			ConfigureWheel(s_settings_interface, port, request);
		}
		INFO_LOG("pcsx2-wasm: USB port {}: {}", port + 1,
			request == WHEEL_NONE ? "none" : USB::GetDeviceSubtypeName("Pad", static_cast<u32>(request)));
		changed = true;
	}
	if (changed && apply_now && VMManager::HasValidVM())
		VMManager::ApplySettings();
}

// The internal resolution: the hardware renderer draws at this multiple of the PS2's own size (the GS clamps it to
// what the device's largest texture allows; the software renderer ignores it). Needs proper testing on more games.
static void CheckUpscaleRequest(bool apply_now)
{
	const int request = s_upscale_request.exchange(0, std::memory_order_acq_rel);
	if (request <= 0 || request == s_upscale)
		return;
	s_upscale = request;
	{
		auto lock = Host::GetSettingsLock();
		s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(request));
	}
	Console.WriteLnFmt("pcsx2-wasm: internal resolution {}x", request);
	if (apply_now && VMManager::HasValidVM())
		VMManager::ApplySettings();
}

// The picture filter: one of PCSX2's TV shaders, applied when the GS presents (the WebGPU device has them all).
static void CheckTvShaderRequest(bool apply_now)
{
	const int request = s_tv_shader_request.exchange(-1, std::memory_order_acq_rel);
	if (request < 0 || request == s_tv_shader)
		return;
	s_tv_shader = request;
	{
		auto lock = Host::GetSettingsLock();
		s_settings_interface.SetIntValue("EmuCore/GS", "TVShader", request);
	}
	if (apply_now && VMManager::HasValidVM())
		VMManager::ApplySettings();
}

// The EE underclock (PCSX2's EE Cycle Rate speedhack), which the page keeps per game. CPU thread.
static void CheckCycleRateRequest(bool apply_now)
{
	const int request = s_cycle_rate_request.exchange(kCycleRateNone, std::memory_order_acq_rel);
	if (request == kCycleRateNone || request == s_cycle_rate)
		return;
	s_cycle_rate = request;
	{
		auto lock = Host::GetSettingsLock();
		s_settings_interface.SetIntValue("EmuCore/Speedhacks", "EECycleRate", request);
	}
	Console.WriteLnFmt("pcsx2-wasm: EE cycle rate {}", request);
	if (apply_now && VMManager::HasValidVM())
		VMManager::ApplySettings();
}

static void ResolvePadBindIndexes()
{
	const Pad::ControllerInfo* info = Pad::GetControllerInfo(Pad::ControllerType::DualShock2);
	for (u32 i = 0; i < NUM_PAD_INPUTS; i++)
		s_pad_bind_index[i] = static_cast<u8>(info->GetBindIndex(s_pad_input_names[i]).value_or(0));
	s_pad_bind_index_ready = true;
}

static void PublishPadSnapshot()
{
	for (u32 port = 0; port < NUM_PAD_PORTS; port++)
	{
		const PadBase* pad = Pad::GetPad(static_cast<u8>(port), 0);
		u32 words[PAD_SNAPSHOT_WORDS] = {};
		if (pad && pad->GetType() == Pad::ControllerType::DualShock2)
		{
			for (u32 i = 0; i < NUM_PAD_INPUTS; i++)
				words[i / 4] |= static_cast<u32>(pad->GetRawInput(s_pad_bind_index[i])) << ((i % 4) * 8);
			words[PAD_SNAPSHOT_WORDS - 1] = 1u | (pad->IsAnalogLightEnabled() ? 2u : 0u);
		}
		for (u32 i = 0; i < PAD_SNAPSHOT_WORDS; i++)
			s_pad_snapshot[port][i].store(words[i], std::memory_order_relaxed);
	}
}

static void ProcessPadEvents()
{
	if (!s_pad_bind_index_ready)
		ResolvePadBindIndexes();

	// Move everything from the ring into the per-input slots.
	const u32 head = s_pad_queue_head.load(std::memory_order_acquire);
	u32 tail = s_pad_queue_tail.load(std::memory_order_relaxed);
	for (; tail != head; tail++)
	{
		const u32 event = s_pad_queue[tail % PAD_QUEUE_SIZE];
		const u32 input = (event >> PAD_EVENT_INPUT_SHIFT) & 31;
		const u32 port = (event >> PAD_EVENT_PORT_SHIFT) & 1;
		if (input >= NUM_PAD_INPUTS)
			continue;

		PadSlot& slot = s_pad_slots[port][input];
		const float value = static_cast<float>(event & 0xFFFF) / 65535.0f;
		if (input >= NUM_PAD_BUTTONS)
		{
			slot.target = value;
			continue;
		}

		const bool pressed = value > 0.0f;
		if (pressed)
			slot.target = value;
		const bool target = slot.down ^ ((slot.pending & 1) != 0); // state once pending edges apply
		if (pressed == target)
			continue; // duplicate edge, or a new pressure

		if (++slot.pending > MAX_PENDING_PAD_EDGES)
			slot.pending -= 2; // drop one whole tap, keep the parity
	}
	s_pad_queue_tail.store(tail, std::memory_order_release);

	// A pad exists only while a VM runs; a port without one keeps its values until it has.
	for (u32 port = 0; port < NUM_PAD_PORTS; port++)
	{
		const PadBase* pad = Pad::GetPad(static_cast<u8>(port), 0);
		const bool connected = pad && pad->GetType() == Pad::ControllerType::DualShock2;
		for (u32 input = 0; input < NUM_PAD_INPUTS; input++)
		{
			PadSlot& slot = s_pad_slots[port][input];
			float out;
			if (input >= NUM_PAD_BUTTONS)
			{
				out = slot.target;
			}
			else
			{
				if (slot.pending != 0 &&
					(slot.changed_at == 0 || (s_key_poll_count - slot.changed_at) >= MIN_KEY_HOLD_POLLS))
				{
					slot.down = !slot.down;
					slot.pending--;
					slot.changed_at = s_key_poll_count;
				}
				out = slot.down ? slot.target : 0.0f;
			}

			if (out == slot.applied || !connected)
				continue;
			slot.applied = out;
			Pad::SetControllerState(port, s_pad_bind_index[input], out);
		}
	}

	PublishPadSnapshot();
}

// CPU thread: hands the page's wheel inputs to the emulated wheel (what a PCSX2 binding does). Values that arrive
// while no wheel is plugged in are dropped.
static void ProcessWheelEvents()
{
	const u32 head = s_wheel_queue_head.load(std::memory_order_acquire);
	u32 tail = s_wheel_queue_tail.load(std::memory_order_relaxed);
	const bool running = VMManager::HasValidVM();
	for (; tail != head; tail++)
	{
		const u32 event = s_wheel_queue[tail % WHEEL_QUEUE_SIZE];
		const u32 port = (event >> WHEEL_EVENT_PORT_SHIFT) & 1;
		if (running && s_wheel_type[port] != WHEEL_NONE)
			USB::SetDeviceBindValue(port, (event >> WHEEL_EVENT_BIND_SHIFT) & 0xFF,
				static_cast<float>(event & 0xFFFF) / 65535.0f);
	}
	s_wheel_queue_tail.store(tail, std::memory_order_release);
}

static void ProcessKeyEvents()
{
	s_key_poll_count++;

	// Move everything from the ring into the per-key slots.
	const u32 head = s_key_queue_head.load(std::memory_order_acquire);
	u32 tail = s_key_queue_tail.load(std::memory_order_relaxed);
	for (; tail != head; tail++)
	{
		const u32 event = s_key_queue[tail % KEY_QUEUE_SIZE];
		const u32 code = event & ~KEY_PRESSED_BIT;
		if (code == 0 || code > NUM_KEYS)
			continue;

		KeySlot& key = s_keys[code];
		const bool pressed = (event & KEY_PRESSED_BIT) != 0;
		const bool target = key.down ^ ((key.pending & 1) != 0); // state once pending edges apply
		if (pressed == target)
			continue; // duplicate edge

		if (++key.pending > MAX_PENDING_KEY_EDGES)
			key.pending -= 2; // drop one whole tap, keep the parity
	}
	s_key_queue_tail.store(tail, std::memory_order_release);

	// Apply at most one edge per key per poll, keeping each state for the minimum time.
	for (u32 code = 1; code <= NUM_KEYS; code++)
	{
		KeySlot& key = s_keys[code];
		if (key.pending == 0 || (key.changed_at != 0 && (s_key_poll_count - key.changed_at) < MIN_KEY_HOLD_POLLS))
			continue;

		key.down = !key.down;
		key.pending--;
		key.changed_at = s_key_poll_count;
		InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(code), key.down ? 1.0f : 0.0f);
	}

	ProcessPadEvents();
	ProcessWheelEvents();
}

static void DiscardKeyEvents()
{
	s_key_queue_tail.store(s_key_queue_head.load(std::memory_order_acquire), std::memory_order_release);
	s_wheel_queue_tail.store(s_wheel_queue_head.load(std::memory_order_acquire), std::memory_order_release);
	s_pad_queue_tail.store(s_pad_queue_head.load(std::memory_order_acquire), std::memory_order_release);
	std::memset(s_pad_slots, 0, sizeof(s_pad_slots));
	std::fill(std::begin(s_keys), std::end(s_keys), KeySlot{});
}

static void RunCPUThreadQueue()
{
	std::unique_lock lock(s_cpu_thread_queue_lock);
	while (!s_cpu_thread_queue.empty())
	{
		std::function<void()> function = std::move(s_cpu_thread_queue.front());
		s_cpu_thread_queue.pop_front();
		lock.unlock();
		function();
		lock.lock();
	}
}

static void CheckStopRequest()
{
	if (s_stop_requested.load(std::memory_order_acquire) && VMManager::HasValidVM() &&
		VMManager::GetState() != VMState::Stopping)
	{
		VMManager::SetState(VMState::Stopping);
	}
}

// Waits on the CPU thread for pcsx2web_boot(). Wakes now and then to run queued work.
static std::string WaitForBootRequest()
{
	while (s_boot_request.load(std::memory_order_acquire) == 0)
	{
		emscripten_futex_wait(&s_boot_request, 0, 100.0);
		RunCPUThreadQueue();
	}

	std::string name(s_boot_bios_name);
	s_boot_request.store(0, std::memory_order_release);
	return name;
}

// The same test as PCSX2's BIOS list (IsBIOS()): a ROM directory with a ROMVER entry.
static bool IsValidBIOSFile(const std::string& bios_name)
{
	const std::string path = Path::Combine(BIOS_DIRECTORY, bios_name);
	if (!FileSystem::FileExists(path.c_str()))
	{
		Console.ErrorFmt("pcsx2-wasm: BIOS '{}' not found.", path);
		return false;
	}

	u32 version, region;
	std::string description, zone;
	if (!IsBIOS(path.c_str(), version, description, region, zone))
	{
		Console.ErrorFmt("pcsx2-wasm: '{}' is not a PS2 BIOS image: it has no ROM directory with a version.", bios_name);
		return false;
	}

	return true;
}

static void RunVM(const std::string& bios_name)
{
	DiscardKeyEvents();
	const BootDisc disc = TakeBootDisc();
	ResetAudioBufferStep();

	{
		auto lock = Host::GetSettingsLock();
		s_settings_interface.SetStringValue("Filenames", "BIOS", bios_name.c_str());
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", s_vu1jit_enabled.load(std::memory_order_acquire));
	}
	CheckCrispRequest(false);
	CheckWheelRequest(false);
	CheckUpscaleRequest(false);
	CheckTvShaderRequest(false);
	CheckCycleRateRequest(false);
	CheckMemcardRequests(false);
	VMManager::ApplySettings();

	if (s_stop_requested.load(std::memory_order_acquire))
	{
		s_state.store(WEB_STATE_STOPPED, std::memory_order_release);
		return;
	}

	// PCSX2 boots whatever file it is given; only its BIOS list checks the contents. Anything else (a
	// blank or wrong file) runs garbage that resets the PS2 over and over, so check it here first.
	if (!IsValidBIOSFile(bios_name))
	{
		s_state.store(WEB_STATE_FAILED, std::memory_order_release);
		return;
	}

	VMBootParameters params;
	params.source_type = CDVD_SourceType::NoDisc;
	params.fast_boot = false;
	params.start_unlimited = s_start_unlimited;

	MakeBootParameters(bios_name, disc, &params);
	Error error;
	EEJit::NewCodeLogSession();
	const VMBootResult result = VMManager::Initialize(params, &error);
	if (result != VMBootResult::StartupSuccess)
	{
		Console.ErrorFmt("pcsx2-wasm: VM failed to start (result {}): {}", static_cast<int>(result),
			error.GetDescription());
		// A Stop while the disc was opening cancels its reads, which can fail the boot: that is a stop.
		s_state.store(s_stop_requested.load(std::memory_order_acquire) ? WEB_STATE_STOPPED : WEB_STATE_FAILED,
			std::memory_order_release);
		return;
	}

	VMManager::SetState(VMState::Running);
	s_state.store(WEB_STATE_RUNNING, std::memory_order_release);

	// Same loop as the Qt EmuThread, minus its event loop.
	for (;;)
	{
		const VMState state = VMManager::GetState();
		if (state == VMState::Running)
		{
			VMManager::Execute();
		}
		else if (state == VMState::Resetting)
		{
			VMManager::Reset();
		}
		else if (state == VMState::Paused)
		{
			// Execute() is not pumping messages while paused.
			ProcessKeyEvents();
			RunCPUThreadQueue();
			CheckStopRequest();
			UpdateAudioOutput();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		else
		{
			break;
		}
	}

	VMManager::Shutdown(false);
	s_memcard_status[0].store(0, std::memory_order_release);
	s_speed.store(0.0f, std::memory_order_relaxed);
	s_vps.store(0.0f, std::memory_order_relaxed);
	s_state.store(WEB_STATE_STOPPED, std::memory_order_release);
	Console.WriteLn("pcsx2-wasm: VM stopped");
}

// One long-lived CPU thread: CPUThreadInitialize/CPUThreadShutdown are per thread, and a VM
// can be booted again on it after a shutdown (as the Qt frontend does).
static void CPUThreadMain()
{
	s_cpu_thread_id = std::this_thread::get_id();

	if (!VMManager::Internal::CPUThreadInitialize())
	{
		Console.Error("pcsx2-wasm: CPU thread initialization failed.");
		s_fatal_error.store(true, std::memory_order_release);
		s_state.store(WEB_STATE_FAILED, std::memory_order_release);
		VMManager::Internal::CPUThreadShutdown();
		return;
	}
	s_cpu_thread_ready.store(true, std::memory_order_release);

	for (;;)
		RunVM(WaitForBootRequest());
}

int main(int argc, char* argv[])
{
	CrashHandler::Install();
	Log::SetConsoleOutputLevel(LOGLEVEL_DEV);
	ParseAudioArguments(argc, argv);

	// The module is linked with -sJSPI, so emscripten_has_asyncify() is nonzero and SDL_Delay() (and
	// what waits through it) would suspend the calling thread's stack with emscripten_sleep(). A
	// suspension SDL does not expect could re-enter it from the event loop; SDL must only block. Set
	// before SDL is initialized (audio, input).
	if (!SDL_SetHint(SDL_HINT_EMSCRIPTEN_ASYNCIFY, "0"))
		Console.Warning("Failed to set SDL_HINT_EMSCRIPTEN_ASYNCIFY.");

	ParseArguments(argc, argv);

	if (!InitializeConfig())
	{
		Console.Error("Failed to initialize config.");
		s_fatal_error.store(true, std::memory_order_release);
		s_state.store(WEB_STATE_FAILED, std::memory_order_release);
		return EXIT_FAILURE;
	}

	GSDeviceNull::SetPresentCallback(&PresentFrame);

	std::thread cpu_thread(CPUThreadMain);
	cpu_thread.join();
	return EXIT_FAILURE;
}

//////////////////////////////////////////////////////////////////////////
// Exports for the page (browser main thread). Must never block.
//////////////////////////////////////////////////////////////////////////

extern "C" {

/// Boots the BIOS named bios_filename inside /bios. 0 if accepted; -1 if a VM is starting
/// or running, the frontend failed to initialize, or the name is not a plain file name.
EMSCRIPTEN_KEEPALIVE int pcsx2web_boot(const char* bios_filename)
{
	if (!bios_filename || s_fatal_error.load(std::memory_order_acquire) || VU1JitFuzzRunning())
		return -1;

	const size_t length = std::strlen(bios_filename);
	if (length == 0 || length >= sizeof(s_boot_bios_name) || std::strchr(bios_filename, '/') ||
		std::strcmp(bios_filename, ".") == 0 || std::strcmp(bios_filename, "..") == 0)
	{
		return -1;
	}

	int state = s_state.load(std::memory_order_acquire);
	do
	{
		if (state == WEB_STATE_STARTING || state == WEB_STATE_RUNNING)
			return -1;
	} while (!s_state.compare_exchange_weak(state, WEB_STATE_STARTING, std::memory_order_acq_rel));

	std::memcpy(s_boot_bios_name, bios_filename, length + 1);
	s_stop_requested.store(false, std::memory_order_release);
	s_boot_request.store(1, std::memory_order_release);
	emscripten_futex_wake(&s_boot_request, 1);
	return 0;
}

/// Asks the running (or starting) VM to shut down; the state then goes to STOPPED.
EMSCRIPTEN_KEEPALIVE void pcsx2web_stop(void)
{
	const int state = s_state.load(std::memory_order_acquire);
	if (state == WEB_STATE_STARTING || state == WEB_STATE_RUNNING)
	{
		s_stop_requested.store(true, std::memory_order_release);
		// A disc read waiting on a slow source must not hold the CPU thread (and the stop) back.
		WebDisc::AbortReads();
	}
}

EMSCRIPTEN_KEEPALIVE int pcsx2web_state(void)
{
	return s_state.load(std::memory_order_acquire);
}

EMSCRIPTEN_KEEPALIVE float pcsx2web_speed(void)
{
	return s_speed.load(std::memory_order_relaxed);
}

EMSCRIPTEN_KEEPALIVE float pcsx2web_vps(void)
{
	return s_vps.load(std::memory_order_relaxed);
}

/// The per-vsync recorder's ring (PERF_RECORDS records of PERF_FIELDS doubles, see s_perf_ring) and
/// how many records have been written so far: record i is at index i % PERF_RECORDS.
EMSCRIPTEN_KEEPALIVE const double* pcsx2web_perf_ring(void)
{
	return s_perf_ring;
}

EMSCRIPTEN_KEEPALIVE unsigned int pcsx2web_perf_written(void)
{
	return s_perf_written.load(std::memory_order_acquire);
}

/// How busy a thread was over the last measurement, in percent: 0 the EE (CPU) thread, 1 the GS thread,
/// 2 the VU1 thread (0 without MTVU). Busy means not waiting for another thread.
EMSCRIPTEN_KEEPALIVE float pcsx2web_thread_usage(int thread)
{
	return (thread >= 0 && thread < 3) ? s_thread_usage[thread].load(std::memory_order_relaxed) : 0.0f;
}

/// Host key name (e.g. "Return") to key code, -1 if unknown.
EMSCRIPTEN_KEEPALIVE int pcsx2web_key_code(const char* key_name)
{
	if (!key_name)
		return -1;

	for (u32 i = 0; i < NUM_KEYS; i++)
	{
		if (std::strcmp(key_name, s_key_names[i]) == 0)
			return static_cast<int>(i + 1);
	}

	return -1;
}

/// Queues a key event for the CPU thread. Dropped if the queue is full or the code invalid.
EMSCRIPTEN_KEEPALIVE void pcsx2web_key(int key_code, int pressed)
{
	if (key_code < 1 || static_cast<u32>(key_code) > NUM_KEYS)
		return;

	const u32 head = s_key_queue_head.load(std::memory_order_relaxed);
	const u32 tail = s_key_queue_tail.load(std::memory_order_acquire);
	if (head - tail >= KEY_QUEUE_SIZE)
		return;

	s_key_queue[head % KEY_QUEUE_SIZE] = static_cast<u32>(key_code) | (pressed ? KEY_PRESSED_BIT : 0u);
	s_key_queue_head.store(head + 1, std::memory_order_release);
}

/// Pad input name (as in the Keys drawer: "Cross", "L2", "LUp", ...) to its number, -1 if unknown.
EMSCRIPTEN_KEEPALIVE int pcsx2web_pad_input(const char* input_name)
{
	if (!input_name)
		return -1;

	for (u32 i = 0; i < NUM_PAD_INPUTS; i++)
	{
		if (std::strcmp(input_name, s_pad_input_names[i]) == 0)
			return static_cast<int>(i);
	}

	return -1;
}

/// Queues the state of one input of the pad on port 0 or 1 for the CPU thread: value 0 to 1 (a button:
/// 0 released, above 0 pressed, L2 and R2 take the pressure; a stick half: how far). 1 if queued, 0 if
/// the queue is full (try again) or the arguments are invalid.
EMSCRIPTEN_KEEPALIVE int pcsx2web_pad(int port, int input, float value)
{
	if (port < 0 || static_cast<u32>(port) >= NUM_PAD_PORTS || input < 0 || static_cast<u32>(input) >= NUM_PAD_INPUTS)
		return 0;

	const u32 quantized = static_cast<u32>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
	const u32 head = s_pad_queue_head.load(std::memory_order_relaxed);
	const u32 tail = s_pad_queue_tail.load(std::memory_order_acquire);
	if (head - tail >= PAD_QUEUE_SIZE)
		return 0;

	s_pad_queue[head % PAD_QUEUE_SIZE] = quantized | (static_cast<u32>(input) << PAD_EVENT_INPUT_SHIFT) |
	                                     (static_cast<u32>(port) << PAD_EVENT_PORT_SHIFT);
	s_pad_queue_head.store(head + 1, std::memory_order_release);
	return 1;
}

/// Plugs an emulated Logitech wheel into USB port 0 or 1, by the core's subtype: 0 Driving Force, 1 Driving Force
/// Pro, 2 Driving Force Pro (rev 11.02), 3 GT Force; -1 unplugs it. Applied at the next vsync or boot.
EMSCRIPTEN_KEEPALIVE void pcsx2web_set_wheel(int port, int type)
{
	if (port >= 0 && static_cast<u32>(port) < NUM_WHEEL_PORTS && type >= WHEEL_NONE && type < NUM_WHEEL_TYPES)
		s_wheel_request[port].store(type, std::memory_order_release);
}

/// Queues the state of one input of the emulated wheel on USB port 0 or 1 for the CPU thread, by its bind index
/// (usb_pad::ControlID: 0 steering left, 1 steering right, 2 throttle, 3 brake, 4-7 D-pad up, down, left, right, 8
/// onwards the wheel's buttons) and a value 0 to 1 (a button: pressed from 0.5). 1 if queued, 0 if the queue is full
/// or the arguments are invalid.
EMSCRIPTEN_KEEPALIVE int pcsx2web_wheel(int port, int bind, float value)
{
	if (port < 0 || static_cast<u32>(port) >= NUM_WHEEL_PORTS || bind < 0 || bind >= usb_pad::CID_COUNT)
		return 0;

	const u32 quantized = static_cast<u32>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
	const u32 head = s_wheel_queue_head.load(std::memory_order_relaxed);
	const u32 tail = s_wheel_queue_tail.load(std::memory_order_acquire);
	if (head - tail >= WHEEL_QUEUE_SIZE)
		return 0;

	s_wheel_queue[head % WHEEL_QUEUE_SIZE] = quantized | (static_cast<u32>(bind) << WHEEL_EVENT_BIND_SHIFT) |
	                                         (static_cast<u32>(port) << WHEEL_EVENT_PORT_SHIFT);
	s_wheel_queue_head.store(head + 1, std::memory_order_release);
	return 1;
}

/// The FFB ring (see s_ffb_ring): the game's raw force feedback commands to the emulated wheel. The page reads the
/// head with Atomics, copies the commands up to it and then stores the tail (word 1). Never blocks.
EMSCRIPTEN_KEEPALIVE const unsigned int* pcsx2web_ffb_ring(void)
{
	return reinterpret_cast<const unsigned int*>(s_ffb_ring);
}

/// Test hook: what the pad on port 0 or 1 holds, as the raw value (0..255) of each input in the order of
/// pcsx2web_pad_input, four to a word (the first in the low byte), then a word with bit 0 set while the
/// pad is connected and bit 1 while its analog light is on. Null for a bad port.
EMSCRIPTEN_KEEPALIVE const unsigned int* pcsx2web_test_pad_state(int port)
{
	if (port < 0 || static_cast<u32>(port) >= NUM_PAD_PORTS)
		return nullptr;

	return reinterpret_cast<const unsigned int*>(s_pad_snapshot[port]);
}

/// The frame descriptor: seq, width, height, pixels address, reserved.
EMSCRIPTEN_KEEPALIVE const unsigned int* pcsx2web_frame(void)
{
	return reinterpret_cast<const unsigned int*>(s_frame_descriptor);
}

/// The display canvas's backing size in device pixels and the device pixel ratio. With WebGPU the
/// GS presents at this size (read at its next present).
EMSCRIPTEN_KEEPALIVE void pcsx2web_surface_size(int width, int height, float scale)
{
	if (width > 0 && height > 0)
		WebGPUHost::SetSurfaceSize(static_cast<u32>(width), static_cast<u32>(height), scale);
}

/// The EE underclock: PCSX2's EE cycle rate, 0 (off) or -1 to -3 (the PS2's main processor at 75, 60 or 50 %),
/// applied at the next vsync or boot. Other values are ignored.
EMSCRIPTEN_KEEPALIVE void pcsx2web_set_ee_cycle_rate(int rate)
{
	if (rate >= -3 && rate <= 0)
		s_cycle_rate_request.store(rate, std::memory_order_release);
}

/// Crisp: nearest-neighbour presentation instead of bilinear (applied at the next vsync or boot).
EMSCRIPTEN_KEEPALIVE void pcsx2web_set_crisp(int crisp)
{
	s_crisp_request.store(crisp ? 1 : 0, std::memory_order_release);
}

/// The internal resolution: 1 (native, as the PS2 draws) or 2 (twice the lines), applied at the next vsync or boot.
/// Other values are ignored.
EMSCRIPTEN_KEEPALIVE void pcsx2web_set_upscale(int multiplier)
{
	if (multiplier == 1 || multiplier == 2)
		s_upscale_request.store(multiplier, std::memory_order_release);
}

/// The picture filter: PCSX2's TV shader (0 none, 1 scanlines, 2 diagonal, 3 triangular, 4 wave, 5 Lottes CRT,
/// 6 4xRGSS, 7 NxAGSS), applied at the next vsync or boot.
EMSCRIPTEN_KEEPALIVE void pcsx2web_set_tv_shader(int shader)
{
	if (shader >= 0 && shader <= 7)
		s_tv_shader_request.store(shader, std::memory_order_release);
}

/// Vsyncs emulated since the module started.
EMSCRIPTEN_KEEPALIVE unsigned int pcsx2web_vsync_count(void)
{
	return s_vsync_count.load(std::memory_order_relaxed);
}

/// Test hook: a screenshot (GSRenderer::SaveSnapshotToMemory) at vsync N, written to
/// /snapshots/frame-N.png in MEMFS.
EMSCRIPTEN_KEEPALIVE void pcsx2web_test_snapshot(unsigned int vsync)
{
	s_snapshot_vsync.store(vsync, std::memory_order_release);
}

/// Address of the log of TFX pipeline keys the WebGPU HW renderer's draws asked for since its device
/// started (WebGPUHost::TFXKeyBlock: session, count, version lo/hi, then count 36-byte keys). The page
/// keeps them per game to prewarm them at the game's next boot (README "Pipeline lists").
EMSCRIPTEN_KEEPALIVE const void* pcsx2web_tfx_log(void)
{
	return WebGPUHost::TFXKeyLog();
}

/// Address of the slot the page writes a game's stored key list into (the same layout; its first word
/// is the slot's state, see WebGPUHost::TFX_SLOT_*). The GS thread takes it at a present.
EMSCRIPTEN_KEEPALIVE void* pcsx2web_tfx_prewarm(void)
{
	return WebGPUHost::TFXPrewarmSlot();
}

/// Address of the EE JIT's code log: the blocks it compiled since the VM started (EEJit::CodeListBlock: session,
/// count, version lo/hi, then count entries of PC and code hash). The page keeps them per game to hand them back
/// at the game's next boot (README "Code lists").
EMSCRIPTEN_KEEPALIVE const void* pcsx2web_ee_code_log(void)
{
	return EEJit::CodeLog();
}

/// Address of the slot the page writes a game's stored code list into (the same layout; its first word is the
/// slot's state, EEJit::CODE_SLOT_*). The EE thread takes it at its next vsync.
EMSCRIPTEN_KEEPALIVE void* pcsx2web_ee_code_list(void)
{
	return EEJit::CodeListSlot();
}

/// Test hook: the WebGPU device destroys itself at its next present, as on a device loss.
EMSCRIPTEN_KEEPALIVE void pcsx2web_test_lose_device(void)
{
	WebGPUHost::RequestTestDeviceLoss();
}

/// Test hook: the copy of the 2D path's frame for vsync N asked for with -capture (the first frame
/// presented at vsync N or later), as {vsync of the copied frame, width, height, pixels address},
/// or null if there is none yet.
EMSCRIPTEN_KEEPALIVE const unsigned int* pcsx2web_test_capture(unsigned int frame)
{
	for (FrameCapture& cap : s_captures)
	{
		const u32 copied = cap.frame.load(std::memory_order_acquire);
		if (frame != 0 && cap.wanted == frame && copied != 0)
		{
			s_capture_header[0] = copied;
			s_capture_header[1] = cap.width;
			s_capture_header[2] = cap.height;
			s_capture_header[3] = static_cast<u32>(reinterpret_cast<uintptr_t>(cap.pixels.data()));
			return s_capture_header;
		}
	}
	return nullptr;
}

// VU1 JIT testing (the page's ?vu1jit= parameter, pcsx2-wasm/README.md; docs/web-port/VU1-JIT-DESIGN.md section 12).

/// Turns the VU1 JIT on (1) or off (0) for the next boots. On by default.
EMSCRIPTEN_KEEPALIVE void pcsx2web_vu1jit_enable(int enabled)
{
	s_vu1jit_enabled.store(enabled != 0, std::memory_order_release);
}

/// Runs the VU1 JIT fuzzer ("SEED:COUNT[:timing|jit|both[:hashes]]") on a worker; poll pcsx2web_vu1jit_fuzz_status().
/// 0 if started; -2 while the CPU thread is still initialising (try again); -1 while a VM is starting or running (the
/// fuzzer uses VU1 itself), a fuzz run is in progress, or the frontend failed.
EMSCRIPTEN_KEEPALIVE int pcsx2web_vu1jit_fuzz(const char* spec)
{
	const int state = s_state.load(std::memory_order_acquire);
	if (!spec || state == WEB_STATE_STARTING || state == WEB_STATE_RUNNING || s_fatal_error.load(std::memory_order_acquire))
		return -1;
	if (!s_cpu_thread_ready.load(std::memory_order_acquire))
		return -2;
	return VU1Jit::StartFuzz(spec) ? 0 : -1;
}

/// The fuzzer's status: u32 state (0 idle, 1 running, 2 passed, 3 differences found, 4 bad spec), programs done,
/// programs in total, differences; u64 instructions checked at byte 16; a NUL-terminated summary at byte 24.
EMSCRIPTEN_KEEPALIVE const void* pcsx2web_vu1jit_fuzz_status(void)
{
	static_assert(offsetof(VU1Jit::FuzzStatus, done) == 4 && offsetof(VU1Jit::FuzzStatus, total) == 8 &&
				  offsetof(VU1Jit::FuzzStatus, differences) == 12 && offsetof(VU1Jit::FuzzStatus, instructions) == 16 &&
				  offsetof(VU1Jit::FuzzStatus, summary) == 24);
	return &VU1Jit::GetFuzzStatus();
}

/// The JIT's counters, u32 each in VU1Jit::PublicStats order: self-test (0 not run, 1 passed, 2 failed), executes,
/// programs alive, compiles, entries into generated code, instructions, of them on the interpreter, refused lookups,
/// lockstep runs and differences, oracle checks and differences, EE-thread Clears.
EMSCRIPTEN_KEEPALIVE const void* pcsx2web_vu1jit_stats(void)
{
	static_assert(sizeof(VU1Jit::PublicStats) == 13 * sizeof(u32));
	return &VU1Jit::GetPublicStats();
}

} // extern "C"

//////////////////////////////////////////////////////////////////////////
// Host callbacks
//////////////////////////////////////////////////////////////////////////

static void RecordPerfVsync(u32 vsync)
{
	static const Threading::ThreadHandle ee = Threading::ThreadHandle::GetForCallingThread();
	const u32 n = s_perf_written.load(std::memory_order_relaxed);
	double* r = &s_perf_ring[(n % PERF_RECORDS) * PERF_FIELDS];
	r[0] = vsync;
	r[1] = emscripten_get_now();
	r[2] = ee.GetCPUTime() / 1000.0;
	r[3] = MTGS::GetThreadHandle().GetCPUTime() / 1000.0;
	r[4] = THREAD_VU1 ? vu1Thread.GetThreadHandle().GetCPUTime() / 1000.0 : 0.0;
	r[5] = PerfCounters::jit_compile_ns.load(std::memory_order_relaxed) / 1e6;
	r[6] = PerfCounters::gpu_pipeline_ns.load(std::memory_order_relaxed) / 1e6;
	r[7] = PerfCounters::gpu_pipelines.load(std::memory_order_relaxed);
	r[8] = VMManager::GetFrameRate();
	r[9] = PerfCounters::ee_build_ns.load(std::memory_order_relaxed) / 1e6;
	r[10] = static_cast<double>(PerfCounters::ee_modules.load(std::memory_order_relaxed));
	r[11] = static_cast<double>(PerfCounters::ee_ahead_modules.load(std::memory_order_relaxed));
	s_perf_written.store(n + 1, std::memory_order_release);
}

void Host::PumpMessagesOnCPUThread()
{
	// Called once per vsync.
	const u32 vsync = s_vsync_count.fetch_add(1, std::memory_order_relaxed) + 1;
	RecordPerfVsync(vsync);
	ProcessKeyEvents();
	RunCPUThreadQueue();
	CheckStopRequest();
	CheckCrispRequest(true);
	CheckWheelRequest(true);
	CheckUpscaleRequest(true);
	CheckTvShaderRequest(true);
	CheckCycleRateRequest(true);
	CheckMemcardRequests(true);
	s_memcard_status[0].store(MemcardBusy::IsBusy() ? MEMCARD_STATUS_BUSY : 0, std::memory_order_release);

	const u32 snapshot = s_snapshot_vsync.load(std::memory_order_acquire);
	if (snapshot != 0 && vsync >= snapshot)
	{
		s_snapshot_vsync.store(0, std::memory_order_relaxed);
		// Queued on the GS ring behind this vsync, so the next presented frame is captured.
		Error error;
		FileSystem::EnsureDirectoryExists(SNAPSHOT_DIRECTORY, false, &error);
		MTGS::RunOnGSThread([path = fmt::format("{}/frame-{}.png", SNAPSHOT_DIRECTORY, snapshot)]() { GSQueueSnapshot(path); });
	}

	UpdateAudioOutput();
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	if (std::this_thread::get_id() == s_cpu_thread_id)
	{
		function();
		return;
	}

	std::unique_lock lock(s_cpu_thread_queue_lock);
	if (!block)
	{
		s_cpu_thread_queue.push_back(std::move(function));
		return;
	}

	bool done = false;
	s_cpu_thread_queue.push_back([&function, &done]() {
		function();
		std::unique_lock done_lock(s_cpu_thread_queue_lock);
		done = true;
		s_cpu_thread_queue_cv.notify_all();
	});
	s_cpu_thread_queue_cv.wait(lock, [&done]() { return done; });
}

void Host::CommitBaseSettingChanges()
{
	// nothing to save, we're all in memory
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	// no settings UI yet
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	// nothing
}

bool Host::LocaleCircleConfirm()
{
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::OpenURL(const std::string_view url)
{
	// noop
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return std::string();
}

void Host::BeginTextInput()
{
	// noop
}

void Host::EndTextInput()
{
	// noop
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	// The GS draws into a fixed virtual window; the page scales the canvas.
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
	wi.surface_width = s_window_width;
	wi.surface_height = s_window_height;
	wi.surface_scale = 1.0f;
	return wi;
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	// GS thread. The WebGPU device presents to the page at the canvas's backing size; GSDeviceNull
	// keeps the fixed virtual window, which the page scales.
	if (g_gs_device && g_gs_device->GetRenderAPI() == RenderAPI::WebGPU)
	{
		WindowInfo wi;
		wi.type = WindowInfo::Type::Web;
		if (!WebGPUHost::GetSurfaceSize(&wi.surface_width, &wi.surface_height, &wi.surface_scale))
		{
			wi.surface_width = s_window_width;
			wi.surface_height = s_window_height;
			wi.surface_scale = 1.0f;
		}
		return wi;
	}

	return GetTopLevelWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
	INFO_LOG("pcsx2-wasm: running '{}' (serial '{}', ELF CRC {:08X})", title, disc_serial, current_crc);
	// The page names a game after PCSX2's game database. For a disc the database does not know, VMManager
	// makes a title up ("Unknown Game", "<serial> [?]", "<elf> [?]"), which the page would show and remember:
	// it gets no title then, and keeps the disc's file name.
	const GameDatabaseSchema::GameEntry* entry = disc_serial.empty() ? nullptr : GameDatabase::findGame(disc_serial);
	PublishGameDetails(entry ? entry->name : std::string(), disc_serial, disc_crc, current_crc);
}

void Host::OnPerformanceMetricsUpdated()
{
	s_speed.store(PerformanceMetrics::GetSpeed(), std::memory_order_relaxed);
	s_thread_usage[0].store(static_cast<float>(PerformanceMetrics::GetCPUThreadUsage()), std::memory_order_relaxed);
	s_thread_usage[1].store(PerformanceMetrics::GetGSThreadUsage(), std::memory_order_relaxed);
	s_thread_usage[2].store(THREAD_VU1 ? PerformanceMetrics::GetVUThreadUsage() : 0.0f, std::memory_order_relaxed);
	s_vps.store(PerformanceMetrics::GetAvgVPS(), std::memory_order_relaxed);
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

void Host::OnGameListFavoriteChanged(const std::string& path)
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
	// noop
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	// noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
	// noop
}

void Host::OnAchievementsRefreshed()
{
	// noop
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	for (u32 i = 0; i < NUM_KEYS; i++)
	{
		if (str == s_key_names[i])
			return i + 1;
	}

	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	if (code < 1 || code > NUM_KEYS)
		return std::nullopt;

	return std::string(s_key_names[code - 1]);
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, 2, count_str.view());
	}

	return ret;
}
