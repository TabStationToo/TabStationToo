// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "wasm/PerfCounters.h"

#include "VU1Jit.h"

#include "MTVU.h"
#include "VU1JitAnalysis.h"
#include "VU1JitEmit.h"
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
#include <unordered_map>
#include <unordered_set>
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
	static constexpr u32 DEFAULT_VERSIONS_PER_PC = 16;
	static std::atomic<u32> s_versions_per_pc{DEFAULT_VERSIONS_PER_PC};
	static std::atomic<u32> s_cache_report_every{0};
	static std::atomic<u32> s_module_block_limit{0};

	void ConfigureCache(u32 versions, u32 report_every, u32 block_limit)
	{
		s_versions_per_pc.store(versions == 32 || versions == 64 ? versions : DEFAULT_VERSIONS_PER_PC, std::memory_order_relaxed);
		s_module_block_limit.store(block_limit == 16 || block_limit == 64 || block_limit == 128 || block_limit == 256 ? block_limit : 0, std::memory_order_relaxed);
		s_cache_report_every.store(report_every >= 10 && report_every <= 600 ? report_every : 0, std::memory_order_relaxed);
	}

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
		struct Program;

		// The targets a JR or JALR went to (section 6.4), per block (PC and signature) that it ends: modules compiled
		// after they were seen go to those that are blocks of the module without leaving generated code.
		struct Jump
		{
			u32 pc;
			Signature sig;
			std::vector<u32> targets; // TPC values (byte addresses, unmasked), in the order they were first seen
		};
		static constexpr u32 MAX_JUMP_TARGETS = 16;
		// Merging: a module is compiled again after MergeExits() << (2 * generation) exits to other modules, the first
		// time, then only if its jumps learned targets since; at most MAX_MERGES times from the same entry, and while
		// merges took less than MERGE_BUDGET_MS plus MERGE_BUDGET_SHARE of the time since the thread's first Execute.
		static constexpr u32 MAX_MERGES = 6;
		static constexpr double MERGE_BUDGET_MS = 250.0;
		static constexpr double MERGE_BUDGET_SHARE = 0.01;
		static u64 MergeExits()
		{
			// VU1JIT_MERGE_EXITS: lower for the fuzzer, whose programs run once.
			static const u64 n = [] { const char* e = std::getenv("VU1JIT_MERGE_EXITS"); return e ? std::strtoull(e, nullptr, 10) : 1024; }();
			return n;
		}

		static bool ChainingEnabled() // VU1JitEmit.cpp's switch: without it, modules hold one block each
		{
			static const bool enabled = !std::getenv("VU1JIT_NOCHAIN");
			return enabled;
		}

		struct ExitSlot
		{
			BlockExit x;
			Program* next = nullptr; // the block that follows this exit, while next_epoch is current
			u32 next_epoch = 0;
			Jump* jump = nullptr; // a JR/JALR's learned targets, once it went to one
			u64 count = 0; // times generated code left by it (VU1JIT_EXITS lists the most common)
			std::vector<std::pair<u32, u64>> targets; // VU1JIT_EXITS: a JR/JALR's targets
		};

		// A module (VU1JitEmit.h, CompileModule): the blocks it holds and the micro memory they cover. A module
		// without a function marks a (PC, signature) whose first instruction runs on the interpreter.
		struct Module
		{
			struct Range
			{
				u32 start;
				u32 size;
			};
			std::vector<Range> ranges; // one per block: the block's PC and size
			std::vector<Signature> sigs; // one per block: its entry signature
			std::vector<u8> entries; // one per block: whether the dispatcher may start there (not in a delay slot)
			std::vector<u8> bytes; // the micro memory it was compiled from, range after range
			u64 pages; // 256-byte pages of the micro memory it covers
			bool needs_compare; // a Clear touched its pages since it was last compared
			bool mismatch; // the last compare failed: other code is there, until a Clear touches its pages again
			WasmJit::LoadedFunction fn;
			std::vector<ExitSlot> exits;
			size_t wasm_bytes; // generated binary bytes, excluding host executable memory
			u64 diagnostic_identity = 0;
			size_t owning_slot;
			u32 programs; // its blocks still listed (Program): it is released with the last one
			// Merging (ThreadState::Merge): exits to other modules since it was compiled, how many times its entry was
			// recompiled into the module it is, and how many targets its jumps had learned then (JumpTargetCount).
			u64 chain_exits;
			u32 generation;
			u32 jump_targets;
		};

		// A block of a module, listed per entry PC: the dispatcher can start the module's function at any of them.
		struct Program
		{
			u32 pc;
			Signature sig;
			Module* module;
			u32 index; // the block's index in the module
			u32 last_start; // budget test: cycles from the entry to its last instruction
			u64 runs;
			size_t owning_slot;
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

		// Section 6.5 (LRU eviction is step 4): the cache is flushed at MAX_MODULES modules or MAX_PROGRAMS blocks listed
		// (a merged module lists all of its blocks).
		static constexpr u32 MAX_MODULES = 2000;
		static constexpr u32 MAX_PROGRAMS = 8192;
		static constexpr size_t MAX_LIMITED_WASM_BYTES = 128 * _1mb;
		static constexpr u32 NUM_PCS = VU1_PROGSIZE / 8;
		enum class DropReason : u8
		{
			FindLimit,
			InstallLimit,
			Merge
		};
		enum class FlushReason : u8
		{
			Reset,
			Settings,
			Modules,
			Entries,
			OutOfMemory,
			Bytes,
			Exit
		};

		using CacheClock = std::chrono::steady_clock;

		static u64 ElapsedNs(CacheClock::time_point start)
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(CacheClock::now() - start).count();
		}

		// Diagnostic fingerprints only. Lookup still compares the complete signature and microcode bytes.
		static u64 Fingerprint(u64 hash, const void* data, size_t size)
		{
			const auto* bytes = static_cast<const u8*>(data);
			for (size_t i = 0; i < size; i++)
				hash = (hash ^ bytes[i]) * 0x100000001b3ULL;
			return hash;
		}

		struct CacheDiagnostics
		{
			static constexpr size_t HISTORY_LIMIT = 32768;
			const u32 interval;
			u32 next_report = 0;
			u64 hits = 0, misses = 0, new_signature = 0, mismatched = 0;
			u64 first_compile = 0, recompile = 0, without_eviction = 0, untracked = 0;
			u64 merged_compiles = 0, compiled_blocks = 0, compiled_bytes = 0;
			u64 generate_ns = 0, manage_ns = 0, drop_ns = 0, merge_ns = 0, flush_ns = 0;
			WasmJit::LoadTiming load;
			std::array<u64, 3> drops{};
			std::array<u64, 7> flushes{};
			std::array<u64, NUM_PCS> drops_by_pc{}, repeats_by_pc{};
			std::unordered_map<u64, bool> history; // true once an entry of this generated identity was removed
			std::unordered_set<u64> signatures;

			explicit CacheDiagnostics(u32 report_every)
				: interval(report_every)
			{
			}

			void OnMiss(u32 pc, const Signature& sig, bool same_signature)
			{
				misses++;
				mismatched += same_signature;
				if (signatures.size() < HISTORY_LIMIT)
					new_signature += signatures.insert(Fingerprint(pc, &sig, sizeof(sig))).second;
			}

			void OnCompile(u32 pc, u64 identity, const CompiledModule& cm, bool merge)
			{
				compiled_blocks += cm.blocks.size();
				compiled_bytes += cm.wasm.size();
				merged_compiles += merge;
				auto it = history.find(identity);
				if (it == history.end())
				{
					if (history.size() == HISTORY_LIMIT)
						untracked++;
					else
					{
						history.emplace(identity, false);
						first_compile++;
					}
				}
				else if (std::exchange(it->second, false))
				{
					recompile++;
					repeats_by_pc[pc >> 3]++;
				}
				else
					without_eviction++;
			}

			void Removed(const Module& module)
			{
				if (const auto it = history.find(module.diagnostic_identity); it != history.end())
					it->second = true;
			}
		};

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
			void Flush(FlushReason reason);
			Program* Find(u32 pc, const Signature& sig);
			Program* Lookup(u32 pc, const Signature& sig, bool* matched_signature = nullptr); // Find without compiling: null if no valid module has it
			static bool Known(void* ctx, u32 pc, const Signature& sig); // for CompileModule
			static const std::vector<u32>* JumpTargets(void* ctx, u32 pc, const Signature& sig); // for CompileModule
			Jump* FindJump(u32 pc, const Signature& sig, bool create);
			bool Matches(Module* m); // the module's code is the micro memory's (section 6.2)
			Program* FindCurrent(u32 pc); // with the signature of VU1's state, null if refused
			Program* Compile(u32 pc, const Signature& sig);
			// Loads a compiled module and lists its blocks; returns its entry block. have_code false: a marker.
			Program* Install(const CompiledModule& cm, bool have_code, u32 generation, bool merge = false);
			// Recompiles m's entry with every block reachable from it, other modules' included, and the jump targets
			// learned since; drops the modules the new one holds entirely.
			void Merge(Module* m);
			bool WantsMerge(Module* m); // after m's exits to other modules reached the threshold
			u32 JumpTargetCount(Module* m); // the targets m's jumps have learned so far
			void Drop(Program* p, DropReason reason);
			void ReportCache();
			const ExitSlot* Run(Program* p);
			void StepInterpreter();
			void PublishStats();

			const u32 m_versions_per_pc = s_versions_per_pc.load(std::memory_order_relaxed);
			const u32 m_block_limit = s_module_block_limit.load(std::memory_order_relaxed);
			std::unique_ptr<CacheDiagnostics> m_diag;
			size_t m_live_wasm_bytes = 0;
			Context m_ctx = {};
			std::array<std::vector<Program*>, NUM_PCS> m_by_pc; // most recently used first
			std::vector<std::unique_ptr<Program>> m_programs;
			std::vector<std::unique_ptr<Module>> m_modules;
			std::vector<std::unique_ptr<Jump>> m_jumps_learned;
			std::chrono::steady_clock::time_point m_started; // the first Prepare (the merges' time budget)
			bool m_started_set = false;
			struct MergedEntry
			{
				u32 pc;
				Signature sig;
				u32 generation;
			};
			std::vector<MergedEntry> m_merged_entries; // how often each entry was merged, past the modules themselves
			u64 m_run_instrs = 0; // instructions the last Run ran
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
			u64 m_native_ops = 0;
			u64 m_helper_ops = 0;
			u64 m_jit_instructions = 0;
			u64 m_edges = 0; // blocks and loop passes followed by the next without leaving generated code
			u64 m_jumps = 0; // block exits at JR/JALR
			u64 m_blocks_compiled = 0;
			u64 m_module_edges = 0; // block exits that go to a block of their module
			u64 m_merges = 0;
			double m_merge_ms = 0;
			u32 m_epoch = 1; // changes whenever a Program may have gone or become stale (chained exits)
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
		if (const u32 interval = s_cache_report_every.load(std::memory_order_relaxed))
			m_diag = std::make_unique<CacheDiagnostics>(interval);
		StateRegistry& r = GetRegistry();
		std::unique_lock lock(r.mutex);
		r.states.push_back(this);
	}

	ThreadState::~ThreadState()
	{
		const size_t programs = m_programs.size();
		Flush(FlushReason::Exit);
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
		WasmJit::Module m;
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

	void ThreadState::Flush(FlushReason reason)
	{
		wasmjit_clear_vu1_modules();
		const auto start = m_diag ? CacheClock::now() : CacheClock::time_point{};
		if (m_diag)
		{
			m_diag->flushes[static_cast<size_t>(reason)]++;
			for (const auto& module : m_modules)
				m_diag->Removed(*module);
		}
		m_live_wasm_bytes = 0;
		for (std::unique_ptr<Module>& m : m_modules)
			WasmJit::Unload(&m->fn);
		m_modules.clear();
		m_programs.clear();
		m_jumps_learned.clear();
		m_merged_entries.clear();
		for (std::vector<Program*>& list : m_by_pc)
			list.clear();
		m_flushes++;
		m_epoch++;
		CpuWasmVU1.SetCommittedCache(0);
		if (m_diag)
			m_diag->flush_ns += ElapsedNs(start);
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
		if (!m_started_set)
		{
			m_started_set = true;
			m_started = std::chrono::steady_clock::now();
		}

		// Settings generated code depends on (section 7.8): the JIT runs only with both overflow clamps on.
		if (!EmuConfig.Cpu.Recompiler.vu0Overflow || !EmuConfig.Cpu.Recompiler.vu1Overflow)
			return false;

		const SettingsKey key = CurrentSettingsKey();
		const bool reset_requested = CpuWasmVU1.TakeFlushRequest();
		if (reset_requested || key != m_key)
		{
			Flush(reset_requested ? FlushReason::Reset : FlushReason::Settings);
			m_key = key;
		}

		// Program-memory changes (section 6.2). Clear ORs the pages before it bumps the generation, so a new
		// generation always comes with its pages; a Clear that lands after this is seen at the next Execute.
		const u32 gen = CpuWasmVU1.GetMicroGeneration();
		if (gen != m_seen_gen)
		{
			m_seen_gen = gen;
			const u64 dirty = CpuWasmVU1.TakeDirtyPages();
			for (std::unique_ptr<Module>& m : m_modules)
			{
				if (m->pages & dirty)
				{
					m->needs_compare = true;
					m->mismatch = false;
				}
			}
			m_epoch++;
		}
		return true;
	}

	// The pointees keep their addresses. Only the owning vector is unordered; the per-PC
	// list still uses ordered removal to preserve its MRU policy.
	template <typename T>
	static void RemoveOwned(std::vector<std::unique_ptr<T>>& objects, T* object)
	{
		const size_t slot = object->owning_slot;
		pxAssert(objects[slot].get() == object);
		if (slot != objects.size() - 1)
		{
			objects[slot] = std::move(objects.back());
			objects[slot]->owning_slot = slot;
		}
		objects.pop_back();
	}

	void ThreadState::Drop(Program* p, DropReason reason)
	{
		const auto start = m_diag ? CacheClock::now() : CacheClock::time_point{};
		if (m_diag)
		{
			m_diag->drops[static_cast<size_t>(reason)]++;
			m_diag->drops_by_pc[p->pc >> 3]++;
			m_diag->Removed(*p->module);
		}
		std::vector<Program*>& list = m_by_pc[p->pc >> 3];
		list.erase(std::find(list.begin(), list.end(), p));
		Module* m = p->module;
		RemoveOwned(m_programs, p);
		if (--m->programs == 0)
		{
			// Its other blocks are no longer listed: nothing can enter it.
			m_live_wasm_bytes -= m->wasm_bytes;
			CpuWasmVU1.SetCommittedCache(m_live_wasm_bytes);
			WasmJit::Unload(&m->fn);
			RemoveOwned(m_modules, m);
		}
		m_dropped++;
		m_epoch++;
		if (m_diag)
			m_diag->drop_ns += ElapsedNs(start);
	}

	Program* ThreadState::FindCurrent(u32 pc)
	{
		Signature sig;
		if (DeriveSignature(VU1, &sig) != Refusal::None)
		{
			m_refused++;
			return nullptr;
		}
		return Find(pc, sig);
	}

	bool ThreadState::Matches(Module* m)
	{
		// The whole module: its blocks go to each other without the dispatcher.
		if (m->mismatch)
			return false;
		if (m->needs_compare)
		{
			const u8* bytes = m->bytes.data();
			for (const Module::Range& r : m->ranges)
			{
				if (std::memcmp(bytes, &VU1.Micro[r.start], r.size) != 0)
				{
					m->mismatch = true;
					return false;
				}
				bytes += r.size;
			}
			m->needs_compare = false;
		}
		return true;
	}

	Program* ThreadState::Lookup(u32 pc, const Signature& sig, bool* matched_signature)
	{
		std::vector<Program*>& list = m_by_pc[pc >> 3];
		for (size_t i = 0; i < list.size(); i++)
		{
			Program* p = list[i];
			// Overlays come back: a module for other code stays listed until it falls off the end (section 6.2).
			if (p->sig != sig)
				continue;
			if (matched_signature)
				*matched_signature = true;
			if (Matches(p->module))
			{
				if (i != 0)
					std::rotate(list.begin(), list.begin() + i, list.begin() + i + 1);
				return p;
			}
		}
		return nullptr;
	}

	Program* ThreadState::Find(u32 pc, const Signature& sig)
	{
		bool same_signature = false;
		if (Program* p = Lookup(pc, sig, m_diag ? &same_signature : nullptr))
		{
			if (m_diag)
				m_diag->hits++;
			return p;
		}
		if (m_diag)
			m_diag->OnMiss(pc, sig, same_signature);
		std::vector<Program*>& list = m_by_pc[pc >> 3];
		if (list.size() >= m_versions_per_pc)
			Drop(list.back(), DropReason::FindLimit);
		return Compile(pc, sig);
	}

	bool ThreadState::Known(void* ctx, u32 pc, const Signature& sig)
	{
		return static_cast<ThreadState*>(ctx)->Lookup(pc, sig) != nullptr;
	}

	Jump* ThreadState::FindJump(u32 pc, const Signature& sig, bool create)
	{
		for (const std::unique_ptr<Jump>& j : m_jumps_learned)
		{
			if (j->pc == pc && j->sig == sig)
				return j.get();
		}
		if (!create)
			return nullptr;
		m_jumps_learned.push_back(std::make_unique<Jump>(Jump{pc, sig, {}}));
		return m_jumps_learned.back().get();
	}

	const std::vector<u32>* ThreadState::JumpTargets(void* ctx, u32 pc, const Signature& sig)
	{
		const Jump* j = static_cast<ThreadState*>(ctx)->FindJump(pc, sig, false);
		return j ? &j->targets : nullptr;
	}

	Program* ThreadState::Compile(u32 pc, const Signature& sig)
	{
		if (m_programs.size() >= MAX_PROGRAMS)
			Flush(FlushReason::Entries);
		else if (m_modules.size() >= MAX_MODULES)
			Flush(FlushReason::Modules);

		const auto t0 = std::chrono::steady_clock::now();
		CompiledModule cm;
		ModuleSource source;
		source.block_limit = m_block_limit;
		source.known = &Known;
		source.jump_targets = &JumpTargets;
		source.ctx = this;
		const bool have_code = CompileModule(1, pc, sig, source, &cm);
		if (m_diag)
			m_diag->generate_ns += ElapsedNs(t0);
		if (!have_code)
			cm.blocks.push_back(CompiledModule::Block{pc, sig, 1, 0, true}); // a marker: the interpreter runs its instruction
		// An entry merged before (whose merged module was dropped since) keeps its count: merges stay bounded.
		u32 generation = 0;
		for (const MergedEntry& e : m_merged_entries)
		{
			if (e.pc == pc && e.sig == sig)
				generation = e.generation;
		}
		Program* entry = Install(cm, have_code, generation);
		const double compile_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		m_compile_ms += compile_ms;
		PerfCounters::Add(PerfCounters::jit_compile_ns, static_cast<u64>(compile_ms * 1e6));
		return entry;
	}

	Program* ThreadState::Install(const CompiledModule& cm, bool have_code, u32 generation, bool merge)
	{
		if (m_block_limit && have_code && m_live_wasm_bytes + cm.wasm.size() > MAX_LIMITED_WASM_BYTES)
		{
			if (cm.wasm.size() > MAX_LIMITED_WASM_BYTES)
			{
				CompiledModule marker;
				marker.blocks.push_back(cm.blocks.front());
				return Install(marker, false, generation);
			}
			Flush(FlushReason::Bytes);
		}
		auto manage_start = m_diag ? CacheClock::now() : CacheClock::time_point{};
		auto m = std::make_unique<Module>();
		m->pages = 0;
		for (const CompiledModule::Block& b : cm.blocks)
		{
			const u32 size = b.instrs * 8;
			m->ranges.push_back(Module::Range{b.pc, size});
			m->sigs.push_back(b.sig);
			m->entries.push_back(b.entry);
			m->bytes.insert(m->bytes.end(), &VU1.Micro[b.pc], &VU1.Micro[b.pc] + size);
			const u32 first = b.pc >> 8, last = (b.pc + size - 1) >> 8;
			m->pages |= ((last == 63) ? ~u64(0) : ((u64(1) << (last + 1)) - 1)) & ~((u64(1) << first) - 1);
		}
		m->needs_compare = false;
		m->mismatch = false;
		m->programs = 0;
		m->wasm_bytes = have_code ? cm.wasm.size() : 0;
		m->chain_exits = 0;
		m->generation = generation;
		m->jump_targets = 0; // after the load below, which may flush the learned targets
		for (const BlockExit& x : cm.exits)
			m->exits.push_back(ExitSlot{x});

		if (have_code)
		{
			if (m_diag)
				m_diag->manage_ns += ElapsedNs(manage_start);
			WasmJit::LoadTiming* const timing = m_diag ? &m_diag->load : nullptr;
			WasmJit::LoadResult result = WasmJit::Load(cm.wasm, "run", &m->fn, timing, true);
			if (result == WasmJit::LoadResult::OutOfMemory && !m_out_of_memory_once)
			{
				// Section 6.5: flush and retry once; a second failure switches the JIT off for this thread.
				m_out_of_memory_once = true;
				Console.Warning("VU1 JIT: out of executable memory, flushing the program cache");
				Flush(FlushReason::OutOfMemory);
				result = WasmJit::Load(cm.wasm, "run", &m->fn, timing, true);
			}
			if (result != WasmJit::LoadResult::Ok)
			{
				Console.Error(fmt::format("VU1 JIT: a module failed to load (result {}, {} bytes, pc {:#x}); VU1 runs on the interpreter",
					static_cast<int>(result), cm.wasm.size(), cm.blocks[0].pc));
				m_usable = false;
				return nullptr;
			}
			if (m_diag)
			{
				// Include the generated module shape and output. A larger merged module is a distinct identity.
				u64 hash = Fingerprint(0xcbf29ce484222325ULL, m->bytes.data(), m->bytes.size());
				for (size_t i = 0; i < m->ranges.size(); i++)
				{
					hash = Fingerprint(hash, &m->ranges[i].start, sizeof(u32));
					hash = Fingerprint(hash, &m->ranges[i].size, sizeof(u32));
					hash = Fingerprint(hash, &m->sigs[i], sizeof(Signature));
				}
				const u8 settings[] = {m_key.vu0_overflow, m_key.vu1_overflow, m_key.add_sub_hack};
				hash = Fingerprint(hash, settings, sizeof(settings));
				hash = Fingerprint(hash, m->entries.data(), m->entries.size());
				m->diagnostic_identity = Fingerprint(hash, cm.wasm.data(), cm.wasm.size());
				m_diag->OnCompile(cm.blocks[0].pc, m->diagnostic_identity, cm, merge);
				manage_start = CacheClock::now(); // hashing and logging are diagnostics, not cache work
			}
			m_compiles++;
			m_blocks_compiled += cm.blocks.size();
			m_module_edges += cm.edges;
			m_native_ops += cm.native_ops;
			m_helper_ops += cm.helper_ops;
		}

		// Every block is an entry for the dispatcher, the module's entry block first.
		Module* raw = m.get();
		raw->jump_targets = JumpTargetCount(raw);
		raw->owning_slot = m_modules.size();
		m_live_wasm_bytes += raw->wasm_bytes;
		m_modules.push_back(std::move(m));
		const u64 drop_before = m_diag ? m_diag->drop_ns : 0;
		Program* entry = nullptr;
		for (u32 k = 0; k < cm.blocks.size(); k++)
		{
			const CompiledModule::Block& b = cm.blocks[k];
			if (!b.entry)
				continue; // entered in a branch delay slot, from the module only
			std::vector<Program*>& list = m_by_pc[b.pc >> 3];
			if (list.size() >= m_versions_per_pc)
			{
				if (k != 0 && list.back()->module == raw)
					continue; // reached inside the module only
				Drop(list.back(), DropReason::InstallLimit);
			}
			auto p = std::make_unique<Program>();
			p->pc = b.pc;
			p->sig = b.sig;
			p->module = raw;
			p->index = k;
			p->last_start = b.last_start;
			p->runs = 0;
			p->owning_slot = m_programs.size();
			raw->programs++;
			list.insert(list.begin(), p.get());
			if (k == 0)
				entry = p.get();
			m_programs.push_back(std::move(p));
		}
		CpuWasmVU1.SetCommittedCache(m_live_wasm_bytes);
		if (m_diag)
			m_diag->manage_ns += ElapsedNs(manage_start) - (m_diag->drop_ns - drop_before);
		m_epoch++;
		return entry;
	}

	u32 ThreadState::JumpTargetCount(Module* m)
	{
		u32 n = 0;
		for (ExitSlot& slot : m->exits)
		{
			if (!slot.x.dynamic_tpc)
				continue;
			if (!slot.jump)
				slot.jump = FindJump(m->ranges[slot.x.block].start, m->sigs[slot.x.block], false);
			if (slot.jump)
				n += static_cast<u32>(slot.jump->targets.size());
		}
		return n;
	}

	bool ThreadState::WantsMerge(Module* m)
	{
		if (m->generation >= MAX_MERGES || !ChainingEnabled())
			return false;
		// A merged module again only for targets its jumps learned since: the rest it could hold, it holds.
		if (m->generation > 0 && JumpTargetCount(m) <= m->jump_targets)
			return false;
		static const bool testing = std::getenv("VU1JIT_MERGE_EXITS") != nullptr; // the fuzzer: every merge it can make
		const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_started).count();
		return testing || m_merge_ms < MERGE_BUDGET_MS + MERGE_BUDGET_SHARE * elapsed;
	}

	void ThreadState::Merge(Module* old)
	{
		const auto t0 = std::chrono::steady_clock::now();
		const u32 pc = old->ranges[0].start;
		const Signature sig = old->sigs[0];
		const u32 generation = old->generation + 1;
		bool listed = false;
		for (MergedEntry& e : m_merged_entries)
		{
			if (e.pc == pc && e.sig == sig)
			{
				e.generation = std::max(e.generation, generation);
				listed = true;
			}
		}
		if (!listed)
			m_merged_entries.push_back(MergedEntry{pc, sig, generation});
		CompiledModule cm;
		ModuleSource source;
		source.jump_targets = &JumpTargets;
		source.ctx = this;
		source.block_limit = m_block_limit;
		// The modules it goes to become one with it (their entries are roots), so that code is compiled into one
		// module only: copies of hot code in several modules would cost instruction cache.
		const auto add_root = [&](u32 tpc, const Signature& tsig) {
			const Program* q = Lookup(tpc, tsig);
			if (!q || q->module == old || !q->module->fn.IsLoaded())
				return;
			const std::pair<u32, Signature> root(q->module->ranges[0].start, q->module->sigs[0]);
			if (std::find(source.roots.begin(), source.roots.end(), root) == source.roots.end())
				source.roots.push_back(root);
		};
		for (const ExitSlot& slot : old->exits)
		{
			const BlockExit& x = slot.x;
			if (x.kind != ExitKind::Boundary || (x.reason != ExitReason::Block && x.reason != ExitReason::Cap) || !x.chain || x.ebit)
				continue;
			if (!x.dynamic_tpc)
				add_root(x.tpc & VU1_PROGMASK, x.sig);
			else if (slot.jump)
			{
				for (const u32 t : slot.jump->targets)
					add_root(t & VU1_PROGMASK, x.sig);
			}
		}
		// And its own blocks (a merged module started from several roots), so that the new one holds them all and
		// replaces it. It is not merged again in any case.
		for (size_t k = 1; k < old->ranges.size(); k++)
		{
			if (!old->entries[k])
				continue; // entered in a delay slot: reached from the others
			const std::pair<u32, Signature> root(old->ranges[k].start, old->sigs[k]);
			if (std::find(source.roots.begin(), source.roots.end(), root) == source.roots.end())
				source.roots.push_back(root);
		}
		old->generation = MAX_MERGES;
		const auto generate_start = m_diag ? CacheClock::now() : CacheClock::time_point{};
		const bool have_code = CompileModule(1, pc, sig, source, &cm);
		if (m_diag)
		{
			m_diag->merge_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(generate_start - t0).count();
			m_diag->generate_ns += ElapsedNs(generate_start);
		}
		if (!have_code)
			return;
		const Program* entry = Install(cm, true, generation, true);
		const auto manage_start = m_diag ? CacheClock::now() : CacheClock::time_point{};
		const u64 drop_before = m_diag ? m_diag->drop_ns : 0;
		m_merges++;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		m_merge_ms += ms;
		static const bool log_merges = std::getenv("VU1JIT_EXITS") != nullptr;
		if (log_merges)
			Console.WriteLn(fmt::format("VU1 JIT merge: entry {:#06x} sig {:08x} generation {}, {} roots, {} blocks, {} exits to "
										"its own blocks, {:.1f} ms",
				pc, sig.Hash(), generation, source.roots.size(), cm.blocks.size(), cm.edges, ms));
		if (!entry || !entry->module->fn.IsLoaded())
			return;

		// The modules whose blocks the new one all holds: nothing would enter them any more.
		const Module* merged = entry->module;
		std::vector<Module*> contained;
		for (const std::unique_ptr<Module>& m : m_modules)
		{
			if (m.get() == merged || !m->fn.IsLoaded())
				continue;
			// (Its entry blocks: the others, entered in a delay slot, are reached from them.)
			bool all = true;
			for (size_t k = 0; k < m->ranges.size() && all; k++)
			{
				if (!m->entries[k])
					continue;
				bool found = false;
				for (size_t j = 0; j < merged->ranges.size() && !found; j++)
				{
					found = merged->entries[j] && merged->ranges[j].start == m->ranges[k].start && merged->sigs[j] == m->sigs[k];
				}
				all = found;
			}
			if (all)
				contained.push_back(m.get());
		}
		for (Module* m : contained)
		{
			std::vector<Program*> programs;
			for (const std::unique_ptr<Program>& p : m_programs)
			{
				if (p->module == m)
					programs.push_back(p.get());
			}
			for (Program* p : programs)
				Drop(p, DropReason::Merge); // the last one releases the module
		}
		if (m_diag)
			m_diag->merge_ns += ElapsedNs(manage_start) - (m_diag->drop_ns - drop_before);
	}

	const ExitSlot* ThreadState::Run(Program* p)
	{
		m_entries++;
		p->runs++;
		NormalizeRings(VU1);
		g_block_base = VU1.cycle;
		Module* m = p->module;
		const u32 id = static_cast<u32>(reinterpret_cast<s32 (*)(s32)>(static_cast<uptr>(m->fn.slot))(static_cast<s32>(p->index)));
		if (id >= m->exits.size())
		{
			m_bad_runs++;
			pxFailRel("VU1 JIT: generated code returned a bad exit");
			return nullptr;
		}
		ExitSlot& x = m->exits[id];
		x.count++;
		ApplyExit(VU1, x.x, g_block_base); // the base of the block, or loop pass, that exited
		const u64 instrs = x.x.instrs + static_cast<u64>(g_chain_instrs);
		m_edges += g_chain_edges;
		m_run_instrs = instrs;
		m_instructions += instrs;
		m_jit_instructions += instrs;
		s_run_instructions += instrs;
		return &x;
	}

	void ThreadState::StepInterpreter()
	{
		if (g_trace)
			Console.WriteLn(fmt::format("  step {:#06x} {} cycle {} status {:#x} mac {:#x} fmaccount {}", VU1.VI[REG_TPC].UL & VU1_PROGMASK,
				Disassemble(*(u32*)&VU1.Micro[VU1.VI[REG_TPC].UL & VU1_PROGMASK], *(u32*)&VU1.Micro[(VU1.VI[REG_TPC].UL & VU1_PROGMASK) + 4]),
				VU1.cycle, VU1.VI[REG_STATUS_FLAG].UL, VU1.VI[REG_MAC_FLAG].UL, VU1.fmaccount));
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
		g_budget_end = start + cycles; // looping blocks test it as the loop below does before it enters a block
		vu1InterpMTVURunning = true;
		Program* next = nullptr; // the block a chained exit names
		bool force_step = false;
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

			Program* p = next;
			next = nullptr;
			if (force_step) // after an exit at an instruction generated code does not run
			{
				force_step = false;
				StepInterpreter();
				continue;
			}
			if (!p && VU1.branch == 0 && VU1.ebit == 0) // never enter inside a delay slot
				p = FindCurrent(VU1.VI[REG_TPC].UL & VU1_PROGMASK);
			if (!m_usable)
				p = nullptr;
			// The interpreter tests the budget before each instruction: the block's last one must start inside it.
			if (!p || !p->module->fn.IsLoaded() || (VU1.cycle - start) + p->last_start >= cycles)
			{
				StepInterpreter();
				continue;
			}

			static const bool block_check = std::getenv("VU1JIT_BLOCKCHECK") != nullptr;
			if (block_check && !m_ls_start)
			{
				m_ls_start = std::make_unique<RunState>();
				m_ls_jit = std::make_unique<RunState>();
				m_ls_ref = std::make_unique<RunState>();
			}
			if (block_check)
				CaptureRunState(m_ls_start.get());
			if (g_trace)
			{
				Console.WriteLn(fmt::format("  block {:#06x} sig {} cycle {} status {:#x} mac {:#x}", p->pc, p->sig.ToString(), VU1.cycle,
					VU1.VI[REG_STATUS_FLAG].UL, VU1.VI[REG_MAC_FLAG].UL));
				const Module::Range& r = p->module->ranges[p->index];
				for (u32 i = 0; i < 64 && p->pc + i * 8 < r.start + r.size; i++)
				{
					const u32* w = reinterpret_cast<const u32*>(&VU1.Micro[p->pc + i * 8]);
					Console.WriteLn(fmt::format("    {:#06x}: {}", p->pc + i * 8, Disassemble(w[0], w[1])));
				}
			}
			const ExitSlot* xs = Run(p);
			if (!xs)
				break;
			const BlockExit& x = xs->x;
			if (g_trace)
				Console.WriteLn(fmt::format("  -> exit {} kind {} reason {} instrs {} tpc {:#x} cycle {} status {:#x} mac {:#x}",
					static_cast<int>(xs - p->module->exits.data()), static_cast<int>(x.kind), static_cast<int>(x.reason), m_run_instrs,
					VU1.VI[REG_TPC].UL, VU1.cycle, VU1.VI[REG_STATUS_FLAG].UL, VU1.VI[REG_MAC_FLAG].UL));
			if (block_check)
			{
				// Debugging: the same instructions on the interpreter from the same state, compared by the contract.
				CaptureRunState(m_ls_jit.get());
				RestoreRunState(*m_ls_start);
				{
					RecordingScope scope(nullptr, nullptr, false);
					for (u64 i = 0; i < m_run_instrs; i++)
						CpuIntVU1.Step();
				}
				CaptureRunState(m_ls_ref.get());
				std::string diff = CompareRunStates(*m_ls_jit, *m_ls_ref);
				if (x.kind == ExitKind::End && diff.find("xgkick") != std::string::npos)
					diff.clear(); // the kick flush follows below for generated code
				if (!diff.empty())
				{
					Console.Error(fmt::format("VU1 JIT block check: block at {:#06x} sig {} exit {} ({} instrs): {}", p->pc,
						p->sig.ToString(), static_cast<int>(xs - p->module->exits.data()), m_run_instrs, diff));
					for (u32 i = 0; i < std::max<u32>(x.instrs, 1) + 1 && p->pc + i * 8 < VU1_PROGSIZE; i++)
					{
						const u32* w = reinterpret_cast<const u32*>(&VU1.Micro[p->pc + i * 8]);
						Console.Error(fmt::format("  {:#06x}: {}", p->pc + i * 8, Disassemble(w[0], w[1])));
					}
				}
				RestoreRunState(*m_ls_jit);
			}
			if (x.kind == ExitKind::End)
			{
				// Generated code ended the program: flush the kick and report the E bit, as _vu1Exec does after its
				// _vuFlushAll (VU1microInterp.cpp:226-240).
				if (VU1.xgkickenable)
					_vuXGKICKTransfer(0, true);
				PublishInterrupts(VU_Thread::InterruptFlagVUEBit);
				continue;
			}
			m_exits[static_cast<u32>(x.reason)]++;
			if (x.reason != ExitReason::Block && x.reason != ExitReason::Cap)
			{
				force_step = true; // the instruction generated code does not run, once the loop head tested the budget
				continue;
			}
			if (x.chain)
			{
				ExitSlot& slot = const_cast<ExitSlot&>(*xs);
				const u32 tpc = VU1.VI[REG_TPC].UL & VU1_PROGMASK;
				Module* m = p->module;
				if (x.dynamic_tpc)
				{
					m_jumps++;
					static const bool exit_stats = std::getenv("VU1JIT_EXITS") != nullptr;
					if (exit_stats)
					{
						auto it = std::find_if(slot.targets.begin(), slot.targets.end(), [tpc](const auto& t) { return t.first == tpc; });
						if (it != slot.targets.end())
							it->second++;
						else if (slot.targets.size() < 8)
							slot.targets.emplace_back(tpc, 1);
					}
					// Learning (section 6.4): the target as generated code would compare it.
					if (!slot.jump)
						slot.jump = FindJump(m->ranges[x.block].start, m->sigs[x.block], true);
					const u32 target = VU1.VI[REG_TPC].UL;
					std::vector<u32>& targets = slot.jump->targets;
					if (targets.size() < MAX_JUMP_TARGETS && std::find(targets.begin(), targets.end(), target) == targets.end())
						targets.push_back(target);
				}
				// A module that often leaves for another one is compiled again with every block it reaches, its jumps'
				// learned targets included; the exits after that go to the new module.
				const Signature sig = x.sig;
				const bool dynamic = x.dynamic_tpc;
				bool merged = false;
				if (++m->chain_exits >= (MergeExits() << (2 * m->generation)))
				{
					m->chain_exits = 0; // tested again after as many exits
					if (WantsMerge(m))
					{
						Merge(m); // p, xs and x may be gone
						merged = true;
					}
				}
				if (merged || dynamic)
				{
					next = Find(tpc, sig);
				}
				else if (slot.next_epoch == m_epoch && slot.next)
					next = slot.next;
				else
				{
					const u64 flushes = m_flushes, dropped = m_dropped;
					next = Find(tpc, sig); // the lookup can release the module that owns x
					// Find may flush the cache or drop this exit's module: remember the link only if it did neither.
					if (m_flushes == flushes && m_dropped == dropped)
					{
						slot.next = next;
						slot.next_epoch = m_epoch;
					}
				}
			}
		}
		VU1.VI[REG_TPC].UL >>= 3;
		PublishStats();
		if (m_diag)
			ReportCache();
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

	void ThreadState::ReportCache()
	{
		CacheDiagnostics& d = *m_diag;
		const u32 vsync = s_vsync.load(std::memory_order_relaxed);
		if (vsync < d.next_report)
			return;
		d.next_report = (vsync / d.interval + 1) * d.interval;

		std::array<u32, 65> spread{};
		for (const auto& list : m_by_pc)
			spread[list.size()]++;
		std::string histogram;
		for (u32 i = 1; i <= m_versions_per_pc; i++)
		{
			if (spread[i])
				histogram += fmt::format("{}{}:{}", histogram.empty() ? "" : ",", i, spread[i]);
		}
		const auto top = [](const auto& by_pc) {
			std::string result;
			std::array<u32, NUM_PCS> pcs;
			for (u32 i = 0; i < NUM_PCS; i++)
				pcs[i] = i;
			std::partial_sort(pcs.begin(), pcs.begin() + 4, pcs.end(), [&](u32 a, u32 b) { return by_pc[a] > by_pc[b]; });
			for (u32 i = 0; i < 4 && by_pc[pcs[i]]; i++)
				result += fmt::format("{}{:04x}:{}", i == 0 ? "" : ",", pcs[i] * 8, by_pc[pcs[i]]);
			return result;
		};
		Console.WriteLnFmt("VU1_CACHE vsync={} cap={} block_limit={} hits={} misses={} new_signature={} mismatch={} first={} repeat_evicted={} "
						   "repeat_other={} untracked={} merge_compiles={} compiled_blocks={} compiled_bytes={} drop_find={} drop_install={} "
						   "drop_merge={} flush_reset={} flush_settings={} flush_modules={} flush_entries={} flush_oom={} flush_bytes={} live_modules={} "
						   "live_entries={} live_wasm_bytes={} generate_ns={} browser_compile_ns={} instantiate_ns={} table_ns={} "
						   "manage_ns={} drop_ns={} merge_extra_ns={} flush_ns={} versions=[{}] top_drop=[{}] top_repeat=[{}]",
			vsync, m_versions_per_pc, m_block_limit, d.hits, d.misses, d.new_signature, d.mismatched, d.first_compile, d.recompile,
			d.without_eviction, d.untracked, d.merged_compiles, d.compiled_blocks, d.compiled_bytes, d.drops[0], d.drops[1],
			d.drops[2], d.flushes[0], d.flushes[1], d.flushes[2], d.flushes[3], d.flushes[4], d.flushes[5], m_modules.size(), m_programs.size(),
			m_live_wasm_bytes, d.generate_ns, d.load.compile_ns, d.load.instantiate_ns, d.load.table_ns, d.manage_ns,
			d.drop_ns, d.merge_ns, d.flush_ns, histogram, top(d.drops_by_pc), top(d.repeats_by_pc));
	}

	void ThreadState::LogSummary() const
	{
		static constexpr const char* reasons[] = {"fallback", "budget", "busy kick", "D/T bit", "unknown target", "cap", "block"};
		static_assert(std::size(reasons) == static_cast<size_t>(ExitReason::Count));
		std::string exits;
		for (u32 i = 0; i < static_cast<u32>(ExitReason::Count); i++)
		{
			if (m_exits[i])
				exits += fmt::format("{}{} {}", exits.empty() ? "" : ", ", reasons[i], m_exits[i]);
		}
		size_t learned = 0;
		for (const std::unique_ptr<Jump>& j : m_jumps_learned)
			learned += j->targets.size();
		Console.WriteLn(fmt::format("VU1 JIT: {} executes, {} modules compiled ({} blocks, {} exits to a block of their module; "
									"{:.1f} ms, {} of them merges of {:.1f} ms), {} modules and {} blocks listed, {} dropped (versions "
									"per PC and merges), {} flushes; {} jump targets learned; {} entries into generated code, {} bad "
									"returns; {} instructions, {} of them on the interpreter; exits: {} ({} of the block exits at "
									"JR/JALR); {} lookups refused for the pipeline state; {} EE-thread Clears",
			m_executes, m_compiles, m_blocks_compiled, m_module_edges, m_compile_ms + m_merge_ms, m_merges, m_merge_ms, m_modules.size(),
			m_programs.size(), m_dropped, m_flushes, learned, m_entries, m_bad_runs, m_instructions, m_fallback_steps,
			exits.empty() ? "none" : exits, m_jumps, m_refused, CpuWasmVU1.GetEEThreadClears()));
		Console.WriteLn(fmt::format("VU1 JIT: {} instructions in generated code ({:.2f}%), compiled ops {} native, {} through helpers; "
									"{} times a block or a loop's pass went to the next without leaving generated code",
			m_jit_instructions, m_instructions ? 100.0 * m_jit_instructions / m_instructions : 0.0, m_native_ops, m_helper_ops,
			m_edges));
		if (std::getenv("VU1JIT_EXITS"))
		{
			// Debugging: the exits of the modules still alive that left generated code most often.
			std::vector<std::pair<const Module*, const ExitSlot*>> all;
			for (const std::unique_ptr<Module>& m : m_modules)
			{
				for (const ExitSlot& x : m->exits)
				{
					if (x.count)
						all.emplace_back(m.get(), &x);
				}
			}
			std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second->count > b.second->count; });
			for (size_t i = 0; i < std::min<size_t>(all.size(), 16); i++)
			{
				const auto& [m, x] = all[i];
				const u32 tpc = x->x.tpc & VU1_PROGMASK;
				const u32* w = reinterpret_cast<const u32*>(&VU1.Micro[tpc]);
				std::string targets;
				for (const auto& [t, n] : x->targets)
					targets += fmt::format(" {:#06x}:{}", t, n);
				Console.WriteLn(fmt::format("VU1 JIT exit: {} times, module at {:#06x} ({} blocks), block {:#06x} exit {} after {} reason {} {} tpc {:#06x} sig {}: {}{}",
					x->count, m->ranges[0].start, m->ranges.size(), x->x.block_pc, static_cast<int>(x - m->exits.data()), x->x.instrs,
					reasons[static_cast<u32>(x->x.reason)], x->x.dynamic_tpc ? "dynamic" : "static", tpc, x->x.sig.ToString(),
					x->x.dynamic_tpc ? "" : Disassemble(w[0], w[1]) + " | " + Disassemble(w[2], w[3]), targets));
			}
		}
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
