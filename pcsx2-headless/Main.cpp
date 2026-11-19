// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Headless runner: boots the VM with no window and no audio (or audio to a WAV file
// with -wav), and logs what the BIOS/game prints. Input comes from an optional script.
// Used to bring up the WebAssembly build under Node before there is a browser
// frontend. Also builds natively.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

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
#include "pcsx2/CDVD/IsoFileFormats.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/Host/AudioStream.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/wasm/EEJit.h"
#include "pcsx2/wasm/IopJit.h"
#include "pcsx2/wasm/JitHeat.h"
#include "pcsx2/wasm/PerfCounters.h"

#include "WavOutput.h"
#ifdef __EMSCRIPTEN__
#include "pcsx2/MTVU.h"
#include "pcsx2/wasm/VU0Jit.h"
#include "pcsx2/wasm/VU1Jit.h"
#include "pcsx2/wasm/VU1JitEmit.h"
#include "pcsx2/wasm/VU1JitFuzz.h"
#include "pcsx2/wasm/VU1JitHooks.h"
#endif

static MemorySettingsInterface s_settings_interface;

static std::string s_app_root;
static std::string s_data_root;
static std::string s_bios_path;
static std::string s_boot_path;
static std::string s_elf_path;
static std::string s_screenshot_path;
static GSRendererType s_renderer = GSRendererType::SW;
static u64 s_max_frames = 600;
static u64 s_frames = 0;
static u64 s_screenshot_frame = 0; // 0 = 10 frames before the end
static std::string s_screenshot_dir;
static u64 s_screenshot_every = 0;
static std::vector<u64> s_screenshot_frames;
static std::optional<bool> s_fast_boot;
static bool s_memory_cards = false;
static bool s_unlimited = false;
static std::vector<s64> s_disc_check_lsns;
static std::string s_load_state_path;
static std::string s_save_state_path;
static std::string s_code_list_path, s_code_log_path; // -eejit-list, -eejit-list-out
static bool s_disc_check = false;
static std::string s_wav_path;
static WavOutput::Clock s_wav_clock = WavOutput::Clock::Emulated;
static u32 s_wav_period = 0;
static u64 s_compile_stats_every = 0; // -jit-compile-stats: vsyncs per printed window, 0 = off
static void PrintCompileStatsTotal();
#ifdef __EMSCRIPTEN__
// VU1 JIT test modes (docs/web-port/VU1-JIT-DESIGN.md section 12).
static std::string s_vu1jit_fuzz;
static u32 s_vu1jit_cache_versions = 0;
static u32 s_vu1jit_module_blocks = 0;
static std::string s_vu1jit_stats_path;
static bool s_vu1jit_report = false;
#endif

// Host key names; the key code is the index + 1 (0 would make an all-zero InputBindingKey). Only the keys of
// PCSX2's default keyboard mapping for pad 1, which the browser frontend also keeps.
static constexpr const char* s_key_names[] = {"Up", "Down", "Left", "Right", "Return", "Backspace", "I", "L", "K",
	"J", "Q", "1", "2", "E", "3", "4", "W", "S", "A", "D", "T", "G", "F", "H"};
static constexpr u32 NUM_KEYS = static_cast<u32>(std::size(s_key_names));

// Scripted pad 1 input: pad button names to the keys of the default keyboard mapping.
static constexpr std::pair<const char*, const char*> s_button_keys[] = {{"Up", "Up"}, {"Down", "Down"},
	{"Left", "Left"}, {"Right", "Right"}, {"Start", "Return"}, {"Select", "Backspace"}, {"Triangle", "I"},
	{"Circle", "L"}, {"Cross", "K"}, {"Square", "J"}, {"L1", "Q"}, {"L2", "1"}, {"L3", "2"}, {"R1", "E"}, {"R2", "3"},
	{"R3", "4"}, {"LUp", "W"}, {"LDown", "S"}, {"LLeft", "A"}, {"LRight", "D"}, {"RUp", "T"}, {"RDown", "G"},
	{"RLeft", "F"}, {"RRight", "H"}};
static constexpr u64 DEFAULT_INPUT_HOLD = 6;
struct InputEvent
{
	u64 frame;
	u32 key_code;
	bool pressed;
};
static std::vector<InputEvent> s_input_events; // sorted by frame once parsed
static size_t s_next_input_event = 0;

// -set Section/Key=Value overrides, applied after the defaults. "EmuCore/GS/extrathreads=4" is section
// "EmuCore/GS", key "extrathreads": the key is whatever follows the last '/' before the '='.
struct SettingOverride
{
	std::string section;
	std::string key;
	std::string value;
};
static std::vector<SettingOverride> s_setting_overrides;

static bool InitializeConfig()
{
	EmuFolders::SetAppRoot();
	if (!s_app_root.empty())
		EmuFolders::AppRoot = Path::Canonicalize(s_app_root);
	if (!s_data_root.empty())
		EmuConfig.CustomDataPath = s_data_root;

	Error error;
	if (!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(&error))
	{
		Console.ErrorFmt("Failed to set up folders: {}", error.GetDescription());
		return false;
	}

	const char* hw_error;
	if (!VMManager::PerformEarlyHardwareChecks(&hw_error))
	{
		Console.ErrorFmt("Hardware check failed: {}", hw_error);
		return false;
	}

	// The OSD/ImGui layer needs a font even when nothing is displayed.
	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
			return false;
		}

		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = roboto_data;
		fi.exclude_ranges = {};
		fi.face_name = nullptr;
		fi.is_emoji_font = false;
		fonts.push_back(fi);
		ImGuiManager::SetFonts(std::move(fonts));
	}

	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);
	VMManager::SetDefaultSettings(si, true, true, true, true, true);
#ifdef __EMSCRIPTEN__
	// The VU1 JIT is on (docs/web-port/VU1-JIT-DESIGN.md): -set EmuCore/CPU/Recompiler/EnableVU1=false
	// runs the VU1 interpreter instead.
	si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU1", true);
	// The same for the VU0 micro-mode JIT: -set EmuCore/CPU/Recompiler/EnableVU0=false.
	si.SetBoolValue("EmuCore/CPU/Recompiler", "EnableVU0", true);
#endif

	// Headless: no renderer output, no audio. Pad 1 keeps the default keyboard bindings only for an input
	// script, which feeds those keys like the browser frontend does. Memory cards only on request.
	si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(s_renderer));
	si.SetIntValue("EmuCore/GS", "ScreenshotFormat", static_cast<int>(GSScreenshotFormat::PNG));
	// The legacy "OutputModule" key is no longer read; without "Backend" Cubeb fails and falls back to null.
	si.SetStringValue("SPU2/Output", "Backend", AudioStream::GetBackendName(AudioBackend::Null));
	WavOutput::ApplySettings(si);
	si.SetBoolValue("InputSources", "SDL", false);
	si.SetBoolValue("InputSources", "XInput", false);
	if (s_input_events.empty())
		Pad::ClearPortBindings(si, 0);
	si.ClearSection("Hotkeys");
	for (u32 i = 0; i < 2; i++)
		si.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), s_memory_cards && i == 0);

	// Show what the PS2 itself prints.
	si.SetBoolValue("Logging", "EnableSystemConsole", true);
	si.SetBoolValue("Logging", "EnableFileLogging", false);
	si.SetBoolValue("Logging", "EnableTimestamps", true);
	si.SetBoolValue("Logging", "EnableEEConsole", true);
	si.SetBoolValue("Logging", "EnableIOPConsole", true);

	if (!s_bios_path.empty())
	{
		si.SetStringValue("Folders", "Bios", std::string(Path::GetDirectory(s_bios_path)).c_str());
		si.SetStringValue("Filenames", "BIOS", std::string(Path::GetFileName(s_bios_path)).c_str());
	}

	// The settings interface stores strings, so ints, floats and bools ("true"/"false") all parse back.
	for (const SettingOverride& ov : s_setting_overrides)
	{
		INFO_LOG("Headless: setting [{}] {} = {}", ov.section, ov.key, ov.value);
		si.SetStringValue(ov.section.c_str(), ov.key.c_str(), ov.value.c_str());
	}

	VMManager::Internal::LoadStartupSettings();
	return true;
}

static std::optional<u32> GetKeyCode(const std::string_view key_name)
{
	for (u32 i = 0; i < NUM_KEYS; i++)
	{
		if (key_name == s_key_names[i])
			return i + 1;
	}
	return std::nullopt;
}

// "<frame>:<button>[+<button>...][:<hold vsyncs>],...", or "@file" with the same entries on lines or separated by
// commas, and '#' comments. Each button is pressed at vsync <frame> and released <hold> vsyncs later.
static bool ParseInputScript(std::string_view script)
{
	std::string file_contents;
	if (script.starts_with('@'))
	{
		std::optional<std::string> contents = FileSystem::ReadFileToString(std::string(script.substr(1)).c_str());
		if (!contents.has_value())
		{
			Console.ErrorFmt("Failed to read input script '{}'", script.substr(1));
			return false;
		}
		// One or more entries per line; '#' starts a comment that runs to the end of the line.
		for (const std::string_view line : StringUtil::SplitString(contents.value(), '\n', true))
		{
			file_contents.append(line.substr(0, line.find('#')));
			file_contents.push_back(',');
		}
		script = file_contents;
	}

	for (const std::string_view raw_entry : StringUtil::SplitString(script, ',', true))
	{
		const std::string_view entry = StringUtil::StripWhitespace(raw_entry);
		if (entry.empty())
			continue;

		const std::vector<std::string_view> fields = StringUtil::SplitString(entry, ':', false);
		const std::optional<u64> frame = (fields.size() >= 2) ? StringUtil::FromChars<u64>(fields[0]) : std::nullopt;
		const std::optional<u64> hold =
			(fields.size() == 3) ? StringUtil::FromChars<u64>(fields[2]) : std::optional<u64>(DEFAULT_INPUT_HOLD);
		const std::vector<std::string_view> buttons =
			(fields.size() >= 2) ? StringUtil::SplitString(fields[1], '+', true) : std::vector<std::string_view>();
		if (!frame.has_value() || !hold.has_value() || hold.value() == 0 || fields.size() > 3 || buttons.empty())
		{
			Console.ErrorFmt("Bad -input entry '{}' (use <frame>:<button>[+<button>...][:<hold vsyncs>])", entry);
			return false;
		}

		for (const std::string_view button : buttons)
		{
			std::optional<u32> key_code;
			for (const auto& [button_name, key_name] : s_button_keys)
			{
				if (StringUtil::compareNoCase(button, button_name))
					key_code = GetKeyCode(key_name);
			}
			if (!key_code.has_value())
			{
				Console.ErrorFmt("Unknown button '{}' in -input entry '{}'", button, entry);
				return false;
			}
			s_input_events.push_back({frame.value(), key_code.value(), true});
			s_input_events.push_back({frame.value() + hold.value(), key_code.value(), false});
		}
	}

	// Releases sort before presses on the same vsync, so back-to-back taps of one button stay two taps.
	std::stable_sort(s_input_events.begin(), s_input_events.end(), [](const InputEvent& lhs, const InputEvent& rhs) {
		return (lhs.frame != rhs.frame) ? (lhs.frame < rhs.frame) : (!lhs.pressed && rhs.pressed);
	});
	return true;
}

static bool ParseCommandLineArgs(int argc, char* argv[])
{
	for (int i = 1; i < argc; i++)
	{
		const std::string_view arg = argv[i];
		const bool has_value = (i + 1) < argc;
		if (arg == "-approot" && has_value)
			s_app_root = argv[++i];
		else if (arg == "-dataroot" && has_value)
			s_data_root = argv[++i];
		else if (arg == "-bios" && has_value)
			s_bios_path = argv[++i];
		else if (arg == "-elf" && has_value)
			s_elf_path = argv[++i];
		else if (arg == "-frames" && has_value)
			s_max_frames = StringUtil::FromChars<u64>(argv[++i]).value_or(s_max_frames);
		else if (arg == "-screenshot" && has_value)
			s_screenshot_path = argv[++i];
		else if (arg == "-screenshot-frame" && has_value)
			s_screenshot_frame = StringUtil::FromChars<u64>(argv[++i]).value_or(0);
		else if (arg == "-screenshot-dir" && has_value)
			s_screenshot_dir = argv[++i];
		else if (arg == "-screenshot-every" && has_value)
			s_screenshot_every = StringUtil::FromChars<u64>(argv[++i]).value_or(0);
		else if (arg == "-screenshot-frames" && has_value)
		{
			for (const std::string_view frame : StringUtil::SplitString(argv[++i], ',', true))
			{
				if (const std::optional<u64> value = StringUtil::FromChars<u64>(frame); value.has_value())
					s_screenshot_frames.push_back(value.value());
			}
		}
		else if (arg == "-fastboot")
			s_fast_boot = true;
		else if (arg == "-slowboot")
			s_fast_boot = false;
		else if (arg == "-memcards")
			s_memory_cards = true;
		else if (arg == "-unlimited")
			s_unlimited = true;
		else if (arg == "-wav" && has_value)
			s_wav_path = argv[++i];
		else if (arg == "-wav-realtime")
			s_wav_clock = WavOutput::Clock::Realtime;
		else if (arg == "-wav-period" && has_value)
			s_wav_period = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
		else if (arg == "-loadstate" && has_value)
			s_load_state_path = argv[++i];
		else if (arg == "-savestate" && has_value)
			s_save_state_path = argv[++i];
		else if (arg == "-input" && has_value)
		{
			if (!ParseInputScript(argv[++i]))
				return false;
		}
#ifdef __EMSCRIPTEN__
		else if (arg == "-vu1jit-fuzz" && has_value)
			s_vu1jit_fuzz = argv[++i];
		else if (arg == "-vu1jit-cache" && has_value)
		{
			const auto cap = StringUtil::FromChars<u32>(argv[++i]);
			if (!cap || (*cap != 16 && *cap != 32 && *cap != 64))
				return false;
			s_vu1jit_cache_versions = *cap;
		}
		else if (arg == "-vu1jit-cache-blocks" && has_value)
		{
			const auto cap = StringUtil::FromChars<u32>(argv[++i]);
			if (!cap || (*cap != 16 && *cap != 64 && *cap != 128 && *cap != 256))
				return false;
			s_vu1jit_module_blocks = *cap;
		}
		else if (arg == "-vu1jit-oracle")
		{
			VU1Jit::SetOracle(true);
			s_vu1jit_report = true;
		}
		else if (arg == "-vu1jit-lockstep")
		{
			VU1Jit::SetLockstep(true);
			s_vu1jit_report = true;
		}
		else if (arg == "-vu0jit-lockstep")
		{
			VU0Jit::SetLockstep(true);
			s_vu1jit_report = true;
		}
		else if (arg == "-vu0jit-slices" && has_value)
			VU1Jit::g_vu0_slices = StringUtil::FromChars<int>(argv[++i]).value_or(1) != 0;
		else if (arg == "-vu1jit-stats" && has_value)
		{
			s_vu1jit_stats_path = argv[++i];
			VU1Jit::SetStats(true);
			s_vu1jit_report = true;
		}
		else if (arg == "-mtvu-sync")
			VU1JitHooks::mtvu_sync = true;
#endif
		else if (arg == "-disc-check")
		{
			s_disc_check = true;
			const std::string_view next = has_value ? std::string_view(argv[i + 1]) : std::string_view();
			if (!next.empty() && next.find_first_not_of("-0123456789,") == std::string_view::npos)
			{
				for (const std::string_view lsn : StringUtil::SplitString(argv[++i], ',', true))
				{
					if (const std::optional<s64> value = StringUtil::FromChars<s64>(lsn); value.has_value())
						s_disc_check_lsns.push_back(value.value());
				}
			}
		}
		else if (arg == "-set" && has_value)
		{
			const std::string_view setting = argv[++i];
			const std::string_view::size_type eq = setting.find('=');
			const std::string_view path = setting.substr(0, eq);
			const std::string_view::size_type slash = path.rfind('/');
			if (eq == std::string_view::npos || slash == std::string_view::npos || slash == 0 ||
				slash + 1 == path.size())
			{
				Console.ErrorFmt("Bad -set '{}' (use Section/Key=Value, e.g. EmuCore/GS/extrathreads=4)", setting);
				return false;
			}
			s_setting_overrides.push_back({std::string(path.substr(0, slash)), std::string(path.substr(slash + 1)),
				std::string(setting.substr(eq + 1))});
		}
		else if (arg == "-eejit" && has_value)
			EEJit::g_mode = StringUtil::FromChars<int>(argv[++i]).value_or(1);
		else if (arg == "-iopjit" && has_value)
			IopJit::g_mode = StringUtil::FromChars<int>(argv[++i]).value_or(1);
		else if (arg == "-eejit-link" && has_value)
			EEJit::g_link = StringUtil::FromChars<int>(argv[++i]).value_or(1) != 0;
		else if (arg == "-eejit-mask" && has_value)
			EEJit::g_dbgmask = StringUtil::FromChars<u32>(argv[++i]).value_or(0);
		else if (arg == "-eejit-stats")
		{
			EEJit::g_stats = true;
			IopJit::g_stats = true;
		}
		else if (arg == "-eejit-spin" && has_value)
			EEJit::g_spin = StringUtil::FromChars<int>(argv[++i]).value_or(1) != 0;
		else if (arg == "-eejit-bg" && has_value)
			EEJit::g_background = StringUtil::FromChars<int>(argv[++i]).value_or(1) != 0;
		else if (arg == "-eejit-ahead" && has_value)
		{
			const int ahead = StringUtil::FromChars<int>(argv[++i]).value_or(1);
			EEJit::g_ahead = ahead != 0;
			EEJit::g_ahead_test = ahead == 2;
		}
		else if (arg == "-eejit-list" && has_value)
			s_code_list_path = argv[++i];
		else if (arg == "-eejit-list-out" && has_value)
			s_code_log_path = argv[++i];
		else if ((arg == "-eejit-hot" || arg == "-iopjit-hot") && has_value)
		{
			// N[,M[,B]]: compile block entries on their Nth run, with the neighbours that ran M times and the entries that ran B times (JitHeat.h).
			JitHeat::Policy& policy = arg == "-eejit-hot" ? JitHeat::ee : JitHeat::iop;
			const std::vector<std::string_view> parts = StringUtil::SplitString(argv[++i], ',', true);
			policy.threshold = parts.empty() ? policy.threshold : StringUtil::FromChars<u32>(parts[0]).value_or(policy.threshold);
			if (parts.size() >= 2)
				policy.neighbour = StringUtil::FromChars<u32>(parts[1]).value_or(policy.neighbour);
			if (parts.size() >= 3)
				policy.batch = StringUtil::FromChars<u32>(parts[2]).value_or(policy.batch);
		}
		else if (arg == "-jit-budget" && has_value)
			JitHeat::budget_ns = StringUtil::FromChars<u64>(argv[++i]).value_or(0) * 1000;
		else if (arg == "-jit-compile-stats" && has_value)
			s_compile_stats_every = StringUtil::FromChars<u64>(argv[++i]).value_or(0);
		else if (arg == "-eejit-trace" && has_value)
		{
			// SHIFT[,FROM,TO]: print the state hash every 2^SHIFT branch tests, and every test in [FROM, TO].
			const std::vector<std::string_view> parts = StringUtil::SplitString(argv[++i], ',', true);
			EEJit::g_trace = true;
			EEJit::g_trace_shift = parts.empty() ? 16 : StringUtil::FromChars<u32>(parts[0]).value_or(16);
			if (parts.size() >= 3)
			{
				EEJit::g_trace_from = StringUtil::FromChars<u64>(parts[1]).value_or(~0ull);
				EEJit::g_trace_to = StringUtil::FromChars<u64>(parts[2]).value_or(0);
			}
		}
		else if (arg == "-renderer" && has_value)
		{
			const std::string_view name = argv[++i];
			if (name == "sw")
				s_renderer = GSRendererType::SW;
			else if (name == "null")
				s_renderer = GSRendererType::Null;
			else
			{
				Console.ErrorFmt("Unknown renderer '{}' (use sw or null)", name);
				return false;
			}
		}
		else if (arg == "-help" || arg == "--help")
		{
			std::fprintf(stderr, "Usage: %s -approot <dir with resources/> -dataroot <dir> -bios <file> [-elf FILE] [-frames N]\n"
								 "         [-renderer sw|null] [-screenshot file.png [-screenshot-frame N]]\n"
								 "         [-screenshot-dir DIR (-screenshot-every N | -screenshot-frames N,N,...)]\n"
								 "         [-fastboot | -slowboot] [-memcards] [-input SCRIPT | -input @FILE]\n"
								 "         [-loadstate FILE] [-savestate FILE] [-unlimited]\n"
								 "         [-wav FILE.wav [-wav-realtime [-wav-period FRAMES]]]\n"
								 "         [-set Section/Key=Value]... [-disc-check [LSN,LSN,...]] [disc image]\n"
								 "         [-eejit 0|1] [-eejit-link 0|1] [-iopjit 0|1] [-eejit-stats] [-eejit-trace SHIFT[,FROM,TO]] [-eejit-mask BITS]\n"
								 "         [-eejit-spin 0|1] [-eejit-bg 0|1] [-eejit-ahead 0|1] [-eejit-list FILE] [-eejit-list-out FILE]\n"
								 "         [-eejit-hot N[,M[,B]]] [-iopjit-hot N[,M[,B]]] [-jit-budget US] [-jit-compile-stats VSYNCS]\n",
				argv[0]);
			std::fprintf(stderr, "  -eejit 0 runs the EE interpreter instead of the EE JIT, -iopjit 0 the IOP interpreter instead of the IOP JIT;\n"
								 "  -eejit-trace prints EE and IOP state hashes to compare runs.\n"
								 "  -eejit-spin 0 runs idle loops iteration by iteration instead of skipping to the next event.\n"
								 "  -eejit-list-out writes the EE JIT's code log (the blocks it compiled) to FILE at the end; -eejit-list\n"
								 "         uses such a file as the code list: those blocks compile on their first run, and ahead when the\n"
								 "         frame limiter has time left. -eejit-ahead 0 ignores code lists (2: compiles ahead every vsync, a test mode). -eejit-bg 0 builds EE JIT\n"
								 "         modules on the EE thread instead of a thread of their own.\n"
								 "  -eejit-hot and -iopjit-hot compile a block entry on its Nth run, with the neighbours in its page that\n"
								 "         ran M times (0: all) and the entries of any page that ran B times (default 128,1,64; 1\n"
								 "         compiles everything on its first run). -jit-budget caps the EE and IOP JITs' compile time per\n"
								 "         vsync, in microseconds (default 4000; 0: no cap).\n"
								 "  -jit-compile-stats prints, every VSYNCS vsyncs and at the end, the JITs' compile time: the total and\n"
								 "         the largest in one vsync, for the EE and IOP JITs (EE thread) and for all JITs.\n");
#ifdef __EMSCRIPTEN__
			std::fprintf(stderr, "         [-vu1jit-fuzz SEED:COUNT[:timing|jit|both|cache[:hashes]]] [-vu1jit-oracle] [-vu1jit-lockstep] [-vu0jit-lockstep] [-vu0jit-slices 0|1]\n"
								 "         [-vu1jit-stats FILE] [-vu1jit-cache 16|32|64] [-vu1jit-cache-blocks 16|64|128|256] [-mtvu-sync]\n");
#endif
			std::fprintf(stderr, "  With no disc image the BIOS boots to its menu. -frames 0 runs until stopped.\n");
			std::fprintf(stderr, "  -elf boots a PS2 executable (always fast boot), with the disc image in the drive if one is given.\n");
			std::fprintf(stderr, "  The screenshot is requested at vsync N (default: 10 before the end) and shows the next frame.\n");
			std::fprintf(stderr, "  -screenshot-dir saves DIR/frame_NNNNNN.png every N vsyncs or at the listed vsyncs.\n");
			std::fprintf(stderr, "  -fastboot skips the BIOS intro (the desktop default); without it the intro plays.\n");
			std::fprintf(stderr, "  -memcards enables memory card slot 1 (memcards/Mcd001.ps2 in the data root, created if missing).\n");
			std::fprintf(stderr, "  -input drives pad 1: <frame>:<button>[+<button>...][:<hold vsyncs, default %u>],...\n"
								 "         buttons: Up Down Left Right Start Select Triangle Circle Cross Square L1 L2 L3 R1 R2 R3\n"
								 "         LUp LDown LLeft LRight RUp RDown RLeft RRight (sticks at full deflection).\n",
				static_cast<unsigned>(DEFAULT_INPUT_HOLD));
			std::fprintf(stderr, "  -unlimited turns the frame limiter off, to measure speed above 100%%.\n");
			std::fprintf(stderr, "  -loadstate boots and loads FILE; -savestate saves FILE when the run ends. Vsyncs count from 0 again.\n");
			std::fprintf(stderr, "  -set overrides a setting after the defaults, e.g. -set EmuCore/GS/extrathreads=4.\n");
			std::fprintf(stderr, "  -wav records what the audio stream hands to a device (32-bit float, 48 kHz stereo; WebAssembly only).\n"
								 "         By default the device reads at every vsync: the SPU2 mix in emulated time, before\n"
								 "         time-stretching. -wav-realtime reads one period (default: OutputLatencyMS, 20 ms) per\n"
								 "         period of wall-clock time: what a listener hears, after time-stretching (SPU2/Output\n"
								 "         SyncMode, TimeStretch by default; -set SPU2/Output/SyncMode=Disabled turns it off).\n");
			std::fprintf(stderr, "  -disc-check reads the listed sectors (negative counts from the end) through the CDVD ISO\n"
								 "         reader, prints an FNV-1a hash of each 2048-byte sector and exits.\n");
#ifdef __EMSCRIPTEN__
			std::fprintf(stderr, "  VU1 JIT (WebAssembly; on by default, off with -set EmuCore/CPU/Recompiler/EnableVU1=false):\n"
								 "  -vu1jit-fuzz runs random VU1 programs without a BIOS or a VM and exits: 'timing' checks the\n"
								 "         timing model at every instruction, 'jit' compares the interpreter and the JIT, 'both' does both.\n"
								 "         'cache' replays 80 fixed-PC overlays before and after eviction and reset.\n"
								 "  -vu1jit-oracle checks the timing model at every VU1 instruction the JIT's dispatcher runs.\n"
								 "  -vu1jit-lockstep runs every VU1 program on the JIT and on the interpreter and compares them.\n"
								 "  -vu0jit-lockstep does the same for VU0 micro programs (the VU0 JIT is on by default, off with EnableVU0=false).\n"
								 "  -vu0jit-slices 0 makes VU0 blocks run only when they fit the EE's slice whole (the interpreter steps the rest).\n"
								 "  -vu1jit-stats writes per-vsync VU1 statistics (runs, cycles, execute time, PATH1 hash) as CSV.\n"
								 "  -mtvu-sync makes the EE wait for the VU thread at each check, so whole runs repeat exactly.\n");
#endif
			return false;
		}
		else if (!arg.empty() && arg[0] != '-')
			s_boot_path = arg;
		else
		{
			Console.ErrorFmt("Unknown argument: {}", arg);
			return false;
		}
	}

	return true;
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
	// not running any UI, so no settings requests will come in
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	// nothing
}

bool Host::LocaleCircleConfirm()
{
	// not running any UI, so no settings requests will come in
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
	WindowInfo wi;
	wi.type = WindowInfo::Type::Surfaceless;
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
	INFO_LOG("Headless: running '{}' (serial '{}', ELF CRC {:08X})", title, disc_serial, current_crc);
}

void Host::OnPerformanceMetricsUpdated()
{
	// Thread usage comes from per-thread CPU clocks; on Emscripten, from wall time minus the time each thread
	// waits for the others (Threading::ScopedWaitTimer). n/a when a thread reports no time.
	static const bool has_thread_clocks = Threading::ThreadHandle::GetForCallingThread().GetCPUTime() != 0;
	std::string threads;
	if (has_thread_clocks)
	{
		threads = fmt::format("EE {:.0f}%, GS {:.0f}%", PerformanceMetrics::GetCPUThreadUsage(),
			PerformanceMetrics::GetGSThreadUsage());
		if (THREAD_VU1)
			threads += fmt::format(", VU {:.0f}%", PerformanceMetrics::GetVUThreadUsage());
		for (u32 i = 0; i < PerformanceMetrics::GetGSSWThreadCount(); i++)
			threads += fmt::format(", SW{} {:.0f}%", i, PerformanceMetrics::GetGSSWThreadUsage(i));
	}
	else
	{
		threads = "thread usage n/a";
	}

	INFO_LOG("Headless: frame {}, {:.1f} VPS, speed {:.1f}%, {}",
		PerformanceMetrics::GetFrameNumber(), PerformanceMetrics::GetAvgVPS(), PerformanceMetrics::GetSpeed(), threads);
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

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	pxFailRel("Not implemented");
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
	return GetKeyCode(str);
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


static void CPUThreadMain(std::atomic<int>* ret)
{
	ret->store(EXIT_FAILURE);

	if (VMManager::Internal::CPUThreadInitialize())
	{
		VMManager::ApplySettings();

		VMBootParameters params;
		params.filename = s_boot_path;
		params.source_type = s_boot_path.empty() ? CDVD_SourceType::NoDisc : CDVD_SourceType::Iso;
		params.fast_boot = s_fast_boot.value_or(false);
		// The real path: PCSX2 lets the ELF open itself through host: only when its path matches the
		// host: root it derives with Path::RealPath() (on macOS /tmp is a link to /private/tmp).
		params.elf_override = s_elf_path.empty() ? std::string() : Path::RealPath(s_elf_path);
		params.save_state = s_load_state_path;
		params.start_unlimited = s_unlimited;
		EEJit::NewCodeLogSession();
		if (!s_code_list_path.empty() && !EEJit::LoadCodeList(s_code_list_path.c_str()))
			Console.ErrorFmt("Headless: could not read the code list '{}'", s_code_list_path);

		const VMBootResult result = VMManager::Initialize(params);
		if (result == VMBootResult::StartupSuccess)
		{
			INFO_LOG("Headless: VM started, running {} frames", s_max_frames);
			VMManager::SetState(VMState::Running);
			while (VMManager::GetState() == VMState::Running)
				VMManager::Execute();
			INFO_LOG("Headless: stopped after {} frames", s_frames);
			EEJit::PrintStats();
			IopJit::PrintStats();
			if (s_compile_stats_every)
				PrintCompileStatsTotal();
			EEJit::TraceFinish();
#ifdef __EMSCRIPTEN__
			if (s_vu1jit_report)
			{
				if (vu1Thread.IsOpen())
					vu1Thread.WaitVU();
				VU1Jit::LogReport();
				if (EmuConfig.Cpu.Recompiler.EnableVU0)
					VU0Jit::LogReport();
				if (!s_vu1jit_stats_path.empty())
				{
					if (VU1Jit::WriteStats(s_vu1jit_stats_path))
						INFO_LOG("Headless: VU1 statistics written to '{}'", s_vu1jit_stats_path);
					else
						Console.ErrorFmt("Headless: failed to write VU1 statistics to '{}'", s_vu1jit_stats_path);
				}
			}
#endif
			// The callback only runs on failure, for example when the memory card was written in the last 300
			// vsyncs (MemcardBusy cancels the save): say so and fail, so no one takes an older file for this run's.
			bool save_failed = false;
			if (!s_save_state_path.empty())
			{
				VMManager::SaveState(s_save_state_path.c_str(), false, false, [&save_failed](const std::string& error) {
					Console.ErrorFmt("Headless: save state failed: {}", error);
					save_failed = true;
				});
				VMManager::WaitForSaveStateFlush();
				if (!save_failed)
					INFO_LOG("Headless: saved state to '{}'", s_save_state_path);
			}
			if (!s_code_log_path.empty() && !EEJit::SaveCodeLog(s_code_log_path.c_str()))
				Console.ErrorFmt("Headless: could not write the code log '{}'", s_code_log_path);
			GSJoinSnapshotThreads();
			VMManager::Shutdown(false);
			ret->store(save_failed ? EXIT_FAILURE : EXIT_SUCCESS);
		}
		else
		{
			Console.ErrorFmt("Headless: VM failed to start (result {})", static_cast<int>(result));
		}
	}

	VMManager::Internal::CPUThreadShutdown();
}

static u64 HashSector(const u8* data, size_t size)
{
	// FNV-1a, 64-bit.
	u64 hash = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < size; i++)
		hash = (hash ^ data[i]) * 0x100000001b3ULL;
	return hash;
}

// Reads sectors through the same InputIsoFile/ThreadedFileReader path the CDVD uses (asynchronous read on the
// reader thread, then FinishRead3 in 2048-byte mode), so offsets past 2 GB and 4 GB can be checked against the file.
static int RunDiscCheck()
{
	if (s_boot_path.empty())
	{
		Console.Error("-disc-check needs a disc image.");
		return EXIT_FAILURE;
	}

	InputIsoFile iso;
	Error error;
	if (!iso.Open(s_boot_path, &error))
	{
		Console.ErrorFmt("Failed to open '{}': {}", s_boot_path, error.GetDescription());
		return EXIT_FAILURE;
	}

	const u32 blocks = iso.GetBlockCount();
	INFO_LOG("Headless: disc check, type {}, {} blocks, block offset {}", static_cast<int>(iso.GetType()), blocks,
		iso.GetBlockOffset());

	std::vector<s64> lsns = s_disc_check_lsns;
	if (lsns.empty())
	{
		// Start, the 2 GB and 4 GB byte boundaries when the image reaches them, and the end.
		lsns = {0, 16, -1, -2};
		for (const u64 boundary : {0x80000000ULL, 0x100000000ULL})
		{
			const s64 lsn = static_cast<s64>(boundary / 2048);
			if (lsn < blocks)
				lsns.insert(lsns.end(), {lsn - 1, lsn});
		}
	}

	int failures = 0;
	for (const s64 requested : lsns)
	{
		const s64 lsn = (requested < 0) ? (static_cast<s64>(blocks) + requested) : requested;
		if (lsn < 0 || lsn >= blocks)
		{
			Console.ErrorFmt("Headless: disc check LSN {} is out of range", requested);
			failures++;
			continue;
		}

		u8 sector[2048];
		iso.BeginRead2(static_cast<uint>(lsn));
		if (iso.FinishRead3(sector, CDVD_MODE_2048) != 0)
		{
			Console.ErrorFmt("Headless: disc check LSN {} failed to read", lsn);
			failures++;
			continue;
		}

		INFO_LOG("Headless: disc check LSN {} (byte offset {}) FNV-1a {:016x}", lsn, static_cast<u64>(lsn) * 2048,
			HashSector(sector, sizeof(sector)));
	}

	iso.Close();
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char* argv[])
{
	CrashHandler::Install();
	Log::SetConsoleOutputLevel(LOGLEVEL_DEV);

	if (!ParseCommandLineArgs(argc, argv))
		return EXIT_FAILURE;

	if (s_disc_check)
		return RunDiscCheck();
#ifdef __EMSCRIPTEN__

	VU1Jit::ConfigureCache(s_vu1jit_cache_versions, 0, s_vu1jit_module_blocks);

	// Needs no BIOS, settings or VM: random programs on the VU1 interpreter and the JIT, on their own thread.
	if (!s_vu1jit_fuzz.empty())
	{
		Log::SetConsoleOutputLevel(LOGLEVEL_INFO); // the interpreter's dev warnings would flood the output
		return VU1Jit::RunFuzz(s_vu1jit_fuzz) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}
#endif

	if (s_wav_path.empty() && (s_wav_clock != WavOutput::Clock::Emulated || s_wav_period != 0))
	{
		Console.Error("-wav-realtime and -wav-period need -wav FILE.");
		return EXIT_FAILURE;
	}
	if (!s_wav_path.empty())
	{
		Error error;
		if (!WavOutput::Enable(s_wav_path, s_wav_clock, s_wav_period, &error))
		{
			Console.ErrorFmt("Failed to open '{}': {}", s_wav_path, error.GetDescription());
			return EXIT_FAILURE;
		}
	}

	if (!InitializeConfig())
	{
		Console.Error("Failed to initialize config.");
		return EXIT_FAILURE;
	}

	std::atomic<int> thread_ret;
	std::thread cputhread(CPUThreadMain, &thread_ret);
	cputhread.join();
	WavOutput::Finish();
	return thread_ret.load();
}

// -jit-compile-stats: compile time per vsync. "EE+IOP" is the EE and IOP JITs, which compile on the EE thread and so
// stall it; "all JITs" adds the VU1 JIT (the total the page's recorder reads, PerfCounters::jit_compile_ns).
namespace CompileStats
{
	struct Window
	{
		u64 cpu_ns = 0, all_ns = 0, cpu_max = 0, all_max = 0, cpu_max_frame = 0, all_max_frame = 0;
		u64 over5 = 0, over10 = 0, over16 = 0; // vsyncs with more than 5, 10 and 16.7 ms of EE and IOP compiling

		void Add(u64 frame, u64 cpu, u64 all)
		{
			cpu_ns += cpu;
			all_ns += all;
			if (cpu > cpu_max)
			{
				cpu_max = cpu;
				cpu_max_frame = frame;
			}
			if (all > all_max)
			{
				all_max = all;
				all_max_frame = frame;
			}
			over5 += cpu > 5000000;
			over10 += cpu > 10000000;
			over16 += cpu > 16666667;
		}

		void Print(const char* what, u64 from, u64 to) const
		{
			INFO_LOG("Headless: JIT compile, {} {}-{}: EE+IOP {:.1f} ms, worst vsync {:.1f} ms (vsync {}), vsyncs over "
					 "5/10/16.7 ms: {}/{}/{}; all JITs {:.1f} ms, worst vsync {:.1f} ms (vsync {})",
				what, from, to, cpu_ns / 1e6, cpu_max / 1e6, cpu_max_frame, over5, over10, over16, all_ns / 1e6,
				all_max / 1e6, all_max_frame);
		}
	};

	static Window s_window, s_total;
	static u64 s_last_cpu = 0, s_last_all = 0, s_window_start = 1;

	static void OnVsync(u64 frame)
	{
		const u64 cpu = EEJit::CompileNs() + IopJit::CompileNs();
		const u64 all = PerfCounters::jit_compile_ns.load(std::memory_order_relaxed);
		s_window.Add(frame, cpu - s_last_cpu, all - s_last_all);
		s_total.Add(frame, cpu - s_last_cpu, all - s_last_all);
		s_last_cpu = cpu;
		s_last_all = all;
		if (frame % s_compile_stats_every == 0)
		{
			s_window.Print("vsyncs", s_window_start, frame);
			s_window = {};
			s_window_start = frame + 1;
		}
	}
} // namespace CompileStats

static void PrintCompileStatsTotal()
{
	CompileStats::s_total.Print("whole run, vsyncs", 1, s_frames);
}

void Host::PumpMessagesOnCPUThread()
{
	// Called once per vsync.
	s_frames++;
	if (s_compile_stats_every)
		CompileStats::OnVsync(s_frames);
	WavOutput::OnVsync();
#ifdef __EMSCRIPTEN__
	VU1Jit::OnVsync();
#endif
	const u64 screenshot_frame = s_screenshot_frame ? s_screenshot_frame : (s_max_frames > 10 ? s_max_frames - 10 : 0);
	if (!s_screenshot_path.empty() && s_frames == screenshot_frame)
	{
		// Queued on the GS ring behind this vsync, so the next presented frame is captured however far the GS
		// thread lags behind. A flag polled by the GS thread caught a timing-dependent frame.
		MTGS::RunOnGSThread([path = s_screenshot_path]() { GSQueueSnapshot(path); });
	}
	if (!s_screenshot_dir.empty() &&
		((s_screenshot_every != 0 && (s_frames % s_screenshot_every) == 0) ||
			std::find(s_screenshot_frames.begin(), s_screenshot_frames.end(), s_frames) != s_screenshot_frames.end()))
	{
		MTGS::RunOnGSThread([path = Path::Combine(s_screenshot_dir, fmt::format("frame_{:06}.png", s_frames))]() {
			GSQueueSnapshot(path);
		});
	}

	// Scripted input, through the same InputManager key path as the browser's keyboard. The pad samples the
	// bindings when the game polls it, so a press here is seen from this vsync on.
	for (; s_next_input_event < s_input_events.size() && s_input_events[s_next_input_event].frame <= s_frames;
		 s_next_input_event++)
	{
		const InputEvent& ev = s_input_events[s_next_input_event];
		INFO_LOG("Headless: vsync {}: {} {}", s_frames, ev.pressed ? "press" : "release", s_key_names[ev.key_code - 1]);
		InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(ev.key_code), ev.pressed ? 1.0f : 0.0f);
	}
	if (s_max_frames != 0 && s_frames >= s_max_frames && VMManager::GetState() == VMState::Running)
		VMManager::SetState(VMState::Stopping);
}

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

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

