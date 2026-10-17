// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "WavOutput.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/SettingsInterface.h"
#include "common/Threading.h"

#include "pcsx2/Host/AudioStream.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
	// 32-bit IEEE float WAV (format 3 with a fact chunk). The header is written as a placeholder and completed in
	// Close(); a run that dies before that leaves zero sizes, which tools/wasm/audio/wavcheck.py reads anyway.
	// Samples go to the file from a writer thread: under Node every write is proxied to the main JS thread and
	// waits for it, which must not happen on the CPU thread.
	class FloatWavFile
	{
	public:
		static constexpr u32 CHANNELS = AudioStream::NUM_INPUT_CHANNELS;

		bool Open(const char* path, Error* error)
		{
			m_file = FileSystem::OpenCFile(path, "wb", error);
			if (!m_file)
				return false;

			std::setvbuf(m_file, nullptr, _IOFBF, 1 << 20);
			if (!WriteHeader(0))
			{
				Error::SetStringView(error, "Failed to write the header.");
				std::fclose(m_file);
				m_file = nullptr;
				return false;
			}
			m_writer = std::thread(&FloatWavFile::WriterThread, this);
			return true;
		}

		u64 GetFrames() const { return m_frames; }

		/// One thread at a time (the device).
		void Write(const float* samples, u32 frames)
		{
			bool wake;
			{
				std::unique_lock lock(m_mutex);
				m_pending.insert(m_pending.end(), samples, samples + static_cast<size_t>(frames) * CHANNELS);
				wake = (m_pending.size() >= WRITE_BLOCK);
			}
			m_frames += frames;
			if (wake)
				m_cv.notify_one();
		}

		void Close(u32 sample_rate)
		{
			if (!m_file)
				return;

			{
				std::unique_lock lock(m_mutex);
				m_stop = true;
			}
			m_cv.notify_one();
			m_writer.join();

			std::fflush(m_file);
			if (FileSystem::FSeek64(m_file, 0, SEEK_SET) != 0 || !WriteHeader(sample_rate))
				Console.Error("Headless: failed to complete the WAV header");
			std::fclose(m_file);
			m_file = nullptr;
		}

	private:
		static constexpr size_t WRITE_BLOCK = 64 * 1024; // samples, 256 KB

		void WriterThread()
		{
			Threading::SetNameOfCurrentThread("WAV writer");
			std::vector<float> block;
			std::unique_lock lock(m_mutex);
			for (;;)
			{
				m_cv.wait(lock, [this]() { return m_stop || m_pending.size() >= WRITE_BLOCK; });
				block.swap(m_pending);
				const bool stop = m_stop;
				lock.unlock();
				if (!block.empty() && std::fwrite(block.data(), sizeof(float), block.size(), m_file) != block.size())
					Console.Error("Headless: WAV write failed");
				block.clear();
				lock.lock();
				if (stop && m_pending.empty())
					return;
			}
		}

		bool WriteHeader(u32 sample_rate)
		{
			// Sizes cap at 4 GB (about 3.1 hours of stereo float at 48 kHz).
			const u64 data_bytes64 = m_frames * CHANNELS * sizeof(float);
			const u32 data_bytes = static_cast<u32>(std::min<u64>(data_bytes64, 0xFFFFFFFFu - HEADER_SIZE));
			u8 header[HEADER_SIZE];
			u8* p = header;
			const auto put_tag = [&p](const char* tag) {
				std::memcpy(p, tag, 4);
				p += 4;
			};
			const auto put_u32 = [&p](u32 v) {
				for (int i = 0; i < 4; i++)
					*(p++) = static_cast<u8>(v >> (i * 8));
			};
			const auto put_u16 = [&p](u16 v) {
				*(p++) = static_cast<u8>(v);
				*(p++) = static_cast<u8>(v >> 8);
			};
			put_tag("RIFF");
			put_u32(HEADER_SIZE - 8 + data_bytes);
			put_tag("WAVE");
			put_tag("fmt ");
			put_u32(18);
			put_u16(3); // WAVE_FORMAT_IEEE_FLOAT
			put_u16(static_cast<u16>(CHANNELS));
			put_u32(sample_rate);
			put_u32(sample_rate * CHANNELS * static_cast<u32>(sizeof(float)));
			put_u16(static_cast<u16>(CHANNELS * sizeof(float)));
			put_u16(32);
			put_u16(0); // no extension
			put_tag("fact");
			put_u32(4);
			put_u32(static_cast<u32>(std::min<u64>(m_frames, 0xFFFFFFFFu)));
			put_tag("data");
			put_u32(data_bytes);
			return (std::fwrite(header, sizeof(header), 1, m_file) == 1);
		}

		static constexpr u32 HEADER_SIZE = 12 + 8 + 18 + 8 + 4 + 8;

		std::FILE* m_file = nullptr;
		u64 m_frames = 0;

		std::thread m_writer;
		std::mutex m_mutex;
		std::condition_variable m_cv;
		std::vector<float> m_pending;
		bool m_stop = false;
	};

	struct State
	{
		bool enabled = false;
		std::string path;
		WavOutput::Clock clock = WavOutput::Clock::Emulated;
		u32 period_frames = 0; // requested; 0 = PCSX2's output latency

		FloatWavFile file;
		u32 sample_rate = 0; // of the first stream; the file has one rate
		bool stretch = false;
		u32 device_period = 0;

		// Written by the device (the CPU thread for the emulated clock, the device thread for the realtime one) and
		// read in Finish() after every stream is gone.
		u64 vsyncs = 0;
		u64 periods = 0;
		u64 short_periods = 0;
		bool overrun_warned = false;
	};

	State s_state;

#ifdef __EMSCRIPTEN__
	class WavAudioStream final : public AudioStream
	{
	public:
		WavAudioStream(u32 sample_rate, const AudioStreamParameters& parameters);
		~WavAudioStream() override;

		void Start(bool stretch_enabled);
		void SetPaused(bool paused) override;

		/// Emulated clock: reads everything buffered.
		void Drain();

		static std::unique_ptr<AudioStream> Create(u32 sample_rate, const AudioStreamParameters& parameters,
			bool stretch_enabled, Error* error);

	private:
		void DeviceThread();
		void Deliver(u32 frames);

		std::vector<SampleType> m_device_buffer;
		std::thread m_device_thread;
		std::atomic_bool m_stop{false};
		std::atomic_bool m_device_paused{false};
	};

	WavAudioStream* s_emulated_stream = nullptr; // CPU thread only

	WavAudioStream::WavAudioStream(u32 sample_rate, const AudioStreamParameters& parameters)
		: AudioStream(sample_rate, parameters)
	{
	}

	WavAudioStream::~WavAudioStream()
	{
		if (m_device_thread.joinable())
		{
			m_stop.store(true, std::memory_order_release);
			m_device_thread.join();
		}
		if (s_emulated_stream == this)
		{
			// The mix up to the last sample SPU2 produced.
			Drain();
			s_emulated_stream = nullptr;
		}
	}

	std::unique_ptr<AudioStream> WavAudioStream::Create(u32 sample_rate, const AudioStreamParameters& parameters,
		bool stretch_enabled, Error* error)
	{
		if (s_state.sample_rate != 0 && s_state.sample_rate != sample_rate)
		{
			Error::SetStringFmt(error, "The WAV file is {} Hz and the console switched to {} Hz.", s_state.sample_rate,
				sample_rate);
			return {};
		}

		// Stereo only (what SPU2 produces): no surround expansion.
		AudioStreamParameters params = parameters;
		params.expansion_mode = AudioExpansionMode::Disabled;
		const bool emulated = (s_state.clock == WavOutput::Clock::Emulated);
		if (emulated)
		{
			// Nothing reads in real time, so nothing can be stretched. A vsync is 800-960 frames; a 200 ms ring keeps
			// SPU2 from dropping chunks between two reads however bursty its output is.
			stretch_enabled = false;
			params.buffer_ms = std::max<u16>(params.buffer_ms, 200);
		}

		std::unique_ptr<WavAudioStream> stream = std::make_unique<WavAudioStream>(sample_rate, params);
		const u32 period = s_state.period_frames ?
							   s_state.period_frames :
							   GetBufferSizeForMS(sample_rate, params.minimal_output_latency ? params.buffer_ms :
																							   params.output_latency_ms);
		s_state.sample_rate = sample_rate;
		s_state.stretch = stretch_enabled;
		s_state.device_period = emulated ? 0 : period;
		stream->m_device_buffer.resize(static_cast<size_t>(emulated ? 0 : period) * NUM_INPUT_CHANNELS);
		stream->Start(stretch_enabled);

		INFO_LOG("Headless: WAV output to '{}': {} Hz, {} clock, time-stretch {}{}", s_state.path, sample_rate,
			emulated ? "emulated" : "realtime", stretch_enabled ? "on" : "off",
			emulated ? std::string() : fmt::format(", device period {} frames", period));
		return stream;
	}

	void WavAudioStream::Start(bool stretch_enabled)
	{
		BaseInitialize(&StereoSampleReaderImpl, stretch_enabled);
		if (s_state.clock == WavOutput::Clock::Emulated)
			s_emulated_stream = this;
		else
			m_device_thread = std::thread(&WavAudioStream::DeviceThread, this);
	}

	void WavAudioStream::SetPaused(bool paused)
	{
		// A paused device stops asking for audio; the file just has no frames for that time.
		m_paused = paused;
		m_device_paused.store(paused, std::memory_order_relaxed);
	}

	void WavAudioStream::Drain()
	{
		const u32 frames = GetBufferedFramesRelaxed();
		if (frames == 0)
			return;

		// InternalWriteFrames() drops a chunk when it does not fit.
		if (frames + CHUNK_SIZE >= GetBufferSize() && !s_state.overrun_warned)
		{
			Console.Warning("Headless: WAV ring buffer full ({} frames): SPU2 chunks may have been dropped", frames);
			s_state.overrun_warned = true;
		}

		if (m_device_buffer.size() < static_cast<size_t>(frames) * NUM_INPUT_CHANNELS)
			m_device_buffer.resize(static_cast<size_t>(frames) * NUM_INPUT_CHANNELS);
		Deliver(frames);
	}

	void WavAudioStream::Deliver(u32 frames)
	{
		ReadFrames(m_device_buffer.data(), frames);
		s_state.file.Write(m_device_buffer.data(), frames);
	}

	void WavAudioStream::DeviceThread()
	{
		Threading::SetNameOfCurrentThread("WAV device");

		// One period per period of wall-clock time. A late wake-up reads the missed periods back to back, so the
		// file's length follows the wall clock.
		using Clock = std::chrono::steady_clock;
		const u32 period = s_state.device_period;
		const auto period_time = std::chrono::duration_cast<Clock::duration>(
			std::chrono::duration<double>(static_cast<double>(period) / static_cast<double>(GetSampleRate())));
		Clock::time_point next = Clock::now();
		while (!m_stop.load(std::memory_order_acquire))
		{
			next += period_time;
			std::this_thread::sleep_until(next);
			if (m_device_paused.load(std::memory_order_relaxed))
				continue;

			// Less than a period buffered: ReadFrames() stretches what there is over the period, or plays silence
			// until the buffer refills.
			if (GetBufferedFramesRelaxed() < period)
				s_state.short_periods++;
			Deliver(period);
			s_state.periods++;
		}
	}
#endif // __EMSCRIPTEN__
} // namespace

bool WavOutput::Enable(const std::string& path, Clock clock, u32 period_frames, Error* error)
{
#ifdef __EMSCRIPTEN__
	if (!s_state.file.Open(path.c_str(), error))
		return false;

	s_state.enabled = true;
	s_state.path = path;
	s_state.clock = clock;
	s_state.period_frames = period_frames ? AudioStream::GetAlignedBufferSize(period_frames) : 0;
	AudioStream::SetFrontendStreamFactory(&WavAudioStream::Create);
	return true;
#else
	Error::SetStringView(error, "-wav needs the WebAssembly build (AudioBackend::Frontend).");
	return false;
#endif
}

bool WavOutput::IsEnabled()
{
	return s_state.enabled;
}

void WavOutput::ApplySettings(SettingsInterface& si)
{
#ifdef __EMSCRIPTEN__
	if (s_state.enabled)
		si.SetStringValue("SPU2/Output", "Backend", AudioStream::GetBackendName(AudioBackend::Frontend));
#endif
}

void WavOutput::OnVsync()
{
#ifdef __EMSCRIPTEN__
	if (!s_emulated_stream)
		return;

	s_state.vsyncs++;
	s_emulated_stream->Drain();
#endif
}

void WavOutput::Finish()
{
	if (!s_state.enabled)
		return;

	const u32 rate = s_state.sample_rate ? s_state.sample_rate : 48000;
	const u64 frames = s_state.file.GetFrames();
	s_state.file.Close(rate);
	s_state.enabled = false;

	const double seconds = static_cast<double>(frames) / rate;
	if (s_state.clock == Clock::Emulated)
	{
		INFO_LOG("Headless: WAV '{}': {} frames ({:.3f} s) at {} Hz, emulated clock (the SPU2 mix before "
				 "time-stretching), {} vsyncs ({:.1f} frames per vsync)",
			s_state.path, frames, seconds, rate, s_state.vsyncs,
			s_state.vsyncs ? static_cast<double>(frames) / s_state.vsyncs : 0.0);
	}
	else
	{
		INFO_LOG("Headless: WAV '{}': {} frames ({:.3f} s) at {} Hz, realtime clock, time-stretch {}, {} periods of "
				 "{} frames, {} short (less than a period buffered)",
			s_state.path, frames, seconds, rate, s_state.stretch ? "on" : "off", s_state.periods,
			s_state.device_period, s_state.short_periods);
	}
}
