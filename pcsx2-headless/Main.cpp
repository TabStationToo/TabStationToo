// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Headless runner: boots the VM with no window, no audio and no input, and logs
// what the BIOS/game prints. Used to bring up the WebAssembly build under Node
// before there is a browser frontend. Also builds natively.

#include <atomic>
#include <chrono>
#include <cstdlib>
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
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"

static MemorySettingsInterface s_settings_interface;

static std::string s_app_root;
static std::string s_data_root;
static std::string s_bios_path;
static std::string s_boot_path;
static std::string s_screenshot_path;
static GSRendererType s_renderer = GSRendererType::SW;
static u64 s_max_frames = 600;
static u64 s_frames = 0;
static u64 s_screenshot_frame = 0; // 0 = 10 frames before the end

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

	// Headless: no renderer output, no audio, no input, no memory cards yet.
	si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(s_renderer));
	si.SetIntValue("EmuCore/GS", "ScreenshotFormat", static_cast<int>(GSScreenshotFormat::PNG));
	si.SetStringValue("SPU2/Output", "OutputModule", "nullout");
	si.SetBoolValue("InputSources", "SDL", false);
	si.SetBoolValue("InputSources", "XInput", false);
	Pad::ClearPortBindings(si, 0);
	si.ClearSection("Hotkeys");
	for (u32 i = 0; i < 2; i++)
		si.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);

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
		else if (arg == "-frames" && has_value)
			s_max_frames = StringUtil::FromChars<u64>(argv[++i]).value_or(s_max_frames);
		else if (arg == "-screenshot" && has_value)
			s_screenshot_path = argv[++i];
		else if (arg == "-screenshot-frame" && has_value)
			s_screenshot_frame = StringUtil::FromChars<u64>(argv[++i]).value_or(0);
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
			std::fprintf(stderr, "Usage: %s -approot <dir with resources/> -dataroot <dir> -bios <file> [-frames N]\n"
								 "         [-renderer sw|null] [-screenshot file.png [-screenshot-frame N]]\n"
								 "         [-set Section/Key=Value]... [disc image]\n", argv[0]);
			std::fprintf(stderr, "  With no disc image the BIOS boots to its menu. -frames 0 runs until stopped.\n");
			std::fprintf(stderr, "  The screenshot is requested at vsync N (default: 10 before the end) and shows the next frame.\n");
			std::fprintf(stderr, "  -set overrides a setting after the defaults, e.g. -set EmuCore/GS/extrathreads=4.\n");
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
	// Thread usage comes from per-thread CPU clocks. Emscripten has none (pthread_getcpuclockid returns ENOENT,
	// so every thread reads 0 CPU time): print n/a there and use a CPU profile to see EE- vs GS-bound instead.
	static const bool has_thread_clocks = Threading::ThreadHandle::GetForCallingThread().GetCPUTime() != 0;
	std::string threads;
	if (has_thread_clocks)
	{
		threads = fmt::format("EE {:.0f}%, GS {:.0f}%", PerformanceMetrics::GetCPUThreadUsage(),
			PerformanceMetrics::GetGSThreadUsage());
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
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
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
		params.fast_boot = false;

		const VMBootResult result = VMManager::Initialize(params);
		if (result == VMBootResult::StartupSuccess)
		{
			INFO_LOG("Headless: VM started, running {} frames", s_max_frames);
			VMManager::SetState(VMState::Running);
			while (VMManager::GetState() == VMState::Running)
				VMManager::Execute();
			INFO_LOG("Headless: stopped after {} frames", s_frames);
			GSJoinSnapshotThreads();
			VMManager::Shutdown(false);
			ret->store(EXIT_SUCCESS);
		}
		else
		{
			Console.ErrorFmt("Headless: VM failed to start (result {})", static_cast<int>(result));
		}
	}

	VMManager::Internal::CPUThreadShutdown();
}

int main(int argc, char* argv[])
{
	CrashHandler::Install();
	Log::SetConsoleOutputLevel(LOGLEVEL_DEV);

	if (!ParseCommandLineArgs(argc, argv))
		return EXIT_FAILURE;

	if (!InitializeConfig())
	{
		Console.Error("Failed to initialize config.");
		return EXIT_FAILURE;
	}

	std::atomic<int> thread_ret;
	std::thread cputhread(CPUThreadMain, &thread_ret);
	cputhread.join();
	return thread_ret.load();
}

void Host::PumpMessagesOnCPUThread()
{
	// Called once per vsync.
	s_frames++;
	const u64 screenshot_frame = s_screenshot_frame ? s_screenshot_frame : (s_max_frames > 10 ? s_max_frames - 10 : 0);
	if (!s_screenshot_path.empty() && s_frames == screenshot_frame)
	{
		// Queued on the GS ring behind this vsync, so the next presented frame is captured however far the GS
		// thread lags behind. A flag polled by the GS thread caught a timing-dependent frame.
		MTGS::RunOnGSThread([path = s_screenshot_path]() { GSQueueSnapshot(path); });
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

