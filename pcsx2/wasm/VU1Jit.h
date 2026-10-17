// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU1 JIT to WebAssembly (docs/web-port/VU1-JIT-DESIGN.md). WasmVU1 is the BaseVUmicroCPU that VMManager selects on
// wasm when MTVU is on and EmuCore/CPU/Recompiler/EnableVU1 is set (off by default in both frontends until the JIT is
// switched on, section 12). It runs on the MTVU thread, where it compiles and calls generated code; anything it does
// not handle runs on the VU1 interpreter, which is the specification.
//
// Step 1a: the dispatcher, the program cache plumbing and the runtime, with an "all-fallback JIT" whose every
// program is one exit to the interpreter at its entry. Generated code comes in step 1b.

#pragma once

#include "VUmicro.h"

#include <atomic>
#include <string>

class WasmVU1 final : public BaseVUmicroCPU
{
public:
	WasmVU1();
	~WasmVU1() override;

	const char* GetShortName() const override { return "wVU1"; }
	const char* GetLongName() const override { return "VU1 WebAssembly JIT"; }

	size_t GetCommittedCache() const override;

	void Shutdown() override;
	void Reset() override;
	void SetStartPC(u32 startPC) override;
	void Execute(u32 cycles) override;
	void Step() override;
	void Clear(u32 addr, u32 size) override;
	void ResumeXGkick() override {}

	// Program-memory changes since a thread last looked (section 6.2): Clear may run on the EE thread (SPR DMA,
	// SPR.cpp:58; VIF MPG without MTVU), so it only touches these atomics.
	u32 GetMicroGeneration() const { return m_micro_gen.load(std::memory_order_acquire); }
	u64 TakeDirtyPages() { return m_dirty_pages.exchange(0, std::memory_order_acq_rel); }
	bool TakeFlushRequest() { return m_flush_requested.exchange(false, std::memory_order_acq_rel); }

	u32 GetEEThreadClears() const { return m_ee_clears.load(std::memory_order_relaxed); }
	void SetCommittedCache(size_t bytes);

private:
	std::atomic<u32> m_micro_gen{0};
	std::atomic<u64> m_dirty_pages{0}; // 256-byte pages of the 16 KB micro memory
	std::atomic<bool> m_flush_requested{true};
	std::atomic<u32> m_ee_clears{0};
	std::atomic<size_t> m_committed_cache{0};
};

extern WasmVU1 CpuWasmVU1;

namespace VU1Jit
{
	// Debug options of the headless runner (section 12). Set before the VM starts.
	void SetLockstep(bool enabled); // -vu1jit-lockstep: every program runs on the JIT and on the interpreter
	void SetOracle(bool enabled); // -vu1jit-oracle: the timing model is checked at every VU1 instruction
	void SetStats(bool enabled); // -vu1jit-stats: per-vsync statistics, kept until WriteStats

	// EE thread, once per vsync (the statistics are kept per vsync).
	void OnVsync();

	// After a run, with the VU thread idle: the summary of every enabled mode, as log lines.
	void LogReport();

	// Writes the per-vsync statistics as CSV. False on an I/O error.
	bool WriteStats(const std::string& path);

	// How many threads released their JIT state on exit (thread_local destructors, section 3.5).
	u32 GetThreadExitReleases();

	// Counters for the page (pcsx2web_vu1jit_stats), updated by the VU thread at the end of each Execute.
	struct PublicStats
	{
		std::atomic<u32> selftest; // 0 not run, 1 passed, 2 failed
		std::atomic<u32> executes;
		std::atomic<u32> programs; // compiled modules alive
		std::atomic<u32> compiles;
		std::atomic<u32> entries; // calls into generated code
		std::atomic<u32> instructions; // VU1 instructions run by the dispatcher (generated code and fallbacks)
		std::atomic<u32> fallback_steps; // ... of which in the interpreter
		std::atomic<u32> refused; // lookups refused for the pipeline state
		std::atomic<u32> lockstep_runs;
		std::atomic<u32> lockstep_diffs;
		std::atomic<u32> oracle_checked;
		std::atomic<u32> oracle_diffs;
		std::atomic<u32> ee_clears;
	};
	PublicStats& GetPublicStats();
} // namespace VU1Jit
