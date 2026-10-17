// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Browser frontend for the WebAssembly build. Linked with PROXY_TO_PTHREAD, so main()
// runs on a worker and the browser main thread only runs the page's JavaScript, which
// talks to us through the pcsx2web_* exports at the bottom of this file. Those exports
// never block: they only touch atomics and a lock-free key queue. Frames reach the
// page through a shared descriptor (see PresentFrame()), the BIOS through MEMFS.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/CDVD/Emscripten/WebDisc.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GS/Renderers/Null/GSDeviceNull.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/Host/AudioStream.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"
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
		FRAME_RESERVED,
		FRAME_DESCRIPTOR_SIZE,
	};
} // namespace

static constexpr u32 WINDOW_WIDTH = 640;
static constexpr u32 WINDOW_HEIGHT = 480;
static constexpr const char* BIOS_DIRECTORY = "/bios";

static MemorySettingsInterface s_settings_interface;
static std::vector<u8> s_roboto_font_data;

static std::atomic<int> s_state{WEB_STATE_WAITING};
static std::atomic<bool> s_fatal_error{false};
static std::atomic<bool> s_stop_requested{false};
// The VU1 JIT for the next boots: off by default, on with the page's ?vu1jit=1 testing parameter.
static std::atomic<bool> s_vu1jit_enabled{false};
static std::atomic<float> s_speed{0.0f};
static std::atomic<float> s_vps{0.0f};

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

//////////////////////////////////////////////////////////////////////////
// Discs, game details and memory cards
//////////////////////////////////////////////////////////////////////////

// Discs: the page opens the user's file (or a URL) in its disc worker (web/disc-worker.js), which
// serves the request block pcsx2web_disc_block() returns (pcsx2/CDVD/Emscripten/WebDisc.h). It writes
// a 0-byte placeholder at /disc/<name> and boots that path with pcsx2web_boot_disc(); the ISO reader
// then reads the disc through WebDiscFileReader (docs/web-port/DISC-DESIGN.md).
//
// Memory cards: slot 1 holds Mcd001.ps2 in /memcards, which the page mounts as IDBFS before main()
// runs and copies to IndexedDB after the card is written and when the VM stops (app.js, "memory
// cards"). Slot 2 stays empty.

static constexpr const char* DISC_DIRECTORY = "/disc";
static constexpr const char* MEMCARD_DIRECTORY = "/memcards";

// pcsx2web_boot_disc() flags.
static constexpr int BOOT_FLAG_FULL_BOOT = 1; // run the BIOS intro instead of fast boot

// The disc for the boot request in flight; written by pcsx2web_boot_disc() between its claim of the
// STARTING state and the request, taken by RunVM() on the CPU thread.
static char s_boot_disc_path[512];
static std::atomic<int> s_boot_disc_flags{0};
static std::atomic<bool> s_boot_disc_requested{false};

struct BootDisc
{
	std::string path; // empty: the BIOS alone
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

// The VU1 JIT fuzzer (pcsx2web_vu1jit_fuzz) uses VU1 itself: no boot while it runs.
static bool VU1JitFuzzRunning()
{
	return VU1Jit::GetFuzzStatus().state.load(std::memory_order_acquire) == 1;
}

static void ConfigureMemoryCards(SettingsInterface& si)
{
	si.SetStringValue("Folders", "MemoryCards", MEMCARD_DIRECTORY);
	si.SetBoolValue("MemoryCards", "Slot1_Enable", true);
	si.SetStringValue("MemoryCards", "Slot1_Filename", "Mcd001.ps2");
	si.SetBoolValue("MemoryCards", "Slot2_Enable", false);
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
		disc.path = s_boot_disc_path;
		disc.full_boot = (s_boot_disc_flags.load(std::memory_order_relaxed) & BOOT_FLAG_FULL_BOOT) != 0;
	}
	return disc;
}

// RunVM() has set up a BIOS boot (no disc, no fast boot); a disc boot changes that.
static void MakeBootParameters(const std::string& bios_name, const BootDisc& disc, VMBootParameters* params)
{
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
	s_boot_disc_flags.store(flags, std::memory_order_relaxed);
	s_boot_disc_requested.store(true, std::memory_order_release);
	s_stop_requested.store(false, std::memory_order_release);
	// The last Stop cancelled disc reads; this boot reads again. Here, before the request, and on the page's
	// thread like pcsx2web_stop(): a Stop that follows this call always cancels this boot's reads.
	WebDisc::ResumeReads();
	s_boot_request.store(1, std::memory_order_release);
	emscripten_futex_wake(&s_boot_request, 1);
	return 0;
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

	// The VU1 JIT stays off until it is switched on (docs/web-port/VU1-JIT-DESIGN.md section 12). RunVM applies the
	// page's testing override (pcsx2web_vu1jit_enable).
	si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", false);

	// Software renderer into GSDeviceNull, no audio, keyboard only (the default port 1
	// keyboard bindings stay), memory card slot 1 only. Hotkeys are cleared: most of them
	// (pause menu, save states, screenshots) need host support that is not there yet.
	si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::SW));
	// The legacy "OutputModule" key is no longer read; without this Cubeb fails and falls back.
	si.SetStringValue("SPU2/Output", "Backend", AudioStream::GetBackendName(AudioBackend::Null));
	si.SetBoolValue("InputSources", "SDL", false);
	si.SetBoolValue("InputSources", "XInput", false);
	si.ClearSection("Hotkeys");
	ConfigureMemoryCards(si);

	// Show what the PS2 itself prints (it reaches the page through Module.print).
	si.SetBoolValue("Logging", "EnableSystemConsole", true);
	si.SetBoolValue("Logging", "EnableFileLogging", false);
	si.SetBoolValue("Logging", "EnableTimestamps", true);
	si.SetBoolValue("Logging", "EnableEEConsole", true);
	si.SetBoolValue("Logging", "EnableIOPConsole", true);

	si.SetStringValue("Folders", "Bios", BIOS_DIRECTORY);

	VMManager::Internal::LoadStartupSettings();
	return true;
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
	s_frame_descriptor[FRAME_SEQ].fetch_add(1, std::memory_order_release);
	s_frame_write_index = (s_frame_write_index + 1) % NUM_FRAME_BUFFERS;
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
}

static void DiscardKeyEvents()
{
	s_key_queue_tail.store(s_key_queue_head.load(std::memory_order_acquire), std::memory_order_release);
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

	{
		auto lock = Host::GetSettingsLock();
		s_settings_interface.SetStringValue("Filenames", "BIOS", bios_name.c_str());
		s_settings_interface.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", s_vu1jit_enabled.load(std::memory_order_acquire));
	}
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

	MakeBootParameters(bios_name, disc, &params);
	Error error;
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
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		else
		{
			break;
		}
	}

	VMManager::Shutdown(false);
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

	for (;;)
		RunVM(WaitForBootRequest());
}

int main(int argc, char* argv[])
{
	CrashHandler::Install();
	Log::SetConsoleOutputLevel(LOGLEVEL_DEV);

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

/// The frame descriptor: seq, width, height, pixels address, reserved.
EMSCRIPTEN_KEEPALIVE const unsigned int* pcsx2web_frame(void)
{
	return reinterpret_cast<const unsigned int*>(s_frame_descriptor);
}

// VU1 JIT testing (the page's ?vu1jit= parameter, pcsx2-wasm/README.md; docs/web-port/VU1-JIT-DESIGN.md section 12).

/// Turns the VU1 JIT on (1) or off (0) for the next boots. Off by default.
EMSCRIPTEN_KEEPALIVE void pcsx2web_vu1jit_enable(int enabled)
{
	s_vu1jit_enabled.store(enabled != 0, std::memory_order_release);
}

/// Runs the VU1 JIT fuzzer ("SEED:COUNT[:timing|jit|both]") on a worker; poll pcsx2web_vu1jit_fuzz_status(). 0 if
/// started; -1 while a VM is starting or running (the fuzzer uses VU1 itself) or a fuzz run is in progress.
EMSCRIPTEN_KEEPALIVE int pcsx2web_vu1jit_fuzz(const char* spec)
{
	const int state = s_state.load(std::memory_order_acquire);
	if (!spec || state == WEB_STATE_STARTING || state == WEB_STATE_RUNNING)
		return -1;
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

void Host::PumpMessagesOnCPUThread()
{
	// Called once per vsync.
	ProcessKeyEvents();
	RunCPUThreadQueue();
	CheckStopRequest();
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
	wi.surface_width = WINDOW_WIDTH;
	wi.surface_height = WINDOW_HEIGHT;
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
	PublishGameDetails(title, disc_serial, disc_crc, current_crc);
}

void Host::OnPerformanceMetricsUpdated()
{
	s_speed.store(PerformanceMetrics::GetSpeed(), std::memory_order_relaxed);
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
