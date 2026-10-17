# Testing audio without listening

Two tools: the headless runner records what PCSX2's audio stream hands to a device into a WAV file, and
`wavcheck.py` measures such files (levels, silence, clipping, gaps, clicks, spectra) and compares two of them.
Neither plays a sound.

## Recording: `pcsx2-headless -wav`

```sh
node build-wasm/bin/pcsx2-headless.js -approot "$PWD/bin" -dataroot <dir> -bios <bios file> -frames 1800 \
  -wav out.wav [-wav-realtime [-wav-period FRAMES]]
```

`-wav` selects `AudioBackend::Frontend` (WebAssembly builds only), a stream the runner registers with
`AudioStream::SetFrontendStreamFactory()`. Without `-wav` the runner keeps the Null backend. The file is 32-bit
float stereo at the console's rate (48 kHz), the samples `AudioStream::ReadFrames()` returns, so after SPU2's mix,
DC filter and volume. Two device clocks:

- **Emulated (default).** The device takes everything buffered at every vsync, on the CPU thread. Time-stretching
  needs a real-time reader, so it is off: the file is the SPU2 mix, **before time-stretching**, 48000 frames per
  emulated second at any emulation speed. It is deterministic: two runs of the same boot give identical files
  (also across `-renderer sw` and `null`). Use it for references and regression checks.
- **Realtime (`-wav-realtime`).** A device thread reads one period (default `SPU2/Output/OutputLatencyMS`, 20 ms,
  as PCSX2's SDL backend asks for; `-wav-period` changes it, e.g. 128 for a Web Audio render quantum) every period
  of wall-clock time. The file is what a listener would hear at the emulator's actual speed, **after
  time-stretching** (`SPU2/Output/SyncMode`, PCSX2's default `TimeStretch`;
  `-set SPU2/Output/SyncMode=Disabled` turns it off), with any underruns. Its length follows the wall clock.

One run records one clock: both clocks read the same ring buffer. The runner prints a summary when it stops
(frames, vsyncs, and for the realtime clock how many periods found less than a period buffered).

## Analysis: `wavcheck.py`

Python 3 with numpy. Reads PCM 16/24/32-bit and float WAV, including files whose header was never completed.

```sh
wavcheck.py info a.wav b.wav             # format and length
wavcheck.py check a.wav [--json a.json]  # levels and defects; --strict exits 1 on any defect
wavcheck.py fingerprint a.wav            # per second: RMS, centroid, rolloff, peak, flatness, 24 band levels
wavcheck.py compare ref.wav test.wav [--drift 2]   # alignment and similarity
wavcheck.py damage in.wav out.wav --seed 1         # a copy with known defects, listed in out.wav.json
wavcheck.py selftest ref.wav --out-dir DIR         # damages a copy of ref.wav and checks what the detectors find
```

`check` reports:

- **Levels:** peak and RMS per channel (dBFS), the active (non-silent) time and its RMS, and the RMS of every
  second as a sparkline (` .:-=+*#%@` from -80 to 0 dBFS).
- **Silence:** spans of 0.2 s or more below -60 dBFS (10 ms frames), marked when they are digital zeros.
- **Clipping:** samples at or above 0.999, samples over full scale (float can exceed 1.0: SPU2's DC filter runs
  after its 16-bit clamp), flat tops (3 or more equal samples above 0.99), and where they are.
- **Gaps:** 0.5 ms or more of exact zeros on every channel with audio (at least -45 dBFS) in the 10 ms on either
  side. This is what an underrunning stream (silence fill) or a dropped buffer leaves.
- **Dropouts:** 2-100 ms dips of 30 dB or more under the audio on both sides that are not exact zeros.
- **Repeated frames:** frames equal to the previous one, above -100 dBFS. After SPU2's DC filter real audio
  practically never repeats a float frame; `AudioStream::ReadFrames()` duplicates frames when it spreads too few
  frames over a period (its underrun resampler). Spans with 20% or more repeated frames are listed.
- **Clicks:** a 24th-order linear predictor per 2048-frame block (fitted on a window twice that size) whitens the
  audio, and the residual filtered back through the same coefficients (a matched filter) turns an impulse or a jump
  into one peak. A click scores over 20 times the block's robust scale and is the size of an impulse of at least
  0.002. Each is classed by the level 20 ms before and after it: `onset` or `stop` (a change of more than 6 dB, how
  game sounds start and end) or `mid` (inside steady audio, where a lost chunk lands). Game audio has some of its
  own: the Dragon Ball Z reference has 18 in 4.4 minutes (10 mid), the BIOS references none. That is why the
  comparison below is the real test.

`fingerprint` prints one row per second; the band sparkline shows each of 24 log-spaced bands (40 Hz-20 kHz) in
6 dB steps below that second's loudest band.

`compare` finds the lag of the test against the reference by cross-correlation (coarse on a 6 kHz copy, then
exact), and reports:

- **waveform similarity:** the cosine of the aligned mono signals (1.0 means the same samples up to a gain), per
  channel, the gain and the SNR of the difference. `identical` means the same samples, frame for frame.
- **per-window similarity** (1 s by default), listing the windows below 0.99.
- **spectral similarity:** the mean over active seconds of the cosine between the two seconds' band magnitudes:
  near 1 for the same sound even when the waveform differs (time-stretched, resampled, shifted in places).
- **new defects:** the test's clicks, gaps, dropouts and repeated-frame spans that the reference does not have at
  the same place (clicks are matched against the reference's candidates at half the threshold, so one that just
  crosses the threshold in one file is not called new).
- `--drift S`: the lag found for each S-second window of the reference (each window searched within 1 s of the
  last good one), and the **timing jumps** between windows: audio inserted (+) or lost (-), to the frame. This
  finds every lost chunk, even those the click detector cannot hear in dense audio. Jumps from defects closer
  together than the window add up. A lag that grows steadily shows time-stretching.
- A verdict: identical, equivalent waveform (similarity 0.99 or more), similar sound (spectral 0.9 or more), or
  different. `--min-similarity X` exits 1 below X.

## Validation

`selftest` writes a damaged copy (8 each by default, 200 ms apart where the audio is above -40 dBFS): inserted
silence of 10-100 ms, audio replaced by zeros for 2-50 ms, single-sample spikes of 0.05-0.3, and lost chunks of
64-512 frames. It then reports what `check` finds (detections the clean file does not explain count as false),
what `compare` reports as new, and whether the drift track accounts for the inserted and lost audio. It passes
when every gap and dropout and 90% of the spikes are found with at most one false detection per minute; lost
chunks are reported but not required, because in noisy or dense audio a cut can be continuous enough to hide
(`compare --drift` finds them all).

## Notes

- Record references with the emulated clock: they do not depend on the machine's speed.
- A realtime recording of a slow game is longer than the mix and stretched; compare it with `--drift` and look at
  the spectral similarity, gaps and repeated frames rather than the waveform similarity.
- Never commit recordings of a BIOS or a game, or anything derived from them.
