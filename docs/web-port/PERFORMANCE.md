# Performance: what has been optimized, and what is in progress

The running record of the speed work on TabStationToo: every optimization, where it lives, what it
gained and how it was checked, then what is being worked on now and what is known to be left.
Update it with every optimization (newest entries at the bottom of each list). Numbers are from an
Apple M1 Max unless noted; "speed" is the emulator's speed in percent of a real PS2, with the frame
limiter off when it is above 100 %.

**Numbers from the Ryzen 7 PRO 7840U before 2026-10-26 were taken with its CPU capped.** On that laptop
TLP (Linux power management) held the CPU back on mains power as well as on battery; fixed on 2026-10-26
(`CPU_SCALING_MAX_FREQ_ON_AC=5132000`, energy preference `performance`). Their absolute speeds are too low.
Comparisons made there stay valid (both sides ran interleaved under the same cap), and so do the shares of
a thread's time, which are ratios. The SVR 2011 numbers below are the ones most affected: with the cap
removed the owner reports that the game runs smoothly (not measured again yet). Before measuring on Linux,
check that `/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq` equals `cpuinfo_max_freq` and that
`tlp-stat -p` (or `powerprofilesctl`) shows no power-saving profile on mains.

## How speed is measured

- **Crowd benchmark** (`tools/wasm/bench/`): a PS2 program with no menu that draws 1, 100, 200 and
  300 animated figures the way games draw characters. Even at 300 figures a real PS2 draws every
  frame in one vsync, so the emulator's speed measures only the emulator.
  `run-bench.mjs` runs it in Zen or Chrome and reports each phase's speed and thread load.
- **Real games, silently in Node** (`pcsx2-headless`, Emscripten's Node 24, no sound or window):
  Guitar Hero into a song and Naruto Shippuden Ultimate Ninja 5's menus and intro, driven by
  scripted button presses; GTA San Andreas (PAL) on foot in Grove Street, from a savestate taken
  after the intro, 2400 vsyncs with a scripted walk, speed counted from vsync 600 (the JITs compile
  from cold after a savestate); Tekken Tag Tournament (NTSC-U, SLUS-20001), a fight on the grass stage
  from the owner's savestate, 1500 vsyncs, speed counted from vsync 600. Game data never enters the
  repository.
- **The page**, while playing:
  - `?threads=1` adds how busy the EE, GS and VU1 threads are to the speed readout. The thread
    near 100 % is the one holding a game back.
  - **Recorder** (same switch): records every vsync's speed, thread load, JIT compiling and GPU
    pipeline creation. Crop to a drop, then Copy or Download.

## Where things started

The page ran the emulator at about 10-20 % in fights (interpreters, software renderer), and in Zen
always on the software renderer. The JITs and the WebGPU renderer then existed, but games such as
Guitar Hero and Naruto still ran under 50 % in Zen and 80-90 % in Chrome.

PCSX2's own speed comes from things a browser does not allow:
- recompilers that write machine code;
- memory tricks (`mmap`, page protection);
- hand-written SSE/AVX.

TabStationToo replaces them with WebAssembly JITs, software checks and SIMD128, and pays browser
costs PCSX2 never had: waking threads goes through JavaScript, and GPU pipelines compile in the
browser.

## Optimizations so far

Every optimization keeps emulation exact unless its row says otherwise. Each was checked with the JIT
fuzzers, lockstep against the interpreters, EE/IOP state hashes, and pixel-identical screenshots, as
noted. Changes inside PCSX2's own code are guarded for WebAssembly (`ARCH_WASM` or
`__EMSCRIPTEN__`) unless noted, so desktop builds are unaffected.

### Browser and page

| What | Where | Gain | Commit |
|---|---|---|---|
| WebGPU in Firefox and Zen. Firefox drops a WebGPU canvas's configuration when a frame is handed to the page, so only the first frame showed and the page used the software renderer in Firefox. The canvas is now reconfigured after each frame where needed. | `pcsx2/GS/Renderers/WebGPU/webgpu_host.js`, `pcsx2-wasm/web/app.js` | Zen moved from the software renderer to WebGPU | `220b6c393` |
| Present at most 512 lines in Firefox and on the 2D path, scaled up by CSS. Firefox copies every WebGPU frame back from the GPU: 9.6 ms a frame at a Retina canvas size, 2.9 ms at 640x512. | `pcsx2-wasm/web/app.js` | Zen, 25 figures: 164 → 367 % | `e034ac41a` |

### Threads and data movement

| What | Where | Gain | Commit |
|---|---|---|---|
| MTGS spins briefly for each VU1 program's XGKICK before sleeping. A sleeping thread costs the VU1 thread a JavaScript `Atomics.notify` per program, which was 13 % of its time. The spin adapts its length. | `pcsx2/MTGS.cpp` | Chrome, 100 figures: 102 → 104-110 % | `0715f8848` |
| Unmasked V4-32 VIF unpacks are copied straight into VU memory instead of through a function call per 16 bytes, later in at most two `memcpy`s. | `pcsx2/Vif_Unpack.cpp` | Chrome, 300 figures: 145 → 157.5 % | `b982eb59e`, `acb0eab2b` |
| The EE and VU threads' hot fields sit on separate 128-byte cache lines (Apple Silicon's line size). | `pcsx2/MTVU.h`, `common/Pcsx2Defs.h` | 0.16 ms per vsync on each thread | `be7b868ff` |
| The frame limiter keeps the time late frames lost (up to three frames), so lighter frames catch it up instead of sleeping. PCSX2's limiter forgives a whole frame as soon as one runs a frame late; GTA San Andreas alternates heavy and light vsyncs (35-60 ms, then 5-15 ms) and hit that every other vsync, so it ran at 85-95 % although its average work fits. Only real-time pacing changes, not emulation; after a stall the game runs up to about 108 % for a moment while it catches up. | `pcsx2/VMManager.cpp` | GTA SA gameplay, headless with the limiter on: 90.0 → 96.3 %. Replaying the owner's page recording: 94.8 → 98-99 % average, worst second 78 → 87 % | `5d2e6ebaa` |

### VU1 JIT (vertex processing)

| What | Where | Gain | Commit |
|---|---|---|---|
| Loops run inside generated code; MAX/MINI and DIV/SQRT/RSQRT are generated natively | `pcsx2/wasm/VU1JitEmit.cpp` and related | VU1 time 29.4 → 22.9 ms per vsync (benchmark) | `0b63055ae` |
| VU registers and flags kept in wasm locals; clamps skipped on clean values | same | 22.9 → 15.4 ms | `d7d7e53ba` |
| Remaining simple lower instructions generated natively | same | 15.4 → 15.2 ms; Chrome, 100 figures: 106 → 224 % | `75b98ac60` |
| A program's blocks jump to each other inside one wasm module, with learned JR/JALR targets and cheaper XGKICK catch-up. Guitar Hero's branchy VU1 code went back to the C++ dispatcher about every 9 instructions. | `pcsx2/wasm/VU1Jit.cpp`, `VU1JitEmit.cpp` | Guitar Hero in a song: VU1 15.1 → 10.0 ms per vsync (12.1 → 7.1 ns per instruction); song opening in Chrome 87 → 130-138 % | `ced73c7b6` |
| A branch or XGKICK in a branch's delay slot runs in generated code | same | (part of the above) | `c45dbbc50` |
| The same generator runs VU0 micro programs, now on by default (`EnableVU0`). GTA San Andreas ran VU0 programs on the interpreter for about 13 % of the EE thread. Checked with `-vu0jit-lockstep` on GTA gameplay (600 vsyncs, 1,008,250 programs, 0 differences), the fuzzer on VU0 (about 17,000 programs each with Liftoff only, TurboFan only and default tiering, 0 differences), GTA state hashes (VU0 registers included, 1000 vsyncs) and software-renderer screenshots. | `pcsx2-wasm/Main.cpp`, `pcsx2-headless/Main.cpp` | GTA SA gameplay (headless): 46.2 → 50.2 VPS | `6c4a3d41f` |
| VU0 blocks stop where the EE's slice ends. The EE runs VU0 in slices of its own cycles, mostly under 16; a block that did not fit a slice whole was not entered, and the interpreter stepped it instead: 23.7 M of the 77.5 M VU0 instructions in 1800 vsyncs of GTA San Andreas, about 6 % of the EE thread. Generated code now leaves before the first instruction that would start past the slice, with the state the interpreter has there; before a run of delay slots (a branch's, an E bit's, an E bit in an E bit's delay slot) it leaves before the instruction that sets them, so the interpreter stops among them as it would. Checked with VU0 lockstep on GTA (1,008,250 runs, 0 differences); the fuzzer, whose VU0 mode now runs a third of its programs in 1-32 cycle slices (Liftoff only, TurboFan only and default tiering, 2 rounds of 20,000 and 40,000 programs, 0 differences; VU1 0 differences); EE/IOP state hashes identical to the build before on GTA, the BIOS and the benchmark. `-vu0jit-slices 0` turns it off. | `pcsx2/wasm/VU0Jit.cpp`, `VU1JitEmit.cpp`, `VU1JitEmit.h`, `VU1JitFuzz.cpp` | GTA SA: VU0 interpreter steps 23.7 M -> 12,800 per 1800 vsyncs; instructions retired 1,045 G -> 995 G per 2400 vsyncs (-4.8 %, the whole process), cycles -4 %; speed from vsync 600 (noisy machine): 86.8 and 97.2 % off, 103.4 and 101.4 % on; re-measured on a quiet machine, medians of 3 interleaved: 93.4 → 96.3 %, instructions 1,042-1,045 G → 995 G | `b67ed7c8b` |
| OPMULA and OPMSUB (the outer product, a cross product in two instructions) are generated natively. They were the last upper ops on op bodies: each call stored and reloaded every register and flag the module keeps in locals. WWE SmackDown vs. Raw 2011 uses them in its lighting; a 6-man Hell in a Cell fight ran 15,800 VU1 programs a vsync with VU1 at 100 %. x, y and z of fs.yzx * ft.zxy (from ACC for OPMSUB) whatever the dest field, MAC's w bits kept from the pending flags, status from all four lanes. Checked with the fuzzer (Liftoff and TurboFan, 27,000 programs at the usual weights and 40,000 with the two ops weighted 40, 0 differences) and 400 vsyncs of the SVR fight with `-mtvu-sync` and the software renderer: VU, EE, IOP, SPU2 and GS memories and the screenshots byte-identical to the build before. | `pcsx2/wasm/VU1JitEmit.cpp` | SVR 2011 fight (headless, null renderer, Ryzen 7 7840U, 3 interleaved runs): 86.6-87.4 → 91.9-92.3 %; in Chromium on the 780M (WebGPU): 88 → 93 % | `9cf1444b2` |
| MAC and status flags kept lazily. A flag-setting op records only its raw result (1.0 in the lanes outside dest, which sets no flag) and the explicit bits (MAC's bits above 16, FSSET's and DIV's status bits, OPMULA/OPMSUB's w bits); a write-back folds the entry into four lane accumulators (minimum and maximum exponent, the signs, the minimum \|x\| - 1) for the sticky bits. MAC and status are computed only where something observes them: FMAND/FMEQ/FMOR, FSAND/FSEQ/FSOR, op bodies and the exits. Before, every FMAC op computed them in full (about 35 wasm ops) and every write-back updated VI[STATUS] (about 12), and the sticky chain kept all of it live through TurboFan. Checked with the fuzzer (VU1 and VU0 modes, Liftoff and TurboFan, 76,000 programs, 0 differences; a planted error in the sticky bits shows up in 730 of 2,569 programs). | `pcsx2/wasm/VU1JitEmit.cpp` | SVR 2011 fight, paused (VU1-bound), headless, quiet machine, 2 interleaved runs each: 91.5-93.0 → 97.1-97.4 % (87.0 % before the OPMULA commit too); live action is EE-bound and stays at 60-61 %, with VU1 72 → 61 % busy | `ccb406594` |
| VU0 runs modules of blocks, and its memory ops natively. VU0 compiled one block per module, so every branch inside a program went back to the C++ dispatcher, and it called the interpreter's op body for every LQ/SQ/ILW/ISW (and the I/D forms), around which generated code stores and reloads all its registers and flags (computing the lazy flags). Tekken Tag Tournament skins every vertex of its fighters on VU0: 17,000 programs a vsync of about 61 instructions and 4 blocks, VU0 43 % of the EE thread, of which about 15 % the C++ around each entry. `CompileModule()` now takes the VU: a VU0 module holds every block reachable from its entry (no `known`; at most 64 blocks and 1024 instructions, as random code otherwise makes modules TurboFan runs out of memory on), no block entered in a delay slot and no edge after an M bit; an edge tests the slice as the dispatcher would (`first_need`). Memory ops address VU0's 4 KB natively; one whose address has bit 14 set (VU0's mapping of VU1's registers) leaves before the instruction for the interpreter; in a branch's or an E bit's delay slot (where VU0 makes no exits) they keep the op body. The modules alone gave nothing (entries 69 M → 30 M per 600 vsyncs, but the generated code grew as much as the dispatcher shrank); with the native memory ops, +28 %. Checked: VU0 fuzzer (7 seeds of 20,000 or 40,000 programs; default tiering, Liftoff only, TurboFan only), 0 differences; VU1 fuzzer 0 differences; `-vu0jit-lockstep` on Tekken (20.2 M runs, 0 differences); EE/IOP/VU0 state hashes over 400 Tekken vsyncs (`-mtvu-sync`) identical to the build before. | `pcsx2/wasm/VU0Jit.cpp`, `VU1JitEmit.cpp`, `VU1Jit.cpp` | Tekken Tag Tournament fight, headless, 3 interleaved pairs: 49-51 → 64-65 % | `81f76df15` |
| An interlocked QMFC2 or CFC2 runs VU0 to its end in one Execute. `vu0Sync()` ran VU0 up to the EE's cycle (stopping at a slice in the middle of a block), then `_vu0FinishMicro()` ran it to its end: two Executes per program for a game that reads every result back with QMFC2.I. Both end on the same VU0 state; what differs is `_vu0FinishMicro()`'s own work (its cycle update, the EE's catch-up, VU1's ExecuteBlock), which it skips when the sync already ended the program. The VU0 JIT tells whether it would have (`VU0Jit::BeginWindow`/`EndWindow`: the last instruction started before the sync's cycle and no M bit stopped it before; `BlockExit::last_cyc` gives that start), and the rest follows in its order. Only with the cycle rate at 0 (it scales each Execute), VU0's D/T interrupts off, and VU0 on the JIT's dispatcher. Guarded for wasm (`pcsx2/VU0.cpp`). A first version that ran the finish whatever the sync would have done was not exact (it added cycles when the sync had ended the program) and was not kept. Checked: EE/IOP/VU0 state hashes identical to the build before over Tekken (600 vsyncs, `-mtvu-sync`), GTA (600) and FFXII gameplay (600). | `pcsx2/VU0.cpp`, `pcsx2/wasm/VU0Jit.cpp` | Tekken: VU0 Executes 30.3 M → 21.8 M per 600 vsyncs; with the idle loop rows below, 74-80 → 84-88 % (headless, interleaved, busy machine) | `8bd1fa3fb` |

### EE and IOP JITs (game code)

| What | Where | Gain | Commit |
|---|---|---|---|
| The common branch end runs in generated code, through one function per module. LDL/LDR/SDL/SDR are generated natively; they were 41 M interpreter calls per run. | `pcsx2/wasm/EEJit.cpp` | EE thread per vsync at 300 figures: 14.2 → 13.1 ms | `8a80a5fb7`, `07e93ed5b` |
| Compile hot code only. New code runs on the interpreter until it has run 128 times. Modules are larger, and compiling has a budget of 4 ms per vsync. | `pcsx2/wasm/JitHeat.h`, `EEJit.cpp`, `IopJit.cpp` | Worst compile in one vsync: Guitar Hero 69 → 7.9 ms, Naruto menus 56.7 → 5.0 ms; total compile 1.3 → 0.34 s | `7852633ef`, `c88cd23f3` |
| Guest registers in wasm locals. The EE keeps the low 64 bits of the 32 GPRs plus HI/LO; the IOP keeps its GPRs plus HI/LO. They load on first use and are written back before every C++ call and at exits. MMI lane ops, MFC0/MTC0 to plain registers and ADDI are now generated natively: the Naruto video's interpreter calls went from 189 M to 19 M per 600 vsyncs. | `pcsx2/wasm/RegCache.h`, `EEJit.cpp`, `IopJit.cpp` | Naruto video (headless): about +11 % | `0391d4a0d` |
| IOP branch start and end stores emitted inline; one EE helper fewer per module | `pcsx2/wasm/IopJit.cpp`, `EEJit.cpp` | (part of the above) | `0db7d0b22` |
| EE modules built on a thread of their own. Building a module (the generated code and its bytes) was 75 % of compiling it, the engine's compile 25 %. When a block is hot, the EE thread copies the pages the build reads and goes on interpreting; a thread builds the module, and the dispatcher compiles and installs it once ready, after checking that its blocks' words are still those it was built from (it then records them as a compile at that moment would, `CodeWrites.h`). Idle loop sites are added under a lock and never changed once made. `-eejit-bg 0` builds on the EE thread. Exact: state hashes identical to the build before over GTA (600 vsyncs), the BIOS (600) and the crowd benchmark (1150). | `pcsx2/wasm/EEJit.cpp`, `CodeWrites.h` | GTA SA from a save state (headless): EE-thread compile time 714-751 → 205-220 ms, worst module 8.4-8.7 → 2.4-7.6 ms; first 750 vsyncs 93 → 95 % and 88 → 92 % | `2a94d1f30` |
| Code lists: the EE JIT logs each block it compiles (PC and code hash); the page keeps the list per game in IndexedDB (`pcsx2-web-code`) and hands it back at the game's next boot. When the frame limiter has time left, listed blocks whose code is in memory are compiled before the game runs them (loading screens, light scenes). Tried and dropped: marking listed blocks hot, or spending each vsync's compile budget on the list, which made the first seconds after a load slower (64-76 % against 80-84 %). `?codelist=0`, `-eejit-ahead 0` turn it off. | `pcsx2/wasm/EEJit.cpp`, `pcsx2/VMManager.cpp`, `pcsx2-wasm/web/app.js` | GTA with the limiter on: 88 modules compiled ahead in spare time; checked in Chrome that the list is stored and handed back (14,375 blocks) | `2a94d1f30` |
| Code lists on the build thread: the build thread builds the list's modules whenever it has nothing else to build (at most 16 waiting); each goes in when the game reaches one of its blocks, or in the frame limiter's spare time. Before, compiling ahead ran only in spare time on the EE thread, so a game already behind got nothing ahead. EE/IOP state hashes with a list identical to the build before (GTA, 600 vsyncs). | `pcsx2/wasm/EEJit.cpp` | GTA right after loading a state, first 750 vsyncs: 317 modules from the list switched in, interpreted steps 19.3 M → 15.4 M | `f06948832` |
| Runs of loads and stores through one base register checked once (alignment, page, mapping). This roughly halves the kernel's thread context save and restore. | `pcsx2/wasm/EEJit.cpp` | Naruto video: +7-9 % more, about +24 % in total (47 → 58.5 VPS headless); Zen at 300 figures: 133 → 150-154 % | `693c92743` |
| Idle loops skipped up to the next event. GTA San Andreas waits for a DMA interrupt by polling two counters (a 10-instruction loop at `0x34dfd8`, run 250 M times per 1200 vsyncs): a third of the EE thread. At a taken backward branch whose loop has only register ALU ops, loads, branches and jumps, one iteration runs on the interpreter, checked step by step; if it changed no register, the iterations whose last branch test comes before the next event only add cycles (memory changes only in event tests), so they are skipped. The site remembers the registers and the values loaded, so the same state skips without running the iteration again. Exact: `-eejit-trace` replays every skipped branch test with its state, and the hashes over 1000 GTA vsyncs (906 M branch tests, 210 M of them skipped) are the same with the skip on, off (`-eejit-spin 0`) and in the build before it; the end state of 1000 non-traced vsyncs is the same on and off. | `pcsx2/wasm/EEJit.cpp` | GTA SA gameplay (headless): 34.7 → 46.2 VPS | `0b9ac18f0`, `582755a74`, `ef46ac2fc` |
| PEXEH, PEXEW, PREVH, PROT3W as byte shuffles, and DIV.S, generated natively: GTA's interpreter calls 21.4 M → 13.6 M per 1200 vsyncs | `pcsx2/wasm/EEJit.cpp` | within noise | `ef46ac2fc`, `035e32569` |
| One function per module (its unit): a module's blocks run in one loop, jump to each other directly, and keep the guest registers in locals from one block to the next. Exact: state hashes identical to the build before over GTA gameplay (600 vsyncs), the BIOS and the benchmark. On its own it does not change the speed: GTA's blocks are 4-5 instructions, and the time goes into the bookkeeping at each taken branch (PC, branch flag, cycles, the branch end call, the next block's checks: about 30 memory operations), which the units make it possible to keep in locals next. | `pcsx2/wasm/EEJit.cpp`, `RegCache.h`, `WasmEmitter.h` | GTA SA headless: 98.6 → 97.8 % (within noise); in the owner's page recordings from one save state, no gain (98.5 against 98.7 %), with 48 % more compile time (1.19 against 0.81 s per 2400 vsyncs headless) and 27 % more generated code, which slowed the first seconds after a load. **Reverted** | `7f41a4f0a`, reverted in the next commit |
| A block's code compare out of line. Each block compared the words it was compiled from inline in its entry checks, 64 bits at a time against constants, and ran that only when its page's generation had changed (once a vsync, `NoteAll()`, or after a write to the page); it made every block much larger. It now calls one C++ function, `CompareCode()`, with a descriptor of the block (host address, length, its words), kept by the module. Exact: end states of SVR 2011's live fight (600 vsyncs, `-mtvu-sync`) and of the crowd benchmark (1200 vsyncs) identical to the build before, apart from GS.bin, which differs between any two runs (the BIOS run differs between any two runs too: the clock). Measured first: keeping the generation check but no compare at all gave 59.6 → 63.3 % on SVR, removing every code check 65.5 %; the mapping check costs nothing measurable; checking pages against their blocks at vsync instead of the per-frame reset (`NoteAll()`) gave nothing, with or without this (61.5 against 61.8 %). | `pcsx2/wasm/EEJit.cpp` | SVR 2011 live fight, headless, 4 interleaved runs each: 59.5-60.7 → 60.0-62.3 % (means 59.9 → 61.6 %) | `718ecb92c` |
| An idle loop site that keeps failing waits without a call. Generated code counts the site's wait down itself (`s_spin_wait`) and calls `SpinTry()` only at 0; before, every taken branch at the site called into C++, which compared the loop's code before it looked at the wait. Tekken's vsync wait (below) made 133,000 such calls a vsync, 5.7 % of the EE thread. Only when a skip starts changes, which changes nothing but the time. | `pcsx2/wasm/EEJit.cpp` | Tekken fight, headless, 2 interleaved pairs: 64.5 → 66 % | `b39f50062` |
| Idle loops that call functions and store, skipped in closed form. Tekken Tag Tournament waits for the vsync in a loop that turns its random number generator (`jal 0x35c678`: seed = seed * 5 + 1, stored back), 133,000 iterations a vsync and about 16 % of the EE thread; the skip refused it (a call and a store). The tried iteration may now leave the loop's words through J, JAL, JR and JALR (that code is part of the memo) and store to aligned main RAM or scratchpad. A loop without stores is skipped as before; one with stores is analysed over the tried iteration's path (`SpinStoreLoop()`): every branch, jump target and address the same whatever the stored words hold, no register depending on them at the end, each stored word an affine function of its own old value (32-bit adds, subtracts, left shifts, moves), no other load touching them. Then n iterations add the same cycles and apply that map n times: one write per word, by powers (`SpinPower()`). With `-eejit-trace` the n iterations run on the interpreter instead and must end on the same words. Checked: state hashes identical to the build before over Tekken (600 vsyncs, 374.6 M branch tests, 513,006 store loop skips each replayed and matched), GTA (600), FFXII gameplay (600) and the BIOS (900); Tekken's end state after 600 untraced vsyncs identical with the skip on and off (every savestate part). | `pcsx2/wasm/EEJit.cpp` | Tekken fight, headless, 3 interleaved pairs: 48-50 → 74-80 % (198.8 M of 200 M iterations skipped per 1500 vsyncs); GTA unchanged (142-146 → 147 %, PAL) | `e75e9a5ca` |

### GPU pipelines (stutter when something new appears)

| What | Where | Gain | Commit |
|---|---|---|---|
| TFX pipelines are created in the background. A draw waits at most 3 ms (4 ms per vsync); if its pipeline is not ready, that draw is skipped for that frame. This is the owner's choice: an object can be missing for a frame or a few the first time an effect appears. `?pipelines=wait` restores waiting. | `pcsx2/GS/Renderers/WebGPU/GSDeviceWebGPUHW.cpp` and related | Naruto boot screens: 658 → 11 ms waiting, worst frame 417 → 29 ms | `d7ae5b791` |
| Each game's pipeline list is stored in IndexedDB (by serial) and prewarmed in the background at the game's next boot | same, `pcsx2-wasm/web/app.js` | Guitar Hero second boot: 1 ms waiting, nothing skipped; Naruto menus second boot: 0 ms | `d7ae5b791`, `4b37cfc7c` |

### WebGPU drawing

| What | Where | Gain | Commit |
|---|---|---|---|
| Feedback draws that read only their own pixel run as compute. WebGPU cannot sample a target inside a pass that draws to it, so each feedback draw copied the target and began a render pass of its own: about 15,000 copies and passes a second in Final Fantasy XII's cutscenes (423,000 in the measured run), which stalled the browser's GPU process. A draw whose shader samples the frame buffer at its own pixel (no filtering or palette, no blending, no depth test or write, area made of up to 8 axis-aligned rectangles) now runs as a dispatch of the fragment shader's own body (`cs_main` in `tfx.wgsl`), reading the old contents from one scratch texture and writing the target to the other; dispatches in one compute pass see each other's writes, so a run of such draws costs one compute pass and two copies (FFXII: 806 runs of about 525 draws). Coverage follows the rasterizer (pixel centres, top-left rule); a draw with an edge within 1/256 of a pixel centre keeps the render path. Pixel-identical: frame hashes on and off the same in FFXII (cutscene and gameplay), GTA SA, Naruto, Dragon Ball Z and Guitar Hero in Chrome, and FFXII in Zen. Not covered: FFXII's other 27 feedback draws in the cutscene (depth-tested triangles with fog and an alpha second pass) and about 700 in gameplay (blended lines; draws reading another texture), and the few in DBZ and Guitar Hero (other shapes and shaders). `?set=EmuCore/GS/WebGPUComputeFeedback=false` turns it off. | `pcsx2/GS/Renderers/WebGPU/GSDeviceWebGPUHW.cpp`, `bin/resources/shaders/webgpu/tfx.wgsl` | FFXII cutscene from vsync 300, Chrome: 50.6 / 51.1 → 99.9 / 100.0 % (limiter on), 54.2 → 114.5 % unlimited; Zen: 25.5 → 97.9 % (median). FFXII gameplay unchanged (100 %) | `5a3dc0238` |

### Video (FMV)

| What | Where | Gain | Commit |
|---|---|---|---|
| WebAssembly SIMD colour conversion for the IPU (yuv2rgb), byte-identical for every input | `pcsx2/IPU/yuv2rgb.cpp` | about 1 % of the EE thread in a video | `6f4323163` |
| IOP import calls skip a debug name lookup unless IOP BIOS tracing is on (shared code, not guarded; the name only feeds that log) | `pcsx2/IopBios.cpp` | about 1 % of the EE thread | `6f4323163` |

Together: Naruto's intro video, headless, 95.3 → 96.9 %.

### Opt-in speedhacks (they change emulation; off unless chosen)

| What | Where | Gain | Commit |
|---|---|---|---|
| Underclock: PCSX2's EE cycle rate (0, -1, -2, -3: the EE at 100, 75, 60, 50 %), chosen per game in the page and remembered per serial; every boot starts at Off. The EE JIT used to leave wasm for every taken branch and turn the idle loop skip off with any rate but 0, so the hack made things slower; now the branch end stays in wasm (the interpreter's own `intUpdateCPUCycles()` for the cycle update) and the idle loop skip models the rate. The JIT is exact against the interpreter at -1 and -2 (state hashes over 600 GTA vsyncs), and unchanged at 0. | `pcsx2/wasm/EEJit.cpp`, `pcsx2-wasm/Main.cpp`, the page | GTA SA gameplay headless, unlimited, medians of 3 interleaved on a busy machine (from vsync 600 of 2400): Off 86.4 %, Light 81.3 % (slower), Medium 108.6 % but CJ falls through the ground there (the game renders almost nothing), Strong 95.9 % (+11 %). GTA's clock keeps the same time at Off, Light and Strong (no slow motion). So for GTA only Strong helps; each game needs trying. | `6d4587379`, `c202859c6` |

### Options that cost speed (not optimizations; off unless chosen)

| What | Where | Cost | Commit |
|---|---|---|---|
| Resolution 2×: the hardware renderer draws at twice the PS2's lines (`pcsx2web_set_upscale()`, the Graphics menu's Resolution; native by default). Switching mid-game gives frames identical to booting at that setting (checked at three points of the benchmark, native, 2×, native again). | `pcsx2-wasm/Main.cpp`, the page | None measurable on the crowd benchmark: Chrome, unlimited, 1792x1344 presented, medians of 3 interleaved runs on a busy machine (load about 19, no quiet window), VPS native / 2×: 100 figures 196.9 / 194.3, 200 figures 120.5 / 122.5, 300 figures 86.9 / 86.8 (the benchmark is EE and VU1 bound). An M1 Max has fill rate to spare; a game that is GS-bound or a weaker GPU will pay more, not yet measured | the Resolution 2× commit |

### Measuring tools (no speed of their own)

- **The crowd benchmark** and its runners: `cfc473aed`, `2298389fb`, `f28aa05f8`, `df4d8a645`,
  `a1dbda87e`, `280678e8b`, `03b02602a`, `091fced07`.
- **Thread usage on wasm** (wall time minus waits) and `?threads=1`: `511b26cc9`, `1e2bb736d`.
- **The Recorder**: `556d474ad`.
- **Save states in the Recorder** (Save state, Load state, `?state=`), to measure from the same frame
  every time: `5b006b75c`.
- **Frame hashes in the benchmark runner** (`run-bench.mjs --capture`), to compare output between
  browsers and settings: `7162e0ba3`.
- **A Node runner crash fix**: `b16508154`.
- **Use Node 24 for headless runs**: Node 22's V8 runs out of heap on games. `36c826032`.

## What it gives now

The benchmark changed along the way (its 100-figure version became the 300-figure version, with
simpler distant figures), so each row compares one version with itself.

| Test | Start | Now |
|---|---|---|
| Benchmark, first version, 100 figures, Chrome | 102 % | 224 % (last measured on that version) |
| Benchmark, first version, 100 figures, Zen | 92.5 % | about 160 % (last measured on that version) |
| Benchmark, 300 figures, Chrome | 146 % | about 155-160 % |
| Benchmark, 300 figures, Zen | 114 % | 150-154 % (EE-bound) |
| Guitar Hero in a song, Chrome, limiter on | 80-87 % | nearly always 100 %, with rare dips |
| Naruto: menus, scene changes, special moves | long freezes on first appearance | first boot short skips; later boots smooth |
| Naruto intro video, Chrome | 82-84 % (measured by the owner before the EE register work) | not yet measured in the page; headless it runs about 24 % faster |
| GTA San Andreas on foot, headless (median of 3, from vsync 600 of 2400) | 34.7 VPS (69 %) | 50.8 VPS (101.5 %); the owner's page recording before this was 78 % on average (57-92 %), EE-bound. In one build with the features switched (a busier machine, medians of 3 interleaved): 31.1 VPS with both off, 43.8 with the idle loop skip, 47.8 with the VU0 JIT too |
| GTA San Andreas on foot, in the page (the owner's recording, after the idle loop skip and the VU0 JIT, before the limiter change) | 78 % average | 94.8 % average in gameplay, 59 % of the time at 99 % or more; dips to 76-85 % where the EE is busy |
| Tekken Tag Tournament, a fight on the grass stage (owner's state), headless, interleaved | 45-51 % | 84-88 % (EE-bound) |
| Tekken Tag Tournament, the same fight in Chrome (headless Chrome on the M1 Max, WebGPU, a fresh profile: no pipeline list, `?codelist=0`), limiter on, from vsync 600 | 50-51 % (mean 50.7) | 85-89 % (mean 87.3); EE thread 100 % busy, GS about 25 %, VU1 about 20 % |

## In progress

- **EE JIT: the bookkeeping at each taken branch** (started 2026-10-22). GTA's hottest blocks are 1-7
  instructions long, and each taken branch costs about 30 memory operations of bookkeeping (the PC and
  branch flag, the cycle counters, the branch end function, the next block's code and mapping checks)
  against the one or two of the game's own work. The units were meant to make room for keeping it in
  locals and were reverted (no gain, slower compiling); the bookkeeping is to be cut in the block per
  function design instead. Each step checked with state hashes.
- **GTA San Andreas in the page** after the limiter change: the owner's recording shows 99.3 % on
  average in gameplay, with EE-bound dips to 81-89 % at the start of the game.

### VIF UNPACK and WebGPU diagnostics (2026-11-18 experiment)

- `TST_WASM_DIAG_VIF=1` reports cumulative UNPACK calls in batches of 16,384 on each worker,
  split by VIF unit, mode 0 versus nonzero mode, and fill versus skip path. The S-32, V2-32
  and V4-32 counts are for unmasked mode 0. V4-32 includes packets handled by the existing
  contiguous copy.
- `TST_WASM_DIAG_GS=1` reports pending and failed TFX pipeline skips and render/depth target
  size mismatch skips at powers of two. Every 16,384 HW draw calls it reports cumulative
  totals, including feedback draws routed through the copy path. This count can include draws
  skipped before the copy. The draw and wait policies are unchanged.
- `TST_WASM_OPT_VIF32=1` adds direct mode-0, unmasked S-32, V2-32 and V4-32 writes.
  The existing contiguous V4-32 copy runs before the experimental path. All three flags are
  off by default. The standalone test at `tools/wasm/tests/vif-unpack-fast.cpp` matched
  9,126 reference cases for VU memory and VIF address, count and cycle state, including address
  wraparound, fill and skip cycles. It also checked both early exits.

God of War (SLPM-67010), first ship scene from the same state in Chrome 157 on Apple Metal,
at native resolution. Both diagnostic builds linked with Emscripten 6.0.12. Each build
reproduced a WebGPU validation failure in all three cold and three warm runs. The
`RGBA16Unorm` colour clip texture exposed `UnfilterableFloat`, while the utility bind group
required `Float`. This left the output black even with `?pipelines=wait`. Both builds produced
the same software-rendered PNG at vsync 475. The EE, IOP, VU and SPU2 state entries matched
in saves requested at vsync 465. The GS entry also varied between two runs of the baseline.
The hardware timings capture the validation failure and cannot establish a VIF speed gain.
Keep the fast path off by default.

For a rendering fix, check the `COLCLIP_RESOLVE` conversion in
`GSDeviceWebGPU::DoStretchRect()` and `ConvertPipelineKey()` in `GSDeviceWebGPU.cpp`.
Verify that the pipeline layout and sampler match the source colour clip texture's native
sample type before repeating the hardware A/B.

### VU1 module size and cache pressure in God of War (2026-11-18 experiment)

The baseline includes the ColorClip binding fix and leaves the experimental VIF path and
diagnostic logging off. Chrome 157 on Apple Metal replayed the same SLPM-67010 ship-deck
state at native resolution, with the frame limiter and default CPU settings. Each measured
run used a fresh Chrome profile, so it started without stored GPU pipeline or EE code lists.
The VU1 cache started empty. It belongs to its worker and a save-state load requests a flush.

The short screen measures vsyncs 450–520. Speed and P95 come from separate runs with tracing
off. The other columns are cumulative-counter differences between EE vsync markers 450 and
520 in diagnostic runs of the same settings. Evictions count dispatcher entries. A repeated
compilation fingerprint includes the full signatures, covered microcode and ranges, relevant
settings and emitted module shape. The fingerprint is used for diagnostics only.

| Versions per PC | Overall Speed | P95 | Recompiled after removal | Find / install evictions | Entry-limit flushes | Live Wasm at end (sampled peak) |
|---|---:|---:|---:|---:|---:|---:|
| 16 | 8.8% | 299 ms | 311 | 474 / 124,914 | 0 | 120.7 (120.7) MiB |
| 32 | 16.7% | 219 ms | 586 | 123 / 5,901 | 2 | 8.9 (17.3) MiB |
| 64 | 15.3% | 246 ms | 906 | 102 / 3,797 | 4 | 1.2 (9.1) MiB |

The original 16-version cache generated 127,637 blocks in the short diagnostic window.
The final 32-version, 64-block candidate generated 548 blocks and repeated 5 previously
removed identities in a short diagnostic run. Per-vsync generation fell from 55.1 to
0.26 ms and browser compilation from 19.2 to 0.09 ms. Churn continued later in the scene.
The separate 300-vsync candidate trace recorded 2,102 repeats and 32,946 generated blocks
at 2.54 ms of generation and 1.33 ms of browser compilation per vsync. With tracing off
and the 64-block limit, the final short screen measured 51.7% Speed with 16 versions
and 55.7% with 32. The 32-version candidate then ran the repeated long comparison. Incremental
global eviction and deferring secondary entries were also tried. Both strategies retained
high code-generation costs and were dropped. The candidate limits generated modules,
including merges, and removes global owning-vector entries by index while preserving each
PC's MRU order. The dispatcher passes a copied exit signature into lookups that can release
the exit's owning module. A bounded module leaving through a missing edge still uses the
usual dispatch and content checks.

The final test interleaved three trials per build over vsyncs 450–749, with tracing off.
Overall Speed is emulated time divided by measured wall time. Speed median and P10 use the
Recorder's ten-vsync smoothing. The figures below are medians of the three trials.

| Build | Wall time, 300 vsyncs | Overall Speed | Smoothed median / P10 | P95 | EE / GS / VU1 |
|---|---:|---:|---:|---:|---:|
| Baseline | 91.29 s | 5.5% | 6.6% / 3.2% | 681 ms | 16% / 4% / 100% |
| 32 versions, 64 blocks | 7.75 s | 64.6% | 70.8% / 45.7% | 50 ms | 100% / 19% / 70% |

The candidate's separate long diagnostic run sampled 36.7 MiB of live generated binaries at
its peak, with zero capacity flushes over the measured window. The experiment caps live
generated binaries at 128 MiB. Browser executable memory is outside that estimate. The
linear-memory sizes at the end of the performance runs had medians of 1,096 MiB for the
baseline and 807 MiB for the candidate.

Both builds showed the scene on WebGPU. The unsynchronized browser snapshots and save
requests are compared separately from the synchronized headless state. One browser
candidate run differed in screenshot hash and several state components. Two candidate
repeats matched the baseline screenshot and all separate state components other than the
version record and internal structures. The synchronized EE, IOP, VU, SPU2 and GS data
matched. The internal state differed only in unused IALU slots with an empty ring and
three serialized host GIF buffer pointers that the loader replaces with its own addresses.
In-session browser reload invalidated the cached modules and resumed the visible scene.
A 520-vsync interpreter lockstep run compared 72,509 VU1 executions with zero differences.
The fixed-PC cache replay passed for all three capacities, as did 1,500 randomized programs.

Use `?vu1cache=32&vu1cacheblocks=64` for this experiment. Default capacity and module limits
stay at 16 and 1,024 pending measurements from other games and scenes.

### God of War, smaller VU1 modules in sustained play (2026-11-18)

With 32 versions per PC, `?vu1cache=32&vu1cacheblocks=16` is available as an experiment.
Both settings ran in the same Chrome 157 / Metal build, at native resolution and stock timing.
Three interleaved trials per arm started from fresh browser profiles. An EE-vsync replay
applied the movement and attack inputs. Physical gamepads were isolated in the test page.

| Scene and measured vsyncs | 64 blocks, effective Speed (%) | 16 blocks, effective Speed (%) | Median wall seconds, 64 / 16 | Median frame P95 ms, 64 / 16 |
|---|---|---|---|---|
| Ship transition, 450–749 | 74.92, 77.64, 60.14 | 81.58, 97.04, 63.63 | 6.68 / 6.14 | 41.62 / 33.20 |
| Ship transition, 450–1649 | 47.59, 49.05, 42.77 | 63.82, 81.78, 64.38 | 42.07 / 31.10 | 91.63 / 38.53 |
| Enemy encounter from a later save, 450–1649 | 51.33, 48.42, 49.11 | 78.79, 64.08, 73.57 | 40.76 / 27.21 | 65.36 / 31.13 |

In separate VU1 diagnostic runs over markers 450–1650, generated blocks fell from
496,547 to 176,236, generation from 10.43 to 2.28 seconds, and browser compilation
from 4.49 to 1.16 seconds. Sampled live module payload peaked at 78.52 and 29.48 MiB.
Lookup hits rose from 13.25 to 23.33 million. Smaller modules leave more work to the
dispatcher. In a late CPU profile of the 16-block build, Dispatch accounted for 12.0%
VU1 self time. On EE, generated blocks and lookup took 58.4% and generated branch-end
and due helpers another 9.6% of the time-weighted samples.

The separate EE word-wise code comparison experiment was reverted. Its inclusive
profile share changed from 4.02% to 3.81%, and its input-replay long runs lost all
three pairs. The VU1 candidate passed 133,387 interpreter lockstep comparisons.
Cross-game coverage was a 600-vsync Tekken 5 startup with fixed RTC. Default settings
are unchanged. Stable 90% was not demonstrated.

## Tried and dropped

Measured, exact or not, and not kept because the gain was too small for the change:

- **EE loop regions, a quick study** (2026-10-23): in FFXII's speed mode about 12 % of the EE thread is
  inside loops, spread over many (largest 0.48 %), and 2.3 % in blocks that loop back to their own start; the
  "units" structure (a module's blocks in one function behind a jump table) makes FFXII's speed mode slower
  (63.7 against 59.6 % on the build of `7f41a4f0a`). Loop regions alone could give at most about 6 %; no
  prototype built. See `docs/web-port/EE-JIT-REGIONS.md`.
- **32-bit values in i32 locals** (2026-10-22, measured, not built): keeping registers that hold a
  sign-extended 32-bit value in i32 locals, to drop the `i32.wrap_i64` / `i64.extend_i32_s` pairs around
  32-bit ops. In three of GTA's hottest modules those conversions are 2.3-3.1 % of all wasm operations
  (a quarter of them zero extensions of addresses, which this would not touch), and 12 of the 293
  operations (4.1 %) of the hot byte-copy loop at `0x54ec3c`, a block of nearly only 32-bit register
  ops. Only part of them could go, and on ARM64 a wrap is free (a w register) and TurboFan folds an
  extend followed by a wrap, so the gain in instructions retired would be well under the 5 % bar.
- **EE JIT units** (one function per module, registers kept across blocks): no gain, slower compiling
  (`7f41a4f0a`, reverted).
- **Lean branch ends** (no PC, code and branch flag stores after a register-only delay slot): exact,
  0.9 % fewer instructions retired on GTA (branch `lean-branch-end`, not merged; also on `perf-experiments`).
- **A lower V8 tier-up budget** (`--wasm-tiering-budget` 1 M or 200 k against 13 M): 6-8 % more
  instructions on GTA, from the extra optimising compiles.
- **The last EE interpreter fallbacks generated natively** (2026-10-22): LWL/LWR/SWL/SWR, CACHE DXLTG,
  EI/DI, ADD, MULT1/MULTU1/MADD/MADDU, DIV/DIVU, SQRT_S/RSQRT_S, BC0F/BC0T as branches, the saturating
  PADD/PSUB ops. Exact (GTA, BIOS and benchmark hashes, a 1000-vsync end state, an op test against the
  interpreter), interpreter calls 13.5 M → 1.5 M per 1,200 GTA vsyncs, but 1,043.4 G → 1,042.4 G
  instructions retired per 2,400 vsyncs (within noise): their interpreter bodies were 0.5 % of the EE
  thread. Branch `native-fallbacks` (`950539bc7`), not merged; also on `perf-experiments`, which keeps both up to date with Yousri-v2.
- **IOP idle loops** (2026-10-22): in GTA San Andreas gameplay the IOP's hottest block (43 % of its block
  entries, `-eejit-stats` now lists the IOP's blocks too) is the kernel's idle loop, `J self; NOP` at
  0xbe9c, entered once per EE event test (about 6,300 times a vsync). The interpreter's wait loop skip
  (`doBranchEnd()`) already skips its waiting to the next IOP event; what each entry costs is the IOP run
  around it and the event test, which run anyway. The generated IOP and VU0 code together are 3.9 % of the
  EE thread. Stepping the idle loop on the interpreter instead of calling its block (exact by
  construction) gave the same instructions retired (996-1,000 G per 2,400 vsyncs, on and off): not kept.
- **Cheaper SPU2 mixing** (2026-10-22): the voices' volume slide tested inline instead of a call per voice and
  sample, and the per-voice IRQ address tests skipped when neither core has its IRQ enabled. Exact (EE/IOP state
  hashes for GTA, the BIOS and the benchmark, the mixed output hashed per sample over 1000 GTA vsyncs, and a
  1000-vsync GTA end state with SPU2.bin, all identical), but no gain: SPU2 stays 3.7-3.8 % of the EE thread in
  GTA (profile share, two runs each). In GTA both cores keep their IRQ enabled all the time, about 45 % of the 48
  voices play, the stopped ones still advance through memory at their pitch, and about 19 % of the volumes slide,
  so the per-voice, per-sample work is what the SPU2 does, not waste. Not kept.
- **IOP block linking** (2026-10-22): the IOP is 21-24 % of the EE thread in Naruto's intro video, DBZ BT3 and
  Guitar Hero (helper A's survey). In Naruto's video it splits into the generated IOP code 10.4 %, the work
  around each run of IOP code 8-9 % (`intExecuteBlock` 3.5 %, `iopEventTest` 2.7 %, the branch start and end
  1 %, `psxJ` fallbacks 0.65 %, the counters 0.65 %), SPU2 3.3 % and the IOP's hardware registers about 1 %. The
  IOP runs in short timeslices, one per EE event test: about 48,000 runs of IOP code per vsync, each of
  about one block (from one taken branch to the next). Tried: after a taken branch, `iopEventTest()`'s usual
  case and the next run of `intExecuteBlock()`'s loop done in generated code, tail-calling the target's
  block when nothing is due and the timeslice has cycles left (with the pending but not due interrupts tested
  as `_psxTestInterrupts()` does): it linked 28-43 M of the taken branches per 900 vsyncs and cut the runs
  started from C++ by a third. Inline in every block it was slower (Naruto 55 against 59.7 VPS, the event test
  inline alone 57); as one function per module it was the same as without it (Naruto 58.7-59.8 against
  59.4-59.9 VPS, DBZ 131.5-133.7 against 131.5-132.3, Guitar Hero 186.3-187.1 against 180.1-186.2, in quiet
  windows): each return to C++ costs about what the generated path does. Not kept.
- **Main RAM without the vtlb** (2026-10-22): accesses below 32 MB taken straight to main RAM, a range
  check instead of the table read. Exact on GTA (state hashes identical over 600 vsyncs), but 0.5 % more
  instructions retired (1,049 G against 1,043 G per 2,400 vsyncs) and the same cycles within noise: the
  vtlb's table read hits the cache and its branch predicts well on the M1, so a true "fastmem" would gain
  little more here, and WebAssembly cannot map memory twice or resume after a fault anyway.
- **Copy and fill loops run natively** (2026-10-22, measured only): EE thread profile samples per
  generated block, the blocks' code decoded from RAM. In GTA San Andreas gameplay (from vsync 720 of
  2400), every block that branches back to its own start is 0.81 % of the EE thread, and the simple
  memory loops among them (byte copy at `0x54ec3c`, byte and word fills, quadword copies) 0.29 %; in
  Naruto's boot (3000 vsyncs, second half) 3.6 % and 0.57 %. Under the 2 % that would justify an
  exact loop accelerator, so none was built: the EE time is spread over thousands of blocks (GTA's
  hottest is 0.48 %).
- **Repeated VU flag write-backs skipped** (2026-10-23): FMAC-pipe ops that set no flags (LQ, SQ, MOVE, NOP
  uppers) repeat the pending flags into the ring, and writing back the same snapshot twice changes nothing (both
  forms of the status update are idempotent), so the emitter tracked compile-time ids for the snapshots and skipped
  a write-back equal to the last one. Exact (fuzzer, Liftoff and TurboFan and VU0 mode, 60,000 programs, 0
  differences), but SVR 2011's fight stayed at 92.2 % against 91.0-92.5 % without it: TurboFan already drops the
  flag work nothing observes once the flags live in locals. Patch kept in the owner's scratch folder, not merged.
- **Regions for SVR 2011** (2026-10-23, measured, not built; the plan is `EE-JIT-REGIONS.md` on `Yousri-v2`).
  Phase 1 with synthetic ELFs (one loop of N ALU ops plus a counter load/add/store and a taken branch, Ryzen 7
  7840U): 14.3 / 15.2 / 17.9 / 41.2 ns per iteration at N = 1 / 4 / 16 / 64, so about 12 ns of bookkeeping per taken
  branch and 0.25-0.5 ns per ALU op; per op of 16 in a loop, loads and stores 0.5 ns, LQC2/SQC2 1.1-1.3, ADD.S/MUL.S
  2.7-3.4, VMUL/VMADD (VU0 macro) 6-7. SVR's live fight takes 170,000 branches a vsync (46,000 backward within
  1 KB, 70,000 forward, 21,000 JAL, 30,000 JR/JALR): about 2 ms of bookkeeping in the EE thread's 28 ms, of which
  loops are a quarter, so loop regions would give it about 2 %, under the plan's 10 % bar. With `perf` (V8's
  `--perf-prof`), JIT code is 40 % of the EE thread and almost all TurboFan; V8's tiering budget is about 7 %
  (60.5 → 64.6 % with `--wasm-tiering-budget=100000`, steady state; a page cannot set it).
- **Cycle skip for SVR 2011** (2026-10-23, built and taken out before it was pushed; patches in the owner's scratch
  folder). PCSX2's EE cycle skipping from the page, per game. The speed readout of the live fight went from 54-62 %
  to 92-105 % at level 2 (Chromium on the 780M: 46 → 88-97 %), but the game's own frame rate fell (27-29 to 22-26
  frames a second, `PerformanceMetrics::GetInternalFPS()`): it draws fewer frames per vsync, and the owner saw it
  just as laggy. SVR runs at 60 frames a second, so only a faster EE thread helps it; check the internal frame rate,
  not only the speed, before calling a speedhack a gain.
- **V8's tiers in SVR 2011, and compilation hints** (2026-10-23). `perf` with V8's `--perf-prof` over the live
  fight (40 s from the save state, EE thread): generated code in Liftoff, V8's baseline tier, is 19-30 % of the
  thread in the first 15 s and still 16 % after 35 s (TurboFan 41 %), because V8 tiers a function up after about
  13 MB of its code has run and a block is one small function. Liftoff code is about 20 % slower than TurboFan in
  a block-like test. The WebAssembly compilation-hints proposal (`metadata.code.compilation_priority`, optimization
  priority 127) made no difference in Chromium 156, by default or with `--experimental-wasm-compilation-hints`
  (test page in the owner's scratch folder), so a page cannot ask for TurboFan today.
- **More EE ideas on SVR 2011's live fight** (2026-10-23, each measured in interleaved headless runs, none kept):
  registers written through to `cpuRegs` as they are set, so that no write-back is ever emitted (61.4 against
  61.9 %); the idle loop skip backing off up to 65,536 calls at sites that never skipped, checked before the code
  compare (60.6 against 59.9 %: the 2 % `SpinTry` shows in profiles is the call itself); the units of `7f41a4f0a`
  built again with their parent (54.2 against 56.5 %); ThinLTO for the whole build (57.9 against 60.2 %); a larger
  minimum IOP slice in `_cpuEventTest_Shared()` (48 → 256: same, 1,024: slower). V8 tier-up tests in Chromium 156:
  a block that leaves by `return_call` is charged its budget like one that returns, and dead code skipped by a
  branch does not count, so neither can make blocks tier up sooner.
  After the out-of-line compare (`718ecb92c`) the code checks still cost about 10 % there (61.3 → 67.5 % with none;
  65.5 % with the generation check but no compare): the compare calls, about 9,000 a vsync, nearly all after
  `NoteAll()`. Returning to the dispatcher to compare instead of calling (no call left in any block) was slower
  (58.3 against 61.4 %: each changed generation breaks the chain of linked blocks), and with all blocks checked at
  vsync too (59.8 against 61.6 %), since that compares every live block, most of which do not run in the frame.

## Known to be left

- **A faster EE compiler** (a future project, not started): translating whole loops and functions instead of
  blocks of 4-5 instructions, the only remaining lever for the translated code's 50-70 % of the EE thread.
  Plan, plain-language explanation and exit criteria in `docs/web-port/EE-JIT-REGIONS.md`.

- **Zen against Chrome** (2026-10-22, one clean pair each, unlimited, in a quiet window): GTA from the
  Grove Street state 108.6 % in Zen against 124.6 % in Chrome (0.87; EE-bound in both, so Firefox's engine
  runs the EE JIT's code about 13 % slower); the crowd benchmark at 300 figures 127.5 % against 160 %
  (0.80), where Zen's GS thread is the limit (90 % busy against 61 %), probably the copy of each frame
  back from the GPU. Not profiled yet: one Firefox Profiler capture of GTA would show whether some
  generated-code patterns cost more in SpiderMonkey.

- **EE JIT compiling** (after `2a94d1f30`): the engine's compile and instantiation (about a quarter
  of compiling) stay on the EE thread, which runs the code; the IOP JIT still builds on the EE thread;
  modules from a code list go in when the game reaches them or in spare time; their engine compile is
  still the EE thread's.
- **GTA San Andreas** (on foot, headless, after the idle loop skip and the VU0 JIT; second half of
  2400 vsyncs). Share of the EE thread:

  | Share | What |
  |---|---|
  | 60 % | EE JIT code, flat: the 50 hottest blocks are 11 %, the branch end function 4 %, the lookup 1 % |
  | 15 % | event tests, of which the IOP 5 %, SPU2 mixing 3.5 %, VIF1 3 %, GIF 1.4 % |
  | 3 % | the idle loop skip itself (about 6,000 calls per vsync: the event that ends a skip comes every 1,000-1,500 EE cycles) |
  | 2 % | the interpreter (code not hot yet) |

  Before the skip and the VU0 JIT (whole run, from the savestate): the idle loop 33 %, VU0 micro
  programs on the interpreter 12 %, events 10 %, the interpreter while the JITs warm up 8 %. The EE
  JIT's interpreter calls are 21 M per 1200 vsyncs (DIV.S 4.6 M, CACHE 2.8 M, PROT3W 1.9 M, PEXEW
  1.3 M, LWL/LWR/SWL/SWR 0.9 M each, SQRT.S 0.9 M): about 1 % of the thread.

- **Naruto's intro video** runs at 82-84 % in Chrome. While the IPU decodes, the game's idle thread
  loops on a kernel syscall (RotateThreadReadyQueue), and each call saves and restores a thread
  context. Share of the EE thread:

  | Share | What |
  |---|---|
  | 48 % | EE JIT code |
  | 20 % | IOP (on the same thread) |
  | 7 % | event tests |
  | 7 % | IPU decoding |

  These shares are from before the EE register work, which made that video about 24 % faster
  headless. A "wait loop" skip like PCSX2's WaitLoop hack would gain a lot there, but it is a new
  timing hack: opt-in only, not started.
- **IOP JIT**: its dispatcher and event test on every IOP branch (about 8 % of the EE thread in that
  video). IOP block linking is not started.
- **EE JIT**:
  - **V8 tiering:** moderately hot functions stay in V8's baseline tier (Liftoff). The video ran
    6.6 % faster with V8 forced to optimise everything; merging a page's blocks into one function
    may get part of that. GTA, whose EE time is spread over 20,000 blocks, ran slower with
    `--no-liftoff` (37.9 against 46.2 VPS headless).
  - **Kernel thread switches:** SYSCALL, ERET and MTC0 Status still call the interpreter (about
    6,000 thread switches per frame in the video).
  - **The FPU's registers** are not cached in locals yet.
  - **Generated code size:** register write-backs made the EE's wasm 39 % larger and compiling
    38 % longer.
- **Zen** is 20-25 % slower than Chrome on the same work. There is no automatic Zen profiler yet;
  the Recorder works there.
- **The first time a game shows a new effect**, a draw can be skipped for a frame (by design). A
  skipped draw into a texture the game draws only once stays wrong until it is redrawn.
- **The VU1 JIT**:
  - 21 % of block-to-block steps still go through a `br_table` (natural loops could become real
    wasm loops);
  - flag computation costs about 10 instructions per flag;
  - a few rare delay-slot cases still go to the interpreter.
- **WWE SmackDown vs. Raw 2011** (NTSC-U, SLUS-21939; a 6-man Hell in a Cell fight from the owner's save
  state, which sits on the pause menu with the whole scene still drawn; Ryzen 7 7840U, under the TLP CPU cap
  noted at the top: the percentages are too low, the shares stand). 92 % headless with the
  null renderer, 93 % in Chromium on the 780M (WebGPU): the GPU is not the limit. VU1 is at 100 %: 15,800
  programs and 2.6 M instructions a vsync, 3.6 MB of PATH1 packets. Share of the VU thread (after `9cf1444b2`):

  | Share | What |
  |---|---|
  | 54 % | generated VU1 code |
  | 16 % | the MTVU ring loop (no heavy call of its own; probably the cache traffic of the data the EE writes) |
  | 12 % | per program: `Dispatch`, `ApplyExit`, `DeriveSignature`, `Find`, `WasmVU1::Execute` |
  | 7 % | XGKICK: `_vuXGKICKTransfer`, `TransferGSPacketData` |
  | 3 % | VIF unpacks on the VU thread |

  With the flag computation removed from generated code (a measurement only, wrong output) the fight runs at
  100 % with VU1 at 84 %, and the EE becomes the limit at 100 %: the flags that something observes (the status
  sticky bits, flushed for every FMAC entry) are about a fifth of VU1's time (18.2 → 14 ms a vsync). The game reads no status flag in its
  VU1 code and MAC flags at only 6 places (FMAND), so computing them lazily, at exits and readers, is the next VU1
  step. The EE thread is then the wall: a quarter of it moves data to VU1 (`vifCode_Unpack` 9 %, `Gif_Unit::Execute`
  6 %, `VIF1transfer` 4 %, `vu1ExecMicro` 3 %). A state taken in live action, not on the pause menu, is still to be
  measured.
- **SVR 2011 in live action** (the same fight unpaused with a scripted Cross, then a state saved 1,800 vsyncs
  later; the owner's page recording of a match agrees: 50-60 % with the EE at 100 % and VU1 at 55-65 %). All of
  this was under the TLP CPU cap noted at the top: with the cap removed (2026-10-26) the owner reports the game
  running smoothly, so the "1.65 times" below overstates what the EE lacks; measure again before planning on it. About 61 %
  headless with the null renderer on a quiet machine (50 % while the browser ran too): the EE needs about 1.65 times
  its speed. Share of the EE thread: generated EE, IOP
  and VU0 code 56 % (thin: no block above 1.2 %), VIF 10 % (`vifCode_Unpack` 5.6 %), `intExecute` 6 %, GIF and
  MTGS 4 %, SPU2 2.5 %, the IOP's C++ 3 %. Underclock does not help (EE cycle rate 0, -1, -2, -3: 48-55 % each).
  The idle loop skip finds 473 sites and skips none: the hottest (`0x1b51b0`, about 3,600 entries a vsync) polls
  bit 8 of a DMA channel's CHCR with a timeout counter in s1, which changes every iteration, so the skip refuses it;
  it is only some 15,000 instructions a vsync. Interpreter calls are about 10,000 a vsync (CACHE 3,900, COP2 2,300).
  So this game was the case for the faster EE compiler (`EE-JIT-REGIONS.md` on `Yousri-v2`), on a capped CPU.
- **SPU2 mixing**: 5-10 % of the EE thread. It processes all 48 voices every sample by design.
- **Tekken Tag Tournament** (the grass stage fight, after `8bd1fa3fb`): EE-bound at 85-89 % in Chrome. Share of the
  EE thread, headless: VU0 33 % (generated code about 20 %; the C++ around each of the 36,000 Executes a vsync about
  13 %: `Dispatch`, `NormalizeRings`, `ApplyExit`, `DeriveSignature`, `vu0ExecMicro`, `Find`), event tests 23 %
  (the IOP 9 %, VIF 6.6 %, GIF 3 %, SPU2 2.5 %), VIF1 unpacks into the MTVU ring 5 %, code checks 2-3 %. Each skinned
  vertex costs two VU0 Executes: `VCALLMS` runs the program's first 16 cycles (InterpVU0's start), `QMFC2.I` the
  rest. Running the start later, with the rest, would change where the program ends against the EE's cycles (event
  tests run VU0 too, without the EE's catch-up), so it is not exact; not done. 34 M COP2 interpreter calls per 1500
  vsyncs remain (VCALLMS, and the moves while VU0 runs, which generated code leaves to the interpreter).

### God of War combat module-size follow-up (2026-11-19)

The 16-block limit with 32 versions per PC remains the experiment. In a fresh A/B on
Chrome and Apple Metal, narrowing the cap to eight blocks cut compilation but did
not improve the main combat window. Both arms used the same replay build and a
fixed 1,200-vsync measurement from the late ship-deck save with sample-matched
controller input during vsyncs 450–1049.

| Window | 16 blocks, effective speed | 8 blocks, effective speed | Smoothed speed P10, 16 / 8 | Frame time P95, 16 / 8 |
|---|---|---|---|---|
| Deck, 450–1649 | 80.92 / 88.56 / 90.47% | 98.02 / 99.87 / 83.07% | 75.06 / 88.85% | 28.46 / 20.72 ms |
| Combat, 450–1649 | 97.66 / 96.67 / 78.15% | 96.08 / 95.71 / 98.92% | 89.41 / 85.90% | 20.18 / 20.69 ms |
| Combat after input, 1350–1649 | 100.00 / 99.94 / 92.62% | 97.11 / 98.53 / 100.00% | 99.42 / 94.69% | 17.19 / 19.06 ms |

While the input was active, 24.38% of VU1 samples were attributed to module
compilation and installation. In the late window the share fell to 17.51%,
while generated VU1 code was 38.44% and dispatch and exits 15.46% before lookup.
On EE, generated blocks and lookup accounted for 61.36%, branch helpers for
9.25% and event/exit work for 10.52%. GS waited or was idle in 58.35% of late
samples. These shares use counts within each role, with long sampling intervals
reported separately. The smaller cap is worth investigating for the deck
transition, while the combat results overlap and its late P10 regressed. The
8- and 32-block trials were removed from the worktree.

#### Ship-deck glare in the WebGPU hardware renderer

These observations describe the phase-three build, before the pass-reuse fix below.

In the Japanese God of War release (SLPM-67010), the default WebGPU hardware
blending overexposes parts of the rainy ship deck in this save. At matched
replay frames, the software renderer shows the enemies and railings behind
the bright patch. Hardware blending accuracy levels 2 through 4 and turning
off the feedback compute optimization leave the bright patch visible. Maximum
blending reproduces the software reference closely, but is impractically slow
on this backend in this scene. Use `?renderer=sw` for this view. It selects
software drawing with WebGPU presentation where available. The rendering
comparison is separate from the native-resolution hardware performance test.

The narrower `EmuCore/GS/HWAccurateAlphaTest=true` override also restores the
deck geometry and lighting. The toggle changes the alpha-fail handling in
`GSRendererHW::EmulateAlphaTest`, which isolates the discrepancy to that
hardware path rather than the presentation filter or texture filtering.
This override was slow on WebGPU as well. The software-drawing link is the
tested workaround. The exact draw and a fast hardware fix remain open.

#### Consuming a pending clear on WebGPU pass reuse (2026-11-19)

The frame-one GS prefix from the combat save isolates two effects. GS draw 6
uses pass-then-fail or feedback for RGB_ONLY. Their first RGB differences are
one code value, with identical alpha and depth. The occlusion error starts at
GS draw 12. The current depth attachment has a pending zero clear. Reusing
the pass leaves that clear pending across subsequent depth writes. When GS
draw 15 opens another pass, its load operation clears those newer values.
The real prefix reproduces 4,656 changed depth texels and 992 changed color
texels at draw 15 in both whole-machine and isolated WebGPU replay.

SendHWDraw now reopens the pass if either attachment has a pending clear or
invalidation. Its existing attachment load operations initialize the target
before drawing and mark it dirty. On combat frame 520, full-frame RGB MAE
against software fell from 51.60 to 0.65 out of 255. The original glare ROI
had 6,255 bright pixels before and zero after. Frames 30, 451, 620 and 1400
also retained the scene and weather with the standard hardware alpha path.

At native resolution on the M6 Mac mini in Chrome 157, the fixed-input
450–1649 combat window ran at 93.24 / 93.02 / 94.27% with this fix and
92.64 / 91.86 / 92.86% at baseline. The fixed median smoothed P10 was
84.03% and the median frame-time P95 was 21.95 ms. The first 300 combat
vsyncs had a median speed of 84.66%. Deck repeats ranged from 87.20 to
94.59%. The combat controls skipped 3 / 2 / 1 pipeline draws in the window
and the fixed runs skipped none. Deck runs had in-window skips in both sets.
These measurements retain the 32-version, 16-block VU1 configuration.
