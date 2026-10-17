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
#include <condition_variable>
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

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/Renderers/Null/GSDeviceNull.h"
#include "pcsx2/GS/Renderers/WebGPU/WebGPUHost.h"
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

// Vsyncs since the module started (CPU thread writes, the page reads).
static std::atomic<u32> s_vsync_count{0};

// Crisp: nearest-neighbour presentation (WebGPU path). -1 = unchanged since the last apply.
static std::atomic<int> s_crisp_request{-1};
static bool s_crisp = false; // CPU thread

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

	// Software renderer into GSDeviceNull, no audio, keyboard only (the default port 1
	// keyboard bindings stay), no memory cards yet. Hotkeys are cleared: most of them
	// (pause menu, save states, screenshots) need host support that is not there yet.
	si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::SW));
	// The legacy "OutputModule" key is no longer read; without this Cubeb fails and falls back.
	si.SetStringValue("SPU2/Output", "Backend", AudioStream::GetBackendName(AudioBackend::Null));
	si.SetBoolValue("InputSources", "SDL", false);
	si.SetBoolValue("InputSources", "XInput", false);
	si.ClearSection("Hotkeys");
	for (u32 i = 0; i < 2; i++)
		si.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);

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
// -set Section/Key=Value, -surface WxH, -gs-idle block and -capture N,M,... (vsync numbers of the
// 2D path's frames to keep a copy of).
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
		else if (arg == "-gs-idle" && has_value)
		{
			// Tests: "block" keeps the GS thread's idle wait blocking with WebGPU, to exercise the
			// device's bounded event-loop turn on its own.
			WebGPUHost::SetIdleWaitsSuspend(std::string_view(argv[++i]) != "block");
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

	{
		auto lock = Host::GetSettingsLock();
		s_settings_interface.SetStringValue("Filenames", "BIOS", bios_name.c_str());
	}
	CheckCrispRequest(false);
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

	Console.WriteLnFmt("pcsx2-wasm: booting BIOS '{}'", bios_name);
	Error error;
	const VMBootResult result = VMManager::Initialize(params, &error);
	if (result != VMBootResult::StartupSuccess)
	{
		Console.ErrorFmt("pcsx2-wasm: VM failed to start (result {}): {}", static_cast<int>(result),
			error.GetDescription());
		s_state.store(WEB_STATE_FAILED, std::memory_order_release);
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
	if (!bios_filename || s_fatal_error.load(std::memory_order_acquire))
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
		s_stop_requested.store(true, std::memory_order_release);
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

/// The display canvas's backing size in device pixels and the device pixel ratio. With WebGPU the
/// GS presents at this size (read at its next present).
EMSCRIPTEN_KEEPALIVE void pcsx2web_surface_size(int width, int height, float scale)
{
	if (width > 0 && height > 0)
		WebGPUHost::SetSurfaceSize(static_cast<u32>(width), static_cast<u32>(height), scale);
}

/// Crisp: nearest-neighbour presentation instead of bilinear (applied at the next vsync or boot).
EMSCRIPTEN_KEEPALIVE void pcsx2web_set_crisp(int crisp)
{
	s_crisp_request.store(crisp ? 1 : 0, std::memory_order_release);
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

} // extern "C"

//////////////////////////////////////////////////////////////////////////
// Host callbacks
//////////////////////////////////////////////////////////////////////////

void Host::PumpMessagesOnCPUThread()
{
	// Called once per vsync.
	const u32 vsync = s_vsync_count.fetch_add(1, std::memory_order_relaxed) + 1;
	ProcessKeyEvents();
	RunCPUThreadQueue();
	CheckStopRequest();
	CheckCrispRequest(true);

	const u32 snapshot = s_snapshot_vsync.load(std::memory_order_acquire);
	if (snapshot != 0 && vsync >= snapshot)
	{
		s_snapshot_vsync.store(0, std::memory_order_relaxed);
		// Queued on the GS ring behind this vsync, so the next presented frame is captured.
		Error error;
		FileSystem::EnsureDirectoryExists(SNAPSHOT_DIRECTORY, false, &error);
		MTGS::RunOnGSThread([path = fmt::format("{}/frame-{}.png", SNAPSHOT_DIRECTORY, snapshot)]() { GSQueueSnapshot(path); });
	}
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
