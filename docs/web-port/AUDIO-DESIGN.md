# Audio in the browser

*Design note for TabStationToo, written 2026-10-16. Phase 1: the choice of output path and the
design, backed by spikes (section 9; the spike programs live outside the repository). Phase 2,
2026-10-17: implemented as designed, with the changes and the measurements with the real emulator
in section 10. Where the two differ, section 10 and the code win.*

**Summary.** PCSX2's SPU2 mixes on the CPU thread (the EE thread) and hands 64-frame chunks to
`AudioStream`. An output backend's device callback pulls them back out with `ReadFrames()`. In the
browser that callback should be an Emscripten **Wasm Audio Worklet** (`-sAUDIO_WORKLET
-sWASM_WORKERS`): `ReadFrames()` then runs on the browser's audio rendering thread, 128 frames per
render quantum, in a backend the page frontend registers through the `AudioBackend::Frontend` hook.
The spikes ran PCSX2's own `AudioStream.cpp` and SoundTouch in headless Chrome 155, headless
Firefox 157 and the desktop app's browser pane (Chrome 152). There:
- At full speed the Wasm worklet had no dropout in any scenario: idle, the page thread blocked 30 ms
  out of every 100 ms or 50 ms out of every 500 ms, 12 busy threads, 640 MB of memory growth during
  playback, and a hidden tab.
- It needs no buffering beyond `AudioStream`'s own 50 ms and no extra thread.
- Loading the 10 MB emulator module into the audio thread costs 2–5 ms and about 2 MB.

The alternatives:
- **PCSX2's SDL backend can't be used.** With PCSX2's default settings it asks for a 1920-frame
  ScriptProcessorNode. That throws, and the boot hangs. With a legal size its callback runs on the
  page thread, and it drops out whenever that thread is busy: 24 dropouts per minute with a
  2048-frame node, 128–212 with 1024 frames.
- **A plain JS AudioWorklet** fed through a ring by a pump pthread also had no dropouts. It adds
  33–40 ms of latency and one thread, so it is the fallback.

Sync mode and output settings:
- Keep time-stretching, PCSX2's default, and turn on QuickSeek. Stretching runs on the EE thread,
  inside `WriteChunk()`. With PCSX2's settings it costs 20–24 ms per second at 70–100 % speed; with
  QuickSeek, 5–6 ms.
- Below full speed, stretching needs a bigger buffer. With PCSX2's 50 ms (`BufferMS`), 3 % of the
  output is silence at 50 % speed, 42 % at 30 % and 73 % at 15 %. The sound is continuous again when
  `BufferMS` × speed is at least about 50–60 ms: 100 ms at 50 %, 200 ms at 30 %, 400 ms at 15 % and
  800 ms at 10 %.
- So the frontend steps `BufferMS` up when the emulator runs slowly and back down at full speed.
  Each step recreates the stream, which costs one gap: 50 ms at full speed, 0.6 s at 15 %.
- Without stretching, the sound crackles at every speed: 59–67 gaps per second at 15–50 % speed.
- The AudioContext runs at 48 kHz, and the browser resamples to the device.
- The page creates the AudioContext inside the Boot gesture.
- Volume and mute are a GainNode after the worklet, remembered in `localStorage`.

Contents: 1 problem · 2 options and evidence · 3 measurements · 4 design · 5 settings · 6 page
behaviour · 7 risks · 8 what is not decided · 9 files and how to test.

## 1. The problem

### 1.1 How PCSX2 makes sound

- **SPU2 has no thread of its own.** The IOP's counters call `SPU2async()` (`IopCounters.cpp`,
  `psxRcntUpdate`). That runs `TimeUpdate()` (`SPU2/spu2sys.cpp`), then `spu2Mix()`
  (`SPU2/Mixer.cpp`), then `spu2Output()` (`SPU2/spu2.cpp`). Every 64 stereo frames,
  `spu2Output()` calls `AudioStream::WriteChunk()`. All of this runs on the CPU thread that runs the
  EE and the IOP: 750 chunks per emulated second at 48 kHz, or 44.1 kHz in PS1 mode
  (`SPU2::GetConsoleSampleRate`).
- **`AudioStream` is a float ring of frames** (`pcsx2/Host/AudioStream.cpp`).
  - The writer is `WriteChunk()` on the CPU thread. If enabled, FreeSurround expansion
    (`ExpandAllocate`) and SoundTouch time-stretching (`StretchWriteBlock`, `UpdateStretchTempo`)
    run inside `WriteChunk()`, so they cost EE time.
  - The reader is `ReadFrames()` (protected). A backend's device callback calls it: the OS audio
    thread for Cubeb, SDL's callback for SDL. This is the pull model.
  - `ReadFrames()` handles underruns. After one, it plays silence until 1/32 of the buffer is filled
    again (1/400 without stretching). A read that finds only part of what it needs spreads those
    frames across the whole request, a crude resampler.
  - It applies `m_volume`.
- **The ring has exactly one reader.** The writer moves the read position itself in
  `StretchOverrun()`. So JavaScript cannot read `m_buffer` directly (PLAN §3.6), and only one
  thread at a time may call `ReadFrames()`.
- **Sizes.** `BufferMS` (default 50) sets the target fill, 2432 frames at 48 kHz. With stretching
  the ring is 16 times that: 38,400 frames, 800 ms.
- **Pausing.** `VMManager` calls `SPU2::SetOutputPaused()`, which calls `AudioStream::SetPaused()`
  (virtual). `SetStretchEnabled()` pauses the stream before it reallocates the ring. So when
  `SetPaused(true)` returns, no `ReadFrames()` may be running, as with Cubeb, which stops its
  stream.
- **Backends.** The backends are Null, Cubeb and SDL. Cubeb has no Emscripten backend. Both
  frontends set Null today (`pcsx2-wasm/Main.cpp`, `pcsx2-headless/Main.cpp`).
  - The headless runner's WAV output in this worktree (`pcsx2-headless/WavOutput.cpp`) adds
    `AudioBackend::Frontend` and `AudioStream::SetFrontendStreamFactory()`, guarded by
    `__EMSCRIPTEN__`. With them, a frontend supplies its own `AudioStream` subclass that calls
    `ReadFrames()`.
  - The page's backend uses the same hook. No further change to `pcsx2/` is needed.

### 1.2 What the browser allows

- **Thread.** Web Audio objects exist only on the page thread (`window`). There are two kinds of
  output callback:
  - `ScriptProcessorNode`. It is deprecated, and its `onaudioprocess` runs on the page thread.
  - `AudioWorkletProcessor.process()`. It runs on the audio rendering thread, 128 frames per render
    quantum.
- **Thread priority.** Chrome asks the OS for real-time priority for its audio threads. On this
  Linux machine it did not get it: in Chrome 155 the "Realtime AudioWorklet" thread is a normal
  thread (`SCHED_OTHER`, nice 0), like the emulator's workers.
- **Autoplay.** A context created without a user activation starts suspended, and its `resume()`
  stays pending until the user acts (§3.8).
- **The page thread** is sometimes busy for 30–50 ms (memory-card copies, 2D blits). It must never
  block.
- **Shared memory.** The page is cross-origin isolated. A shared `WebAssembly.Memory` can be passed
  to an AudioWorklet, and `Atomics.notify` works there. `Atomics.wait` does not.
- **Memory growth.** Growing a shared memory gives `memory.buffer` a new, longer
  `SharedArrayBuffer`. The old one is never detached and keeps aliasing the first bytes. So a view
  of a region that existed when the view was made stays valid.
- **Sample rate.** The PS2 outputs 48 kHz. An AudioContext defaults to the device's rate, which is
  44.1 kHz on the test machine (PulseAudio, USB dock). `new AudioContext({sampleRate: 48000})` makes
  the browser resample to the device, outside our threads.

## 2. Options and evidence

Each spike runs PCSX2's real `AudioStream.cpp`, from the base tree, built with the app's flags. A
fake SPU2 on the `main()` pthread feeds it (section 3). Only the consumer side differs.

### (a) PCSX2's SDL backend over SDL3's Emscripten driver: rejected

From the source (SDL 3.4.18, `src/audio/emscripten/SDL_emscriptenaudio.c` and `SDL_audio.c`, as
built into `deps-wasm/lib/libSDL3.a`):

- **Output is a `ScriptProcessorNode`**, and the driver sets `ProvidesOwnCallbackThread`. So the
  node's `onaudioprocess` runs on the page thread, and from there:
  1. It calls `SDL_PlaybackAudioThreadIterate`.
  2. That takes the device mutex (a pthread mutex on the page thread).
  3. Then it runs `SDLAudioStream::AudioCallback`, which calls `AudioStream::ReadFrames()`.
  4. Finally JS copies the samples into the `AudioBuffer`.
- **SDL opens its own `new AudioContext()`** at the device's rate. If that is not 48 kHz, SDL's
  resampler also runs on the page thread.
- **While autoplay holds the context, SDL feeds a silence timer** (`setInterval`) on the page thread.
- **Buffer size.** SDL uses `device->sample_frames = SDL_GetDefaultSampleFramesFromFreq() × 2`.
  PCSX2 sets `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES` to `GetBufferSizeForMS(48000,
  OutputLatencyMS = 20)`, which is 960 frames. That gives a 1920-frame node, and a
  ScriptProcessorNode's size must be a power of two from 256 to 16384.

By experiment, with the real `SDLAudioStream.cpp` and `libSDL3.a`:

- **With PCSX2's default settings, the open fails and the boot hangs.**
  `createScriptProcessor(1920)` throws `IndexSizeError` on the page thread, inside SDL's proxied
  open. The calling pthread never returned from `AudioStream::CreateStream()`; the run was stopped
  after 24 s. In the emulator, the boot would hang in `SPU2::Open()`.
  "Minimal output latency" asks for 2 × 2400 frames and fails the same way.
- **Only some `OutputLatencyMS` values work:** those whose 64-aligned frame count, doubled, is a
  power of two. 5, 10, 21 and 42 ms give 512, 1024, 2048 and 4096-frame nodes.
- **With a legal size, it drops out when the page thread is busy** (§3.2):
  - 2048-frame node: 24 dropouts per minute with the page thread blocked 50 ms every 500 ms.
  - 1024-frame node: 44 per minute with the page idle, and 128–212 per minute when it is blocked.
  - The callback interval was 42.8 ms (p50) for 2048 frames. Its p99 was 57 ms with the page idle
    and 82–101 ms with the page blocked.
- **A pure-JS ScriptProcessorNode does the same:** a sine made in JS, with no SDL and no wasm.
  1024 frames had 6 dropouts in 10 s with an idle page; 2048 and 4096 had none.
- **Verdict.** It is tied to the page thread, which stalls for 30–50 ms. Surviving those stalls
  needs nodes of at least 2048 frames, and the node's double buffering then adds 43–85 ms to
  `AudioStream`'s 50 ms. The API is deprecated.

### (b) A plain JS AudioWorklet reading a ring, fed by a pump thread: works; the fallback

The design:
- **The ring** is static data in the wasm heap, so it never moves and is never freed. It has a
  64-word header and 8192 interleaved stereo float frames. A write counter (pump) and a read counter
  (worklet) are monotonic `u32`s on separate cache lines.
- **The pump thread** is a pthread. It waits on a doorbell word with `emscripten_futex_wait`, with a
  5 ms timeout. When it wakes, it calls `ReadFrames(target − fill)` and copies the frames into the
  ring.
- **The worklet** (`ring-worklet.js`, 60 lines of plain JS, no wasm in that scope) copies 128
  frames per quantum. Below the low-water mark it rings the doorbell: `Atomics.add`, then
  `Atomics.notify`.
- **Its views** are made from `memory.buffer` once the ring exists, so memory growth cannot break
  them.

Evidence (§3):
- No dropouts in any pinned scenario: page thread blocked, 12 busy threads, 640 MB of memory
  growth. Firefox 157 runs it too.
- The pump woke every 5.1 ms (p50); p99 was 7.5 ms and the maximum 11 ms, or 20 ms with 12 busy
  threads.
- The ring must hold more than one device burst. On this Linux machine Chrome renders 20 ms of
  quanta back to back (the tap saw 20–28 ms between calls). With a 1024-frame target the worklet
  ran short of frames 7 times in 6 s; with 2048 frames, never.
- Doorbell-only waking (50 ms timeout) dropped out in most scenarios: a median of one dropout per
  15 s run, and 16 in the worst run. Its refill came only every other burst.
- Costs:
  - The ring adds 33–40 ms of latency: its mean fill was 40 ms with the 5 ms timeout and 33 ms with
    the doorbell alone.
  - It is one more pthread, using 0.4–0.8 % of a core.

Strengths:
- It needs no new build flags and works in every build (Devel, ASan).
- No PCSX2 code runs on the audio thread.
- The pump thread can drain the stream by itself when the browser stops pulling.

### (c) Emscripten Wasm Audio Worklet calling `ReadFrames()`: chosen

- **It links.** `-sAUDIO_WORKLET=1 -sWASM_WORKERS=1` links with the app's flags (`-pthread
  -sPROXY_TO_PTHREAD -sSUPPORT_LONGJMP=wasm`, growth to 2 GB) in Emscripten 6.0.12, with no
  `signature mismatch`. The JS grows by 8.7 KB.
- **The app's file-system options still work** in the worklet scope: `--preload-file`,
  `FORCE_FILESYSTEM` and `-lidbfs.js` (variant `wasmwl-fs`). With `-sAUDIO_WORKLET`, the module's
  own JS is evaluated in the `AudioWorkletGlobalScope`.
- **How it starts** (`src/lib/libwebaudio.js`, `src/audio_worklet.js`):
  1. On the page thread, `emscripten_start_wasm_audio_worklet_thread_async()` loads the module's JS
     with `audioWorklet.addModule()`. It then posts `wasmModule`, `wasmMemory` and a stack that we
     allocate.
  2. The scope instantiates the module synchronously as a Wasm Worker, with its own stack and TLS.
  3. `emscripten_create_wasm_audio_worklet_processor_async()` and
     `emscripten_create_wasm_audio_worklet_node()` register a processor. Its C callback receives
     each render quantum.
- **Start-up costs:**
  - From `addModule` to a connected node: 7–20 ms in Chrome, 123 ms in Firefox 157.
  - Instantiating the real `pcsx2-wasm.wasm` (9,982,603 bytes, a prebuilt copy) inside an
    AudioWorkletGlobalScope, with stub imports and a shared memory of the app's size: 2–5 ms and
    about 2 MB more renderer memory.
  - The page's first instance of the same module took 143–178 ms and 218 MB, because it initialises
    the static data. The worklet's instance shares that memory and does not.
- **Results:** no dropouts in any pinned scenario (§3.2), in Firefox, in the browser pane or with
  the tab hidden (§3.6–3.7), with no ring and no pump.
- **Memory growth is handled.** Emscripten's processor rebuilds its output views when `HEAPF32`'s
  buffer changes, and growing 640 MB during playback caused no glitch.
- **The audio thread must not block.** `Atomics.wait` is not allowed there, and Emscripten's futex
  wait spins in a worklet (`workerSupportsFutexWait` in `libwasm_worker.js`). `ReadFrames()` takes
  no lock and allocates nothing; its underrun path uses at most 1 KB of `alloca`. So this holds
  today, and it must stay that way.
- **Cost:** the audio thread used about 0.3 % of a core more than with the JS ring (1.6 % against
  1.3 %, with the measuring tap included). No pump thread is needed.
- **When the browser stops pulling** (autoplay, a suspended or interrupted context), nothing reads
  the stream. With stretching, the stream filled to about 200 ms while the context was suspended.
  After `resume()`, that backlog played and then underran: 3 dropouts.
  - The design uses a heartbeat instead (§4.3): the CPU thread detaches the worklet and drains the
    stream at the real-time rate while the heartbeat stops.
  - With it, the stream was back at its normal fill (3505 frames) on resume, and no quantum ran
    short.

### Recommendation

Use (c).
- It keeps PCSX2's own model: the device callback pulls with `ReadFrames()`, as with Cubeb on the
  desktop.
- It has the least latency: only `AudioStream`'s 50 ms plus the browser's.
- It adds no thread, and needs no tuning for each device's burst size.
- It survives the page thread's stalls and memory growth.

(b) is the fallback if a browser cannot start a Wasm Audio Worklet, or if PCSX2 code on the audio
thread turns out to be a problem. The page-side code is the same; only the consumer changes. (a)
is not usable.

## 3. Measurements

**Setup.**
- Machine: Ryzen 7 PRO 7840U (8 cores, 16 threads), 30 GB RAM, PulseAudio with a USB-C dock sink at
  44.1 kHz.
- Browsers:
  - Chrome 155 headless (`--headless=new --mute-audio`, throwaway profile, driven over DevTools).
  - Firefox 157 headless, with `media.volume_scale` 0 and console output to stdout.
- The page was served by `tools/wasm/serve.py` on 127.0.0.1:8150.
- Nothing was audible. Every output went through a measuring tap, an AudioWorklet that analyses its
  input and writes zeros, and the browsers were muted as well.
- Other agents' builds and emulators loaded the machine (load average 13–33). The comparison runs
  were pinned with `taskset` to CPUs 12–15 (physical cores 6 and 7) at normal priority, 3 rounds
  with the backends interleaved. Functional runs used nice 19 on CPUs 8–11.

**The fake SPU2.**
- `main()`, a pthread under `PROXY_TO_PTHREAD`, plays the EE thread.
- It writes a 440 Hz sine at amplitude 0.5 through `AudioStream::WriteChunk()` in 64-frame chunks,
  paced to an emulation speed.
- At full speed it is frame-limited: each emulated frame's 800 samples come in the first half of the
  16.7 ms frame, as a frame-limited EE produces them.
- The slow-speed runs vary each frame's duration by ±30 %.
- `WriteChunk()` is timed on that thread: the "EE cost".

**The stream.** It is PCSX2's `AudioStream` with the default `AudioStreamParameters`, unless a
table says otherwise: `BufferMS` 50, stretching on, QuickSeek off. It was built with `-O3 -msimd128
-msse4.1 -pthread`, against prebuilt `libpcsx2-soundtouch.a`, `libfreesurround.a`, `libcommon.a`
and `libfmt.a` copied from `pcsx2-web-wasm/build-wasm`.

**Metrics.**
- **Dropout:** a discontinuity in the sine, where the second difference exceeds 0.02 (a clean sine
  stays under 0.002 at 44.1–48 kHz). Discontinuities within 50 ms count as one dropout.
- **Gaps per second:** runs of at least 32 zero samples, per second.
- **Silent %:** the share of zero samples. 0.08–0.17 % is the tap's floor: it appears in every
  clean run.
- **CPU:** per thread, from `/proc/<pid>/task/*/schedstat`.
- **Fills:** sampled every 10 ms.
- Each run is 15 s measured after a 2 s warm-up.

### 3.1 Callback, latency, CPU (Chrome 155, page idle, full speed)

| | (a) SDL, 2048-frame node | (b) JS ring + pump | (c) Wasm worklet |
|---|---|---|---|
| Who calls `ReadFrames()` | page thread, every 42.8 ms (p99 57) | pump pthread, every 5.1 ms (p99 7.5, max 11) | audio thread, every 128 frames, in 20 ms bursts |
| `AudioStream` fill, mean | 52 ms | 51 ms | 50 ms |
| Extra buffering | node double buffering, 43–85 ms | ring, 40 ms mean | none |
| Browser latency (`baseLatency` + `outputLatency`) | 11.6 + 56 ms | 11.6 + 56 ms | 11.6 + 56 ms |
| Estimated total | 160–200 ms | about 160 ms | about 120 ms |
| Page thread CPU | 1.8–2.2 % (+0.3 ms per call) | 1.1 % | 1.3 % |
| Audio rendering thread CPU (with the tap) | 1.0–1.2 % | 1.3 % | 1.6 % |
| Extra pthread | none | 0.8 % of a core | none |

- Every variant also costs the browser's fixed share while the context runs: the
  AudioOutputDevice thread (0.8–0.9 %) and the audio service process (1.6–2.1 %).
- The page-thread figure includes the test page's own 10 ms timer.
- The browser latency figures are what Chrome reports for this machine's sink, and they depend on
  the device.

### 3.2 Dropouts per minute (Chrome 155, pinned, median of 3 rounds, worst in parentheses)

PCSX2's default stretching at full speed:

| Scenario | (a) SDL 1024 | (a) SDL 2048 | (b) ring, 5 ms timeout | (b) ring, doorbell only | (c) Wasm worklet |
|---|---|---|---|---|---|
| Page idle | 44 (52) | 4 (8) | 0 (0) | 4 (24) | 0 (0) |
| Page blocked 30 ms every 100 ms | 212 (220) | 4 (12) | 0 (0) | 4 (64) | 0 (0) |
| Page blocked 50 ms every 500 ms | 128 (148) | 24 (40) | 0 (0) | 4 (8) | 0 (0) |
| 12 busy threads in the page | — | 8 (12) | 0 (0) | 0 (4) | 0 (0) |
| 640 MB memory growth mid-run (from another thread) | — | 8 (16) (not at the growth) | 0 (0) | — | 0 (0) |

- One dropout in a 15 s run is 4 per minute.
- In the growth runs, the 640 MB were allocated on a separate thread. Allocating them on the
  producer thread stalled it for 650 ms and silenced every backend for 1.27 s, an artefact of the
  test.
- (b) with the 5 ms timeout and (c) had no dropout in any of their 15 runs: 225 s each. The machine's
  load average during these runs was 11–27.

### 3.3 Time-stretching on the EE thread

**SoundTouch alone** (`st-bench.cpp`), PCSX2's settings (sequence 30 ms, seek window 20 ms, overlap
10 ms, no anti-alias filter). Thread CPU time for one `StretchWriteBlock` step: `putSamples(64)`,
then `receiveSamples` until it returns nothing. The input is a two-partial tone with noise.

| Tempo | wasm (Node 24), µs per chunk | native x86-64 (g++ -O3), µs per chunk | wasm with QuickSeek |
|---|---|---|---|
| 1.0 | 32.4 | 17.8 | 9.6 |
| 0.5 | 53.6 | 32.5 | 11.8 |
| 0.15 | 159 | 78.8 | 25.2 |

**In the stream, measured in the browser** (EE ms per second of real time; the fake EE writes
750 × speed chunks per second):

| Speed | No stretching | Stretching (PCSX2 default) | Stretching + QuickSeek |
|---|---|---|---|
| 100 % | 0.6 | 20.0 | 5.3 |
| 90 % | 1.3 | 20.9 | 6.2 |
| 70 % | 1.1 | 23.9 | 5.8 |
| 50 % | 0.8 | 17.5 | 5.6 |
| 30 % | 0.4 | 11.8 | 3.2 |
| 15 % | 0.3 | 6.8 | 2.3 |

Medians of 3 rounds; frame times vary by ±30 %. With frame-limited production at full speed (the
dropout runs), stretching took 18–25 ms per second.

- With PCSX2's settings, stretching takes 2.0–2.4 % of the EE thread at 70–100 % speed. That is
  most of what SPU2's mixing costs itself (about 3 %).
- QuickSeek cuts it to 0.5–0.6 %.
- In wasm, SoundTouch takes its plain C++ path, because its x86 SSE routines are compiled out
  (`STTypes.h`). That path is 1.8 times slower than the native SSE build without QuickSeek, and 1.5
  times with it.
- Turning the SSE routines on in wasm (a scratch copy of SoundTouch, with `STTypes.h` and
  `cpu_detect_x86.cpp` patched to run them through Emscripten's SSE emulation) gained under 2 %
  without QuickSeek (31.9 µs per chunk at tempo 1.0) and 24 % with it (7.3 µs), probably because
  clang already vectorises the C++ path. That is not worth a patch to `3rdparty/`.

### 3.4 Below full speed

Stretching on and off, frame times ±30 %, Chrome 155:

| Speed | Stretching: silent % / gaps per s | + QuickSeek | No stretching: silent % / gaps per s |
|---|---|---|---|
| 100 % | 0.14 / 0 | 0.14 / 0 | 0.70 / 1.3 (224 dropouts/min) |
| 90 % | 0.12 / 0 | 0.12 / 0 | 7.8 / 15 |
| 70 % | 0.08 / 0 (0–3 dropouts per run) | 0.09 / 0 | 26 / 39 |
| 50 % | 3.2 / 1.3 | 3.1 / 1.3 | 44 / 59 |
| 30 % | 42 / 8.8 | 42 / 8.5 | 66 / 67 |
| 15 % | 73 / 6.7 | 73 / 6.7 | 80 / 65 |

Medians of 3 rounds.

- **No stretching crackles at every speed.** Even at 100 %, uneven frames under- and overrun the
  50 ms ring. Below that it inserts a gap every 15–25 ms.
- **Stretching is continuous at 90–100 %, and almost so at 70 %.** At 50 % it has a short gap about
  once a second.
  Below that, with PCSX2's 50 ms buffer, it does not keep up. The output comes in bursts 7–9 times
  a second, and after each underrun `ReadFrames()` waits until 1/32 of the ring (25 ms of output)
  is buffered again. With a bigger buffer the stretcher keeps up (below).
- The consumer makes no difference here. At 15 %, the JS ring and the Wasm worklet both gave 67–73 %
  silence in 6.8 gaps per second, with one large read or with 128-frame reads.
- **The stream's nominal rate is no fix.** `SetNominalRate()` set to the speed (0.15) made the
  stretcher aim at slow motion, and the output was 98 % silent. The idea is dropped.

**A bigger `BufferMS` fixes it.** Stretching with QuickSeek, frame times ±30 %, Chrome 155, pinned,
median of 2 rounds. Each cell: silent % / dropouts per minute / mean `AudioStream` fill.

| Speed | `BufferMS` 50 | 100 | 200 | 400 | 800 |
|---|---|---|---|---|---|
| 100 % | 0.13 / 0 / 53 ms | 0.15 / 0 / 105 ms | 0.16 / 0 / 205 ms | 0.16 / 0 / 417 ms | |
| 50 % | 2.2 / 58 / 35 ms | 0.08 / 0 / 71 ms | 0.08 / 0 / 142 ms | 0.08 / 0 / 285 ms | |
| 30 % | 41 / 618 / 18 ms | 1.2 / 20 / 55 ms | 0.08 / 0 / 110 ms | 0.08 / 0 / 220 ms | |
| 15 % | 73 / 410 / 17 ms | 63 / 558 / 26 ms | 43 / 304 / 48 ms | 0.08 / 0 / 152 ms | |
| 10 % (1 run) | | | | 46 / 120 / 102 ms | 0.08 / 0 / 250 ms |

- The rule: the sound is continuous when `BufferMS` × speed ≥ about 50–60 ms.
- The cost:
  - Latency grows with the buffer. At full speed the fill follows `BufferMS`, so only the 50 ms step
    belongs there.
  - At 15 % speed with 400 ms, the fill is 152 ms. Picture and sound are then about 150 ms apart in
    wall-clock time, in a scene that is already in slow motion.
  - The EE cost stays at 3–6 ms per second with QuickSeek.
  - The ring is 16 × `BufferMS`: 2.4 MB at 400 ms, 4.9 MB at 800 ms.
- **Changing the buffer while playing** means recreating the stream, as `SPU2::CheckForConfigChanges()`
  does when `StreamParameters` change. With the gated worklet (§4.2), the new stream attached
  12–22 ms after the old one went away.
  - At full speed, the switch caused one 48 ms gap.
  - At 15 % speed, switching from 50 to 400 ms caused one 0.6 s gap while the new stream filled.
    After it, there was no dropout in the remaining 8.5 s; before it, the run had a dropout every
    0.14 s.

### 3.5 Memory growth and suspended contexts

- **Memory growth.** The heap grew from 64 MB to 684 MB in 10 allocations of 64 MB, on a thread
  other than the producer, while audio played.
  - (b) and (c) had no dropout and no short quantum.
  - The ring's views were made before the growth and stayed valid.
  - Emscripten's worklet rebuilt its output views.
- **Suspended context**, 3 s, then `resume()` (§2 (c)):

| | Fill while suspended | Fill after resume | Dropouts after resume |
|---|---|---|---|
| Nobody drains (as Cubeb would behave) | grows in steps to 9728 frames (203 ms) | backlog plays, then underruns to 0 | 3 |
| Heartbeat + drain on the EE thread (§4.3) | stays at 1800–3200 | 3505, normal | 0 |

The gated, page-lifetime worklet of §4.2 behaved the same: it detached 113 ms after `suspend()`,
drained 138,127 frames in 3 s and attached again 3 ms after `resume()`.

### 3.6 Firefox 157 and the browser pane (Chrome 152)

**Firefox 157**, headless, pinned like Chrome, 2 rounds of 15 s. Dropouts per minute, median (worst):

| | (a) SDL 2048 | (b) ring, 5 ms timeout | (c) Wasm worklet |
|---|---|---|---|
| Page idle | 4 (4) | 0 (0) | 0 (0) |
| Page blocked 50 ms every 500 ms | 14 (16) | 0 (0) | 0 (0) |
| Worklet start | — | 63 ms (`addModule`) | 123 ms |
| `outputLatency` | 46 ms | 48 ms | 59 ms |

Firefox reports `baseLatency` as 0.

**The desktop app's browser pane** (Chrome 152.0.7977.149, Electron). Each run was started by a click
on the page; the tap kept it silent. One 10 s run each:

| | Dropouts | Short quanta | Notes |
|---|---|---|---|
| (c) Wasm worklet, page idle | 0 | 0 | start 15.6 ms, fill 51 ms |
| (c) Wasm worklet, page blocked 50 ms every 500 ms | 0 | 0 | start 14 ms |
| (b) ring, page idle | 0 | 0 | pump wake p50 5.1 ms, max 10.4 ms |
| (a) SDL 2048, page blocked 50 ms every 500 ms | 4 | — | callback interval p99 87.5 ms, max 105 ms |

### 3.7 Tab hidden

- Measured with a second tab brought to the front for 5 s.
- The AudioContext kept running: its clock advanced 5.0 s while 5.2 s of wall time passed.
- Atomics-based waits in pthreads are not timers, so background throttling does not touch them.
- With each backend playing (2 rounds of 15 s, hidden from 4 s to 10 s), (b) and (c) had no
  dropout. (a) had one per run, after the tab came back (13.1 s and 13.8 s).

### 3.8 Autoplay (Chrome 155, default policy, trusted input through DevTools)

| Step | Result |
|---|---|
| AudioContext made on load, no user input yet | `suspended`; its `resume()` stays pending |
| First keydown (Enter) or click | user activation; the pending `resume()` resolves; `resume()` in the handler: `running` in 12–49 ms |
| `suspend()` by script, then `resume()` 7 s later (transient activation expired, sticky kept) | `running` in 12 ms |
| A second AudioContext made 7 s after the gesture | `running` |

So one key press or click per page visit unlocks audio for the rest of the visit, because Chrome's
autoplay policy works on sticky activation. Firefox's and Safari's policies were not tested with
input. The design calls `resume()` inside the Boot handler anyway.

## 4. Design

```
 page thread (app.js)                  audio rendering thread (Wasm Audio Worklet)
  Boot gesture: new AudioContext(48 kHz)  every 128 frames: heartbeat++
  _pcsx2web_audio_start() ──────────▶      if a stream is attached: ReadFrames(128) → outputs
  node → GainNode (volume, mute)
     → destination                     CPU thread (EE + IOP + SPU2), once per vsync:
                                         heartbeat moving? attach the stream to the worklet
 SPU2 → AudioStream::WriteChunk()        heartbeat stopped ≥ 100 ms? detach, drain at real time
  (stretch on the CPU thread)            SetPaused / ~stream: detach, wait for the callback to leave
```

### 4.1 Threads

- **CPU thread (EE).** It runs SPU2, `WriteChunk()` and stretching, as on the desktop. Once per
  vsync, in `Host::PumpMessagesOnCPUThread()` and in `RunVM()`'s paused loop, it also runs
  `UpdateAudioOutput()`: attach, detach and drain (§4.3), and the buffer steps (§4.4).
- **Audio rendering thread.** It reads the attached stream, 128 frames per quantum.
- **Page thread.** It owns the AudioContext, the GainNode and the start of the worklet. It never
  touches samples.
- There is no new pthread. `PTHREAD_POOL_SIZE` stays 8.

### 4.2 The C++ side (`pcsx2-wasm/Main.cpp`, a new "Audio" section)

**`class WebAudioStream final : public AudioStream`**
- Created by the factory registered with `AudioStream::SetFrontendStreamFactory()` in `main()`,
  before the CPU thread starts.
- `Create()` calls `BaseInitialize(&StereoSampleReaderImpl, stretch_enabled)` and becomes
  `s_audio_stream`. It starts detached.
- `SetPaused(true)` detaches the stream and returns only once the worklet is out of its callback;
  `SetPaused(false)` lets the next vsync attach it. The destructor detaches and clears
  `s_audio_stream`.
- `Render(float* l, float* r, u32 n)` runs on the audio thread. It calls `ReadFrames()` into a
  member buffer, then de-interleaves. When the stream's rate differs from the context's (PS1 mode:
  44.1 kHz into a 48 kHz context), it reads `ceil(n × ratio)` frames with a fractional accumulator
  and interpolates linearly.

**Attach gate.**
- The gate is `std::atomic<WebAudioStream*> s_audio_reader` plus
  `std::atomic<bool> s_audio_in_callback`. Both sides use `seq_cst`, like Dekker's algorithm.
- The callback:
  1. sets `in_callback`;
  2. loads `s_audio_reader`;
  3. renders, or writes silence if no stream is attached;
  4. clears `in_callback`.
- Detaching, on the CPU thread: store `nullptr`, then spin while `in_callback` is set (at most one
  quantum's work, microseconds).
- Only the CPU thread attaches or detaches. So `ReadFrames()` always has exactly one caller:
  - the worklet while attached;
  - the CPU thread's drain while detached.

**`static bool AudioRender(int, const AudioSampleFrame*, int, AudioSampleFrame* outputs, int, const
AudioParamFrame*, void*)`** is the processor callback. It bumps the heartbeat, goes through the
gate, keeps counters in the audio block, and returns `true`.

**Exports** (page thread, non-blocking like every `pcsx2web_*` export):
- `int pcsx2web_audio_start(void)`:
  - Registers `Module.pcsx2webAudioContext` with `emscriptenRegisterAudioObject()` (an `EM_ASM`).
  - Calls `emscripten_start_wasm_audio_worklet_thread_async()` with a 64 KB static stack, then the
    processor and the node (`outputChannelCounts = {2}`, no inputs).
  - When done, calls `Module.pcsx2webAudioReady(node)`. On failure, sets the block's `WORKLET` word
    to "failed" and calls `Module.pcsx2webAudioReady(null)`.
  - Returns 0 if the start was accepted, or -1 if a start already ran.
- `const void* pcsx2web_audio(void)`: the audio block.

### 4.3 Attach, detach, drain

Once per vsync, on the CPU thread, with `now` from `Common::Timer`:

- **Heartbeat advanced** since the last look, a stream exists, and it is not paused: attach. Set
  `s_audio_reader` to the stream and stop draining.
- **Heartbeat unchanged for at least 100 ms while attached:** detach. Possible causes: the context
  is suspended or interrupted, autoplay holds it, the device stalled, or the page was stopped.
- **While detached and a stream exists:** call `ReadFrames()` for the frames due since the last
  drain, and discard them. Cap one drain at 1 s of frames. The stream then stays in the state real
  playback would leave it in, so latency and stretching are normal at the next attach (§3.5).
- At 15 % speed a vsync comes every 111 ms, so detection takes 100–220 ms. That is enough.

### 4.4 Buffer steps for slow scenes

- **The steps.** `BufferMS` is one of 50, 100, 200, 400 or 800, picked from the emulation speed
  (§3.4):

  | Speed | `BufferMS` |
  |---|---|
  | ≥ 75 % | 50 |
  | 50–75 % (designed: 40–75 %) | 100 |
  | 28–50 % (22–40 %) | 200 |
  | 14–28 % (12–22 %) | 400 |
  | < 14 % (< 12 %) | 800 |

  Implemented with the new bounds (§10.2): the real emulator's output is burstier than the spike's,
  and at the designed lower bounds `BufferMS` × speed is only 37–48 ms.

- **The speed** comes from `s_speed`. The GS thread writes it in `Host::OnPerformanceMetricsUpdated()`
  every 0.5 s. The CPU thread reads it in `UpdateAudioOutput()`.
- **Hysteresis** (implemented: up after 1 s; down after 10 s, at 1.5 times a step's lower bound but
  at most 15 points above it, so 90 % for the 50 ms step; §10.2):
  - Step up once the speed has stayed below the current step's range for 2 s.
  - Step down only once it has stayed in a faster step's range, with 20 % to spare, for 5 s.
  - So a scene that hovers near a boundary does not switch back and forth. Each switch costs a gap
    (§3.4).
- **Applying a step**, on the CPU thread, the way the Qt frontend applies any settings change from
  the CPU thread's message pump:
  1. Write `SPU2/Output BufferMS` into the base settings layer, under the settings lock.
  2. Call `VMManager::ApplySettings()`.
  3. SPU2 recreates the stream (`CheckForConfigChanges()` sees new `StreamParameters`). The old
     `WebAudioStream` detaches in its destructor, and the next vsync attaches the new one.

  Implemented differently (§10.2): step 2 sets `EmuConfig.SPU2.StreamParameters.buffer_ms` and calls
  `SPU2::CheckForConfigChanges()` with the previous SPU2 options, because `ApplySettings()` also
  waits for the VU1 and GS threads and reloads every setting and input binding. The old stream hands
  what it still holds (at most 1 s) to the new one, which writes it through its stretcher, and the
  new stream attaches at once when the worklet runs.
- **A new boot** starts at 50 ms.
- **Tests and the page** can read the current step from the audio block.

### 4.5 The audio block (shared words, version 1)

Static, zero-initialised, 64-byte aligned. Every word is a little-endian `u32`, accessed with
`Atomics` on the JS side.

| Word | Name | Written by | Meaning |
|---|---|---|---|
| 0 | `MAGIC` | C++ | `0x44554154` ("TAUD") |
| 1 | `VERSION` | C++ | 1 |
| 2 | `WORKLET` | page thread (C) | 0 not started, 1 starting, 2 running, 3 failed |
| 3 | `STREAM_RATE` | CPU thread | 0 no stream, else its sample rate (48000; 44100 in PS1 mode) |
| 4 | `ATTACHED` | CPU thread | 1 while the worklet reads the stream |
| 5 | `HEARTBEAT` | audio thread | render quanta seen |
| 6 | `FRAMES` | audio thread | frames read from the stream |
| 7 | `SHORT_QUANTA` | audio thread | quanta that found fewer than 128 frames buffered (underruns) |
| 8 | `DRAINED` | CPU thread | frames discarded while detached |
| 9 | `CONTEXT_RATE` | page thread (C) | the AudioContext's sample rate |
| 10 | `BUFFER_MS` | CPU thread | the current `BufferMS` step |
| 11 | `FILL` | CPU thread | frames buffered in the stream at the last vsync (implementation) |
| 12 | `ATTACHES` | CPU thread | times a stream was attached (implementation) |
| 13 | `STEPS` | CPU thread | `BufferMS` steps applied (implementation) |
| 14 | `STREAMS` | CPU thread | streams created (implementation) |
| 15 | reserved | | 0 |

Tests and the page read the block through `window.PCSX2Web.audio` (§9).

### 4.6 The page side (`pcsx2-wasm/web/app.js`, a new "audio" section)

- **`audioUnlock()`**, called synchronously at the top of `boot()` (an immediate or a queued boot,
  both inside the gesture), and from a capture-phase `keydown` / `pointerdown` listener while sound
  is wanted but the context isn't running:
  - The first time, it creates `new AudioContext({sampleRate: 48000, latencyHint: 'interactive'})`
    and a GainNode connected to the destination.
  - It calls `resume()` and ignores a rejection.
  - The listener never consumes the event, so the key still reaches the PS2.
- **`audioStart()`**, once the runtime is ready and a context exists:
  - It sets `Module.pcsx2webAudioContext` and `Module.pcsx2webAudioReady = (node) =>
    node.connect(gain)` (or, with `null`, the unavailable state), then calls `_pcsx2web_audio_start()`.
  - It runs once per page. The worklet lives as long as the page.
- **Volume:**
  - `gain.gain.setTargetAtTime(target, ctx.currentTime, 0.015)`, so mute and unmute never click.
  - `target = muted ? 0 : (volume / 100)²`.
  - Read from and written to `localStorage` with `readPref`/`writePref` (§6.5).
- **VM states:**
  - Starting or running: sound is wanted, and the page calls `resume()`. (Implemented: a queued
    boot counts too, so the context made at its press is not suspended until the boot.)
  - Stopped or failed: ramp the gain down for 30 ms, then `ctx.suspend()`. That gives back about
    4 % of a core of browser audio work (§3.1). The next Boot resumes. (Implemented: the ramp starts
    at the Stop press, before the stream goes away, since the page sees the stop only at its next
    500 ms poll.)
- **`ctx.onstatechange`:** a state other than `running` that the page did not ask for while sound is
  wanted means the browser holds the sound. The Sound control shows it, and the next gesture
  resumes.
- **Nothing reads samples on the page thread.** Samples never touch the page thread.

### 4.7 Build (`pcsx2-wasm/CMakeLists.txt`, an "Audio" block)

- Link options:
  - `-sAUDIO_WORKLET=1`;
  - `-sWASM_WORKERS=1`;
  - `-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$emscriptenRegisterAudioObject,$emscriptenGetAudioObject`
    (check that CMake/Ninja keep the `$`). Implemented instead as `EM_JS_DEPS()` in `Main.cpp`, next
    to the `EM_ASM` that uses them, which keeps the `$` out of CMake.
- Exports: add `_pcsx2web_audio` and `_pcsx2web_audio_start` to `PCSX2_WASM_EXPORTS`.
- `pcsx2-headless` is unchanged. It has no AudioContext; it uses Null or the WAV output.
- After linking, check for `signature mismatch`.

## 5. Settings (`InitializeConfig()` in `pcsx2-wasm/Main.cpp`)

| Key (`SPU2/Output`) | Value | Why |
|---|---|---|
| `Backend` | `Frontend` | the `WebAudioStream` factory |
| `SyncMode` | `TimeStretch` | PCSX2's default. It is the only mode without constant crackle, at any speed (§3.4) |
| `StretchUseQuickSeek` | `true` | stretching costs 0.5–0.6 % of the EE thread instead of 2.0–2.4 % (§3.3); PCSX2 exposes the same option to desktop users |
| `ExpansionMode` | `Disabled` | stereo only, see below |
| `BufferMS` | 50 at full speed, stepped up to 800 below full speed (§4.4) | 50 absorbs Chrome's 20 ms device bursts and frame-limited production; slow scenes need `BufferMS` × speed ≥ about 50–60 ms (§3.4) |
| `StandardVolume`, `OutputMuted` | 100, false (defaults) | volume and mute are the page's GainNode: instant, no EE work |

- **Sample rate.** The context runs at 48 kHz in every browser that allows it. The browser resamples
  to the device on its own audio threads. The 44.1 kHz PS1 rate is converted in `Render()`.
- **Expansion modes.** None in v1.
  - FreeSurround would decode on the EE thread, inside `WriteChunk()`. Its cost was not measured.
  - Browsers usually expose a stereo destination (`maxChannelCount` 2).
  - Stereo comes first. A later option could offer 5.1 when `destination.maxChannelCount >= 6` and
    the EE has headroom.
- **Below full speed.** Keep stretching on, and step the buffer (§4.4). The sound is then continuous
  but stretched: slow scenes sound slowed down, at the same pitch. The slow-speed hint stays as it
  is.

## 6. Page behaviour

This section extends `UI-SPEC.md`, which reserves "volume, first on the right" in the transport.
The UI-SPEC needs a new section 11, "Sound", with the following.

### 6.1 Starting sound (autoplay)

- **Boot** (click, or Enter on the screen) runs `audioUnlock()` inside the gesture. Sound starts
  with the first frames.
- **A queued boot** (Boot pressed while the runtime is still downloading) unlocks at the press.
- **`?bios=` auto-boot**, with no user input yet:
  - The context is created suspended. The emulator runs silently; the CPU thread drains the stream.
  - The first key press or click anywhere resumes the context. The key still goes to the PS2.
  - With the first frame, one subtitle: "Press a key or click to turn the sound on."
- **If the worklet cannot start**, the Sound control shows the unavailable state, and one subtitle
  says "This browser can't play the emulator's sound." The emulator runs on.
- Implemented as above, plus: the gesture listener also takes `pointerup` (touch grants the
  activation on release); the held or unavailable subtitle replaces the first-boot hint, which then
  waits for the next boot. UI-SPEC §11 is the reference for the page.

### 6.2 Sound control

- **Button `#btn-sound`**, first in `.t-right`, before Crisp.
  - Phosphor icons:
    - `speaker-high`: volume 50 % or more;
    - `speaker-low`: below 50 %;
    - `speaker-x`: muted or volume 0;
    - `speaker-slash`: the browser holds the sound, or sound is unavailable.
  - Label "Sound".
  - `popovertarget="sound-menu"`, `aria-controls`, `aria-expanded`.
  - Accessible name: "Sound, 80 percent", "Sound, muted" or "Sound, waiting for a key or click".
  - Tooltip at narrow widths: "Sound (M)".
  - It is never disabled.
- **Popover `#sound-menu`**, like the BIOS menu: `role="group"`, `aria-label="Sound"`, opens upward
  and focuses its first control.
  - **Volume:** `input[type=range]`, 0–100 in steps of 5, labelled "Volume". The value is shown in
    Chivo Mono ("80 %") and given as `aria-valuetext` "80 percent". Arrows step by 5, Page Up/Down
    by 10, Home/End go to the ends.
  - **Mute:** a quiet toggle button with `aria-pressed`, "Mute", with an 11 px mono key cap "M".
  - **Fine print**, only when it applies: "The browser holds the sound until you press a key or
    click." or "This browser can't play the emulator's sound."
- **Shortcut M** toggles mute.
  - Guard: focus is in the theatre, no modifiers. M is not a pad key, so it is added to the page
    shortcuts in UI-SPEC §8 and swallowed like the others.
  - Subtitle and announcer: "Sound off." / "Sound on."
- **Behaviour.** Moving the slider while muted unmutes. Changes apply at once (ramped) and are saved
  on `change`.
- **Accessibility.** The control works by keyboard throughout and has visible focus. Forced-colors
  mode uses system colours for the slider. Nothing animates beyond the existing popover rules.
- **About.** Remove "audio" from "Not yet: …".
- Implemented as above, with these details (UI-SPEC §11): the accessible name is the button's
  `aria-label`; the tooltip reads "Sound" with the key cap M; the value is shown as "80%" like the
  page's other percentages; M is matched on `e.key`, so it is the key labelled M on any layout, and
  it also works inside the Sound menu; the pressed Mute item gets the transport's 2 px bar under its
  icon; the menu is right-aligned to its button; touch-only devices hide the M key cap.

### 6.3 Stop, boot again, game change

- **Stop:** the stream goes away (`SPU2::Close()`), and the worklet plays silence. The page fades the
  gain over 30 ms and suspends the context.
- **Boot again** resumes it. Sticky activation allows this even without a new gesture (§3.8), and
  Boot is a gesture anyway.
- **A rate change** (PS1 mode, `SPU2::UpdateSampleRate`) or a buffer step (§4.4) recreates the
  stream. The CPU thread detaches the old stream and attaches the new one; the context stays as it
  is.

### 6.4 Tab hidden, device change, interruptions

- **Hidden tab.** Keep playing.
  - Chrome keeps the context running in a background tab (§3.7), and the emulator keeps running.
  - The browser shows the tab's speaker icon, and the user can mute the tab.
  - No automatic mute in v1.
- **Device change.** Nothing to do.
  - The context follows the system's default output.
  - Its sample rate stays 48 kHz; only `outputLatency` changes.
  - No device picker in v1 (`setSinkId` could add one later).
- **Interruptions.** These are a context the browser suspends or interrupts (Safari's
  `interrupted`), or a render error. In each case:
  1. The state change marks the sound as held.
  2. The heartbeat stops, and the CPU thread drains.
  3. The next gesture calls `resume()`; one attempt is also made without a gesture.

### 6.5 Volume and mute, remembered per browser

- `pcsx2web.volume`: `"0"`–`"100"`, default 100.
- `pcsx2web.muted`: `"1"` or `"0"`, default `"0"`.
- Both go through `readPref`/`writePref`, so they fail quietly when storage is blocked. This follows
  `pcsx2web.crisp`.
- They are applied when the page loads, before any context exists.
- **Test switch** `?mute=1`: forces the gain to 0 for that visit without touching the stored values.
  Automated runs use it together with `--mute-audio`.

## 7. Risks

1. **Wasm Audio Worklet support.**
   - Chrome 155, Chrome 152 (the browser pane) and Firefox 157 were tested. Safari was not.
   - Emscripten polyfills the worklet message port that Chrome and Safari lack
     (`audio_worklet.js`), and Chrome uses that path today.
   - If `pcsx2web_audio_start` fails, the page says so and runs without sound.
   - Plan B is (b). Its spike code exists, and only the consumer changes.
2. **PCSX2 code on the audio thread.**
   - `ReadFrames()` must stay lock-free and allocation-free.
   - In Devel builds, a `pxAssert` there would log from the audio thread.
   - ASan with worklets is untested. Devel and ASan builds can fall back to Null if needed.
3. **The module's JS runs in the AudioWorkletGlobalScope.** Code added to the glue that touches
   `window` or `document` at load (a `--pre-js`, for example) would break the worklet's start. Keep
   page code in `app.js`.
4. **Slow scenes depend on the buffer steps.**
   - With 50 ms, stretched output has gaps below 50 % speed: 42 % silence at 30 %, 73 % at 15 %.
   - The steps make it continuous in the spike, down to 10 %. Dragon Ball Z: Budokai Tenkaichi 3
     fights (10–20 %) should land on the 400–800 ms steps.
   - Sound then lags the picture by 150–250 ms, and every step change costs a gap of up to 0.6 s.
   - With real EE stalls (heavy frames), the thresholds may need adjusting.
   - Even when continuous, music and voices stretched to 10–20 % tempo sound smeared. SoundTouch
     does not do better at such ratios.
5. **QuickSeek's effect on music was not measured.** The sine tests show no glitches. It is PCSX2's
   own option (the Qt frontend's stretch settings). Turning it back off costs about 1.5 % more of
   the EE thread.
6. **Linux scheduling.** Chrome's audio thread is not real-time there. A machine whose cores are all
   saturated can still starve it, whatever the design. The emulator's own threads will load 3–5
   cores.
7. **Device bursts.** They were 20 ms here. Bluetooth or Android outputs may render larger bursts.
   `AudioStream`'s 50 ms absorbs 20 ms easily; the short-quanta counter will show if `BufferMS`
   must grow.
8. **The browser's own cost:** about 4 % of a core while the context runs (§3.1). Suspending after
   Stop limits it to play time.
9. **Not tested with the real emulator.** These are spike measurements with PCSX2's real
   `AudioStream` and a fake SPU2. Production is burstier: the IOP's event timing, and EE stalls in
   heavy frames. Implementation must re-measure with the BIOS and the test game (§9).

## 8. What is not decided

- **Muting below a speed threshold** instead of stretching. Below 15 % the stretched sound is
  continuous but very slow, so this is the owner's call. v1 plays it.
- **A smaller full-speed `BufferMS`** (30–40 ms) for less latency, once the real emulator's
  production jitter is known.
- **The step thresholds** (§4.4), once the real emulator's speed fluctuation is known.
- **5.1 output and a device picker.**

## 9. Files and how to test

**The implementation will touch:**
- `pcsx2-wasm/Main.cpp`: the "Audio" section, the settings in `InitializeConfig()`, the factory in
  `main()`, and `UpdateAudioOutput()` in `Host::PumpMessagesOnCPUThread()` and the paused loop.
- `pcsx2-wasm/CMakeLists.txt`: the "Audio" block.
- `pcsx2-wasm/web/app.js`: the "audio" section, `boot()`, the shortcut and the transport.
- `pcsx2-wasm/web/index.html`, `style.css`, `icons.svg`.
- `pcsx2-wasm/assets/sync-assets.mjs`: add `speaker-high`, `speaker-low`, `speaker-x` and
  `speaker-slash` from `@phosphor-icons/core` 2.1.1.
- `docs/web-port/UI-SPEC.md` §11 and `pcsx2-wasm/README.md`.
- No change to `pcsx2/` beyond the `AudioBackend::Frontend` hook.

**Spikes** (outside the repository, in `tabstationtoo-wt/scratch/audio/spike/`; not for commit):
- `audiospike.cpp` and `core/` (PCSX2's `AudioStream.cpp` and `SDLAudioStream.cpp` from the base
  tree, `stubs.cpp`, `flags.sh`), built by `build.sh` (variants `sdl`, `ring`, `wasmwl`,
  `wasmwl-fs`, `wasmwl2`).
- `web/`:
  - `spike.js`, the page;
  - `tap-worklet.js`, the silent measuring tap;
  - `ring-worklet.js`, option (b)'s worklet;
  - `spn.html`, the pure-JS ScriptProcessorNode reference;
  - `autoplay.html`;
  - `instantiate.html` and `instantiate-worklet.js`.
- `run.mjs` (Chrome over DevTools, with CPU and memory sampling, key and click injection and tab
  hiding) and `run-firefox.mjs`.
- `matrix.sh` (§3.2–3.4), `extra.sh` (hidden tab, Firefox), `bufgrid.sh` (§3.4 buffer table),
  `aggregate.py`, `summarize.py`.
- `st-bench.cpp` (SoundTouch cost); `st-simd/` (the SSE experiment).
- `prebuilt/`: the copied libraries and `pcsx2-wasm.wasm`, with `SOURCE.txt` and `SHA256SUMS`.
- `results/*.jsonl`, the raw results of every run cited here.
  - `matrix.jsonl`, `firefox.jsonl`, `hidden.jsonl`, `bufgrid.jsonl` and `bufgrid10.jsonl`: the
    pinned runs.
  - `gated.jsonl`, `switch.jsonl`, `suspend.jsonl`, `instantiate.jsonl`, `autoplay.jsonl` and
    `spnref.jsonl`: the rest.
- The SDL hang and the pane runs were read from their logs and are quoted in §2 and §3.6.
- The matrix ran the first worklet backend: one node per stream, with the gate in the stream.
  `audiospike.cpp` now holds the gated, page-lifetime version of §4.2. `wasmwl` and `wasmwl2` both
  build from it; use `drain=1` for the heartbeat drain.
  - That version also had no dropout and no short quantum in 8 pinned runs: idle, page blocked
    50/500 and 30/100, 12 busy threads, 2 rounds (`results/gated.jsonl`).
  - The switch and suspend runs of §3.4–3.5 used it too.

**Rerunning.**

```sh
cd tabstationtoo-wt/scratch/audio/spike
./build.sh sdl ring wasmwl wasmwl-fs wasmwl2      # Emscripten 6.0.12
python3 ../../../audio/tools/wasm/serve.py --port 8150 --quiet web &
./matrix.sh matrix 3 transport load grow slow     # about 40 min, pinned to CPUs 12-15
./bufgrid.sh; ./extra.sh                          # about 25 min
python3 aggregate.py results/matrix.jsonl         # or any results file
# one run: node run.mjs --autoplay --url 'http://127.0.0.1:8150/index.html?variant=wasmwl2&drain=1&speed=0.15&quickseek=1&buffer_ms=400'
```

**Acceptance tests for the implementation** (headless Chrome `--mute-audio`, `?mute=1`):
1. **BIOS menu, 60 s:** `SHORT_QUANTA` stays 0 after the first second, `ATTACHED` is 1, and
   `FRAMES` grows by 48,000 per second.
2. **Page-thread stalls:** the same, with the page thread blocked 50 ms every 500 ms (a test hook).
3. **Stop and boot again three times:** each boot attaches, each Stop suspends the context, and no
   `pcsx2web_*` export blocks.
4. **`?bios=` auto-boot:** `WORKLET` is running, `ATTACHED` is 0 and `DRAINED` grows; then a key
   press resumes the context and attaches.
5. **The test game** in a fight (`to_battle_input.txt` with the headless runner's `-wav` for the
   content, and the page for the counters):
   - no crash;
   - `BUFFER_MS` reaches the 400 or 800 step, and `SHORT_QUANTA` stops growing once it does;
   - the EE cost of stretching stays about 0.5 % of the CPU thread (profile).
6. **Tab hidden for 10 s:** the counters keep advancing.
7. **Keyboard-only use of the Sound control** at desktop and phone widths.

## 10. Implementation status (2026-10-17)

### 10.1 What was built

- **`pcsx2-wasm/Main.cpp`, "Audio" section.** `WebAudioStream` (an `AudioStream` registered with
  `SetFrontendStreamFactory()`), the seq_cst gate (`s_audio_reader`, `s_audio_in_callback`),
  `AudioRender()` (the processor callback: heartbeat, gate, `Render()` with linear resampling when
  the rates differ, then `SmoothAudioEdges()`), `UpdateAudioOutput()` (once per vsync and in the
  paused loop: heartbeat, attach, detach, drain, `UpdateAudioBufferStep()`), the exports
  `pcsx2web_audio_start()` and `pcsx2web_audio()`, `ConfigureAudio()` (the settings of §5) and
  `ResetAudioBufferStep()` (every boot starts at 50 ms). `-audio-null` in `main()`'s arguments keeps
  the Null backend (the page's `?audio=null`).
- **`pcsx2-wasm/CMakeLists.txt`.** The two exports, and `-sAUDIO_WORKLET=1 -sWASM_WORKERS=1`. The
  module's JS grew from 142,031 to 151,434 bytes and the `.wasm` by 7.7 KB. No `signature mismatch`.
- **`pcsx2-wasm/web/`.** `app.js`, "audio" section: the AudioContext and GainNode, gestures, the
  Sound control, `window.PCSX2WebAudio`; `audio-tap.js` (the `?audiotap` test recorder);
  `index.html`, `style.css`, `icons.svg` (four Phosphor speaker icons through
  `assets/sync-assets.mjs` and pnpm). UI-SPEC §11 is the page's specification.
- `pcsx2/` is unchanged beyond the `AudioBackend::Frontend` hook. `pcsx2-headless` is unchanged.

### 10.2 Changes from the design, and why

1. **Step bounds 75/50/28/14 % instead of 75/40/22/12 %, and step-up after 1 s instead of 2 s.**
   With the real BIOS intro under load, the 200 ms step left gaps at 18–24 % speed (`BufferMS` ×
   speed 36–48 ms), and the 400 ms step at 11–13 %. The new bounds keep the product at 50 ms or
   more at each step's lower edge below full speed.
2. **Step-down after 10 s at min(1.5 × bound, bound + 15) instead of 5 s at 1.2 × bound.** A
   1,290 s run of Budokai Tenkaichi 3, boot to fight, took 37 steps with the 5 s rule, and many came
   in pairs (down, then up again seconds later), each pair costing gaps. Replaying that run's speed
   series through both rules (`stepsim.py`): 29 → 20 steps, and time spent with `BufferMS` × speed
   under 50 ms 56 s → 44 s; the price is 8 s more at full speed with more than 100 ms of buffer.
3. **A step does not call `VMManager::ApplySettings()`** (§4.4): it writes the base layer, sets
   `EmuConfig`, and calls `SPU2::CheckForConfigChanges()`.
4. **A step carries the old stream's buffered frames into the new stream** (through its
   stretcher), and the new stream attaches at once instead of at the next vsync. Steps down then cost
   no gap; steps up still can, while the bigger buffer refills after the carried audio.
5. **Edges into and out of silence are 64-frame ramps** (`SmoothAudioEdges()`, on the audio thread
   after the gate): an underrun's silence, a stream going away and a stream starting no longer
   click. Before it, the gaps in a BIOS capture came with a stop click and an onset click (8 of the
   run's 9 clicks); after it, no capture had a click at a gap's edges (`wavcheck.py check`).
6. **The Stop press starts the 30 ms fade**, before the VM stops (§4.6).
7. `Module.pcsx2webAudioReady(node | null)` instead of `pcsx2webAudioNode`; `EM_JS_DEPS()` instead
   of `DEFAULT_LIBRARY_FUNCS_TO_INCLUDE`; block words 11–14 defined (§4.5).
8. Test switches: `?mute=1`, `?audio=null`, `?audiotap=SECONDS` and `?audiorate=HZ` (UI-SPEC §11).

### 10.3 Measurements with the real emulator

Headless Chrome 155 (`--mute-audio`, `?mute=1` except the mute and volume test), the page's
`?audiotap` recorder before the gain, `tools/wasm/audio/wavcheck.py`; the desktop app's browser
pane (Chrome 152) with `?mute=1`. Other agents' builds and browsers kept the load average at 8–25
throughout, so speeds are lower than on an idle machine (the BIOS intro ran at 8–29 % instead of
25 %). The drivers and raw results are in `tabstationtoo-wt/scratch/audio/impl/` (outside the
repository).

| Test | Result |
|---|---|
| Worklet start (`pcsx2web_audio_start()` to the node) | 25–134 ms in 16 runs; 85 ms in the pane |
| Memory growth while attached (512 → 614 → 712 MB at every boot) | no worklet error; the recorder lost no block |
| BIOS boot, NTSC-U and PAL | the intro plays; sound flows from the first frames; no clicks |
| Full-speed scene (game menus, 90–100 %, 50 ms step), 11.7 s against the headless reference | spectral similarity 0.975; 4 of 12 one-second windows match the reference waveform at 0.99 or more (the stretcher is inactive at tempo 1); the rest is the same music on a shifted timeline (the page's run took other menu paths in French). Two gaps of 23 and 28 ms (load average 15) |
| Budokai Tenkaichi 3 from `?disc=`, boot to fight, 1,290 s | 117 gaps, 31.6 s in all (2.4 %): 8.6 s around steps, the rest mostly in loading screens at 9–16 % speed |
| The fight alone, 422 s at 9.5–26 % (mean 13.8 %), 400/800 ms steps | 12 gaps, 3.1 s (0.73 %); 166 short quanta per minute. Against the reference's fight (31 s, stretched about 7 times here): spectral similarity 0.86; 7 clicks the reference does not have, all within 1.5 s of a gap; the clipping is the game's own (the reference has it too) |
| Page thread blocked 50 ms every 500 ms, 28.6 s | 0 short quanta |
| Tab hidden 11.2 s | context clock +10.93 s, heartbeat +4,098 quanta, 0 short quanta, stayed attached |
| `?bios=` without a gesture | context suspended, Sound shows "waiting for a key or click", the subtitle shows, `ATTACHED` 0, drained 47,500–49,100 frames per second; a click resumed and attached in 69–540 ms; the key K did the same in 169 ms and still reached the PS2 (press and release) |
| An interruption (`ctx.suspend()` by script) | resumed and attached again 35 ms later (one try without a gesture; sticky activation allows it) |
| Stop and Boot again, twice | the context suspended 267–1,169 ms after the Stop click (when the VM stops); the heartbeat stood still while stopped; Boot again attached the new stream 449–801 ms after the click; 1,000 calls of the exports and stats took 6–8 ms on the page thread |
| Mute and volume | M muted (gain 0, "Sound off.") and unmuted (gain 1, "Sound on."); 10 × ArrowLeft in the menu gave volume 50 % and gain 0.25; both remembered; Esc closed the menu to its button. Same in the pane |
| Resampling (`?audiorate=44100` and `52920`) | no clicks, no defects beyond the one step gap the 48 kHz runs have |
| Latency at full speed | stream fill 43–75 ms (mean 54) + 2.7 ms quantum + Chrome's 11.6 + 56 ms = about 124 ms; at the 400 and 800 ms steps the fill is 150–330 ms |
| EE-thread cost, pinned pairs in the page (BIOS intro frames 60–430, CPUs 8–11 against 12–15, swapped) | sound/Null time 0.990, 1.036, 0.963: no difference outside the ±4 % noise |
| EE-thread cost, CPU profile (headless runner, the same EE-side work: QuickSeek stretching, realtime device) | `AudioStream` 0.27 % of the EE thread's working time (SoundTouch 0.17 %); SPU2 mixing with its output 1.94 % against 1.71 % with Null |
| Headless runner | `-wav` output of the BIOS (600 frames) bit-identical to the reference; builds with no `signature mismatch` |
| Accessibility (Chrome's tree) | button "Sound, 100 percent" / "Sound, muted" / "Sound, waiting for a key or click" / "Sound, unavailable", `aria-expanded`, `aria-keyshortcuts` M; slider "Volume" (Chrome reports the value as "95" although `aria-valuetext` is "95 percent"); Mute `aria-pressed`; menu group "Sound" |

### 10.4 Known gaps

- **Slow scenes still break up when the machine is overloaded.** The fight lost 0.73 % of its time
  to gaps with load averages of 8–25 on this 16-thread machine; the loading screens at 9–16 % lost
  more. Each step up can cost a gap while the bigger buffer refills.
- **Below about 15 % the stretched sound is continuous but smeared**, as §8 says. Still the owner's
  call whether to mute there.
- **The BIOS without its NVM** (the page loads only the main file) boots to its first-run language
  screen, which is silent, so the BIOS menu's music could not be compared with the headless
  reference in the page. The intro could, and only at its slow speed.
- Safari, Firefox with the real emulator, Devel and ASan builds, and PS1 discs (44.1 kHz) were not
  tested; the resampler was tested through `?audiorate`.
- On touch-only devices the transport's tooltips (Sound's included) stay shown after a tap, because
  a tap leaves `:hover` on the button; this predates sound and affects every transport button.
