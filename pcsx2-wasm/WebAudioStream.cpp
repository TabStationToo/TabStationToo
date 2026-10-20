// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Audio output for the browser: a Wasm Audio Worklet (-sAUDIO_WORKLET) that calls
// AudioStream::ReadFrames on the browser's audio rendering thread. SPU2 writes the ring on the
// CPU thread as on desktop; neither side waits for the other, and the page's main thread only
// runs the Web Audio calls, which are all asynchronous.
//
// One AudioContext per sample rate (48 kHz for PS2 games, 44.1 kHz in PS1 mode), created on the
// first stream and kept for the page's lifetime: a context hosts a single worklet thread, whose
// stack has to stay allocated as long as the context exists. Streams attach to it and detach,
// and the context is suspended while none is attached.

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"

#include "pcsx2/Host/AudioStream.h"

#include <emscripten/console.h>
#include <emscripten/em_js.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <emscripten/webaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

namespace
{
	class WebAudioStream;

	enum class OutputState : int
	{
		Starting,
		Ready,
		Failed,
	};

	struct WebAudioOutput
	{
		u32 sample_rate = 0;
		std::atomic<OutputState> state{OutputState::Starting};

		// The attached stream, and whether the audio thread is using it right now (see Detach()).
		std::atomic<WebAudioStream*> source{nullptr};
		std::atomic<bool> in_callback{false};

		// The worklet thread's stack. Never freed: that thread runs on it as long as the context exists.
		void* stack = nullptr;

		// Main thread only.
		EMSCRIPTEN_WEBAUDIO_T context = 0;
		EMSCRIPTEN_WEBAUDIO_T node = 0;
	};

	class WebAudioStream final : public AudioStream
	{
	public:
		WebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
		~WebAudioStream() override;

		bool Open(bool stretch_enabled, Error* error);
		void SetPaused(bool paused) override;

		/// Audio thread: fills the planar output with num_frames frames per channel.
		void Render(float* planar, u32 num_frames, u32 num_channels);

	private:
		void Attach();
		void Detach();

		WebAudioOutput* m_output = nullptr;
		std::atomic<bool> m_output_paused{false};
	};
} // namespace

static constexpr const char* PROCESSOR_NAME = "tabstationtoo-audio";
static constexpr u32 OUTPUT_CHANNELS = 2;
static constexpr u32 MAX_RENDER_FRAMES = 128;
// The worklet's own stack, which also holds the frames Emscripten marshals for each callback.
static constexpr u32 WORKLET_STACK_SIZE = 64 * 1024;

// CPU thread only (SPU2 creates its streams there).
static std::array<WebAudioOutput, 2> s_outputs;

static void ProxyToMainThread(void (*func)(void*), WebAudioOutput* output)
{
	if (!emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), func, output))
		emscripten_err("pcsx2-wasm: could not reach the page's main thread for Web Audio");
}

// Resumes or suspends the context. Browsers start audio only once the page has had a click or a
// key press; until then resume() stays pending, so the next press on the page resumes it.
EM_JS_DEPS(pcsx2web_audio, "$emscriptenGetAudioObject");
// clang-format off
EM_JS(void, pcsx2web_audio_set_running, (EMSCRIPTEN_WEBAUDIO_T handle, bool running), {
	var context = emscriptenGetAudioObject(handle);
	if (!context)
		return;
	context.pcsx2webWanted = running;
	if (!running)
	{
		if (context.state === 'running')
			context.suspend();
		return;
	}
	if (context.state !== 'running')
		context.resume().catch(() => {});
	if (context.pcsx2webUnlock || context.state === 'running')
		return;
	var events = ['pointerdown', 'keydown', 'touchend'];
	var unlock = () => {
		if (!context.pcsx2webWanted || context.state === 'running')
		{
			events.forEach((type) => window.removeEventListener(type, unlock, true));
			context.pcsx2webUnlock = null;
			return;
		}
		context.resume().catch(() => {});
	};
	context.pcsx2webUnlock = unlock;
	events.forEach((type) => window.addEventListener(type, unlock, true));
});
// clang-format on

// Main thread: matches the context's state to whether a stream is attached. Posted after every
// attach and detach, and reads the current state, so the last one posted wins.
static void UpdateOutputOnMainThread(void* userdata)
{
	WebAudioOutput* const output = static_cast<WebAudioOutput*>(userdata);
	if (output->state.load(std::memory_order_acquire) != OutputState::Ready)
		return;

	pcsx2web_audio_set_running(output->context, output->source.load(std::memory_order_acquire) != nullptr);
}

// Audio thread.
static bool ProcessAudio(int num_inputs, const AudioSampleFrame* inputs, int num_outputs, AudioSampleFrame* outputs,
	int num_params, const AudioParamFrame* params, void* userdata)
{
	WebAudioOutput* const output = static_cast<WebAudioOutput*>(userdata);
	if (num_outputs < 1)
		return true;

	AudioSampleFrame& frame = outputs[0];
	const u32 num_frames = static_cast<u32>(frame.samplesPerChannel);
	const u32 num_channels = static_cast<u32>(frame.numberOfChannels);

	// Paired with Detach() and SetPaused(): either they see this flag and wait, or this sees
	// their change. Both sides use sequentially consistent operations for that.
	output->in_callback.store(true, std::memory_order_seq_cst);
	WebAudioStream* const stream = output->source.load(std::memory_order_seq_cst);
	if (stream)
		stream->Render(frame.data, num_frames, num_channels);
	else
		std::memset(frame.data, 0, num_frames * num_channels * sizeof(float));
	output->in_callback.store(false, std::memory_order_release);
	return true;
}

static void OnProcessorCreated(EMSCRIPTEN_WEBAUDIO_T context, bool success, void* userdata)
{
	WebAudioOutput* const output = static_cast<WebAudioOutput*>(userdata);
	if (!success)
	{
		emscripten_err("pcsx2-wasm: could not create the audio worklet processor; there is no sound");
		output->state.store(OutputState::Failed, std::memory_order_release);
		return;
	}

	int channel_counts[1] = {static_cast<int>(OUTPUT_CHANNELS)};
	EmscriptenAudioWorkletNodeCreateOptions options = {};
	options.numberOfInputs = 0;
	options.numberOfOutputs = 1;
	options.outputChannelCounts = channel_counts;
	output->node = emscripten_create_wasm_audio_worklet_node(context, PROCESSOR_NAME, &options, &ProcessAudio, output);
	emscripten_audio_node_connect(output->node, context, 0, 0);

	output->state.store(OutputState::Ready, std::memory_order_release);
	UpdateOutputOnMainThread(output);
}

static void OnWorkletStarted(EMSCRIPTEN_WEBAUDIO_T context, bool success, void* userdata)
{
	WebAudioOutput* const output = static_cast<WebAudioOutput*>(userdata);
	if (!success)
	{
		emscripten_err("pcsx2-wasm: could not start the audio worklet; there is no sound");
		output->state.store(OutputState::Failed, std::memory_order_release);
		return;
	}

	WebAudioWorkletProcessorCreateOptions options = {};
	options.name = PROCESSOR_NAME;
	emscripten_create_wasm_audio_worklet_processor_async(context, &options, &OnProcessorCreated, output);
}

static void StartOutputOnMainThread(void* userdata)
{
	WebAudioOutput* const output = static_cast<WebAudioOutput*>(userdata);

	EmscriptenWebAudioCreateAttributes attributes = {};
	attributes.latencyHint = "interactive";
	attributes.sampleRate = output->sample_rate;
	output->context = emscripten_create_audio_context(&attributes);
	if (!output->context)
	{
		emscripten_err("pcsx2-wasm: could not create an AudioContext; there is no sound");
		output->state.store(OutputState::Failed, std::memory_order_release);
		return;
	}

	emscripten_start_wasm_audio_worklet_thread_async(
		output->context, output->stack, WORKLET_STACK_SIZE, &OnWorkletStarted, output);
}

// CPU thread: the output for a sample rate, started on first use. Null if that rate failed.
static WebAudioOutput* GetOutput(u32 sample_rate, Error* error)
{
	for (WebAudioOutput& output : s_outputs)
	{
		if (output.sample_rate == sample_rate)
		{
			if (output.state.load(std::memory_order_acquire) == OutputState::Failed)
			{
				Error::SetStringView(error, "Web Audio could not be started in this browser.");
				return nullptr;
			}
			return &output;
		}
	}

	for (WebAudioOutput& output : s_outputs)
	{
		if (output.sample_rate == 0)
		{
			output.stack = std::aligned_alloc(16, WORKLET_STACK_SIZE);
			if (!output.stack)
			{
				Error::SetStringView(error, "Out of memory for the audio worklet.");
				return nullptr;
			}
			output.sample_rate = sample_rate;
			ProxyToMainThread(&StartOutputOnMainThread, &output);
			return &output;
		}
	}

	Error::SetStringFmt(error, "No Web Audio output left for a sample rate of {} Hz.", sample_rate);
	return nullptr;
}

WebAudioStream::WebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
	: AudioStream(sample_rate, parameters)
{
}

WebAudioStream::~WebAudioStream()
{
	Detach();
}

bool WebAudioStream::Open(bool stretch_enabled, Error* error)
{
	m_output = GetOutput(m_sample_rate, error);
	if (!m_output)
		return false;

	BaseInitialize(&StereoSampleReaderImpl, stretch_enabled);
	Attach();
	return true;
}

void WebAudioStream::Attach()
{
	WebAudioStream* expected = nullptr;
	if (!m_output->source.compare_exchange_strong(expected, this, std::memory_order_seq_cst))
	{
		// SPU2 destroys its stream before it creates the next one, so this does not happen.
		Console.Error("pcsx2-wasm: a Web Audio stream is already playing; this one stays silent.");
		m_output = nullptr;
		return;
	}

	ProxyToMainThread(&UpdateOutputOnMainThread, m_output);
}

void WebAudioStream::Detach()
{
	if (!m_output)
		return;

	m_output->source.store(nullptr, std::memory_order_seq_cst);
	// The audio thread may have picked this stream up just before; it is done within one callback.
	while (m_output->in_callback.load(std::memory_order_seq_cst))
		std::this_thread::yield();

	ProxyToMainThread(&UpdateOutputOnMainThread, m_output);
	m_output = nullptr;
}

void WebAudioStream::SetPaused(bool paused)
{
	AudioStream::SetPaused(paused);
	m_output_paused.store(paused, std::memory_order_seq_cst);

	// AudioStream pauses before it reallocates its buffers (SetStretchEnabled()), so once this
	// returns the audio thread must not be reading them.
	if (paused && m_output)
	{
		while (m_output->in_callback.load(std::memory_order_seq_cst))
			std::this_thread::yield();
	}
}

void WebAudioStream::Render(float* planar, u32 num_frames, u32 num_channels)
{
	if (m_output_paused.load(std::memory_order_seq_cst) || num_channels != OUTPUT_CHANNELS)
	{
		std::memset(planar, 0, num_frames * num_channels * sizeof(float));
		return;
	}

	alignas(16) SampleType interleaved[MAX_RENDER_FRAMES * OUTPUT_CHANNELS];
	float* const left = planar;
	float* const right = planar + num_frames;
	for (u32 done = 0; done < num_frames;)
	{
		const u32 count = std::min(num_frames - done, MAX_RENDER_FRAMES);
		ReadFrames(interleaved, count);
		for (u32 i = 0; i < count; i++)
		{
			left[done + i] = interleaved[i * 2];
			right[done + i] = interleaved[i * 2 + 1];
		}
		done += count;
	}
}

std::unique_ptr<AudioStream> AudioStream::CreateWebAudioStream(u32 sample_rate, const AudioStreamParameters& parameters,
	bool stretch_enabled, Error* error)
{
	// Stereo only: the worklet node has one stereo output, and there is no surround setting here.
	AudioStreamParameters stereo_parameters = parameters;
	stereo_parameters.expansion_mode = AudioExpansionMode::Disabled;

	std::unique_ptr<WebAudioStream> stream = std::make_unique<WebAudioStream>(sample_rate, stereo_parameters);
	if (!stream->Open(stretch_enabled, error))
		stream.reset();

	return stream;
}
