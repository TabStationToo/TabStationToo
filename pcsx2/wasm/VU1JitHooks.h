// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Hooks that the VU1 JIT's test modes install in the VU1 interpreter and MTVU (docs/web-port/VU1-JIT-DESIGN.md
// 3.2, 9.4, 9.6). WebAssembly only: the call sites are guarded with ARCH_WASM. All are null or false in normal
// runs, so each one costs a predictable test. They are set while the VU thread is idle, or by the thread that runs
// VU1 (lockstep switches them around its two passes on the VU thread; the fuzzer runs VU1 on its own thread).

#pragma once

#include "common/Pcsx2Types.h"

namespace VU1JitHooks
{
	// _vuXGKICKTransfer (VUops.cpp), MTVU path: replaces its two GIF calls. complete is true where the interpreter
	// calls TransferGSPacketData (the transfer ends a tag), false where it calls CopyGSPacketData. The PATH1
	// recorder of the lockstep mode and of the fuzzer (VU1JitLockstep.cpp).
	using Path1Fn = void (*)(u8* data, u32 size, bool complete);
	extern Path1Fn path1;

	// The two GIF calls the hook replaces, for a recorder that passes the data on to the GS.
	void Path1ToGif(u8* data, u32 size, bool complete);

	// _vu1Exec (VU1microInterp.cpp), program end on MTVU: receives the E- or T-bit interrupt flag instead of
	// vu1Thread.mtvuInterrupts. The interrupt sink of the lockstep mode and of the fuzzer.
	using InterruptFn = void (*)(u32 flag);
	extern InterruptFn interrupt;

	// VU_Thread::Get_MTVUChanges (MTVU.cpp): waits for the VU thread first, so the EE sees each program's events at
	// the same point of its own execution (the deterministic mode of the headless runner, -mtvu-sync).
	extern bool mtvu_sync;

	// MTVU_VU_EXECUTE (MTVU.cpp): runs CpuVU1->Execute(cycles) itself when set, timed and counted
	// (headless -vu1jit-stats).
	using ExecuteFn = void (*)(u32 cycles);
	extern ExecuteFn execute;
} // namespace VU1JitHooks
