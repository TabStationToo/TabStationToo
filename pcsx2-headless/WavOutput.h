// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

class Error;
class SettingsInterface;

// -wav: records what PCSX2's AudioStream hands to an audio device into a 32-bit float WAV file, through a stream
// registered for AudioBackend::Frontend (WebAssembly builds only). Two device clocks:
//  - Emulated: the device takes everything buffered at every vsync, on the CPU thread. Time-stretching cannot act
//    without a real-time reader, so it is off: the file is the SPU2 mix sample for sample, 48000 frames per emulated
//    second whatever the emulation speed. Deterministic, for references and regression checks.
//  - Realtime: a device thread reads one period every period of wall-clock time, as SDL's callback does, with
//    time-stretching as SPU2/Output SyncMode says (PCSX2's default is on). The file is what a listener would hear at
//    the emulator's actual speed, after time-stretching, with any underruns.
namespace WavOutput
{
	enum class Clock
	{
		Emulated,
		Realtime,
	};

	/// Opens the file. period_frames is the realtime device's read size (0: PCSX2's output latency, as SDL uses).
	bool Enable(const std::string& path, Clock clock, u32 period_frames, Error* error);
	bool IsEnabled();

	/// Selects the WAV output in the base settings layer (does nothing unless enabled).
	void ApplySettings(SettingsInterface& si);

	/// CPU thread, once per vsync: the emulated clock's device takes everything buffered.
	void OnVsync();

	/// After the VM has shut down: completes the file's header and prints a summary.
	void Finish();
} // namespace WavOutput
