// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU1 JIT differential testing (docs/web-port/VU1-JIT-DESIGN.md 9.1, 9.4): what a run leaves behind, the PATH1
// recorder and the interrupt sink that stand in for the GIF and for mtvuInterrupts, and the comparison by the contract.

#pragma once

#include "VU.h"
#include "VUmicro.h"

#include <string>
#include <vector>

namespace VU1Jit
{
	// PATH1 output of a run: the bytes in order and the offsets where complete tags ended (TransferGSPacketData).
	// Generated code may split partial transfers differently (section 7.7), which CopyGSPacketData does not see, so
	// partial transfer boundaries are not part of it.
	struct Path1Record
	{
		std::vector<u8> bytes;
		std::vector<u32> tag_ends;
		u64 hash = 0; // FNV-1a of the bytes, kept up to date
		u64 size = 0; // bytes recorded (also without keep_bytes)
		bool keep_bytes = true;

		void Reset();
		void Append(const u8* data, u32 n, bool complete);
	};

	// The state a run leaves behind (section 9.1): VU1's registers and pipes, its data memory, whether it still runs,
	// and what it sent out.
	struct RunState
	{
		VURegs regs;
		alignas(16) u8 mem[VU1_MEMSIZE];
		bool running;
		Path1Record path1;
		u32 interrupts; // E- and T-bit interrupt flags published (VU_Thread::InterruptFlag*)
	};

	void CaptureRunState(RunState* s); // the registers, the memory and the run flag (not path1 or interrupts)
	void RestoreRunState(const RunState& s);

	// Installs the PATH1 recorder and the interrupt sink (VU1JitHooks.h) on this thread for its lifetime. Scopes nest:
	// PATH1 data goes to the innermost scope's record, then outward while each scope passes it through, and to the
	// GIF if every scope does (a null record passes data on unchanged). Interrupts go to the innermost scope that
	// takes them and are never published: its owner publishes them once (PublishInterrupts).
	class RecordingScope
	{
	public:
		RecordingScope(Path1Record* path1, u32* interrupts, bool pass_through);
		~RecordingScope();

		RecordingScope(const RecordingScope&) = delete;
		RecordingScope& operator=(const RecordingScope&) = delete;

	private:
		friend void RecordPath1(u8* data, u32 size, bool complete);
		friend void RecordInterrupt(u32 flag);

		Path1Record* m_path1;
		u32* m_interrupts;
		bool m_pass_through;
		RecordingScope* m_outer;
	};

	// Publishes E/T-bit interrupt flags as the interpreter does (mtvuInterrupts.fetch_or), or to the enclosing
	// RecordingScope if one takes interrupts.
	void PublishInterrupts(u32 flags);

	// Compares a run's result with the interpreter's by the contract, after normalising what is dead or unobservable.
	// Returns an empty string when they match, else the first differing field and both values.
	std::string CompareRunStates(const RunState& jit, const RunState& ref);

	// Checks CompareRunStates: it must report a change to each contract field and ignore each normalised one.
	// Overwrites both states. Returns the number of failures, which it also logs.
	u32 SelfTestCompare(RunState& base, RunState& other);
} // namespace VU1Jit
