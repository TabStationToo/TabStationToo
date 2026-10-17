# VU1 JIT to WebAssembly: design (M5)

*Written 2026-10-16 at `a65e4ea5cd` (branch `wasm`), after phase 1 of the VU1 JIT: a study of the VU1
interpreter and of microVU, a runtime code generation spike (`tools/wasm/wasm-jit-spike/`, results in its
`README.md`), and measurements of the VU1 workload in Dragon Ball Z: Budokai Tenkaichi 3 (BT3) fights.
Revised 2026-10-17 after an accuracy review and a performance review; §17 lists every review item and
what changed. No JIT code exists yet. This document replaces M5 tasks 1–3 of `docs/web-port/PLAN.md` for
VU1 (§15.2).*

**How to read this.** "Verified" means read in this tree at `a65e4ea5cd`; paths are relative to the
repository root and line numbers are those of that commit. "Measured" means observed by the spike (Node
24.19.0 with V8 13.6, and Chrome 155, on a Ryzen 7 PRO 7840U shared with other jobs, so ±30% between
runs), by the phase-1 workload instrumentation (BT3, 1500 PAL vsyncs from a savestate at the start of a
fight, MTVU on), or by the reviews' analyses of that data. "Estimate" means a judgement nobody has
measured. Game data (microprograms, dumps, screenshots, savestates) stays outside the repository; only
statistics appear here.

---

## 1. Summary

- **Why.** In BT3 fights the emulator runs at 10–20% of full speed. VU1 runs on its own thread (MTVU,
  enabled on wasm in `c7b5a59027`) and needs 99–152 ms of interpreter time per PAL vsync, against a
  20 ms budget (measured). VU1 is the limit today. Below about 50 ms it no longer is: the EE (about
  50 ms) and each of the software renderer's two helper threads (about 33–56 ms) come next (§10.3).
- **What.** A JIT that compiles VU1 microprograms to WebAssembly on the VU thread, at run time, and calls
  them through that thread's function table. The spike proved this works on every thread we use, in
  Node and Chrome, and that generated code can match the interpreter bit for bit.
- **The reference is the interpreter, not microVU.** The JIT must give exactly the results of the wasm
  VU1 interpreter running on the MTVU thread (`pcsx2/VU1microInterp.cpp`, `pcsx2/VUops.cpp`,
  `pcsx2/VUflags.cpp`): registers, flags, cycle counts, VU memory, GS packets. microVU has a different
  timing, flag, Q/P, XGKICK and rounding model (§2.2), so its analysis cannot be ported. We reuse its
  architecture (program cache, blocks keyed by pipeline state) and implement the interpreter's model.
- **Timing at compile time.** Inside a block every stall, pipe flush, flag/Q/P write-back and VI-backup
  decision depends only on the instruction words and the pipeline state at block entry. The compiler
  works them out by running the interpreter's own timing primitives (`_vuTestPipes`,
  `_vuTest*Stalls`, `_vuAdd*Stalls`, extern in `pcsx2/VUops.h:33-39`) on a scratch `VURegs`, and
  emits only the data work. The one data-dependent timing event (an XGKICK that finds the previous kick
  still running, 0.15% of kicks in BT3) leaves generated code.
- **Unit.** One wasm module per microprogram (entry PC + entry pipeline state + content of the micro
  memory it covers), one exported function `i32 run(i32 entry)` with all its blocks inside, VU registers
  in wasm locals, VU memory and the `VURegs` struct at constant addresses in the shared memory.
- **State format unchanged.** Between programs, and whenever generated code exits (also inside a delay
  slot), the whole state is in `VURegs` exactly as the interpreter keeps it, pipes and pending branch or
  E-bit included. So the interpreter can take over at any instruction boundary (fallback, and the
  lockstep oracle), and savestates do not change.
- **Fallback is the interpreter.** Ops without native code call the interpreter's op body; instructions
  the JIT does not handle (D/T bits, a branch in a branch delay slot, a busy XGKICK, unknown words)
  exit to `InterpVU1::Step()`, which always runs at least one instruction before generated code is
  entered again; programs that keep failing run on the interpreter; and
  `EmuCore/CPU/Recompiler/EnableVU1=false` forces the interpreter everywhere.
- **Correctness is tested, not argued.** A timing-model oracle (random programs and every instruction of
  BT3 and BIOS runs), a random-program fuzzer (full ISA including XGKICK, special float values, random
  entry states and forced exits, both V8 tiers), a lockstep mode that runs every program on both and
  compares the state, and BT3 and BIOS runs in a deterministic MTVU mode with pixel-identical
  screenshots.
- **Targets.** Measured as VU1 execute time (the time inside `CpuVU1->Execute`), as a ratio to the
  interpreter in paired runs: at least 5× after step 1 (estimate 10–18 ms per vsync) and 8× after
  step 2 (estimate 5.5–9 ms), which meets the goal of VU1 well under 20 ms. The visible effect, fights
  about 2–3× faster (roughly 35–40% of full speed, estimate), comes with step 1; after that, further VU1
  gains only save CPU time until the EE JIT and the WebGPU renderer land.
- **Plan.** Step 1 in three parts (§13): 1a plumbing, timing model, oracle and lockstep without any code
  generation; 1b generated code for BT3's most used ops, with a go/no-go speed measurement on the two
  programs that hold half of BT3's instructions; 1c the rest of BT3's ops and the 10^6-program fuzzer.
  Step 2: every op native, learned resume entries, the rare control flow. Steps 3 (flag liveness, clamps,
  shapes) and 4 (structured control flow, cache policy) are deferred behind the EE JIT and GS work. The
  JIT is off by default until step 2's exit criteria hold and the owner agrees to switch it on.

---

## 2. Goal and constraints

### 2.1 The workload (measured, BT3 fights, per PAL vsync)

| Quantity | Value |
| --- | --- |
| VU1 instructions | 980 k (953 k over the first 600 vsyncs); per 2-vsync game frame: median 2.0 M, p90 2.14 M, max 3.79 M |
| Program runs | 2 982, 329 instructions per run on average; every run ends on an E-bit; 83% are MSCNT continuations |
| VU1 cycles | 994 k, 17% of a PAL vsync's 5.9 M cycles at 294.912 MHz; stall cycles 1.4% (hand-scheduled code) |
| VU1 execute time (inside `CpuVU1->Execute`) | 99 ms quiet machine, 152 ms under load: 104–160 ns per instruction |
| Where it goes | per-instruction loop 27–28%, opcode dispatch 10%, register-usage decode for stalls 19–20%, stall bookkeeping and flushes 18–19%, op bodies 18–19%, flag calls 1.3%, waiting 3–4%, VIF/GIF/ring 2% |
| Other threads (profiled, 400 vsyncs, two runs; the profiler inflates every thread about 1.2–1.8×) | EE 49–55 ms, VU 171–188 ms, MTGS 14 ms, each of the two SW rasteriser helpers 67–69 ms |
| Micro-memory uploads | 8.1 (11.6 KB), of which 4.2 change content (3.0 KB): about 8.6 program swaps per game frame |
| Distinct microprograms | 4, all overlays at address 0 (1016, 808, 3192 and 3608 bytes); 19 (entry, range) combinations |
| Control flow | branches 6.9% (conditional 5.7%), 20.7 instructions between jumps; 124 (entry, target) pairs, 168 with pipeline state; 92.6% of block entries hit the most common state of their target; 29% of block entries have an empty pipeline |
| Flags | 45% of instructions write MAC/status, nothing reads them (no FM*/FS*/FSSET); CLIP 4.6%, clip reads 3% |
| Q, P, I | DIV 2.2%, WAITQ 0.04%, Q readers 4.7%; no EFU op; LOI 0.54%, I readers 1.1% |
| XGKICK | 1.13 per run; 0.15% start while the previous kick runs |
| D/T/M bits | never |
| Ops used | 25 upper kinds (NOP 46.7%, MADDA 14.7, MADD 7.4, MULA 7.4, MUL 6.9 of which MULq 4.6, CLIP 4.6, ADD 4.5, SUB 4.0, FTOI 3.7, …) and 30 lower kinds (LQ 28.8, NOP 28.0, integer ALU 15.6, SQ 8.7, branches 6.9, flag ops 3.0, MOVE 2.4, DIV 2.2, LQI 1.4, SQI 1.0, …) |
| Concentration | programs 11 and 14 hold 50.3% of the instructions and 49.6% of the interpreter time; upper NOP, MADDA, MADD, MULA, MUL/MULq, CLIP, SUB, ADD, FTOI0/4 and MULw are 97.9% of upper ops; lower LQ, NOP, SQ, IADDIU, IADD, IBNE, IADDI, MOVE, DIV, FCAND, IBEQ, LQI, IBGTZ, SQI, FCOR and IAND are 96.3% of lower ops |

The upper ops used are ABS, ADD, ADDA, ADDi, ADDq, ADDw, ADDx, ADDy, CLIP, FTOI0, FTOI4, ITOF0, MADDAy,
MADDAz, MADDi, MADDw, MULAw, MULAx, MULi, MULq, MULw, MULx, NOP, SUB, SUBw. The lower ops are B, BAL,
DIV, FCAND, FCGET, FCOR, IADD, IADDI, IADDIU, IAND, IBEQ, IBGTZ, IBNE, ILWR, ISUBIU, ISW, ISWR, JR, LOI,
LQ, LQI, MOVE, MR32, NOP, SQ, SQD, SQI, WAITQ, XGKICK, XTOP. The workload is deterministic: instruction
and run counts were identical across runs.

### 2.2 What the JIT must match

The wasm VU1 interpreter on the MTVU thread is the specification. Where microVU differs (verified in the
study), the JIT follows the interpreter:

| Area | microVU (x86) | Interpreter on wasm (the JIT copies this) |
| --- | --- | --- |
| Rounding | chop, DAZ/FTZ through MXCSR | IEEE round to nearest, denormals kept (wasm has no mode) |
| O/U flags | only with VUOverflowHack | always computed |
| Status flag | only in blocks that read it (`vuFlagHack`) | always updated |
| Flag model | 4 flag instances | the FMAC pipe, 4-cycle write-back |
| Q/P | instance flipping, P released early | FDIV/EFU pipes, with the `efu.Cycle -= 1` quirk |
| XGKICK | whole packet, one instruction later | 1 qword per 2 cycles, progressive |
| VI branch delay | 4-instruction look-back | 2-cycle window, one register |
| Upper and lower write the same VF | lower runs with `noWriteVF` | lower op discarded |
| ILW latency | every VI reader stalls | only branches stall |
| `MOVE vfN, vfN` | NOP | pipe entry |
| D-bit | ignored | honoured |
| Branch in the E-bit delay slot | made a NOP | executed |
| I register load | clamped | not clamped |
| EFU math | polynomials | `pow()` and libm |
| Cycle count | per block | exact per instruction |

The JIT also copies the interpreter's bugs and quirks (§7.10). Accuracy fixes go into the interpreter
first; the lockstep mode then shows where the JIT must follow.

### 2.3 Non-goals

- VU0 (micro or macro), the EE and the IOP. The EE JIT is a later M5 task with its own design. In BT3,
  COP2 and VU0 code is 3.4–4.2% of the EE thread's busy time (about 2 ms per vsync, measured), so a VU0
  JIT is not worth it there.
- VU1 without MTVU (`EmuCore/Speedhacks/vuThread=false`): that path has different contracts (VPU_STAT,
  `cpuRegs.cycle`, `nextBlockCycles`) and keeps the interpreter.
- Speed hacks or approximations: no option trades exactness for speed.
- Desktop builds. All new code is wasm-only; changes to PCSX2 files are small and guarded.
- Asynchronous compilation, our own tiering, a persistent code cache, `return_call_indirect`: not needed
  for VU1 (§5, §10).

---

## 3. Architecture

### 3.1 Where the JIT sits

```
EE thread                                       VU thread ("MTVU", a pthread = a web worker)
---------                                       --------------------------------------------
vu1ExecMicro -> vu1Thread.ExecuteVU ---ring---> MTVU_VU_EXECUTE (MTVU.cpp:143-159)
                                                  VU1.cycle = 0; top, itop, vuFBRST snapshotted
VIF MPG -> vu1Thread.WriteMicroMem ----ring---> MTVU_VU_WRITE_MICRO (:160-166)
                                                  CpuVU1->Clear(addr, size); bytes copied
SPR DMA to VU1 micro -> CpuVU1->Clear --------> (EE thread, no ring: SPR.cpp:58; §6.2)
                                                CpuVU1->Execute(3,000,000)  = WasmVU1::Execute
                                                  |- program cache: (pc, signature, content)
                                                  |- compiler -> module bytes -> JS: Module, Instance,
                                                  |  table slot (all on this thread)
                                                  |- run(entry) -> exit id   (generated code)
                                                  '- CpuIntVU1.Step()        (anything else)
                                                FinishGSPacketMTVU; semaXGkick.Post; vuCycles[]
```

### 3.2 Files

New, wasm only (`pcsx2/wasm/`, built under `if(EMSCRIPTEN)` in `pcsx2/CMakeLists.txt`):

| File | Content |
| --- | --- |
| `pcsx2/wasm/VU1Jit.h`, `VU1Jit.cpp` | `WasmVU1`, the `BaseVUmicroCPU`; the dispatcher; requests from the EE thread; statistics |
| `pcsx2/wasm/VU1JitCache.h`, `.cpp` | programs, content validation, entry lookup, versions, eviction |
| `pcsx2/wasm/VU1JitAnalysis.h`, `.cpp` | decode, the timing model and signatures, block discovery, liveness and clean-register analyses |
| `pcsx2/wasm/VU1JitEmit.cpp` | instruction emission |
| `pcsx2/wasm/VU1JitHelpers.cpp` | the C++ functions generated code imports |
| `pcsx2/wasm/VU1JitLockstep.cpp` | the differential mode: snapshots, the PATH1 recorder, the interrupt sink |
| `pcsx2/wasm/VU1JitFuzz.cpp` | the timing oracle and the fuzzer, shared by the headless runner and the page |
| `pcsx2/wasm/WasmEmitter.h` | the spike's `wasm_emitter.h`, extended (§5.1) |
| `pcsx2/wasm/WasmJitRuntime.js` | `--js-library`: compile, instantiate, table slots, release (the spike's `jit_runtime.js`, trimmed) |
| `pcsx2-headless/` additions | `-vu1jit-fuzz`, `-vu1jit-oracle`, `-vu1jit-lockstep`, `-vu1jit-stats`, `-vu1jit-dump`, `-vu1jit-replay`, `-mtvu-sync` (§9, §12) |
| `pcsx2-wasm/` additions | a testing query parameter for the JIT, a fuzz entry that runs on a worker, the JIT statistics in the console (§12) |

Changes to PCSX2's files, all small and guarded with `ARCH_WASM` or `EMSCRIPTEN`:

- `pcsx2/VMManager.cpp` `UpdateCPUImplementations` (`:2741-2765`): select `CpuWasmVU1` when
  `THREAD_VU1 && EmuConfig.Cpu.Recompiler.EnableVU1`, after `vu1Thread.WaitVU()` so the pointer never
  changes under a running program.
- Link options of `pcsx2-wasm` and `pcsx2-headless`: `-sALLOW_TABLE_GROWTH` and the JS library.
- `pcsx2-wasm/Main.cpp` and `pcsx2-headless/Main.cpp`: `EnableVU1=false` until the JIT is switched on
  (§12).
- `pcsx2/VUops.cpp` `_vuXGKICKTransfer` (`:1826-1921`): an optional PATH1 recorder hook for the lockstep
  mode and the fuzzer (one guarded function-pointer test around the two GIF calls).
- `pcsx2/VU1microInterp.cpp` (`:237`): an optional interrupt sink for the lockstep mode (one guarded
  pointer test around the `mtvuInterrupts.fetch_or`).
- `pcsx2/MTVU.cpp` `Get_MTVUChanges` (`:351`): an optional `WaitVU()` first, for the deterministic mode
  of the headless runner (§9.6).

`REC_VU1` stays `false` on wasm (`pcsx2/Config.h:1504-1506`): it means microVU, and it changes GIF packet
sizing (`pcsx2/Gif_Unit.h:606`) in a way the interpreter, and so the JIT, must not see.

### 3.3 The CPU provider

`class WasmVU1 final : public BaseVUmicroCPU` (`pcsx2/VUmicro.h:29-78`), `CpuWasmVU1`, short name
`wVU1`, long name "VU1 WebAssembly JIT", `IsInterpreter = false`.

| Method | Thread | Behaviour |
| --- | --- | --- |
| `Execute(cycles)` | VU | The dispatcher (§3.4). Delegates to `CpuIntVU1.Execute` when the JIT is unsupported for the current settings (§12) or failed to initialise. |
| `Clear(addr, size)` | VU (MTVU writes), sometimes EE (`SPR.cpp:58`) | Increments an atomic micro-memory generation and ORs the range's pages into an atomic dirty mask (`fetch_or`), so both threads may call it. Never compares or frees: the bytes are not written yet (`MTVU.cpp:164-165`). |
| `Reset()` | EE (`ClearCPUExecutionCaches`, `VMManager.cpp:2767-2786`, on boot, savestate load and every CPU, gamefix or speedhack change) | `vu1Thread.WaitVU()` if the thread is open; then `CpuIntVU1.Reset()`, which zeroes the FMAC and IALU ring positions and counts (`VU1microInterp.cpp:274-283`) as the interpreter's own `Reset` would; then sets an atomic "flush requested". The VU thread flushes its cache at its next `Execute`: modules and table slots belong to that thread. |
| `Shutdown()` | EE | As `Reset`. The memory goes when the VU thread exits (§3.5). |
| `SetStartPC(pc)` | VU | `VU1.start_pc = pc`, as the interpreter. |
| `Step()` | debugger | `CpuIntVU1.Step()`. |
| `ResumeXGkick()` | — | Nothing (the interpreter's does nothing either). |
| `GetCommittedCache()` | any | Estimated cache bytes (§6.5). |

### 3.4 The dispatcher

`WasmVU1::Execute` keeps the interpreter's outer loop (`InterpVU1::Execute`,
`VU1microInterp.cpp:296-323`) and calls generated code where it can:

```cpp
void WasmVU1::Execute(u32 cycles)
{
	if (!m_enabled_for_settings)
		return CpuIntVU1.Execute(cycles);
	ServiceRequests();                       // flush on Reset, settings key, first call; collect Clears
	VU1.VI[REG_TPC].UL <<= 3;                // byte PCs inside Execute, as the interpreter
	const u64 start = VU1.cycle;
	vu1InterpMTVURunning = true;
	m_ctx.budget_end = start + cycles;       // read by generated code
	bool interpreter_only = false;           // set by a budget exit
	while ((VU1.cycle - start) < cycles)
	{
		if (!vu1InterpMTVURunning)           // the interpreter's loop head, VU1microInterp.cpp:309-317
		{
			if (VU1.branch == 1) { VU1.VI[REG_TPC].UL = VU1.branchpc; VU1.branch = 0; }
			break;
		}
		Entry* e = nullptr;
		if (!interpreter_only && VU1.branch == 0 && VU1.ebit == 0)  // never enter inside a delay slot
			e = m_cache.FindOrCompile(VU1.VI[REG_TPC].UL & VU1_PROGMASK); // null if the state is refused
		if (!e)
		{
			CpuIntVU1.Step();                // exact by construction
			continue;
		}
		const Exit& x = e->Run();            // through this thread's table slot
		if (x.kind == Exit::End)             // generated code already set running = false
		{
			if (VU1.xgkickenable)
				_vuXGKICKTransfer(0, true);
			PublishInterrupt(VU_Thread::InterruptFlagVUEBit);  // fetch_or, or the lockstep sink
			continue;                        // the loop head applies a pending branch
		}
		m_cache.NoteExit(e, x);              // statistics, learning (§6.4)
		if (x.reason == Exit::Budget)
			interpreter_only = true;         // the interpreter finishes this Execute
		else
			CpuIntVU1.Step();                // always progress: the exit point may itself be an entry
	}
	VU1.VI[REG_TPC].UL >>= 3;
}
```

- **Progress after every exit.** A `BOUNDARY` exit leaves the state at the start of an instruction that
  may itself be a known entry with the same signature (a busy kick stays busy since no cycles pass,
  `vuFBRST` is constant for the run, and the all-fallback JIT of step 1a exits at its own entry). So at
  least one interpreter step always follows. The loop then keeps stepping while a branch or E-bit is
  pending, and enters generated code again at the next known entry (main or learned, §6.4).
- **Budget.** The 3,000,000-cycle budget is checked at every instruction by the interpreter. Generated
  code checks it once per block against `m_ctx.budget_end` and exits before a block whose last
  instruction would start past the budget (§7.6). Re-entering would exit again at once, so the
  interpreter finishes the `Execute`, which is at most one block. With 300–600-instruction programs it
  only matters for runaway programs.
- **Program end.** The order is the interpreter's (`VU1microInterp.cpp:210-242`): the end instruction's
  kick catch-up (before its ops), `VIBackupCycles = 0`, the `_vuFlushAll` effects and `running = false`
  happen in generated code; the XGKICK flush and the interrupt follow in C++; a branch taken in the E-bit
  delay slot is left pending (`branch = 1`) and applied by the loop head, so a budget that runs out on
  that very instruction leaves the same state as the interpreter (§5.3).
- **Interrupts** go through `PublishInterrupt`, which is `mtvuInterrupts.fetch_or` except in the lockstep
  mode (§9.4). A D/T stop always happens inside an interpreter step (§7.6), so `s_mtvu_dt_stop` stays
  private to the interpreter.
- **Signatures.** `DeriveSignature` has a fast path: empty pipes and no VI backup (`fmaccount == 0 &&
  !fdiv.enable && !efu.enable && ialucount == 0 && VIBackupCycles == 0`) is the "clean" signature. Every
  E-bit end leaves exactly this state (`_vuFlushAll`, `VUops.cpp:130-186`), so almost every MSCAL and
  MSCNT start is clean. It refuses states it cannot represent (§4.2), which then run on the interpreter
  until they become representable, usually one instruction. Whether a kick is active is not part of the
  signature: generated code tests it at run time (§4.4).

### 3.5 Threads and object lifetime

Measured by the spike: compile, register and run on the same thread; each pthread has its own instance
of the main module and its own function table; a slot filled on one thread is null on another.

- Generated code exists only on the VU thread. The cache is a `thread_local` object created by the first
  `Execute` on a thread. Its destructor releases every module (`removeFunction`, drop the JS handles) on
  that thread when MTVU's thread function returns (`VU_Thread::Close`). Emscripten reuses pool workers, and
  their JS state outlives the pthread, so nothing may be left behind. Step 1a checks that `thread_local`
  destructors run on pthread exit under Emscripten 6.0.12; if not, `MTVU.cpp` calls a release function
  before its thread function returns (guarded).
- The EE thread never touches the cache. It requests (`Reset`, `Shutdown`, the settings key) through
  atomics, always after `vu1Thread.WaitVU()`. Its only other call, `Clear` from the SPR path, touches
  nothing but the atomic generation and dirty mask.
- Compilation is synchronous on the VU thread inside `Execute` (§10.5 for its cost). The EE waits for
  VU1 anyway (67% idle in BT3), and Chrome's 8 MB main-thread limit on synchronous compiles does not
  apply to workers.
- Memory growth by another thread is safe for existing instances (measured), and wasm memory never
  moves, so absolute addresses (`&VU1`, `VU1.Mem`, `&vu1Thread.vifRegs`) are constants in the code.
  They go into the settings key (§6.5) so a change would flush the cache.

### 3.6 What is reused, and what is not

From the interpreter (the specification, used directly so it cannot drift):

- `VU1regs_UPPER_OPCODE` / `VU1regs_LOWER_OPCODE`: pipe, register reads and writes, latencies. They read
  the global `VU1.code`; the compiler sets it around each lookup and restores it. It runs on the VU
  thread inside `Execute`, before the code it compiles runs, and every interpreter step rewrites
  `VU1.code` before using it. No PCSX2 change needed. The compiler zeroes each `_VURegsNum` before the
  call, because these functions leave fields unset (§4.2).
- The timing primitives `_vuTestUpperStalls`, `_vuTestLowerStalls`, `_vuTestPipes`, `_vuClearFMAC`,
  `_vuAddUpperStalls`, `_vuAddLowerStalls` (extern, `VUops.h:33-39`) and `_vuFlushAll`, run on a scratch
  `VURegs` (§4.2).
- The op bodies `VU1_UPPER_OPCODE` / `VU1_LOWER_OPCODE` as per-op helpers (§5.6, §8).
- `_vuXGKICKTransfer` for every GS transfer, and `InterpVU1::Step` for every fallback.

From microVU (`pcsx2/x86/microVU*`), as designs, re-implemented in architecture-independent C++:

- The program cache: programs listed per start PC, each holding a copy of the micro-memory ranges it
  compiled (`mVUsetupRange`, `microVU_Compile.inl:36-108`), compared lazily after `Clear`
  (`mVUcmpProg`/`mVUsearchProg`, `microVU.cpp:216-299`), most recently used first.
- Blocks keyed by pipeline state (`microBlockManager`, `microVU.h:140-274`), with a new signature type
  compared by `memcmp`.
- The two-pass structure (analyse, then emit), bad-op and evil-branch detection, and the JR jump cache
  idea (`microVU_Compile.inl:1039-1074`).

From the spike: the emitter, the FMAC/flag/CLIP/MAX/MINI lowering, clean-register analysis, clamp cache
and loop hoisting, the V8-friendly shapes, the JS runtime and the fuzzer's special-value generator.

Not reused: microVU's x86 emitter and register allocator, its clamping and MXCSR handling, its flag
instances, Q/P instances, XGKICK and VI-delay models, and `microVU_Analyze.inl`'s stall computations. They
implement a different machine (§2.2).

---

## 4. Compilation

### 4.1 Programs, entries and blocks

- A **program** is what one module holds: the code reachable from an entry PC, keyed by
  (entry PC, entry signature, content of the micro-memory ranges it covers). BT3 has 19 such
  combinations from 4 microprograms (§2.1). Like microVU, programs are listed per entry PC; code shared by
  two entry PCs is compiled into both (a few KB; not worth sharing).
- An **entry** is a (PC, signature) at which the dispatcher may call the function, always with no branch
  or E-bit pending. Every program has its main entry; learning adds more (§6.4).
- A **block** is a straight run of instructions from a node to the instruction after a branch's delay
  slot, or to the program end. A **node** is (PC, signature, control state), where the control state is
  the pending branch (countdown and target) and the pending E-bit countdown at that PC. Entries are nodes
  with an empty control state; other nodes only exist inside a program, for example the branch target
  reached with an E-bit pending (§7.5). The same PC appears once per distinct node.

### 4.2 The timing model and signatures

The **signature** is the part of the interpreter's state that decides timing and is static along a path:

| Component | Source in `VURegs` |
| --- | --- |
| Pending FMAC entries in ring order, at most 4: cycles remaining, `regupper`, `xyzwupper`, `reglower`, `xyzwlower`, `flagreg` (normalised, below) | `fmac[]`, `fmacreadpos`, `fmaccount` |
| FDIV: busy, cycles remaining | `fdiv.enable`, `sCycle + Cycle - cycle` |
| EFU: busy, cycles remaining (after the `Cycle -= 1` quirk) | `efu` |
| Pending IALU entries: register mask, cycles remaining | `ialu[]` |
| VI backup: cycles left and register number | `VIBackupCycles`, `VIRegNumber` |

About 48 bytes, compared with `memcmp` and hashed. Everything else is runtime data and stays in wasm
locals or memory: VF, VI, ACC, I, Q, P, R, the pending `q`/`p`, the pending `macflag`, `statusflag`,
`clipflag`, the flag snapshots held by pipe entries, `fdiv.reg`/`fdiv.statusflag`, `efu.reg`,
`VIOldValue`, the `xgkick*` fields (including whether a kick is active), and `cycle`.

**Normalisation.** Ring positions are not observable: a signature lists entries from the read position.
Two interpreter fields hold garbage and are normalised wherever a signature is derived or compared
(`DeriveSignature`, the oracle, lockstep):

- `_vu1Exec`'s `uregs`/`lregs` are uninitialised locals (`VU1microInterp.cpp:40-41`; only `lregs.cycles`
  is zeroed, `:121`). The FMAC-pipe register functions of SQ, SQD, SQI (`VUops.cpp:2468-2500`), CLIP
  (`:2325-2335`), FSAND to FCGET (`:2597-2715`), MTIR, RINIT and RXOR never set `VFwxyzw`, so the
  entry's `xyzwupper`/`xyzwlower` (`:336, 342`) take stack bytes. Their register field is 0, and stalls
  only match non-zero read registers (`:235-242`), so timing is unaffected; the xyzw field is zeroed
  when its register is 0.
- `flagreg` is the op's whole `VIwrite` mask; only its CLIP and STATUS bits are ever tested (`:54-62`),
  so it is reduced to them.

**Exactness and refused states.** Only completion times matter (the flush test, the stalls and
`_vuFlushAll` all use `sCycle + Cycle`), so "cycles remaining" is exact for every live entry that started
at or before the current cycle and completes within its latency. Two cases fall outside, and
`DeriveSignature` refuses them (the interpreter steps instead):

- **Stale entries.** `MTVU_VU_EXECUTE` sets `VU1.cycle = 0` (`MTVU.cpp:145`). After a budget exit, or
  with a savestate made mid-program in a non-MTVU session (§11), pipe entries then have `sCycle > cycle`.
  The flush tests and the FMAC and IALU stall tests subtract in u64 (`VUops.cpp:45, 82, 97, 116, 217,
  284`), wrap, and treat them as complete; but the FDIV and EFU stall tests compare `sCycle + Cycle >
  cycle` (`:249-255, 262-275`) and stall to the old time, about 3 M cycles, after which the budget ends
  the `Execute`. The first `_vuTestPipes` empties the pipes, so the refusal costs about one interpreter
  step.
- **Out-of-range remainders** (more cycles remaining than the pipe's latency): not produced by the
  interpreter; refused so that a corrupt savestate cannot reach generated code.

**The model.** For each instruction of a block the compiler loads the signature into a scratch `VURegs`
(absolute times relative to an arbitrary base, `xgkickenable = 0`, so `_vuTestPipes` skips the kick since
the scratch is not `&VU1`), then mirrors `_vu1Exec` (`VU1microInterp.cpp:38-247`) step by step: fetch, E/D/T
bits (an E-bit sets the countdown to 2 at every fetch, `:47-50`), register info into zeroed
`_VURegsNum`s, `cycle++` and the upper stall test, the I-bit or lower path with `_vuTestPipes` and the
VI-backup decrement (including its `(u8)` truncation, `:101, 128`, and reading the scratch's cycle where
`_vu1Exec` names the global `VU1`, `:91`), the pipe adds, branch and E-bit countdowns. The stalls,
flushes and pipe updates are the interpreter's own functions acting on the scratch; only the ~40 lines of
orchestration are copied. From the scratch's before/after state the compiler records, per instruction:

- the cycle at the instruction (after stalls) relative to block entry, and the stall;
- which FMAC entries, and whether FDIV, EFU and IALU, flushed at its `_vuTestPipes` (counts and enables
  before and after; FMAC flushes are in ring order);
- whether its op runs, is discarded (§7.9) or must see old values;
- whether an FMAC, FDIV, EFU or IALU entry was added, and its static fields;
- `VIBackupCycles`/`VIRegNumber` before the ops, for branch reads;
- the control state after it (branch and E countdowns).

The block's exit nodes (fall-through, taken, JR) come from the scratch at the end. The orchestration
copy is the only place the model can drift from the interpreter, and the timing oracle (§9.2) tests
exactly that. Refactoring `_vu1Exec` so both share the orchestration was considered and rejected for now:
data and timing are interleaved in the op bodies (`_vuBackupVI`, XGKICK, branches), so a timing-only mode
would mean a broad change to PCSX2's interpreter.

### 4.3 Discovery

From the main entry, a worklist of nodes:

1. Decode the block; classify each word with a decoder that mirrors the interpreter's opcode tables
   (unknown words become an instruction fallback, §8).
2. Run the timing model, which gives the successor nodes.
3. Successors: fall-through and taken targets of B, BAL and IBxx (targets are static:
   `(TPC after fetch + imm11 * 8) & 0x3fff`, `VUops.cpp:1463-1468`); for JR/JALR, every BAL/JALR return
   address in the program with the JR's exit signature, plus targets learned at run time (§6.4).
4. Stop at the program end (the instruction at whose end the E countdown reaches 0; §7.6) and at nodes
   already seen.

Caps: 8 signatures per PC, 1024 blocks and 4096 emitted instruction pairs per module. A successor beyond
a cap becomes an exit to the interpreter. Loops converge after one or two signatures in practice (BT3:
168 states over 124 targets; the largest program is 451 pairs). Within a program, VU code is fixed:
`Clear` through the ring only takes effect at the next `Execute`, because the VU thread does not run ring
commands while a program runs. The one exception is an SPR DMA into VU1 micro memory, which the EE thread
writes directly while a program may be running (`SPR.cpp:27-58`); that race exists for the interpreter
too (§6.2).

### 4.4 Analyses

- **Observation points for flags, Q, P and pipe snapshots.** Readers of committed `VI[MAC]`,
  `VI[STATUS]`, `VI[CLIP]` (FM*, FS*, FC*) and of `VI[Q]`/`VI[P]`; readers of the pending flags: FSSET
  (status bits 0–5, `VUops.cpp:1391-1396`), DIV/SQRT/RSQRT (they read and modify the pending status, and
  their FDIV entry snapshots it, `:352-361`), OPMULA/OPMSUB (the pending MAC w bits, which their status
  update also reads) and every FMAC-pipe op's snapshot; every exit (which materialises the pipes, §5.3);
  and the program end, which observes everything. A backward liveness pass over the block graph decides
  which flag words, snapshots and write-backs must be computed (step 3; steps 1–2 compute all).
- **Clean registers** (the spike's must-analysis): registers known to hold clamped values, so operand
  clamps can be skipped; clamped-copy cache and loop-invariant hoisting (step 3).
- **Register use**: which VF/VI/ACC/I/Q/P/R and pending-flag locals each entry must load and each exit
  must store.
- **XGKICK**: no static shortcut. A kick can be active at any entry: at a learned entry, at the main
  entry reached mid-program after a fallback, or at a continuation after a budget exit. So the inline
  `xgkickenable` test (§7.7) is emitted at every VU-memory write, every XGKICK and every instruction
  after which generated code may exit (about 15% of BT3's instructions, one load and a predictable
  branch each: about 0.1–0.2 ms per vsync, estimate).

### 4.5 Emission order per instruction

Generated code applies effects in the interpreter's order (§7.1). For the common instruction, for example
`MADDAy ACC, vf1, vf2y` with `LQ vf3, 0(vi1)`, that is: the flag write-backs of the FMAC entries that
complete now (scalar local moves), the upper op (clamp operands, `f32x4.mul`, `f32x4.add`, MAC and status,
result clamp, dest merge into the ACC local), the lower op (address, `v128.load`, dest merge into the VF3
local), and the pipe snapshot (three local moves). The cycle counter is updated once per block. Stalls,
flushes and pipe bookkeeping cost nothing at run time: they were decided at compile time.

---

## 5. WebAssembly code generation

### 5.1 Emitter

`pcsx2/wasm/WasmEmitter.h` starts from the spike's `wasm_emitter.h` (types, imported memory and table,
imported and defined functions, locals, exports, scalar and SIMD128 opcodes) and adds `br_table`, `select`,
`i64` arithmetic, `i32.load16_u`/`i32.store16`, `i32.extend16_s`, lane loads and stores, and a
`v128.const` pool kept in locals (§5.5). It produces bytes only; the engine validates them. A Devel build
option validates each module with `WebAssembly.validate` and dumps the bytes of any module that fails to
compile.

### 5.2 Module layout and granularity

One module per program version:

- Imports: `env.mem` (the shared memory, declared shared with min 0 and max 65536 pages, which covers
  `-sMAXIMUM_MEMORY=2GB`, `cmake/BuildParameters.cmake:92`) and a fixed set of helper functions (§5.6),
  bound to the main module's raw exports (`wasmExports`), not JS wrappers.
- One defined function, exported as `run`, of type `(i32) -> i32`.
- No data or element sections and no salt: V8 shares code between byte-identical modules while one is
  alive (measured), which makes recompiling after an overlay swap cheap.
- At most 4096 emitted instruction pairs (§4.3): about 80 k wasm instructions, about 5 ms of synchronous
  Liftoff work on the VU thread at 1.2 µs per VU instruction (measured rate). The spike measured
  TurboFan cost growing faster than linearly (8–11 ms for 16 k wasm instructions, 50 ms for 64 k), so
  without a cap a pathological program (about 25 k pairs under the block cap) would cost about 30 ms of
  Liftoff and seconds of background TurboFan.

Why not smaller units (measured): a module costs 0.15–0.4 ms and about 20 KB however small, while one
function inside a module costs about 7.5 µs and 5 KB. Per-block modules would also force the VU registers
through memory at every block transition. Why not bigger: programs are small (BT3's largest is 451
instruction pairs, about 20 KB of wasm), and one function per program keeps all registers in locals for
the whole run.

### 5.3 Calling convention and exits

- C++ calls `reinterpret_cast<s32 (*)(s32)>(slot)(entry_index)`: 5.2–6.6 ns per call (measured), 3,000
  calls per vsync.
- The function loads, from `VURegs` at constant addresses, the registers it uses (VF0 and VI0 included,
  §5.4), the pending flags, the committed flags, `VI[Q]`, `VI[P]`, `VI[I]`, `cycle` and, for entries with
  pending pipe state, the snapshot values. Then it branches to the entry's block.
- It returns an **exit id**, an index into the program's exit table: `{kind, pc, node}` with kind `END`
  (program ended) or `BOUNDARY` (state complete at the start of instruction `pc`, reason: fallback,
  budget, busy kick, D/T bit, unknown target, cap).
- Exit code is shared: one stub per (kind, signature, control state), with the PC and the runtime values
  (branch target, snapshots) in locals, not one stub per block.
- **The kick is already caught up.** Stubs never feed the kick: the last instruction before any possible
  exit has fed it at its own `_vuTestPipes` point, before its ops, as the interpreter does (§7.7). So at
  every boundary the `xgkick*` fields are the interpreter's, and a kick that the last instruction started
  is left untouched.
- **Every stub stores** what the function may have changed: written registers, pending and committed
  flags, Q/P, `cycle`, the VI backup fields, the pipe state, and TPC and the control state exactly as the
  interpreter holds them at that boundary:
  - TPC is the byte address of the next instruction, unmasked: `pc + 8` after the last instruction
    (0x4000 after 0x3ff8; the interpreter masks only at the start of each step, `VU1microInterp.cpp:292`),
    JR/JALR targets unmasked (`VI * 8`), branch targets masked (`VUops.cpp:1463-1468`);
  - `branch` is 0 or 1 and `branchpc` the pending target (static, or the runtime JR/JALR target);
    `ebit` is 0 or 1. Exits happen inside delay slots too: a busy XGKICK in an E-bit delay slot (the
    common `NOP[E]` then `XGKICK` pattern) needs `ebit = 1` (`:210-212`), one in a branch delay slot needs
    `branch = 1` and `branchpc` (`:194-199`). `takedelaybranch` is false at every instruction boundary
    (the interpreter clears it in the same step that sets it, `:200-206`), and the JIT exits before a
    branch in a branch delay slot anyway (§7.5).
- For `BOUNDARY` the stub materialises the pending pipe entries from the static signature and the runtime
  snapshot locals: entries from ring slot 0, `fmacwritepos = count & 3`, `sCycle = cycle - elapsed`, the
  same for IALU, FDIV and EFU. For `END` it writes empty pipes with consistent positions
  (`readpos == writepos`, count 0), `running = false`, and leaves a branch taken in the E-bit delay slot
  pending: `branch = 1`, `branchpc` = target, TPC = delay slot + 8, as the interpreter does, whose branch
  fix-up sits in its loop after the budget test (`:307-317`). Positions must stay consistent, because
  `_vuClearFMAC` adds at `fmacwritepos` and flushes start at `fmacreadpos`.

### 5.4 Where state lives

| State | During a run | Notes |
| --- | --- | --- |
| VF0–VF31, ACC | one `v128` local each (used ones only) | VF0 is loaded like the others, never constant-folded: VU0 can write VU1's VF and VI through its 0x4000 mapping (`VUops.cpp:1135-1150`). VU1 ops never write VF0. |
| VI0–VI15 | `i32` locals holding the 16-bit value | stored with `i32.store16`: the interpreter writes only `US[0]`. VI0 is a register too: it is loaded, and SQD/SQI/LQI can change it (§7.9). |
| I, Q, P, R, committed MAC/STATUS/CLIP | `i32` locals | `VI[REG_*]` at exit |
| Pending `macflag`, `statusflag`, `clipflag`, `q`, `p` | `i32` locals | they carry over between programs, so they are always written back |
| Pipe snapshots (flags of FMAC entries, FDIV `reg`/`statusflag`, EFU `reg`) | `i32` locals per pending entry | renamed with local moves at block transitions |
| VI backup old value | `i32` local | the cycles and register number are static |
| `cycle` | `i64` local | `VU1.cycle` is `u64` |
| Last taken branch target | `i32` local | `branchpc` at exit |
| VU data memory (16 KB) | memory, `VU1.Mem` constant base, addresses masked with `& 0x3fff` | never relies on traps |
| `xgkick*` fields | memory | read and written by `_vuXGKICKTransfer`; `xgkickenable` tested inline |
| `vuFBRST`, `vifRegs.top/itop`, `vu1InterpMTVURunning`, the dispatcher context | memory, constant addresses | |

Liftoff spills locals freely and TurboFan allocates them across the function; 16 x64 vector registers are
not enough for 32 VF registers, so some spilling is expected (the spike's kernel already ran at 1.0–1.3 ns
per VU instruction with this layout).

### 5.5 SIMD and scalar lowering

FMAC ops work on all four lanes with `f32x4.add/sub/mul`, which give per lane exactly what the
interpreter's scalar `f32` code gives (IEEE round to nearest, denormals kept; verified in the compiled
`VUops.cpp.o`: no fused ops, `f32.sqrt` and `f32.div` only). Lowering costs measured in V8 (x64):

- One instruction: `f32x4.add/mul`, `pmin`/`pmax`, compares, `i32x4.min/max_s/u`, `and`/`andnot`,
  `narrow`, `shr_s`, `bitmask`, and a lane broadcast written as a 32-bit-lane `i8x16.shuffle` (`vpshufd`).
- Three: `v128.bitselect` (dest merges today; a lane shuffle of old and new that V8 matches to one blend
  is to be measured in step 3), `i32x4.ne`. Six: a byte-reversing shuffle. Seven or eight: IEEE
  `f32x4.min/max`, which the VU never needs (MAX/MINI are integer compares).
- Operand clamp (`vuDouble`) and result clamp (`VU_MAC_UPDATE`) are the same 7-op integer sequence
  (spike's `Clamp`). MAC flags: three compares, two narrows, one `bitmask`; status: `i32x4.eq` and a
  bitmask, as in the spike. CLIP: compares, narrow, `bitmask`, and a 128-byte interleave table in the
  main module's data.
- Constants live in locals: Node 24's V8 rematerialises every `v128.const` at each use inside a loop
  (3 instructions); Chrome 155 does not. This "tuned" shaping gave 2× in Node and 1.2× in Chrome.
- **Never** relaxed SIMD (`relaxed_madd` is fused on FMA hardware and differed in 53% of special-value
  runs), never IEEE min/max for VU MAX/MINI, never a fused or reassociated form, and never an exponent add
  in place of a multiply by a power of two (FTOI, §7.8).
- Integer ops are scalar `i32` with 16-bit wraparound (`& 0xffff`, `i32.extend16_s` for signed
  compares). EFU math is never inlined (§5.6).
- All the costs above are x64. ARM64 has no single-instruction movemask, so `i8x16.bitmask` and
  `i32x4.bitmask` become instruction sequences there, and steps 1–2 use two bitmasks per FMAC op
  (unmeasured; §14).

### 5.6 Helpers

Imports of main-module C++ functions (4.3–7.1 ns per call, measured), all in `VU1JitHelpers.cpp`:

| Import | Calls | When |
| --- | --- | --- |
| `h_xgkick_catchup(i64 cycle)` | `_vuXGKICKTransfer((cycle - xgkicklastcycle) - 1, false)` | at the `_vuTestPipes` point of a VU-memory write, an XGKICK or the last instruction before a possible exit, behind the inline `xgkickenable` test (§7.7) |
| `h_upper(i32 code)`, `h_lower(i32 code)` | `VU1.code = code; VU1_UPPER/LOWER_OPCODE[...]()` | ops without native code; EFU ops always (they use `pow()` and libm, so the helper is exact by construction) |
| `h_stats(i32 id)` | counters | only with `-vu1jit-stats` |

**Helper protocol.** Before a call, the function stores to `VURegs` every local the op may read
(conservatively: its source registers, ACC, I, Q, P, R, the pending flags and `q`/`p`); after the call it
reloads every local the op may write. Per-op helpers are allowed only for ops whose body touches no timing
state; branches, XGKICK and the ops that call `_vuBackupVI` (IADD, IADDI, IADDIU, IAND, IOR, ISUB,
ISUBIU, MTIR, LQD, LQI, SQD, SQI; `VUops.cpp:420-434`) are always native. The upper/lower hazard rules
(§7.9) apply to helper-executed ops the same way as to native ones.

### 5.7 Control flow

- **Steps 1–2:** a dispatch loop. The function body is `block $exit (loop $dispatch (block $bN … (block
  $b0 (br_table …)) …))`; each block ends by setting the next block id and branching back, or by falling
  into its layout successor. Simple, always valid, and cheap: on a copy of the spike kernel (TurboFan only,
  bit-exact) one dispatch per 13 ops cost 2–3% on plain exact code, two 8–10%, four 15–17% (measured by
  the performance review on a loaded machine; ratios only). BT3 dispatches about once per 26 ops, so
  under 10% in this shape; large functions with many live vector registers are still unmeasured (§14).
- **Step 4:** structured control flow: natural loops become wasm `loop`s, diamonds become `if`/`else`,
  and the dispatch `br_table` remains only for irreducible flow and JR targets.
- **JR/JALR:** a compare chain over the targets compiled for the JR's exit signature (usually one to
  three return addresses); a miss exits `BOUNDARY` and is counted for learning. JR targets are computed as
  the interpreter does (`VI * 8`, masked only at the next fetch, `VUops.cpp:1621-1625`).
- No `return_call_indirect`: blocks do not call each other, so engine support for tail calls (Safari)
  does not matter.

---

## 6. Program cache and invalidation

### 6.1 Lookup

- A direct-mapped array of 2048 slots (PC / 8) holds, per entry PC, a most-recently-used list of
  programs. The common case (same program as last time, no `Clear` since) is one array load, one
  generation compare and one signature compare.
- Each program has a hash table of its entries (PC, signature) → entry index, used after a fallback.

### 6.2 Validation

- `Clear(addr, size)` increments `m_micro_gen` and ORs the range into a 64-bit dirty mask of 256-byte
  pages, both atomics (`fetch_add`, `fetch_or`), because the SPR path calls it on the EE thread
  (`SPR.cpp:58`). Nothing else. At the start of each `Execute` the VU thread exchanges the mask to 0 and
  files it under the current generation in a small private ring.
- A program stores the generation at which it was last validated, its ranges, a copy of their bytes and
  the page mask of its ranges. On lookup, if its generation is current it is valid; else, if its pages
  intersect the pages dirtied since (when the ring has wrapped, assume all pages), compare its ranges with
  `VU1.Micro` (`memcmp`, at most 3.6 KB in BT3). A match updates the generation; a mismatch moves to the
  next program of that PC, or compiles a new one.
- The SPR path writes `VU1.Micro` from the EE thread without waiting for the VU thread (`SPR.cpp:27-33`
  waits only for reads under MTVU). A write that lands while a program runs is a race for the interpreter
  too, which fetches each instruction from memory; generated code behaves as if the write landed after
  the program, which is one of the interpreter's possible outcomes. BT3's use of this path is not
  measured; step 1a counts EE-thread `Clear`s.
- Cost in BT3 (estimate): 8 `Clear`s per vsync, so a few dozen compares of a few KB per vsync, well
  under 0.1 ms. Comparing on every `Execute` instead would cost 1–2 ms per vsync, which is why the
  generation exists.
- This is software change detection: there is no page protection in wasm. It is also what makes overlays
  cheap: after BT3 swaps a microprogram back in, the old module matches again and runs at once.

### 6.3 When to compile

On the second clean-signature run of the same code from the same PC. Runs are counted per (entry PC,
fingerprint), the fingerprint being a hash of the 512 bytes of micro memory from the entry, in a small
table that `Clear` does not reset. A per-PC counter reset by `Clear` would compile some programs late or
never: in BT3, program 15 never runs twice at its PC without a `Clear` in between (1,255 runs), and
programs 5 and 18 manage it 4 times each (measured from the phase-1 run log). The spike measured that even
Liftoff code beats a fully optimised copy of the interpreter's arithmetic 2–5×, and the real interpreter
is several times slower than that copy, so a program pays off from its first or second run; the threshold
only avoids compiling programs that run once. Step 4 tunes it.

### 6.4 Learned entries and versions

After a `BOUNDARY` exit and the interpreter steps that follow it, the dispatcher looks up the current
(PC, signature) in the program's entry table. A miss is counted per (PC, signature); so is a JR to an
unknown target. When one reaches 2 hits, the program is recompiled with that entry (or JR target) added,
and the new version replaces the old one in the cache (the old module is freed after the swap; it is not
running, since the dispatcher is). Without this, every busy kick sends the rest of its run to the
interpreter: in BT3 about 2,500 runs per 1500 vsyncs, almost all in the 4.5k–7.3k-instruction programs 2
and 3 with 10–14 kicks per run, hold 0.82% of all instructions, so about 0.4% of instructions
(0.4–0.65 ms per vsync) would run in the interpreter (estimated from the phase-1 run log). After a busy
kick the signature depends on the kick's length, but it is usually the kick instruction's own upper FMAC
entry alone (the kick's cycle bump completes everything older), so few entries are learned.

Resume entries land in step 2; JR target learning, version caps and signature normalisation by liveness
in step 4. At most 8 versions per program; after that, misses run on the interpreter until the next known
entry.

### 6.5 Flushes, eviction and memory

- **Flush everything** when requested by `Reset` (boot, savestate load, any CPU/gamefix/speedhack change),
  on the first `Execute` of a new VU thread, and when the **settings key** changes: `vu0Overflow`,
  `vu1Overflow`, `VuAddSubHack`, and the addresses of `VU1`, `VU1.Mem`, `VU1.Micro` and the other baked
  constants. BT3 recompiles everything in 10–20 ms (§10.5), so finer invalidation is not worth it.
- **Eviction:** least recently used, beyond 2000 programs or the engine's cache cap: 48 MB of estimated
  cache in V8 (about 20 KB per module when first used, 5 KB when freed memory is reused; measured), and a
  smaller cap for JavaScriptCore, whose executable memory is a fixed pool shared with the main module's
  code (its size is unknown to us; 16 MB until measured). Freeing is `removeFunction` and dropping the JS
  handles; V8 collects the module later, and the memory is reused, not returned to the OS (measured).
- **Out of executable memory:** a compile or instantiate that throws `RangeError` flushes the cache and
  retries once; a second failure switches the JIT off for the session, with one log line.
- **Programs that fail:** a module that does not compile or instantiate for another reason, or a program
  whose instructions run more than 25% in fallbacks over 1000 runs, is marked interpreter-only by content
  hash until the next flush, with one log line.

---

## 7. Exact semantics and how each is reproduced

### 7.1 Per-instruction step order

| Interpreter step (`_vu1Exec`, `VU1microInterp.cpp`) | JIT |
| --- | --- |
| Fetch, `TPC += 8` (`:44-45`) | static; TPC only exists at exits, unmasked (§5.3) |
| E sets `ebit = 2` at every fetch; D/T with `vuFBRST & 0x400/0x800` set `ebit = 1` and `s_mtvu_dt_stop` (`:47-84`) | E static (an E-bit in the delay slot re-arms the countdown, §7.6); D/T: runtime test of `vuFBRST`, exit to the interpreter before the instruction if enabled |
| Upper register info, `cycle++` (in `vu1Exec`), upper stall test (`:88-93`) | timing model |
| I-bit: `_vuTestPipes`, VI-backup decrement, upper op, `VI[I] = lower word`; lower info zeroed (`:96-109`) | flushes, upper op reading the old I, then `I = imm` |
| No I-bit: lower info, lower stall, `_vuTestPipes`, VI-backup decrement, hazards, upper then lower (`:110-186`) | flushes, then the ops with the hazard rules of §7.9 |
| `_vuClearFMAC`, upper pipe add, lower pipe add (`:187-192`); the snapshot is taken after both ops | static entry; snapshot local moves |
| Branch countdown, delay branch (`:194-208`) | static control flow (§7.5) |
| E-bit countdown and program end (`:210-242`) | static end sequence (§7.6) |
| `fmacwritepos++` (`:245-246`) | static, normalised at exits |

### 7.2 Pipeline and stalls

All of `VUops.cpp:37-434` happens at compile time in the timing model: FMAC latency 4 and the stall rule
on `VFread0/1` (reads of VF0, ACC, Q and I never stall); FDIV 7 (DIV, SQRT) and 13 (RSQRT) with every
FDIV-pipe op waiting for the previous one; EFU latencies 11–54 with the `efu.Cycle -= 1` side effect at
every EFU-pipe stall test while busy (`:270`); IALU entries for ILW and ILWR only, tested only by
branches, with masks `1 << It` (so `ILW vi0` still stalls branches on vi0); ISW and ISWR are IALU-pipe
ops that never set `cycles`, so the model zeroes it as `_vu1Exec` does (`VU1microInterp.cpp:121`);
`_vuTestPipes` order FMAC, FDIV, EFU, IALU, which matters because FSSET's FMAC write-back and the FDIV
write-back both touch DS/IS; `MOVE vf0` without a pipe entry and `MOVE vfN, vfN` with one. Run time pays
only for the data side of each event (§7.3, §7.4) and for one cycle add per block.

### 7.3 Flags and their delays

- **MAC** (`VUflags.cpp:15-87`), per lane in dest: S = sign; Z for ±0; U and Z for a denormal result,
  which becomes ±0; O for exponent 255, clamped to ±`0x7f7fffff` when `vu1Overflow`. Lanes outside dest
  have all four bits cleared. OPMULA and OPMSUB update x, y and z only and leave w's bits as they were
  (`VUops.cpp:841-870`). ABS, MAX, MINI, FTOI, ITOF and CLIP do not touch MAC or status. With Fd = vf0
  the flags are still computed.
- **Status.** `VU_STAT_UPDATE` replaces the pending status with the four O/U/S/Z bits only, which also
  drops pending D/I and FSSET bits (`VUflags.cpp:89-97`). DIV/SQRT/RSQRT clear `0x30` and set I or D;
  FSSET sets `(imm12 & 0xFC0) | (status & 0x3F)`.
- **Write-back** happens when the FMAC entry flushes, 4 cycles later: `VI[MAC] = snapshot`, and
  `VI[STATUS] = (VI & 0xFF0) | low4 | low4 << 6` (the sticky bits), or the FSSET form when the entry's
  `flagreg` has the status bit (`VUops.cpp:54-62`). Every FMAC-pipe op flushes a snapshot, including LQ,
  SQ, MOVE and FSAND, which re-applies the pending flags of the last flag-setting op.
- **Clip.** CLIP shifts `clipflag = (clipflag << 6 | 6 new bits) & 0xFFFFFF` (integer compares, w taken
  as |w|, denormals widened); FCSET sets it; `VI[CLIP]` is written at flush only when `flagreg` has the
  clip bit.
- **Readers** (FSAND/FSEQ/FSOR on `VI[STATUS] & 0xFFF`, FMAND/FMEQ/FMOR on `VI[MAC] & 0xFFFF`,
  FCAND/FCEQ/FCOR writing VI1, FCGET) read the committed values; FSSET, DIV/SQRT/RSQRT and OPMULA/OPMSUB
  read pending ones (§4.4).

How: pending flags and committed flags are `i32` locals; the snapshot of each FMAC entry is copied into
per-entry locals at the add; each flush event of the timing model becomes the write-back above on locals.
Steps 1–2 compute every flag of every op. Step 3 computes only what an observation point can see (§4.4):

- `VI[MAC]` and the pending MAC at the program end need the full MAC of the last flag-setting op before
  it on each path, and of every op whose snapshot is live at an exit.
- The sticky bits are an OR over every snapshot flushed since the last FSSET, so they can never be
  dropped, only made cheap: `low4(a) | low4(b) == low4(a | b)`, so it is enough to OR each op's
  lane-condition vectors into one accumulator (an `or` per op) and reduce once. A snapshot that repeats
  the pending flags (an FMAC-pipe op that sets no flags) adds nothing unless an FSSET intervened, which the
  analysis tracks. OPMULA/OPMSUB keep the full computation (their w bits come from the previous op).
- The fuzzer must hold at 0 differences with and without these elisions; any doubt keeps the full form.

BT3 never reads MAC or status, so in step 3 about 45% of instructions lose most of their flag work.

### 7.4 Q and P

DIV, SQRT and RSQRT write the pending `q` at once; `VI[Q]` and the status bits `& 0xC30` are written when
the FDIV entry flushes (`VUops.cpp:92-109`). Q readers (MULq and the like) read the committed `VI[Q]`
and never stall; WAITQ and any FDIV-pipe op wait. EFU ops write the pending `p`; `VI[P]` at flush; MFP
reads the committed P without stalling. In generated code these are locals plus a write-back at the static
flush point. DIV, SQRT and RSQRT are inlined (scalar `f32.div`/`f32.sqrt` on clamped operands, the
divide-by-zero cases, `vuDouble` on the result; `VUops.cpp:931-1012`); EFU ops call `h_lower` (§5.6).

### 7.5 Branches, delay slots and the VI branch delay

- A branch's condition is evaluated at the branch instruction, the delay slot runs, then control goes to
  the target or falls through (the interpreter's `branch = 2` countdown). The link of BAL/JALR is the
  address after the delay slot, `/ 8`, computed from the unmasked TPC (`VUops.cpp:1603-1641`), so a BAL
  at 0x3ff8 links 0x801.
- **VI branch delay.** IBxx read `VIOldValue` instead of the register when `VIBackupCycles > 0` and the
  register matches (`VUops.cpp:1486-1595`); JR/JALR do not. The window (2 cycles after one of the 12
  backup ops, the oldest value kept on repeated writes, decremented by `min(1 + stall, VIBackupCycles)`)
  is static, so the compiler knows at each branch whether it reads the old value; generated code keeps the
  old value in a local copied at the backup op.
- **A branch in a branch delay slot** (`takedelaybranch`, `VUops.cpp:1470-1484`): exit to the interpreter
  before the first branch; it runs until `branch == 0`. Static handling only if a game shows it matters.
- **E-bit in a branch delay slot:** one instruction runs at the target, then the program ends. The target
  is a separate node with the E countdown in its key (§4.1), never the normal block at that PC. Static in
  step 2; a fallback in step 1.
- **A branch in the E-bit delay slot:** the program ends with the branch pending (`branch = 1`), and the
  dispatcher's loop head jumps to the target as the interpreter's does (§5.3). Static in step 1 (BT3's
  continuations may use it).

### 7.6 E, D, T and M bits; the program end

- **E:** an E-bit sets the countdown to 2 at every fetch (`VU1microInterp.cpp:47-50`), so the program
  ends at the end of the instruction after the last E-bit fetched: normally the delay slot, but an E-bit
  inside the delay slot re-arms the countdown and the program runs one more instruction. The model
  follows the countdown, so this is static. The end sequence, after the end instruction's ops (which
  fed the kick at its `_vuTestPipes` point, §7.7): `VIBackupCycles = 0`;
  every pending pipe entry is written back in `_vuFlushAll` order and `cycle` becomes the latest
  completion (`VUops.cpp:130-186`); `running = false`; TPC and the control state as the interpreter leaves
  them (§5.3); return `END`. The dispatcher flushes the kick and reports the E-bit interrupt (§3.4).
- **D and T** with `vuFBRST` bits set stop the program after the instruction and report a T-bit interrupt
  (`VU1microInterp.cpp:47-84, 234-240`). Generated code tests `vuFBRST` at such an instruction and exits to
  the interpreter before it; BT3 never sets them. The VU thread never touches VPU_STAT, VIF1 or the INTC.
- **M:** ignored on VU1, as in the interpreter.
- **Budget:** each block compares `cycle` at its last instruction's start with `budget_end` and exits to
  the interpreter otherwise; the interpreter then finishes the `Execute` exactly as it would have
  (§3.4).

### 7.7 XGKICK

The interpreter transfers one qword per 2 cycles from the kick (`cyclecount = 1` at the kick) through the
`_vuXGKICKTransfer` call in every `_vuTestPipes` (`VUops.cpp:188-208, 1826-1943`). On MTVU, partial tags
go through `CopyGSPacketData` and complete ones through `TransferGSPacketData` (`:1875-1885`), which on
the VU thread is a copy followed by `ExecuteGSPacketMTVU` (`Gif_Unit.h:620-631`).

- While a kick is active, the amount transferred by cycle C is the same however often the function is
  called: each call adds `C - xgkicklastcycle - 1` and sets `xgkicklastcycle = C - 1`. The transferred
  bytes only differ if VU memory changes in between, and the tag is only read when a new tag starts. So
  generated code calls `h_xgkick_catchup(C)` only at the instructions that need it, always at the
  instruction's `_vuTestPipes` point (before its ops, C its cycle after stalls) and only when
  `xgkickenable` is set (an inline load and branch, always emitted, §4.4; BT3: 0.23% of stores happen
  during a kick). The instructions that need it: a VU-memory write (SQ, SQD, SQI, ISW, ISWR, the only ops
  that write VU memory during a run), an XGKICK, and the last instruction before any possible exit (the
  last of each block, since the next block may exit for the budget; the one before a fallback or D/T
  instruction; a JR's delay slot; the end instruction). The GS receives the same bytes and the same
  complete tags; only the number of partial copies differs, which `CopyGSPacketData` does not see. The
  lockstep mode records tags and bytes to prove it.
- **Why at that point, not in the exit stub.** An XGKICK starts its kick after its own `_vuTestPipes`,
  with `lastcycle = C`; feeding at C after it would add −1 cycle and leave fields the interpreter never
  has. And at the program end, the end instruction feeds the kick at its cycle before the `_vuFlushAll`
  bump, then the final `_vuXGKICKTransfer(0, true)` runs. That split matters: with `flush = true`,
  `GetGSPacketSize` sums every tag up to the end of packet and cancels the kick when the total reaches
  0x4000 ("Bios does this", `Gif_Unit.h:591-615`), while non-flush calls read one tag at a time. Feeding
  extra non-flush cycles would change which tags are read singly and so the cancel outcome, and the BIOS
  is a test target.
- The kick itself is inline: catch up at the XGKICK's `_vuTestPipes` point, then if `xgkickenable` is
  still set the previous kick is running, so exit to the interpreter before this instruction; it flushes
  the old kick, adds `2 * qwords` to `VU1.cycle` and calls `_vuTestPipes` mid-instruction
  (`:1887-1888, 1915-1919`), which no static schedule can follow. This is the one exit after a partial
  instruction: the kick has been fed up to the XGKICK's own cycle, and the interpreter's step, which
  repeats the fetch and stalls, feeds it 0 more cycles, so the transfers and their order are the
  interpreter's (the contract notes it, §9.1). Generated code makes this test before the instruction's
  pipe write-backs, which do not interact with the kick, so everything else is the boundary's state. Otherwise store the kick fields
  (`addr = (VI[is] & 0x3ff) * 16`, `diff`, size 0, no end packet, `lastcycle = cycle`, `cyclecount = 1`).
- After a kick ends, the interpreter stops feeding cycles, while a batched catch-up feeds them all. So
  `xgkickcyclecount` and `xgkicklastcycle` differ once `xgkickenable` is 0. They are dead then (the next
  kick resets them, nothing else reads them) and the comparison ignores them (§9.1).

### 7.8 Float semantics and clamping

- Operands go through `vuDouble` (`VUops.cpp:440-457`): exponent 0 becomes ±0, exponent 255 becomes
  ±FLT_MAX **only if `vu0Overflow`**, the VU0 setting, even on VU1 (`:449`). Results go through the MAC
  update with the **VU1** setting (`VUflags.cpp:39`). Both default to on (`Pcsx2Config.cpp:459, 463`).
- **The JIT runs only with both on.** Then every operand of FMAC and FDIV ops is finite, ACC included,
  and those ops cannot produce a NaN. With either off, the interpreter runs (§12); this keeps the
  interpreter for about 70 `GameIndex.yaml` entries with a VU clamp mode of 0.
- **Where NaN can still arise**, and how it is kept exact:
  - EFU ops (EATAN of 0/0, for example) run the interpreter's own code through `h_lower`.
  - FTOI4/12/15 multiply the raw, unclamped operand by 2^k and then choose the saturation value by the
    sign of the product (`floatToInt`, `VUops.cpp:877-888`). For a NaN input the wasm spec leaves the
    sign of the resulting NaN undefined. x64 and arm64 engines keep the input's sign in both scalar
    (`f32.mul`, the interpreter) and SIMD (`f32x4.mul`, the JIT) multiplies, so the two agree in practice;
    the fuzzer feeds NaNs to FTOI and runs on arm64 (§9.7), and the multiply is never replaced by an
    exponent add (which would also differ for denormal and huge inputs).
- MADD/MSUB: `acc ± fs * ft` with two roundings, never fused. `VuAddSubHack` changes ADDi only
  (`VUops.cpp:567-578`): with the hack on, ADDi goes through `h_upper`.
- Ops defined on bit patterns use integer code: MAX/MINI (signed compares, reversed when both are
  negative), CLIP, FTOI (scale by 2^k, saturate when the exponent field is ≥ 0x9E, NaN included by sign;
  `i32x4.trunc_sat` gives 0 for NaN, so an extra select is needed), ITOF (convert, then scale by 2^-k),
  MOVE/MR32, ABS as the interpreter computes it.
- The VU1FPCR rounding and DAZ/FTZ settings have no effect on wasm (`FPControl.h`); ExtraOverflow and
  SignOverflow are microVU-only. Neither enters the JIT.

### 7.9 Integer, memory and other ops

- VI writes touch the low 16 bits only. Ops whose 4-bit destination field is 0 skip the write, so VI0
  normally stays as it is; but SQD, SQI and LQI test the 5-bit field (`_Ft_`/`_Fs_ != 0`) before changing
  `VI[field & 15]` (`VUops.cpp:1196-1197, 1214-1215, 1233`), so with the field equal to 16 they decrement
  or increment VI0. VI0 is therefore a real register in generated code (§5.4).
- LQ/SQ address `((s16)imm11 + VI[is]) * 16`, truncated to 16 bits, then `& 0x3fff`; LQD/SQD
  pre-decrement and LQI/SQI post-increment, with the interpreter's field tests above. Partial dest writes
  merge with memory. ILW with several lanes takes the last one; ISW stores `(u16, 0)` per lane
  (`:1236-1284`).
- RNEXT with Ft = 0 does not advance the LFSR (`:1335-1345`). XTOP/XITOP read `vu1Thread.vifRegs`
  (`:1815-1824, 1945-1954`).
- **Upper/lower hazards** (`VU1microInterp.cpp:130-186`): if the lower op writes the same VF index as the
  upper op (the index only), the whole lower op is discarded, VI side effects included (LQI's increment),
  but its timing still counts; upper CLIP with lower FCSET discards the lower op; a lower op that reads the
  upper op's VF destination sees the old value. Generated code computes the upper result into a temporary
  and commits it after the lower op.
- **Unknown words** leave `_VURegsNum` uninitialised in the interpreter (`VUops.cpp:3603-3613`), which is
  undefined behaviour in release builds. The JIT never compiles them: it exits to the interpreter, which
  then does whatever it does. Zero-initialising `lregs`/`uregs` in `_vu1Exec` is a separate, cheap fix
  for the interpreter (it would also remove the garbage pipe fields of §4.2).

### 7.10 Quirks kept on purpose

`efu.Cycle -= 1` on every EFU stall test; DIV not setting the sticky DS/IS bits; status update dropping
pending D/I; Z set with U on denormal results; `vuDouble` reading the VU0 overflow setting; OPMULA/OPMSUB
leaving w's MAC bits; the discarded lower op; the 5-bit test of LQI, SQD and SQI, which can change VI0;
JR targets and the TPC after 0x3ff8 unmasked until the next fetch, and BAL/JALR links computed from them;
an E-bit in the E-bit delay slot extending the program; a taken branch in the E-bit delay slot applied
after the budget test; I loads not clamped; `MOVE vfN, vfN` taking a pipe entry; ILW stalling only
branches; the `(u8)` truncation of the VI-backup decrement; FDIV/EFU stalls to a stale completion time
after `VU1.cycle = 0` (the interpreter's, since such states are refused, §4.2). Each one is a fuzzer case.

---

## 8. Fallbacks

| Level | What | Used for |
| --- | --- | --- |
| Op | `h_upper`/`h_lower` run the interpreter's op body inside generated code (§5.6) | ops without native code (step 1); EFU ops; ADDi with `VuAddSubHack` |
| Instruction | `BOUNDARY` exit; at least one `InterpVU1::Step()`, more while a branch or E-bit is pending or the state is refused, then a known (PC, signature) entry; after a budget exit the interpreter finishes the `Execute` | D/T bits enabled, branch in a delay slot, a busy XGKICK, unknown words, E-bit in a branch delay slot (step 1), caps, budget, a JR miss, refused states |
| Program | interpreter-only by content hash (§6.5) | modules that fail to compile, programs mostly in fallbacks |
| Whole JIT | `CpuIntVU1` everywhere | `EnableVU1=false`, MTVU off, either overflow clamp off, JIT initialisation failure, repeated out-of-memory compiles |

Every level hands over through `VURegs` in the interpreter's own format, so fallbacks are exact without
extra code. Each fallback reason is counted (`-vu1jit-stats`), so step exits can require "no fallback in
BT3 except busy kicks".

---

## 9. Correctness strategy

### 9.1 The contract

After every program, and at every `BOUNDARY` exit, generated code must leave exactly the interpreter's
**live** state:

- `VF[32]` (VF0 included), `ACC`, `VI[32]` (VI0, TPC, flags, Q, P, R and I included), pending `q`, `p`,
  `macflag`, `statusflag`, `clipflag`, `cycle`;
- the live pipe entries in ring order, compared by completion time (`sCycle + Cycle`) and static fields
  (normalised as in §4.2); `VIBackupCycles`, and `VIOldValue`/`VIRegNumber` while it is non-zero;
  `branch`, `branchpc` while a branch is pending, `ebit`, `takedelaybranch`; `xgkickenable` and, while it
  is set, the other `xgkick*` fields (at a busy-kick exit, fed up to the XGKICK's own cycle, §7.7);
- the 16 KB of VU1 data memory; the PATH1 output (concatenated bytes and the sequence of complete tags);
  the interrupt bits published (recorded through the sink, §9.4); the `vuCycles` value.

Normalised (dead or unobservable): ring positions, dead pipe slots, the xyzw fields of pipe entries whose
register is 0 and the `flagreg` bits other than CLIP and STATUS (§4.2), dead `xgkick*` counters (§7.7),
`VIOldValue`/`VIRegNumber` when `VIBackupCycles == 0`, `branchpc`/`delaybranchpc` with no branch pending,
`VU1.code`. The lockstep code lists each normalised field with the reason.

### 9.2 Timing-model oracle

- `-vu1jit-fuzz timing`: random programs run through `CpuIntVU1.Step()`; after each instruction,
  `DeriveSignature(VU1)` and the cycle delta must equal what the model predicted from the previous
  signature, and the recorded flush events must match the changes of the real pipe state. Covers stalls on
  every pipe, the EFU quirk, VI backup windows including the `(u8)` truncation, I-bit instructions, both
  hazards, E-bit re-arming, and refused (stale) states.
- `-vu1jit-oracle`: the same check on every VU1 instruction of a real run (BT3, the BIOS).
- It needs no code generation, so it lands first (step 1a).

### 9.3 Code generation fuzzer (Node)

`pcsx2-headless -vu1jit-fuzz SEED:COUNT[:MODE]` runs without a BIOS or a VM: it points `VU1.Mem` and
`VU1.Micro` at its own buffers, uses default settings with MTVU on, and runs on its own pthread (which
then owns its JIT instance).

- Random programs, weighted toward BT3's mix but covering every opcode, every dest mask and broadcast
  lane, the full 5-bit register fields (16–31 included, which reach VI0 through SQD/SQI/LQI), register
  aliasing (fd = fs = ft, upper/lower collisions), I-bit words, E-bit placements (including an E-bit in
  the E-bit delay slot and branches around it), branches forward and backward with counted loops, VI
  backup windows, D/T bits with `vuFBRST` on and off, the budget, and the end of micro memory (BAL and
  program ends at 0x3ff8).
- **XGKICK, XTOP and XITOP are included.** `GetGSPacketSize` only reads VU memory and the settings
  (`Gif_Unit.h:591-615`), the PATH1 recorder stands in for the two GIF calls, and XTOP/XITOP read two
  plain fields that the fuzzer sets. VU memory gets plausible GIF tags (short packets, multi-tag packets,
  packets that wrap at 0x4000 and totals that reach it), so busy kicks, kicks in delay slots and the
  batched catch-up are exercised, not only on BT3.
- States with 60% special operands (the spike's generator: denormals, ±0, ±Inf, NaNs, FLT_MAX/MIN, random
  bits), random VF0 and VI0, NaNs into FTOI.
- **Random entry states and forced exits.** A share of runs start after a random-length interpreter
  prefix of the same program, so generated code is entered with pending pipes, VI backup windows and an
  active kick; another share marks random instructions as fallbacks, so `BOUNDARY` exits happen
  everywhere (delay slots included) and execution re-enters at learned entries.
- Each program runs on the interpreter and on the JIT from the same state, and the contract (§9.1) is
  compared; each JIT optimisation can be switched off, so a difference can be bisected to one.
- **Both V8 tiers.** Fuzzed programs are fresh and run a few times, so they stay in Liftoff code (the
  spike saw tier-up after 1.5–4 ms of calls). Every volume is also run under `node --no-liftoff`
  (TurboFan only; about 5–10 minutes per 10^6 small programs at 13–18 µs per instruction compiled), so
  the optimising tier, and step 3's shuffle and blend patterns, are fuzzed too.
- Volumes: 10^5 programs at step 1b, 10^6 for step 1c, 10^7 for step 2 (the spike ran 4.8 × 10^5 runs
  with 0 differences and found one real bug on the way). A fixed-seed set of about a minute per tier is
  the regression check for every change to the JIT.

### 9.4 Lockstep mode in the emulator

`-vu1jit-lockstep` (headless) runs every `MTVU_VU_EXECUTE` twice on the VU thread:

1. snapshot `VURegs` and the 16 KB of VU memory (the VU thread's inputs `vifRegs.top/itop` and `vuFBRST`
   are read-only during a run);
2. run the JIT with the PATH1 recorder replacing the GIF calls (nothing reaches the GS) and the interrupt
   sink replacing `mtvuInterrupts.fetch_or`, both for the dispatcher's E-bit and for any interrupt the
   interpreter fallback publishes; snapshot the result and restore the state;
3. run the interpreter with the recorder in pass-through (it reaches the GS) and the sink recording;
4. compare by the contract, including the two recorded interrupt words; publish the interpreter's
   interrupt bits once with `fetch_or`; on a difference, log the program, the entry state and the first
   differing field, dump them for replay (§9.5) and mark the program interpreter-only.

`mtvuInterrupts` itself is never snapshotted or restored: the EE thread clears bits in it concurrently
(`MTVU.cpp:351-423`), and the GS handler on the VU thread sets SIGNAL, FINISH and LABEL bits in it during
the interpreter pass. Without the sink, the JIT pass would publish its E-bit before the interpreter pass,
and the EE could see two.

The game continues on the interpreter's results, so a lockstep run is an interpreter run as far as the
game can tell, and every program is compared, not whole frames. The 16 KB copies (about 3 per run,
roughly 150 MB per vsync, 130–190 ms per vsync with the interpreter, 4–5 minutes for 1500 vsyncs) make
lockstep a debugging mode, not a product mode.

### 9.5 Corpus replay

`-vu1jit-dump DIR` (with lockstep) writes each distinct (program content, entry state) once;
`-vu1jit-replay DIR` replays them in the fuzzer harness, without a BIOS or a game, which now supports
XGKICK. The BT3 corpus is game data: it stays outside the repository, as do the phase-1 microprogram
dumps.

### 9.6 Whole-run checks

Whole runs are not deterministic under MTVU: the EE picks up the VU thread's SIGNAL, FINISH and LABEL
events and E/T-bit interrupts whenever it polls (`Get_MTVUChanges`, `MTVU.cpp:351-423`, from
`VUmicro.cpp:32-35` and `Vif1_Dma.cpp:235`). Today the EE mostly waits for VU1; with the JIT the VU
thread runs ahead, so the EE sees those events at other emulated times, and frames can differ without
any JIT error. (`vuCycles` is not the cause with default settings: `vu1Instant = true` and
`EECycleSkip = 0`, `Pcsx2Config.cpp:386, 393`, make `Get_vuCycles` irrelevant, `MTVU.cpp:453-463`.)

- **Deterministic mode.** `-mtvu-sync` (headless only, off by default) makes `Get_MTVUChanges` call
  `WaitVU()` first (guarded, `MTVU.cpp:351`), so the EE always sees a program's events at the same point.
  Its control: two interpreter runs in this mode, at different machine loads, must give identical
  screenshots and per-vsync instruction counts (step 1a). Then JIT and interpreter must be
  pixel-identical in this mode.
- The BT3 fight savestate, 1500 vsyncs with the fight input script, screenshots every 50 frames
  (`-screenshot-every`, `-screenshot-dir`), in deterministic mode: JIT and interpreter pixel-identical,
  and a lockstep run with 0 differences.
- The BIOS from boot to its menu (600 frames), the same way.
- Savestates across backends, in deterministic mode: save with the JIT, load with the interpreter and the
  reverse, then compare screenshots after 300 vsyncs.
- In normal mode the same screenshots are reported next to a control pair of interpreter runs, as a
  sanity check, not a gate.

### 9.7 Engines

Exactness rests on the wasm spec (IEEE `f32`, no rounding modes, deterministic integer SIMD); NaN bits
matter only for FTOI4/12/15 with NaN inputs, and EFU ops run the interpreter's code (§7.8). The fuzzer
and lockstep run in Node, in both tiers. Each step's exit also runs the fixed-seed fuzzer in Chrome,
which needs the page's fuzz entry (§12): `pcsx2-headless` links for Node only (`-sENVIRONMENT=node
-sNODERAWFS=1`, `pcsx2-headless/CMakeLists.txt:22-23`). On arm64 (Chrome on an Android phone or an Apple
Silicon Mac, if one is available, §14) the spike kernel is measured before step 1b and the fixed-seed
fuzzer runs at step 2's exit. Firefox and Safari are untested: the features (SIMD128, shared memory,
synchronous compilation in workers, table growth) are standard; their speed and JavaScriptCore's
executable-memory limit are unknown (§6.5).

---

## 10. Performance

### 10.1 The metric

**VU1 execute time**: the time inside `CpuVU1->Execute` per PAL vsync, as the phase-1 instrumentation
measured it (`BeginExecute`/`EndExecute` around the call in `MTVU.cpp`). It holds generated code, the
dispatcher, compiles, fallbacks and the XGKICK/GIF transfers made inside `Execute`. It leaves out VIF
unpacks and ring handling on the VU thread (about 1.4–2.1 ms per vsync, estimate from the profiles), which
the JIT does not change. Today: 99 ms on a quiet machine, 152 ms under load.

Machine load alone moves that baseline by 1.5×, so every target is a ratio to the interpreter from
paired runs (§10.6), plus one absolute figure on a quiet machine when one is available.

### 10.2 Where the time goes after the JIT (estimates)

| Part, ms per vsync | Step 1 | Step 2 | Steps 3–4 |
| --- | --- | --- | --- |
| Generated code: full flags at steps 1–2 (the spike's plain exact code ran 2.9 ns per op in Node, 1.8 in Chrome; BT3 has 1.25 non-NOP ops per pair; 3.6–6 ns per pair with spills and dispatch); 2–5 ns per pair after step 3 | 3.6–6 | 3.6–6 | 2.0–4.9 |
| Fixed cost per run: dispatcher, lookup, register loads and stores, × 2,982 runs | 0.9–3.0 | 0.9–1.5 | 0.9 |
| XGKICK/GIF transfers inside `Execute` | 0.8–1.1 | 0.8–1.1 | 0.8–1.1 |
| Fallbacks at 104–160 ns per instruction (≤ 5% / ≤ 0.1% of 980 k) | ≤ 5.1–7.8 | ≤ 0.2 | ≤ 0.2 |
| **VU1 execute time** | **10–18** | **5.5–9** | **3.8–7** |
| Ratio to today's 99 ms | 5.5–9.5× | 11–18× | 14–26× |

The spike's numbers behind the first line: on a 13-instruction vertex loop, exact generated code ran at
1.0 ns (Node) and 1.3 ns (Chrome) per VU instruction with all optimisations, 2.9 / 1.8 ns without, against
13.5 / 16.2 ns for a copy of the interpreter's arithmetic and dispatch without its timing model. The real
interpreter costs 104–160 ns per instruction. Even Liftoff code (2.7–7.1 ns) beats the interpreter's copy
at full TurboFan speed. Helper calls cost 4.3–7.1 ns and calls into generated code 5.2–6.6 ns (3,000 per
vsync, about 20 µs).

### 10.3 The next limit, and what each step is worth

Per-thread busy time from the phase-1 profiles (400 vsyncs, two runs, ms per vsync; the profiler inflates
every thread by about 1.2–1.8×, as the VU thread's 171–188 ms against 99–152 ms unprofiled shows): EE
49–55, MTGS 14, and each of the two SW rasteriser helpers 67–69, 1.3× busier than the EE in the same
measurement. Unprofiled, the EE needs about 50 ms and each helper about 33–56 ms (with the default
`extrathreads=2`).

- So VU1 stops being the limit once its execute time is below roughly 50 ms. Fights are then bounded by
  max(EE, SW helpers): about 2–3× faster than today, roughly 35–40% of full speed (estimate).
- **Step 1 delivers that whole visible gain.** Step 2 (5.5–9 ms) meets the goal of VU1 well under 20 ms,
  which full speed will need once the EE JIT exists, and it brings full coverage and the switch-on.
- **Steps 3 and 4 change nothing visible in BT3** until the EE JIT (and, for the SW helpers, the WebGPU
  renderer) lands. Their only value meanwhile is CPU time, which matters on devices with fewer cores than
  busy threads (EE, VU, MTGS, two SW helpers and the page). They are deferred (§13).

### 10.4 Targets

Measured on the BT3 fight savestate over 600 vsyncs, VU1 execute time, median of paired runs (§10.6):

| | Today | Step 1 | Step 2 | Step 3 | Step 4 |
| --- | --- | --- | --- | --- | --- |
| VU1 execute time, ratio to the interpreter | 1× | ≥ 5× | ≥ 8× | ≥ 10× | ≥ 12× |
| The same on a quiet machine, ms per vsync | 99 | ≤ 20 | ≤ 12 | ≤ 10 | ≤ 8 |
| Instructions run in generated code | 0% | ≥ 95% | ≥ 99.9% | ≥ 99.9% | ≥ 99.9% |
| Fixed cost per run | — | ≤ 1 µs | ≤ 0.5 µs | ≤ 0.5 µs | ≤ 0.3 µs |
| Compile time over the run | — | ≤ 100 ms | ≤ 50 ms | ≤ 50 ms | ≤ 1% of VU1 execute time |
| Cache memory | — | ≤ 10 MB | ≤ 10 MB | ≤ 10 MB | ≤ 10 MB, bounded under an overlay stress test |

Step 1b has its own go/no-go figure: on programs 11 and 14 (half of BT3's instructions), generated code at
least 5× faster than the interpreter on the same runs (§13).

### 10.5 Compile cost (measured in the spike, estimated for BT3)

- About 48 bytes and 19 wasm instructions per VU instruction. A 1805-instruction program: 0.8–1.0 ms to
  compile, 0.04 ms to instantiate, about 2 ms for its first (Liftoff) run, 24–32 ms of TurboFan work on
  V8's background threads, after which later calls use optimised code. A 197-instruction program: 0.2–0.3 ms
  and 0.25 ms. Overall about 1.2 µs of synchronous work per VU instruction plus 0.15–0.4 ms per module.
- BT3 compiles about 19 programs of up to 451 instructions once per flush: about 10–20 ms on the VU
  thread and a few tens of ms of background compilation. Recompiling identical bytes while the old module
  is alive costs 0.02–1.2 ms.
- The module cap (§5.2) bounds the synchronous cost of one module at about 5 ms.

### 10.6 How to measure

- VU1 execute time per vsync from a counter around `Execute` (the phase-1 instrumentation, cleaned up and
  kept behind `-vu1jit-stats`, costs 5–8%; exits use runs without it for wall time), plus JIT counters:
  compiles, compile ms, runs, instructions in generated code, fallbacks by reason, cache bytes, and
  per-program execute time (for step 1b's figure).
- Paired runs: pinned with `taskset`, JIT and interpreter side by side, swapped between rounds, medians
  and ranges of the per-round ratio reported; the machine is shared, so single runs mean little.
- Node is pessimistic for SIMD code (constant rematerialisation), so each step also reports one Chrome
  figure from the page (§12).

### 10.7 Alternatives considered

- **A pre-scheduled interpreter** (from the performance review): run the existing op bodies through a
  per-block list of micro-ops built by the same timing model, with no runtime code generation. It removes
  decode, stall bookkeeping and most of the loop (about 65% of today's time) and keeps the op bodies and
  flag calls; the review estimates 30–45 ms per vsync on a quiet machine. Not chosen, because that lands
  just under the ~50 ms limit on a quiet machine and above it under load (45–68 ms), so the visible gain
  is not reliable; it saves less CPU time on low-core devices; the JIT's runtime (emitter, JS runtime, per-thread cache, lockstep) is what the EE
  JIT reuses; and its main cost and risk, the timing model, signatures and lockstep, are shared with the
  JIT and come first anyway (step 1a). It stays the fallback plan if step 1b's go/no-go fails.
- **A decode cache in the interpreter** (cache `_VURegsNum` and op pointers per PC, invalidated on
  `Clear`; estimated 1.25–1.35×, a few days): not planned. It changes the reference's hot loop while the
  JIT is being built against it, and after step 2 fallbacks are under 0.1% of instructions; lockstep at
  4–5 minutes per 1500 vsyncs is fast enough. It could be an interim speed-up if the owner wants one
  (§14).

---

## 11. Savestates

- **Saving** happens with the VU thread idle: `vuMicroFreeze` waits for it before writing the VU
  registers (`VUmicroMem.cpp:56-57`), `vuJITFreeze` waits again (`pcsx2/wasm/RecStubs.cpp:16-31`) and
  `mtvuFreeze` asserts it (`MTVU.cpp:51`). Between programs the whole state is in `VURegs`, so
  `vuMicroFreeze` (`VUmicroMem.cpp:101-147`) saves it as for the interpreter. `vuJITFreeze` keeps writing
  its two empty 96-byte blocks, so the format does not change.
- **Loading** waits for the VU thread and calls `ClearCPUExecutionCaches` (`SaveState.cpp:51-65`), so
  `Reset` flushes the cache and resets the ring positions. `mtvuFreeze` then re-sends the whole micro
  memory through the ring (`MTVU.cpp:54-58`), which calls `Clear` on the VU thread as well.
- A state saved mid-program (possible from a non-MTVU session) has pipe state, possibly stale against the
  reset `VU1.cycle` (§4.2), or a pending branch: the dispatcher derives its signature, or refuses it and
  steps the interpreter until the pipes are clean and the delay slot is over (§3.4).
- States made with the JIT and with the interpreter load in either, and differ only in the normalised
  fields of §9.1. The cross-load test is in §9.6.

---

## 12. Settings and switches

- **The switch** is the existing `EmuCore/CPU/Recompiler/EnableVU1` (`Pcsx2Config.cpp:532`). On wasm it
  selects the JIT when MTVU (`EmuCore/Speedhacks/vuThread`, on by default on wasm,
  `VMManager.cpp:3707-3710`) is on. Changing it already triggers a CPU-implementation switch.
- **Off by default until proven.** Both frontends build their settings in memory at each start
  (`pcsx2-wasm/Main.cpp:372-395`, `pcsx2-headless/Main.cpp:146-170`) from `SetDefaultSettings`, which sets
  `EnableVU1=true`. Until the switch-on decision they set `EnableVU1=false` explicitly; opting in is
  `-set EmuCore/CPU/Recompiler/EnableVU1=true` (headless) or a testing query parameter on the page,
  documented in `pcsx2-wasm/README.md`. The page parameter is needed from step 1a, because each step's
  Chrome figure and Chrome fuzzer run through the page: with it, the page also offers a fuzz entry (an
  exported function that starts the fuzzer on a worker and is polled for its result, so the page thread
  never blocks) and prints the JIT statistics to the console. The decision needs step 2's exit criteria
  and the owner's agreement; then the override goes away.
- **Forcing the interpreter** afterwards: `EnableVU1=false`, the same way. No page control until
  `UI-SPEC.md` asks for one.
- **Automatic fallback to the interpreter**, logged once: MTVU off; `vu0Overflow` or `vu1Overflow` off
  (for example a game database clamp mode); initialisation failure; repeated out-of-memory compiles
  (§6.5). `VuAddSubHack` is handled (§7.8).
- With `EnableVU1=false`, `VMManager` logs "VU1 Recompiler is not enabled" among its performance
  warnings (`VMManager.cpp:3461-3465`), which is accurate.
- **Debug options** of the headless runner, not settings: `-vu1jit-lockstep`, `-vu1jit-oracle`,
  `-vu1jit-stats FILE`, `-vu1jit-dump DIR`, `-vu1jit-replay DIR`, `-vu1jit-fuzz SEED:COUNT[:MODE]`,
  `-vu1jit-off=flags,clamps,timing,…` to disable single optimisations, and `-mtvu-sync` (§9.6).

---

## 13. Implementation plan

Each step ends with the owner's review; nothing is committed or pushed without the owner asking.

### Step 1a: plumbing, timing model, oracle and lockstep, no code generation

1. `pcsx2/wasm/WasmEmitter.h` and `WasmJitRuntime.js` from the spike; `-sALLOW_TABLE_GROWTH` and the JS
   library in `pcsx2-headless` and `pcsx2-wasm`; a self-test that compiles and calls a trivial module on
   the VU thread at the first `Execute`; the `thread_local` release check on thread exit.
2. `pcsx2/wasm/VU1Jit.{h,cpp}`: `WasmVU1`, the selection in `VMManager.cpp` `UpdateCPUImplementations`,
   the `EnableVU1=false` overrides in both `Main.cpp`, `Reset` (`WaitVU`, `CpuIntVU1.Reset()`, flush
   request) and `Shutdown`, the thread-safe `Clear`, a counter of EE-thread `Clear`s.
3. The dispatcher (§3.4), first as an "all-fallback JIT" whose every program is one `BOUNDARY` exit at its
   entry: with the forced step it is the interpreter, and it proves the plumbing and the exit rules.
4. `pcsx2/wasm/VU1JitAnalysis.{h,cpp}`: decode, the timing model, signatures with normalisation and
   refusal, `DeriveSignature`. `pcsx2/wasm/VU1JitFuzz.cpp`: the timing oracle on random programs and
   `-vu1jit-oracle` on real runs.
5. `pcsx2/wasm/VU1JitLockstep.cpp`: snapshots, the PATH1 recorder (hook in `VUops.cpp`), the interrupt
   sink (hook in `VU1microInterp.cpp`); `-mtvu-sync` (hook in `MTVU.cpp`); the headless options.
6. The page: the testing query parameter, the fuzz entry, the statistics (`pcsx2-wasm/Main.cpp`,
   `pcsx2-wasm/web/`, `pcsx2-wasm/README.md`).

Exit: the oracle at 0 differences over 10^6 random programs and over every instruction of the BIOS boot
and 1500 BT3 fight vsyncs; the deterministic-mode control (two interpreter runs at different loads,
identical screenshots and counts); the all-fallback JIT pixel-identical to the interpreter in
deterministic mode and 0 lockstep differences; the self-test passing in Node and Chrome; off by default.

### Step 1b: first generated code, and a go/no-go

1. `pcsx2/wasm/VU1JitCache.{h,cpp}`: the program cache with generation validation, MRU lists, the
   fingerprint threshold, interpreter-only marking. Discovery with the caps, the dispatch loop, shared
   exit stubs with the full control state and the kick catch-up rules.
2. `pcsx2/wasm/VU1JitEmit.cpp`, `VU1JitHelpers.cpp`: native code for BT3's most used op kinds (upper
   NOP, MADDA, MADD, MULA, MUL/MULq, CLIP, SUB, ADD, FTOI0/4, MULw; lower LQ, NOP, SQ, IADDIU, IADD,
   IBNE, IADDI, MOVE, DIV, FCAND, IBEQ, LQI, IBGTZ, SQI, FCOR, IAND), the branches, LOI, XGKICK's normal
   path and the program end; everything else through `h_upper`/`h_lower`. Full flags.
3. The fuzzer at 10^5 programs in both tiers; lockstep on BT3.

Exit (go/no-go): fuzzer 0 differences; lockstep 0 differences over 1500 BT3 fight vsyncs; on programs 11
and 14, generated code at least 5× faster than the interpreter on the same runs (paired). If it is not,
stop and report before going further: the pre-scheduled interpreter (§10.7) then becomes the
alternative, reusing step 1a.

### Step 1c: the rest of step 1

1. Native code for the remaining BT3 op kinds (§2.1) and their families (all ADD/SUB/MUL/MADD/MSUB forms,
   MAX/MINI, FTOI/ITOF variants), SQRT/RSQRT, the remaining integer, load/store and flag ops, XTOP/XITOP.
2. The fuzzer at 10^6 programs in both tiers, with random entry states and forced exits; corpus dump and
   replay; the fixed-seed fuzzer in Chrome; the statistics.

Exit:

- timing oracle and fuzzer (both tiers) at 0 differences over 10^6 programs; lockstep 0 differences over
  the BIOS boot and 1500 BT3 fight vsyncs; deterministic-mode screenshots pixel-identical in both;
- ≥ 95% of BT3's instructions in generated code; VU1 execute time ≥ 5× faster than the interpreter in
  paired runs (§10.4), with one Chrome figure;
- a JR miss, a busy kick (also in an E-bit delay slot), a D/T bit, the budget, a branch in the E-bit delay
  slot and a refused stale state each exercised by a fuzzer or directed case;
- off by default.

### Step 2: full coverage

1. Native code for every remaining op: EFU stays a helper by design; FSSET/FS*/FM*/FC* families, R ops,
   MFIR/MTIR, MFP, WAITP, OPMULA/OPMSUB, the rest of the FTOI/ITOF and CLIP forms.
2. Learned resume entries after `BOUNDARY` exits (§6.4); static handling of an E-bit in a branch delay
   slot; the busy-kick path verified; branch-in-delay-slot stays a fallback unless a game needs it.
3. Fuzzer over the full ISA at 10^7 programs in both tiers; savestate cross-load tests; the fixed-seed
   fuzzer in Chrome, and on arm64 if a device is available.

Exit: 0 differences everywhere; in BT3 no fallback except the busy-kick instructions themselves and
≥ 99.9% of instructions in generated code; VU1 execute time ≥ 8× faster than the interpreter (≤ 12 ms
on a quiet machine); then the switch-on decision (§12).

### Steps 3 and 4: deferred

They save CPU time but nothing visible in BT3 until the EE JIT or the WebGPU renderer lands (§10.3).
They start after that work, or earlier if a low-core device shows that the VU thread's CPU time matters.

- **Step 3, flag liveness, clamps and shapes:** observation points and backward liveness for MAC,
  status, clip, Q, P and pipe snapshots, with the pending-flag readers of §4.4; the sticky accumulator
  (§7.3); write-back elision; clean-register analysis, clamped-copy cache, loop-invariant clamp hoisting
  (1.1–1.5× in the spike); VI-backup and old-value elision where no branch can read them; register
  load/store trimming per entry and exit; V8 shapes (constants in locals, 32-bit-lane shuffles, `eq`
  rather than `ne`, blend lowering for dest merges). Exit: 0 differences with each optimisation on and
  off, in both tiers; ≥ 10×; Liftoff-only runs (`node --liftoff-only`) ≥ 3× faster than the interpreter.
- **Step 4, control flow and cache policy:** structured control flow (loops, diamonds), dispatch only
  where needed; JR target learning and version caps (§6.4); signature normalisation by liveness, so that
  states that differ only in dead pipe entries share blocks; LRU eviction with memory accounting; an
  overlay stress test (random uploads and runs) for bounded memory; compile-threshold tuning. Exit: ≥ 12×
  (Node and Chrome reported); compile time ≤ 1% of VU1 execute time over 1500 vsyncs; bounded cache
  memory under the stress test; 0 differences.

Then: `docs/web-port/PLAN.md` M5 updated with the measured results, and the EE JIT design starts from the
same runtime (emitter, JS runtime, per-thread cache, lockstep pattern).

---

## 14. Risks and open questions

| Risk | Mitigation |
| --- | --- |
| The orchestration copy in the timing model drifts from `_vu1Exec` (now or after an upstream merge) | The primitives are the interpreter's own; the oracle tests the copy on random programs and real runs; re-run it after every upstream merge touching `VU1microInterp.cpp` or `VUops.cpp` |
| An exit or entry corner (delay slots, budget, stale pipes, active kick at an entry) leaves a state the interpreter would not | The exit rules are the interpreter's state at that boundary (§5.3); the fuzzer forces exits everywhere and enters from random states; lockstep on BT3 and the BIOS |
| Too many signatures per PC | Caps and fallback (§4.3); measured per program in step 1b; normalisation in step 4 |
| TurboFan spills in large dispatch-loop functions keep code slower than the estimate | Measured in step 1b on programs 11 and 14, before most of the codegen; structured control flow in step 4 |
| Step 1b's go/no-go fails | Stop and report; the pre-scheduled interpreter reuses step 1a (§10.7) |
| Whole-run comparisons differ without a JIT error (MTVU event timing) | Deterministic mode with a control for the gates; lockstep is the primary check (§9.6) |
| Node figures understate Chrome (or the reverse) | Report both at each exit |
| arm64: unmeasured `bitmask` sequences in every FMAC op at steps 1–2 | Measure the spike kernel on arm64 Chrome before step 1b if a device is available; flag liveness (step 3) removes most of them if needed |
| Firefox and Safari: unmeasured compile cost and speed; JavaScriptCore's fixed executable-memory pool | Features are standard; per-engine cache cap and out-of-memory handling (§6.5); measure before claiming speed there |
| The interpreter's MTVU path is itself young (`c7b5a59027`, "needs proper testing") | The JIT follows it exactly, so a fix lands in one place; lockstep keeps them together |
| Only BT3 and the BIOS are available as real workloads | The fuzzer covers the ISA; more games when the owner provides them |
| An undefined-behaviour path in the interpreter (unknown words) | Never compiled; the interpreter fix is separate |
| SPR DMA writes VU1 micro memory from the EE thread during a program (an existing race) | Thread-safe `Clear`; generated code behaves as one of the interpreter's possible orders; EE-thread `Clear`s are counted (§6.2) |

Open questions for the owner:

1. The switch-on criteria: are step 2's exit plus 0 lockstep differences on BT3 and the BIOS enough, or
   should another game be tested first?
2. Deferring steps 3 and 4 until the EE JIT or the WebGPU renderer makes VU1 visible again (§10.3): agreed?
3. Is an arm64 device available (Chrome on an Android phone, or an Apple Silicon Mac) for one spike
   measurement before step 1b and the step-2 fuzz run?
4. The page's testing query parameter and fuzz entry from step 1a (§12): agreed? (The Chrome figures need
   it.)
5. An interim interpreter decode cache (estimated 1.25–1.35× on VU1, a few days, a change to the
   interpreter's hot loop): the recommendation is no, to keep the reference fixed while the JIT is built.

Settled since the first draft: VU0 micro-mode JIT, not planned (§2.3).

---

## 15. Proposed wording changes (not applied)

### 15.1 `AGENTS.md`, "WebAssembly pitfalls"

Replace the bullet "There is no `mmap` aliasing, page protection or fault handling: no fastmem, no
recompilers, no code generation at runtime. Memory "mappings" are heap allocations." with two bullets:

> - There is no `mmap` aliasing, page protection or fault handling: no fastmem, and PCSX2's x86/arm64
>   recompilers and `common/emitter` (native code) cannot run. Memory "mappings" are heap allocations,
>   and code changes must be detected in software (content compares), not by write protection.
> - Generating WebAssembly at run time is possible (`docs/web-port/VU1-JIT-DESIGN.md`,
>   `tools/wasm/wasm-jit-spike/`): build a module in memory, compile and instantiate it with
>   `new WebAssembly.Module`/`Instance` on the thread that will run it, and call it through that thread's
>   function table. Each pthread has its own instance and table: compile, call and release on the same
>   thread. Generated code must match the interpreter it replaces bit for bit and is tested against it
>   (fuzzing in both engine tiers, and lockstep); never use relaxed SIMD in it.

### 15.2 `docs/web-port/PLAN.md`, M5

- Task 1 ("JIT runtime"): one module per VU1 program, compiled synchronously on the owning thread; no
  async queue or hotness threshold beyond a second-run rule for VU1; imports bound to raw `wasmExports`;
  bytes copied before compiling; per-thread cache released on thread exit.
- Task 2 becomes: "**VU1 JIT, v1** (L). Keep microVU's program cache and its blocks-keyed-by-pipeline-state
  pattern; implement the interpreter's timing and flag model, not microVU's (`microVU_Analyze.inl` and the
  flag instances model a different machine). Emit SIMD128, fall back to the interpreter per op and per
  instruction. See `docs/web-port/VU1-JIT-DESIGN.md`."
- Task 3: full coverage (step 2); the optimisations of §13 steps 3 and 4 after the EE JIT. Remove "Then
  enable MTVU (`THREAD_VU1` tied to `REC_VU1`)": MTVU already runs the interpreter on wasm, and `REC_VU1`
  stays false.
- Task 5 (`return_call_indirect` linking) does not apply to VU1; keep it for the EE.
- The thread diagram in §3.1 (MTVU "idle on non-x86"), the MTVU blockers listed in §3.3, and M3 task 11
  ("Optional: MTVU-safe VU1 interpreter") are out of date since `c7b5a59027`.

---

## 16. References (at `a65e4ea5cd`)

- Interpreter: `pcsx2/VU1microInterp.cpp:38-247` (`_vu1Exec`; `:40-41, 121` the register-info locals,
  `:47-50` the E-bit, `:194-212` the countdowns), `:274-283` (`Reset`), `:290-294` (`Step`), `:296-323`
  (`Execute`); `pcsx2/VUops.cpp:37-72` (FMAC flush), `:92-109` (FDIV flush), `:130-186` (`_vuFlushAll`),
  `:188-208` (`_vuTestPipes`), `:210-296` (stall tests), `:317-349` (FMAC add), `:352-371` (FDIV/EFU add),
  `:420-434` (`_vuBackupVI`), `:440-457` (`vuDouble`), `:877-888` (`floatToInt`), `:931-1012`
  (DIV/SQRT/RSQRT), `:1135-1150` (`GET_VU_MEM`), `:1184-1234` (LQI, SQD, SQI), `:1463-1484` (branch target,
  `_setBranch`), `:1486-1641` (branches), `:1826-1943` (XGKICK), `:2325-2335, 2411-2500, 2543-2715`
  (register functions that leave `VFwxyzw` unset); `pcsx2/VUflags.cpp:15-97`; `pcsx2/VUops.h:33-39`;
  `pcsx2/VU.h:80-210` (the pipe structs, `VURegs`, `VUPIPE_*`).
- MTVU: `pcsx2/MTVU.cpp:15, 143-166, 282, 351-423, 441-465`; `pcsx2/VUmicro.cpp:25-36`;
  `pcsx2/Vif1_Dma.cpp:235`; `pcsx2/VUmicro.h:20, 29-78, 183-190`; `pcsx2/Config.h:1500-1511, 1528, 1541`;
  `pcsx2/Gif_Unit.h:591-631`; `pcsx2/SPR.cpp:27-58`.
- Selection, settings, savestates: `pcsx2/VMManager.cpp:2741-2797, 2992-3002, 3461-3465, 3707-3710`;
  `pcsx2/Pcsx2Config.cpp:386-393, 453-465, 524-542, 577-582`; `pcsx2/SaveState.cpp:51-65, 215-216`;
  `pcsx2/VUmicroMem.cpp:56-57, 101-147`; `pcsx2/MTVU.cpp:46-58` (`mtvuFreeze`); `pcsx2/wasm/RecStubs.cpp:16-31`;
  `pcsx2-headless/CMakeLists.txt:17-30`.
- microVU: `pcsx2/x86/microVU.cpp:102-114, 216-299, 341-346`, `microVU.h:46-77, 140-274`,
  `microVU_Compile.inl:36-108, 1039-1074`, `microVU_IR.h:22-61`.
- Spike: `tools/wasm/wasm-jit-spike/README.md` and its sources.

---

## 17. Review notes (2026-10-17)

Two reviews of the first draft, one for accuracy against the source and one for performance and
feasibility. Every confirmed item was re-checked in the source or the phase-1 data before the change;
the suspected ones were checked and are all accepted. The reviews' analysis scripts and their dispatch-loop
measurement live with the phase-1 data, outside the repository.

### 17.1 Accuracy review

1. **Dispatcher hangs after a `BOUNDARY` exit.** Accepted: one interpreter step always follows a
   `BOUNDARY` exit, and a budget exit hands the rest of the `Execute` to the interpreter (§3.4, §8).
2. **Exits inside delay slots stored `branch = 0`.** Accepted: stubs store `branch`, `branchpc` and
   `ebit` as the interpreter holds them at that boundary (§5.3).
3. **An E-bit in the E-bit delay slot.** Accepted: the countdown re-arms at every fetch; the model
   follows it (§4.2, §7.6, §7.10).
4. **VI0 and VF0 not constant.** Accepted: SQD added beside LQI/SQI; both registers are loaded from
   `VURegs` (§5.4, §7.9).
5. **Garbage pipe-entry fields.** Accepted: normalisation of xyzw when the register is 0 and of
   `flagreg`; zeroed `_VURegsNum` in the model, which also covers ISW/ISWR's unset `cycles` (§4.2, §7.2).
   Checked: no register function sets a non-zero `VFwrite` without `VFwxyzw`.
6. **Stale entries after `VU1.cycle = 0`.** Accepted: `DeriveSignature` refuses them; the `(u8)`
   truncation is part of the model (§4.2, §11).
7. **"NaN bits never arise" was false.** Accepted for FTOI4/12/15 (§7.8, §9.7). FTOI0 does no arithmetic
   and keeps the bits.
8. **XGKICK and XTOP/XITOP left out of the fuzzer.** Accepted (§9.3).
9. **Budget ending on a program end with a taken branch in the E-bit delay slot.** Accepted: END leaves
   the branch pending and the dispatcher's loop head applies it, as the interpreter's (§5.3).
10. **`Reset` did less than the interpreter's.** Accepted: `CpuIntVU1.Reset()` after `WaitVU` (§3.3).
11. **Masking at the end of micro memory.** Accepted: TPC is stored unmasked after every instruction, and
    links use it (§5.3, §7.5).

- a. **XGKICK reachability.** Accepted: the inline test is always emitted (§4.4).
- b. **The END catch-up cycle.** Accepted and confirmed in `_vu1Exec`: `_vuTestPipes` feeds the kick
  before the end instruction's ops, and `_vuFlushAll`'s bump happens before the 0-cycle flush. Working
  this through showed that exit stubs should not feed the kick at all (after an XGKICK that starts a kick
  they would feed −1 cycle): every instruction before a possible exit feeds it at its own `_vuTestPipes`
  point instead (§5.3, §7.7).
- c. **E-bit in a branch delay slot at step 2.** Accepted: nodes carry the control state (§4.1, §7.5).
- d. **Lockstep and the interrupt bits.** Accepted: an interrupt sink, no snapshot or restore of
  `mtvuInterrupts` (§9.4). The sink also covers interrupts published by the interpreter fallback inside
  the JIT pass.
- e. **Whole-run determinism.** Accepted: `-mtvu-sync` with a control run; normal-mode screenshots are a
  sanity check (§9.6).
- f. **Fuzzer starting states.** Accepted: random interpreter prefixes, forced exits, random VF0/VI0 and
  full 5-bit fields (§9.3).
- g. **Pending-flag readers for liveness.** Accepted; DIV/SQRT/RSQRT added, since they modify the pending
  status too (§4.4, §7.3).
- h. **`Clear` from the EE thread.** Accepted: atomic generation and dirty mask; the existing race is
  described (§4.3, §6.2).

### 17.2 Performance review

1. **The SW renderer is the next limit; steps 3–4 bring nothing visible.** Accepted. Re-running the
   review's per-thread analysis on both profiles gave the same figures (§2.1, §10.3). Steps 3–4 are
   deferred (§13). Step 2's ≤ 12 ms target stays: it is the owner's goal of VU1 well under 20 ms, which
   full speed needs once the EE JIT exists.
2. **Pixel-identity gate without a JIT bug.** Accepted (as 17.1 e); the `vuCycles` sentence in the old
   §9.4 was wrong with default settings and is corrected (§9.6).
3. **A busy kick sends the rest of its run to the interpreter until learning.** Accepted, with learned
   resume entries in step 2 rather than step 1: step 1's ≥ 95% target is not affected by 0.4% of
   instructions, while step 2's ≥ 99.9% is. The review's figures were re-derived from the phase-1 run log
   (2,535 runs, 0.82% of instructions, §6.4).
4. **Step 1 carried most of the work, with no speed figure until its end.** Accepted: steps 1a, 1b and 1c,
   with a go/no-go on programs 11 and 14 at 1b (re-checked: 50.3% of instructions, 49.6% of interpreter
   time) (§13).
5. **A pre-scheduled interpreter, or a decode cache.** Partly accepted: recorded as alternatives, the
   pre-scheduled interpreter as the fallback plan behind 1b's go/no-go. Not chosen as the plan, because it
   lands just under the ~50 ms limit on a quiet machine and above it under load, saves less CPU time,
   does not build the EE JIT's runtime, and shares the JIT's main correctness work anyway (§10.7). The
   decode cache is an open question with a recommendation against it (§14).
6. **The ≤ 10 ms target inside the estimate, and two metrics.** Accepted: one metric (time inside
   `Execute`), targets as paired ratios plus a quiet-machine figure (§10.1, §10.4).
7. **The fuzzer only tested Liftoff code.** Accepted: every volume also under `--no-liftoff` (§9.3).
8. **XGKICK out of the fuzzer.** Accepted (as 17.1 item 8).
9. **The Chrome fuzzer needed a build that did not exist.** Accepted: the page's testing parameter and
   fuzz entry in step 1a, which also answers the old open question 2 (§12, §13).
10. **No cap on module size.** Accepted: 4096 emitted pairs per module and shared exit stubs (§4.3, §5.2,
    §5.3).
11. **arm64 and Safari unmeasured.** Accepted: an arm64 measurement before 1b if a device is available
    (open question 3), per-engine cache caps, `RangeError` handling (§5.5, §6.5, §9.7). The size of
    JavaScriptCore's pool is still unknown here too.
12. **The compile threshold reset by `Clear`.** Accepted: runs counted per (PC, fingerprint), not reset by
    `Clear`; re-checked from the run log (§6.3). Compiling every program on its first run was not taken,
    because one-shot programs (BIOS start-up code) would pay a compile each.
13. **Minor errors.** Accepted: VU1 is 17% busy, not a third; BT3's compile cost is 10–20 ms, not "a few
    ms" (§2.1, §10.5).

The review's answer to the old open question 3 was also checked: COP2 and VU0 functions are 3.4–4.2% of
the EE thread's busy time in both profiles (1.7–2.3 ms per vsync, profiled), so VU0 is settled as a
non-goal (§2.3).
