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
#include "pcsx2/SPU2/spu2.h"
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
	if (!bios_filename || !disc_path || s_fatal_error.load(std::memory_order_acquire))
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
// full speed the stream's buffer (BufferMS) steps up, so that time-stretching keeps the sound continuous.

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
// together they bound the frames one ReadFrames() call reads, and so its alloca() on the worklet's 64 KB stack.
static constexpr u32 AUDIO_MAX_QUANTUM = 512;
static constexpr double AUDIO_MIN_STEP = 0.5;
static constexpr double AUDIO_MAX_STEP = 2.0;
static constexpr u32 AUDIO_DRAIN_CHUNK = 1024;
static constexpr double AUDIO_HEARTBEAT_TIMEOUT = 0.1; // s without a render quantum: detach and drain
static constexpr double AUDIO_MAX_DRAIN = 1.0; // s drained at most at one vsync

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

// BufferMS for slow scenes (AUDIO-DESIGN.md 4.4): stretched sound is continuous once BufferMS x speed is about
// 50-60 ms. Each step is for speeds (percent) from its minimum up, which keeps that product at 50 ms or more below
// full speed. Changing it recreates the stream, so the speed must call for a bigger step for 1 s first, and for a
// smaller one, well beyond its boundary (AudioStepDownSpeed()), for 10 s: games swing between fast and slow
// scenes within seconds, and each switch can cost a short gap.
static constexpr std::array<u16, 5> AUDIO_BUFFER_STEPS = {50, 100, 200, 400, 800};
static constexpr std::array<float, 5> AUDIO_STEP_MIN_SPEED = {75.0f, 50.0f, 28.0f, 14.0f, 0.0f};
static constexpr double AUDIO_STEP_UP_AFTER = 1.0; // s
static constexpr double AUDIO_STEP_DOWN_AFTER = 10.0; // s
static u32 s_audio_step = 0; // CPU thread
static Common::Timer::Value s_audio_step_up_since = 0;
static Common::Timer::Value s_audio_step_down_since = 0;

// While a step recreates the stream (CPU thread): the old stream hands what it still holds to the new one, so the
// switch plays on instead of leaving a gap while the new buffer fills.
static bool s_audio_stepping = false;
static std::vector<AudioStream::SampleType> s_audio_carry; // interleaved stereo frames
static u32 s_audio_carry_rate = 0;

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
		if (s_audio_stepping && s_audio_stream == this)
		{
			// Everything buffered, refilling or not (the new stream decides when it plays), at most 1 s.
			const u32 frames = std::min(GetBufferedFramesRelaxed(), m_sample_rate);
			m_filling = false;
			s_audio_carry.resize(static_cast<size_t>(frames) * NUM_INPUT_CHANNELS);
			if (frames > 0)
				ReadFrames(s_audio_carry.data(), frames);
			s_audio_carry_rate = m_sample_rate;
		}
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
		// At most the rest of one quantum's work: microseconds, unless the audio thread is preempted.
		while (s_audio_in_callback.load(std::memory_order_seq_cst))
			std::this_thread::yield();
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

	std::unique_ptr<WebAudioStream> stream = std::make_unique<WebAudioStream>(sample_rate, params);
	stream->BaseInitialize(&StereoSampleReaderImpl, stretch_enabled);
	if (s_audio_stepping && s_audio_carry_rate == sample_rate)
	{
		// What the stream this one replaces still held, through the new stretcher like any SPU2 output.
		const u32 chunks = static_cast<u32>(s_audio_carry.size() / (CHUNK_SIZE * NUM_INPUT_CHANNELS));
		for (u32 i = 0; i < chunks; i++)
			stream->WriteChunk(&s_audio_carry[static_cast<size_t>(i) * CHUNK_SIZE * NUM_INPUT_CHANNELS]);
	}
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	stream->m_drain_time = Common::Timer::ConvertValueToSeconds(now);
	s_audio_stream = stream.get();
	s_audio_block[AUDIO_STREAM_RATE].store(sample_rate, std::memory_order_relaxed);
	s_audio_block[AUDIO_BUFFER_MS].store(params.buffer_ms, std::memory_order_relaxed);
	s_audio_block[AUDIO_STREAMS].fetch_add(1, std::memory_order_relaxed);
	// A stream that replaces another (a buffer step, PS1 mode) plays at once.
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
	si.SetIntValue("SPU2/Output", "BufferMS", AUDIO_BUFFER_STEPS[0]);
	if (s_audio_null)
		Console.WriteLn("pcsx2-wasm: -audio-null: no sound (PCSX2's Null audio backend)");
}

// RunVM(), before the boot's settings are applied: every boot starts at the full-speed buffer.
static void ResetAudioBufferStep()
{
	s_audio_step = 0;
	s_audio_step_up_since = 0;
	s_audio_step_down_since = 0;
	auto lock = Host::GetSettingsLock();
	s_settings_interface.SetIntValue("SPU2/Output", "BufferMS", AUDIO_BUFFER_STEPS[0]);
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

// The new BufferMS goes into the base layer, so a later settings reload (an ELF change applies core settings) keeps
// it, and straight to SPU2, which recreates the stream. VMManager::ApplySettings() would do the same, but also waits
// for the VU1 and GS threads and reloads every setting and input binding.
static void ApplyAudioBufferStep(u32 step, float speed)
{
	const u16 buffer_ms = AUDIO_BUFFER_STEPS[step];
	{
		auto lock = Host::GetSettingsLock();
		s_settings_interface.SetIntValue("SPU2/Output", "BufferMS", buffer_ms);
	}
	Pcsx2Config old_config;
	old_config.SPU2 = EmuConfig.SPU2;
	EmuConfig.SPU2.StreamParameters.buffer_ms = buffer_ms;
	s_audio_stepping = true;
	SPU2::CheckForConfigChanges(old_config);
	s_audio_stepping = false;
	s_audio_carry.clear();
	s_audio_carry_rate = 0;

	s_audio_step = step;
	s_audio_step_up_since = 0;
	s_audio_step_down_since = 0;
	s_audio_block[AUDIO_STEPS].fetch_add(1, std::memory_order_relaxed);
	Console.WriteLnFmt("pcsx2-wasm: audio buffer {} ms at {:.0f}% speed", buffer_ms, speed);
}

static void UpdateAudioBufferStep(Common::Timer::Value now)
{
	const float speed = s_speed.load(std::memory_order_relaxed);
	if (!s_audio_stream || speed <= 0.0f || VMManager::GetState() != VMState::Running)
	{
		s_audio_step_up_since = 0;
		s_audio_step_down_since = 0;
		return;
	}

	const auto held_for = [now](Common::Timer::Value& since) {
		if (since == 0)
			since = now;
		return Common::Timer::ConvertValueToSeconds(now - since);
	};
	const u32 slower = AudioStepForSpeed(speed, false);
	const u32 faster = AudioStepForSpeed(speed, true);
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

	// Software renderer into GSDeviceNull, sound through the page's Web Audio worklet, keyboard
	// only (the default port 1 keyboard bindings stay), memory card slot 1 only. Hotkeys are
	// cleared: most of them (pause menu, save states, screenshots) need host support that is not
	// there yet.
	si.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(GSRendererType::SW));
	ConfigureAudio(si);
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
	ResetAudioBufferStep();

	{
		auto lock = Host::GetSettingsLock();
		s_settings_interface.SetStringValue("Filenames", "BIOS", bios_name.c_str());
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
			UpdateAudioOutput();
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
	ParseAudioArguments(argc, argv);

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
