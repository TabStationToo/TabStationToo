// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "VU1Jit.h"

#include "MTVU.h"
#include "VU1JitAnalysis.h"
#include "VU1JitFuzz.h"
#include "VU1JitHooks.h"
#include "VU1JitLockstep.h"
#include "WasmEmitter.h"
#include "WasmJitRuntime.h"

#include "common/FileSystem.h"
#include "common/FPControl.h"
#include "common/Threading.h"

#include "fmt/format.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

WasmVU1 CpuWasmVU1;

// Imported by the self-test module (WasmJitRuntime.js binds it as env.h_selftest): proves that generated code calls
// main-module C++ functions directly.
extern "C" EMSCRIPTEN_KEEPALIVE int vu1jit_selftest_helper(int x)
{
	return x * 3 + 1;
}

namespace VU1Jit
{
	static bool s_lockstep = false;
	static bool s_oracle = false;
	static PublicStats s_public_stats;
	static std::atomic<u32> s_thread_exit_releases{0};

	PublicStats& GetPublicStats()
	{
		return s_public_stats;
	}

	u32 GetThreadExitReleases()
	{
		return s_thread_exit_releases.load(std::memory_order_acquire);
	}

	//////////////////////////////////////////////////////////////////////////
	// Per-vsync statistics (section 10.6)

	namespace
	{
		struct VsyncStats
		{
			u32 runs = 0;
			u32 instructions = 0; // run by the dispatcher (WasmVU1 only)
			u32 fallback_steps = 0;
			u64 cycles = 0;
			u64 execute_ns = 0;
			u64 path1_bytes = 0;
			u32 path1_tags = 0;
			u64 path1_hash = 0;
			u32 ebits = 0;
			u32 tbits = 0;
		};
	} // namespace

	static std::atomic<u32> s_vsync{0};
	static std::vector<VsyncStats> s_vsync_stats; // written by the thread that runs VU1, read when it is idle
	static u64 s_run_instructions = 0; // dispatcher counters of the current Execute, for the statistics
	static u64 s_run_fallback_steps = 0;

	void OnVsync()
	{
		s_vsync.fetch_add(1, std::memory_order_relaxed);
	}

	// VU1JitHooks::execute: runs MTVU_VU_EXECUTE's CpuVU1->Execute timed and recorded, for WasmVU1 and the
	// interpreter alike. PATH1 data and interrupts pass through unchanged.
	static void StatsExecute(u32 cycles)
	{
		const u32 vsync = s_vsync.load(std::memory_order_relaxed);
		static thread_local Path1Record path1;
		path1.keep_bytes = false;
		path1.Reset();
		u32 interrupts = 0;
		s_run_instructions = 0;
		s_run_fallback_steps = 0;

		const auto t0 = std::chrono::steady_clock::now();
		{
			RecordingScope scope(&path1, &interrupts, true);
			CpuVU1->Execute(cycles);
		}
		PublishInterrupts(interrupts);
		const auto t1 = std::chrono::steady_clock::now();

		if (s_vsync_stats.size() <= vsync)
			s_vsync_stats.resize(vsync + 1);
		VsyncStats& v = s_vsync_stats[vsync];
		v.runs++;
		v.instructions += static_cast<u32>(s_run_instructions);
		v.fallback_steps += static_cast<u32>(s_run_fallback_steps);
		v.cycles += VU1.cycle; // MTVU_VU_EXECUTE starts every run at cycle 0 (MTVU.cpp:145)
		v.execute_ns += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
		v.path1_bytes += path1.size;
		v.path1_tags += static_cast<u32>(path1.tag_ends.size());
		v.path1_hash = (v.path1_hash ^ path1.hash) * 0x100000001b3ULL + v.runs;
		v.ebits += (interrupts & VU_Thread::InterruptFlagVUEBit) != 0;
		v.tbits += (interrupts & VU_Thread::InterruptFlagVUTBit) != 0;
		path1.tag_ends.clear();
	}

	bool WriteStats(const std::string& path)
	{
		auto fp = FileSystem::OpenManagedCFile(path.c_str(), "wb");
		if (!fp)
			return false;
		std::fprintf(fp.get(), "vsync,runs,instructions,fallback_steps,cycles,execute_us,path1_bytes,path1_tags,path1_hash,ebits,tbits\n");
		for (size_t i = 0; i < s_vsync_stats.size(); i++)
		{
			const VsyncStats& v = s_vsync_stats[i];
			std::fprintf(fp.get(), "%zu,%u,%u,%u,%llu,%.1f,%llu,%u,%016llx,%u,%u\n", i, v.runs, v.instructions, v.fallback_steps,
				static_cast<unsigned long long>(v.cycles), static_cast<double>(v.execute_ns) / 1000.0,
				static_cast<unsigned long long>(v.path1_bytes), v.path1_tags, static_cast<unsigned long long>(v.path1_hash),
				v.ebits, v.tbits);
		}
		return std::ferror(fp.get()) == 0;
	}

	void SetLockstep(bool enabled)
	{
		s_lockstep = enabled;
	}

	void SetOracle(bool enabled)
	{
		s_oracle = enabled;
	}

	void SetStats(bool enabled)
	{
		VU1JitHooks::execute = enabled ? &StatsExecute : nullptr;
	}

	//////////////////////////////////////////////////////////////////////////
	// Programs and the per-thread state (sections 3.5, 4.1, 6)

	namespace
	{
		enum class ExitKind : u8
		{
			Boundary, // the state is complete at the start of the instruction at pc
			End, // the program ended
		};

		enum class ExitReason : u8
		{
			Fallback, // an instruction generated code does not run (step 1a: every instruction)
			Budget,
			BusyKick,
			DTBit,
			UnknownTarget,
			Cap,
			Count
		};

		struct Exit
		{
			ExitKind kind;
			ExitReason reason;
			u32 pc;
		};

		// One module (section 4.1): code reachable from an entry PC, for an entry signature and the content of the
		// micro memory it covers. Step 1a's programs are "all fallback": the module exits at its entry.
		struct Program
		{
			u32 pc;
			Signature sig;
			u32 range_start;
			u32 range_size;
			u64 pages; // 256-byte pages of the micro memory it covers
			std::vector<u8> bytes; // the micro memory it was compiled from
			bool needs_compare; // a Clear touched its pages since it was last compared
			WasmJit::LoadedFunction fn;
			std::vector<Exit> exits;
			u64 runs;
		};

		// Read and written by generated code at its constant address (section 5.4).
		struct alignas(16) Context
		{
			u64 budget_end;
			u32 runs; // incremented by every all-fallback program: proves the call and the constant addresses
			u32 selftest_word;
		};

		// What a flush depends on (section 6.5): settings generated code bakes in, and constant addresses.
		struct SettingsKey
		{
			bool vu0_overflow;
			bool vu1_overflow;
			bool add_sub_hack;
			uptr regs;
			uptr mem;
			uptr micro;

			bool operator==(const SettingsKey& o) const = default;
		};

		static constexpr u32 MAX_PROGRAMS = 2000; // section 6.5; LRU eviction is step 4
		static constexpr u32 NUM_PCS = VU1_PROGSIZE / 8;

		class ThreadState
		{
		public:
			ThreadState();
			~ThreadState();

			// Per Execute: the self-test on first use, flush requests and the settings key, program-memory changes.
			// False when the JIT cannot run (the interpreter then runs this Execute).
			bool Prepare();

			void Dispatch(u32 cycles);
			void ExecuteLockstep(u32 cycles);

			TimingOracle& Oracle() { return m_oracle; }
			u32 LockstepRuns() const { return m_lockstep_runs; }
			u32 LockstepDifferences() const { return m_lockstep_diffs; }
			void LogSummary() const;

		private:
			bool SelfTest();
			void Flush();
			Program* Find(u32 pc, bool allow_compile);
			Program* Compile(u32 pc, const Signature& sig);
			void Drop(Program* p);
			const Exit& Run(Program* p);
			void StepInterpreter();
			void PublishStats();

			Context m_ctx = {};
			std::array<std::vector<Program*>, NUM_PCS> m_by_pc; // most recently used first
			std::vector<std::unique_ptr<Program>> m_programs;
			SettingsKey m_key = {};
			u32 m_seen_gen = ~0u;
			bool m_selftest_done = false;
			bool m_usable = false;
			bool m_out_of_memory_once = false;

			TimingOracle m_oracle;
			std::unique_ptr<RunState> m_ls_start;
			std::unique_ptr<RunState> m_ls_jit;
			std::unique_ptr<RunState> m_ls_ref;
			u32 m_lockstep_runs = 0;
			u32 m_lockstep_diffs = 0;

			// Statistics.
			u64 m_executes = 0;
			u64 m_compiles = 0;
			double m_compile_ms = 0;
			u64 m_entries = 0;
			u64 m_instructions = 0;
			u64 m_fallback_steps = 0;
			u64 m_refused = 0;
			u64 m_exits[static_cast<size_t>(ExitReason::Count)] = {};
			u64 m_bad_runs = 0;
			u64 m_dropped = 0;
			u64 m_flushes = 0;
		};

		// Alive ThreadStates, for the report. Never destroyed: the VU thread's state goes when that thread exits, which
		// can be after static destructors ran (VU_Thread's own destructor joins it).
		struct StateRegistry
		{
			std::mutex mutex;
			std::vector<ThreadState*> states;
		};
		StateRegistry& GetRegistry()
		{
			static StateRegistry* registry = new StateRegistry();
			return *registry;
		}
	} // namespace

	static ThreadState& GetThreadState()
	{
		// thread_local: generated code lives in one thread's table (section 3.5). The destructor releases every module
		// when the thread exits.
		static thread_local ThreadState state;
		return state;
	}

	ThreadState::ThreadState()
	{
		StateRegistry& r = GetRegistry();
		std::unique_lock lock(r.mutex);
		r.states.push_back(this);
	}

	ThreadState::~ThreadState()
	{
		const size_t programs = m_programs.size();
		Flush();
		{
			StateRegistry& r = GetRegistry();
			std::unique_lock lock(r.mutex);
			const auto it = std::find(r.states.begin(), r.states.end(), this);
			if (it != r.states.end())
				r.states.erase(it);
		}
		if (programs || m_selftest_done)
			s_thread_exit_releases.fetch_add(1, std::memory_order_release);
	}

	static SettingsKey CurrentSettingsKey()
	{
		SettingsKey k;
		k.vu0_overflow = EmuConfig.Cpu.Recompiler.vu0Overflow;
		k.vu1_overflow = EmuConfig.Cpu.Recompiler.vu1Overflow;
		k.add_sub_hack = EmuConfig.Gamefixes.VuAddSubHack;
		k.regs = reinterpret_cast<uptr>(&VU1);
		k.mem = reinterpret_cast<uptr>(VU1.Mem);
		k.micro = reinterpret_cast<uptr>(VU1.Micro);
		return k;
	}

	bool ThreadState::SelfTest()
	{
		// A module that imports the shared memory and a C++ helper, reads a word at its constant address and is called
		// through this thread's table (sections 3.5, 5.2, 5.3).
		using namespace WasmJit;
		Module m;
		const u32 type = m.type({ValType::I32}, {ValType::I32});
		m.import_memory("env", "mem", 0, 65536, true);
		const u32 helper = m.import_function("env", "h_selftest", type);
		Function f(1);
		f.local_get(0);
		f.call(helper);
		f.i32_const(0);
		f.i32_load(static_cast<u32>(reinterpret_cast<uptr>(&m_ctx.selftest_word)));
		f.i32_add();
		m.export_function("run", m.add_function(type, f));
		const std::vector<u8> bytes = m.finish();

		char* engine = wasmjit_engine();
		const std::string engine_name = engine ? engine : "unknown engine";
		std::free(engine);

		const auto t0 = std::chrono::steady_clock::now();
		LoadedFunction fn;
		const LoadResult result = Load(bytes, "run", &fn);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		bool ok = false;
		s32 value = 0;
		if (result == LoadResult::Ok)
		{
			m_ctx.selftest_word = 0x1000;
			value = reinterpret_cast<s32 (*)(s32)>(static_cast<uptr>(fn.slot))(5);
			ok = (value == 0x1000 + 16);
			Unload(&fn);
		}
		s_public_stats.selftest.store(ok ? 1 : 2, std::memory_order_relaxed);
		if (ok)
			Console.WriteLn(fmt::format("VU1 JIT: self-test passed on {} (compile, instantiate and table slot {:.2f} ms)", engine_name, ms));
		else
			Console.Error(fmt::format("VU1 JIT: self-test failed on {} (load result {}, value {:#x}); VU1 runs on the interpreter",
				engine_name, static_cast<int>(result), value));
		return ok;
	}

	void ThreadState::Flush()
	{
		for (std::unique_ptr<Program>& p : m_programs)
			WasmJit::Unload(&p->fn);
		m_programs.clear();
		for (std::vector<Program*>& list : m_by_pc)
			list.clear();
		m_flushes++;
		CpuWasmVU1.SetCommittedCache(0);
	}

	bool ThreadState::Prepare()
	{
		if (!m_selftest_done)
		{
			m_selftest_done = true;
			m_usable = SelfTest();
		}
		if (!m_usable)
			return false;

		// Settings generated code depends on (section 7.8): the JIT runs only with both overflow clamps on.
		if (!EmuConfig.Cpu.Recompiler.vu0Overflow || !EmuConfig.Cpu.Recompiler.vu1Overflow)
			return false;

		const SettingsKey key = CurrentSettingsKey();
		if (CpuWasmVU1.TakeFlushRequest() || key != m_key)
		{
			Flush();
			m_key = key;
		}

		// Program-memory changes (section 6.2). Clear ORs the pages before it bumps the generation, so a new
		// generation always comes with its pages; a Clear that lands after this is seen at the next Execute.
		const u32 gen = CpuWasmVU1.GetMicroGeneration();
		if (gen != m_seen_gen)
		{
			m_seen_gen = gen;
			const u64 dirty = CpuWasmVU1.TakeDirtyPages();
			for (std::unique_ptr<Program>& p : m_programs)
			{
				if (p->pages & dirty)
					p->needs_compare = true;
			}
		}
		return true;
	}

	void ThreadState::Drop(Program* p)
	{
		std::vector<Program*>& list = m_by_pc[p->pc >> 3];
		list.erase(std::find(list.begin(), list.end(), p));
		WasmJit::Unload(&p->fn);
		m_programs.erase(std::find_if(m_programs.begin(), m_programs.end(), [p](const auto& q) { return q.get() == p; }));
		m_dropped++;
	}

	Program* ThreadState::Find(u32 pc, bool allow_compile)
	{
		std::vector<Program*>& list = m_by_pc[pc >> 3];
		if (list.empty() && !allow_compile)
			return nullptr;

		Signature sig;
		if (DeriveSignature(VU1, &sig) != Refusal::None)
		{
			m_refused++;
			return nullptr;
		}

		for (size_t i = 0; i < list.size(); i++)
		{
			Program* p = list[i];
			if (p->sig != sig)
				continue;
			if (p->needs_compare)
			{
				if (std::memcmp(p->bytes.data(), &VU1.Micro[p->range_start], p->range_size) != 0)
				{
					Drop(p);
					i--;
					continue;
				}
				p->needs_compare = false;
			}
			if (i != 0)
				std::rotate(list.begin(), list.begin() + i, list.begin() + i + 1);
			return p;
		}
		return allow_compile ? Compile(pc, sig) : nullptr;
	}

	// The all-fallback module of step 1a: ctx.runs++, then exit 0, a BOUNDARY exit at the entry.
	static std::vector<u8> BuildFallbackModule(u32 runs_address)
	{
		using namespace WasmJit;
		Module m;
		const u32 type = m.type({ValType::I32}, {ValType::I32});
		m.import_memory("env", "mem", 0, 65536, true);
		Function f(1);
		f.i32_const(0);
		f.i32_const(0);
		f.i32_load(runs_address);
		f.i32_const(1);
		f.i32_add();
		f.i32_store(runs_address);
		f.i32_const(0);
		m.export_function("run", m.add_function(type, f));
		return m.finish();
	}

	Program* ThreadState::Compile(u32 pc, const Signature& sig)
	{
		if (m_programs.size() >= MAX_PROGRAMS)
			Flush();

		auto p = std::make_unique<Program>();
		p->pc = pc;
		p->sig = sig;
		p->range_start = pc;
		p->range_size = 8; // step 1a: the entry instruction only
		p->pages = u64(1) << (pc >> 8);
		p->bytes.assign(&VU1.Micro[pc], &VU1.Micro[pc] + p->range_size);
		p->needs_compare = false;
		p->exits.push_back({ExitKind::Boundary, ExitReason::Fallback, pc});
		p->runs = 0;

		const auto t0 = std::chrono::steady_clock::now();
		const std::vector<u8> bytes = BuildFallbackModule(static_cast<u32>(reinterpret_cast<uptr>(&m_ctx.runs)));
		WasmJit::LoadResult result = WasmJit::Load(bytes, "run", &p->fn);
		if (result == WasmJit::LoadResult::OutOfMemory && !m_out_of_memory_once)
		{
			// Section 6.5: flush and retry once; a second failure switches the JIT off for this thread.
			m_out_of_memory_once = true;
			Console.Warning("VU1 JIT: out of executable memory, flushing the program cache");
			Flush();
			result = WasmJit::Load(bytes, "run", &p->fn);
		}
		m_compile_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (result != WasmJit::LoadResult::Ok)
		{
			Console.Error(fmt::format("VU1 JIT: a module failed to load (result {}); VU1 runs on the interpreter", static_cast<int>(result)));
			m_usable = false;
			return nullptr;
		}
		m_compiles++;

		Program* raw = p.get();
		m_programs.push_back(std::move(p));
		std::vector<Program*>& list = m_by_pc[pc >> 3];
		list.insert(list.begin(), raw);
		CpuWasmVU1.SetCommittedCache(m_programs.size() * 20 * _1kb); // about 20 KB per module (section 6.5)
		return raw;
	}

	const Exit& ThreadState::Run(Program* p)
	{
		m_entries++;
		p->runs++;
		const u32 runs = m_ctx.runs;
		const u32 id = static_cast<u32>(reinterpret_cast<s32 (*)(s32)>(static_cast<uptr>(p->fn.slot))(0));
		if (m_ctx.runs != runs + 1 || id >= p->exits.size())
		{
			// The generated code did not run as built. Count it, and take the safe exit.
			m_bad_runs++;
			return p->exits[0];
		}
		return p->exits[id];
	}

	void ThreadState::StepInterpreter()
	{
		if (s_oracle)
			m_oracle.Before();
		CpuIntVU1.Step();
		if (s_oracle)
			m_oracle.After();
		m_instructions++;
		m_fallback_steps++;
		s_run_instructions++;
		s_run_fallback_steps++;
	}

	// The dispatcher (section 3.4): InterpVU1::Execute's loop (VU1microInterp.cpp:296-323), calling generated code at
	// known entries and stepping the interpreter everywhere else.
	void ThreadState::Dispatch(u32 cycles)
	{
		const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU1FPCR); // no effect on wasm; as the interpreter
		m_executes++;
		if (s_oracle)
			m_oracle.BeginRun(vu1Thread.vuFBRST);

		VU1.VI[REG_TPC].UL <<= 3;
		const u64 start = VU1.cycle;
		vu1InterpMTVURunning = true;
		m_ctx.budget_end = start + cycles;
		bool interpreter_only = false; // set by a budget exit: the interpreter finishes this Execute
		bool first_lookup = true; // step 1a compiles programs at the entry of an Execute only
		while ((VU1.cycle - start) < cycles)
		{
			if (!vu1InterpMTVURunning)
			{
				// The interpreter's loop head (VU1microInterp.cpp:309-317): a branch taken in the E-bit delay slot.
				if (VU1.branch == 1)
				{
					VU1.VI[REG_TPC].UL = VU1.branchpc;
					VU1.branch = 0;
				}
				break;
			}

			Program* p = nullptr;
			if (!interpreter_only && VU1.branch == 0 && VU1.ebit == 0) // never enter inside a delay slot
				p = Find(VU1.VI[REG_TPC].UL & VU1_PROGMASK, first_lookup);
			first_lookup = false;
			if (!p)
			{
				StepInterpreter();
				continue;
			}

			const Exit& x = Run(p);
			if (x.kind == ExitKind::End)
			{
				// Generated code ended the program (step 1b): flush the kick and report the E bit, as _vu1Exec does
				// after its _vuFlushAll (VU1microInterp.cpp:226-240).
				if (VU1.xgkickenable)
					_vuXGKICKTransfer(0, true);
				PublishInterrupts(VU_Thread::InterruptFlagVUEBit);
				continue;
			}
			m_exits[static_cast<u32>(x.reason)]++;
			if (x.reason == ExitReason::Budget)
				interpreter_only = true;
			else
				StepInterpreter(); // always progress: the exit point may itself be an entry
		}
		VU1.VI[REG_TPC].UL >>= 3;
		PublishStats();
	}

	// Section 9.4: the program on the JIT, with the GIF and the interrupts recorded, then on the interpreter from the
	// same state, which the game continues from.
	void ThreadState::ExecuteLockstep(u32 cycles)
	{
		if (!m_ls_start)
		{
			m_ls_start = std::make_unique<RunState>();
			m_ls_jit = std::make_unique<RunState>();
			m_ls_ref = std::make_unique<RunState>();
		}
		CaptureRunState(m_ls_start.get());

		m_ls_jit->path1.Reset();
		m_ls_jit->interrupts = 0;
		{
			RecordingScope scope(&m_ls_jit->path1, &m_ls_jit->interrupts, false);
			Dispatch(cycles);
		}
		CaptureRunState(m_ls_jit.get());

		RestoreRunState(*m_ls_start);
		m_ls_ref->path1.Reset();
		m_ls_ref->interrupts = 0;
		{
			RecordingScope scope(&m_ls_ref->path1, &m_ls_ref->interrupts, true);
			CpuIntVU1.Execute(cycles);
		}
		CaptureRunState(m_ls_ref.get());

		m_lockstep_runs++;
		const std::string diff = CompareRunStates(*m_ls_jit, *m_ls_ref);
		if (!diff.empty())
		{
			if (m_lockstep_diffs++ < 20)
			{
				const RunState& s = *m_ls_start;
				Console.Error(fmt::format("VU1 JIT lockstep: run {} from pc {:#06x} (cycle {}, branch {}, ebit {}) differs: {}",
					m_lockstep_runs, (s.regs.VI[REG_TPC].UL << 3) & VU1_PROGMASK, s.regs.cycle, s.regs.branch, s.regs.ebit, diff));
			}
		}
		PublishInterrupts(m_ls_ref->interrupts);
		s_public_stats.lockstep_runs.store(m_lockstep_runs, std::memory_order_relaxed);
		s_public_stats.lockstep_diffs.store(m_lockstep_diffs, std::memory_order_relaxed);
	}

	void ThreadState::PublishStats()
	{
		PublicStats& s = s_public_stats;
		s.executes.store(static_cast<u32>(m_executes), std::memory_order_relaxed);
		s.programs.store(static_cast<u32>(m_programs.size()), std::memory_order_relaxed);
		s.compiles.store(static_cast<u32>(m_compiles), std::memory_order_relaxed);
		s.entries.store(static_cast<u32>(m_entries), std::memory_order_relaxed);
		s.instructions.store(static_cast<u32>(m_instructions), std::memory_order_relaxed);
		s.fallback_steps.store(static_cast<u32>(m_fallback_steps), std::memory_order_relaxed);
		s.refused.store(static_cast<u32>(m_refused), std::memory_order_relaxed);
		s.oracle_checked.store(static_cast<u32>(m_oracle.GetCounters().checked), std::memory_order_relaxed);
		s.oracle_diffs.store(static_cast<u32>(m_oracle.GetCounters().differences + m_oracle.GetCounters().chain_differences),
			std::memory_order_relaxed);
		s.ee_clears.store(CpuWasmVU1.GetEEThreadClears(), std::memory_order_relaxed);
	}

	void ThreadState::LogSummary() const
	{
		static constexpr const char* reasons[] = {"fallback", "budget", "busy kick", "D/T bit", "unknown target", "cap"};
		static_assert(std::size(reasons) == static_cast<size_t>(ExitReason::Count));
		std::string exits;
		for (u32 i = 0; i < static_cast<u32>(ExitReason::Count); i++)
		{
			if (m_exits[i])
				exits += fmt::format("{}{} {}", exits.empty() ? "" : ", ", reasons[i], m_exits[i]);
		}
		Console.WriteLn(fmt::format("VU1 JIT: {} executes, {} programs compiled ({:.1f} ms), {} alive, {} dropped after a "
									"program-memory change, {} flushes; {} entries into generated code, {} bad returns; "
									"{} instructions, {} of them on the interpreter; exits: {}; {} lookups refused for "
									"the pipeline state; {} EE-thread Clears",
			m_executes, m_compiles, m_compile_ms, m_programs.size(), m_dropped, m_flushes, m_entries, m_bad_runs,
			m_instructions, m_fallback_steps, exits.empty() ? "none" : exits, m_refused, CpuWasmVU1.GetEEThreadClears()));
		if (s_oracle)
			m_oracle.LogSummary("VU1 JIT timing oracle");
		if (s_lockstep)
			Console.WriteLn(fmt::format("VU1 JIT lockstep: {} runs compared, {} differences", m_lockstep_runs, m_lockstep_diffs));
	}

	void LogReport()
	{
		StateRegistry& r = GetRegistry();
		std::unique_lock lock(r.mutex);
		for (const ThreadState* s : r.states)
			s->LogSummary();
		if (r.states.empty())
			Console.WriteLn("VU1 JIT: not used in this run");
	}
} // namespace VU1Jit

//////////////////////////////////////////////////////////////////////////
// WasmVU1

WasmVU1::WasmVU1()
{
	m_Idx = 1;
	IsInterpreter = false;
}

WasmVU1::~WasmVU1() = default;

size_t WasmVU1::GetCommittedCache() const
{
	return m_committed_cache.load(std::memory_order_relaxed);
}

void WasmVU1::SetCommittedCache(size_t bytes)
{
	m_committed_cache.store(bytes, std::memory_order_relaxed);
}

void WasmVU1::Shutdown()
{
	// The modules belong to the VU thread: it flushes them at its next Execute, or releases them when it exits.
	if (vu1Thread.IsOpen())
		vu1Thread.WaitVU();
	m_flush_requested.store(true, std::memory_order_release);
}

void WasmVU1::Reset()
{
	// EE thread (ClearCPUExecutionCaches: boot, savestate load, CPU/gamefix/speedhack changes). As the interpreter's
	// own Reset, which zeroes the FMAC and IALU ring positions and counts, once the VU thread is idle.
	if (vu1Thread.IsOpen())
		vu1Thread.WaitVU();
	CpuIntVU1.Reset();
	m_flush_requested.store(true, std::memory_order_release);
}

void WasmVU1::SetStartPC(u32 startPC)
{
	VU1.start_pc = startPC;
}

void WasmVU1::Step()
{
	CpuIntVU1.Step();
}

void WasmVU1::Clear(u32 addr, u32 size)
{
	// Called before the bytes are written (MTVU.cpp:164-165), sometimes on the EE thread: only atomics here.
	if (size == 0)
		return;
	addr &= VU1_PROGMASK;
	const u32 end = std::min<u32>(addr + size, VU1_PROGSIZE);
	const u32 first = addr >> 8;
	const u32 last = (end - 1) >> 8;
	const u64 pages = ((last == 63) ? ~u64(0) : ((u64(1) << (last + 1)) - 1)) & ~((u64(1) << first) - 1);
	m_dirty_pages.fetch_or(pages, std::memory_order_release);
	m_micro_gen.fetch_add(1, std::memory_order_release);

	if (vu1Thread.IsOpen() &&
		static_cast<void*>(Threading::ThreadHandle::GetForCallingThread()) != static_cast<void*>(vu1Thread.GetThreadHandle()))
	{
		m_ee_clears.fetch_add(1, std::memory_order_relaxed);
	}
}

void WasmVU1::Execute(u32 cycles)
{
	// Without MTVU the JIT is never used (section 2.3): this runs on the EE thread then.
	if (!THREAD_VU1)
		return CpuIntVU1.Execute(cycles);

	VU1Jit::ThreadState& t = VU1Jit::GetThreadState();
	if (!t.Prepare())
		return CpuIntVU1.Execute(cycles);

	if (VU1Jit::s_lockstep)
		t.ExecuteLockstep(cycles);
	else
		t.Dispatch(cycles);
}
