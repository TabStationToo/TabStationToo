// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU1 JIT test harness (docs/web-port/VU1-JIT-DESIGN.md 9.2, 9.3), shared by the headless runner and the page:
//
// - TimingOracle checks the timing model (VU1JitAnalysis.h) at every instruction the interpreter runs: the cycle
//   count, the pipeline signature, the control state and the data the pipe write-backs leave in VI[STATUS], VI[MAC],
//   VI[CLIP], VI[Q] and VI[P]. It runs on random programs here, and on real runs from the dispatcher
//   (-vu1jit-oracle).
// - The fuzzer runs random programs without a BIOS or a VM, on its own thread: "timing" checks them with the oracle,
//   "jit" runs each one on the interpreter and on WasmVU1 from the same state and compares the results by the
//   contract (section 9.1).

#pragma once

#include "VU1JitAnalysis.h"

#include <atomic>
#include <string>

namespace VU1Jit
{
	struct OracleCounters
	{
		u64 steps = 0; // instructions seen
		u64 checked = 0; // ... checked against the model (the state before them had a signature)
		u64 refused[static_cast<size_t>(Refusal::Count)] = {};
		u64 unknown = 0; // instructions with unknown words (not modelled)
		u64 differences = 0;
		u64 chained = 0; // checked against a model carried along the run without re-deriving its state
		u64 chain_differences = 0;

		// Coverage: how often each timing case was seen.
		u64 upper_stalls = 0;
		u64 lower_stalls = 0;
		u64 fdiv_waits = 0;
		u64 efu_waits = 0;
		u64 ialu_waits = 0;
		u64 fmac_writebacks = 0;
		u64 fdiv_writebacks = 0;
		u64 efu_writebacks = 0;
		u64 ibits = 0;
		u64 discards = 0;
		u64 old_vf = 0;
		u64 old_clip = 0;
		u64 vi_backups = 0;
		u64 vi_backups_chained = 0;
		u64 vi_old_reads = 0; // IBxx that read VIOldValue
		u64 branches_taken = 0;
		u64 delay_branches = 0; // a branch in a branch delay slot
		u64 ebit_in_branch_delay = 0;
		u64 branch_in_ebit_delay = 0;
		u64 ebit_rearms = 0;
		u64 dt_stops = 0;
		u64 ends = 0;
		u64 kicks = 0;
		u64 busy_kicks = 0;
		u64 links = 0;

		void Add(const OracleCounters& o);
	};

	// Section 9.2. Before() and After() bracket one CpuIntVU1.Step() on the thread that runs VU1.
	class TimingOracle
	{
	public:
		void BeginRun(u32 fbrst); // a new Execute: the state may have changed outside a step
		void Invalidate(); // the state changed outside a step
		void Before();
		void After();

		const OracleCounters& GetCounters() const { return m_counters; }
		void SetLabel(const char* label) { m_label = label; }
		void SetMaxReports(u32 n) { m_max_reports = n; }

		// The summary as log lines, prefixed with what.
		void LogSummary(const char* what) const;

	private:
		void Report(const std::string& what);

		VURegs m_pre;
		bool m_pre_running = false;
		Signature m_pre_sig = {};
		Refusal m_pre_refusal = Refusal::None;
		Control m_pre_ctl;
		u32 m_pc = 0;
		u32 m_lower = 0;
		u32 m_upper = 0;
		u32 m_fbrst = 0;

		TimingModel m_model;
		TimingModel m_chain;
		bool m_chain_valid = false;

		OracleCounters m_counters;
		u32 m_reports = 0;
		u32 m_max_reports = 10;
		const char* m_label = "";
	};

	// The fuzzer. spec: "SEED:COUNT[:MODE]" with MODE timing (the default), jit, or both.
	struct FuzzStatus
	{
		std::atomic<int> state{0}; // 0 idle, 1 running, 2 passed, 3 differences found, 4 bad spec or setup failure
		std::atomic<u32> done{0}; // programs
		std::atomic<u32> total{0};
		std::atomic<u32> differences{0};
		std::atomic<u64> instructions{0};
		char summary[512] = {};
	};
	FuzzStatus& GetFuzzStatus();

	// Runs the fuzzer on a new thread and waits for it. Returns 0 if every check passed. Needs no VM: it must not
	// run while one does (it uses the global VU1 and saves and restores it).
	int RunFuzz(const std::string& spec);

	// The same without waiting (the page's fuzz entry). False if a run is in progress.
	bool StartFuzz(const std::string& spec);
} // namespace VU1Jit
