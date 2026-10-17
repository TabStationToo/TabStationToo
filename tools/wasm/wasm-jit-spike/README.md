# Runtime WebAssembly code generation spike (VU1 JIT, phase 1)

A standalone Emscripten program that answers the platform questions behind a VU1 JIT before any
emulator code is written. Can a pthread that behaves like the MTVU thread build a WebAssembly
module in memory, compile and instantiate it synchronously, and call it from C++? What does that
cost? How fast is generated VU-style SIMD code, and can it match PCSX2's VU interpreter bit for
bit? It does not touch the emulator.

- `wasm_emitter.h`: a small emitter for wasm binaries: types, imported memory, table and
  functions, functions with locals, exports, scalar and SIMD128 opcodes.
- `vu_model.h`: a miniature VU1. It has the register state and a decoded form of the instructions
  a vertex-transform loop uses (MULAbc, MADDAbc, MADDbc, MULbc, ADD, SUB, MUL, MADD, MAX, MINI,
  CLIP, LQI, SQI, FCAND, IADD, IADDIU, ISUBIU, IBNE). It also has an interpreter that copies the
  arithmetic of `pcsx2/VUops.cpp` and `pcsx2/VUflags.cpp` with the same per-instruction dispatch,
  and a compiler from that form to one wasm function. The interpreter copies `vuDouble` operand
  clamping, `VU_MAC_UPDATE` result clamping, MAC and status flags, CLIP and integer MAX/MINI. It
  leaves out what the real interpreter also does per instruction: decoding, the pipeline and stall
  model, delay slots and the flag pipeline. So it is cheaper than the real interpreter. The compiler has
  options for each optimisation measured below.
- `jit_runtime.js` (`--js-library`): `new WebAssembly.Module`/`Instance` on the calling thread,
  `addFunction`/`wasmTable.set`, release, `gc()`, and RSS (Node).
- `spike.cpp`: the tests. `main()` runs on a pthread (`-sPROXY_TO_PTHREAD`) like the CPU thread.
  Most tests run on a second pthread, like MTVU.
- `build.sh`: builds `spike-node` (`-sENVIRONMENT=node -sNODERAWFS`) and `spike-web`.
- `index.html`: runs `spike-web` in a browser and prints its output (`?tests=...&args=k=v;k=v`).
- `lowering.mjs`: compiles a module that the spike dumped (`dump=DIR`) on its own. Under
  `node --no-liftoff --print-wasm-code` it shows V8's machine code. Chrome's release builds print
  nothing.

## Run

```sh
source ~/emsdk/emsdk_env.sh
tools/wasm/wasm-jit-spike/build.sh OUT        # default OUT: build-wasm/wasm-jit-spike
node --expose-gc OUT/spike-node.js all        # or basic,grow,mainsize,latency,calls,many,kernel,progsize,fuzz,nan,simdops
node --liftoff-only OUT/spike-node.js kernel  # baseline tier only (--no-liftoff: optimizing tier only)
python3 tools/wasm/serve.py --port 8171 OUT &
node tools/wasm/webgpu-spike/run-headless.mjs --no-default-flags --timeout 600 --profile-root DIR \
  --url 'http://127.0.0.1:8171/index.html?tests=all' [-- --js-flags=--expose-gc]
```

Options (`key=value` after the test list): `sizes=`, `reps=`, `seconds=` (latency); `calls=`;
`modules=`, `modsize=`, `rounds=` (many); `verts=`, `seeds=`, `warm=`, `slice=`, `rounds=` (kernel);
`unrolls=` (progsize); `trials=`, `progs=` (fuzz); `iters=` (simdops); `dump=DIR`.

## Results

Measured on 2026-10-16 with Emscripten 6.0.12 on a Ryzen 7 PRO 7840U (Zen 4, AVX-512). Node
24.19.0 is emsdk's (V8 13.6). Chrome is 155.0.8059.84, headless. The flag family is the app's:
`-O3 -fno-exceptions -msimd128 -msse4.1 -pthread -sSUPPORT_LONGJMP=wasm -sPROXY_TO_PTHREAD
-sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=2GB`, plus `-sALLOW_TABLE_GROWTH`. Other jobs shared the
machine, so the timed runs were pinned with `taskset -c 8,10,12`. Kernel variants run in
interleaved slices, and the tables give medians. Timings vary by ±30% between runs; ratios
measured within one run are steadier. In Chrome, `performance.now()` has 5 µs resolution, so
calls are timed in batches.

### 1. Does it work?

It works on every thread and with memory growth, in Node and in Chrome. All 37 checks of `all`
pass in both.

- The generated module imports `env.mem` and calls `jit_helper_add`, a C++ export of the main
  module, as `env.h_add`. It also calls through `env.table` with `call_indirect`, and its export
  is called from C++ through a function pointer. The module declares `env.mem` as shared, min 0,
  max 65536 pages. A shared import must declare a maximum at least as large as the memory's
  (32768 pages here).
- It works on the proxied main thread, on a second pthread and on the main runtime thread (the
  page's thread, or Node's main thread).
- `addFunction(instance.exports.f)` gives a table index (`-sALLOW_TABLE_GROWTH`). So do
  `wasmTable.grow(n)` and `wasmTable.set(i, f)`.
- A `longjmp` from a C++ helper unwinds through the generated frame to `setjmp`
  (`-sSUPPORT_LONGJMP=wasm`).
- Each pthread has its own instance of the main module and its own table. A slot filled on one
  thread is null on another.
- Another pthread grew the memory from 64 to 780 MB. An instance created before the growth then
  read and wrote above the old size. A module compiled from bytes above the old size worked on that
  worker and on the main runtime thread. Emscripten's `GROWABLE_HEAP_*` wrappers cover the library
  code.
- `new WebAssembly.Module` does not accept a view of a SharedArrayBuffer. `HEAPU8.slice()` makes
  a copy, which costs 0.01–0.06 ms here.
- In Chrome, on the main thread only, synchronous compilation refuses modules over 8 MB:
  `RangeError: WebAssembly.Compile is disallowed on the main thread, if the buffer size is larger
  than 8MB`. Workers have no such limit (16 MB was tested), and neither does Node.

### 2. Costs

Compile cost against size, for straight-line SIMD code (`latency`). Times are in ms and are
medians of 4–10 fresh modules. "Liftoff" is the lazy first call. "TurboFan" is the first call
under `--no-liftoff`, which compiles synchronously.

| wasm instrs | bytes | `new Module` Node / Chrome | `new Instance` | first call, Liftoff | first call, TurboFan Node / Chrome |
| ---: | ---: | --- | ---: | --- | --- |
| 52 | 221 | 0.18 / 0.36–0.41 | 0.04–0.06 | 0.06 / 0.06 | 0.25 / 0.23 |
| 196 | 557 | 0.18 / 0.35 | 0.04–0.06 | 0.07 / 0.07 | 0.33 / 0.29 |
| 1000 | 2.4 K | 0.22 / 0.37 | 0.04–0.06 | 0.11 / 0.11 | 0.60 / 0.69 |
| 4000 | 9.4 K | 0.30 / 0.43 | 0.04–0.07 | 0.23 / 0.27 | 1.9 / 2.0 |
| 16000 | 37 K | 0.55 / 0.66 | 0.04–0.07 | 0.73 / 0.85 | 8.3 / 11 |
| 64000 | 149 K | 1.7 / 1.6 | 0.04–0.07 | 2.7 / 2.8 | 50 / 51 |

- Both engines compile lazily by default. `new Module` only decodes and validates, plus a fixed
  0.15–0.4 ms per module. Each function is compiled with Liftoff, the baseline tier, on its first
  call. That costs about 40 ns per wasm instruction. With `--no-wasm-lazy-compilation`, the
  Liftoff work moves into `new Module`.
- TurboFan, the optimizing tier, costs 0.5–0.8 µs per wasm instruction and grows faster than
  linearly. With dynamic tiering, the default, it compiles on a background thread once a function
  has spent its budget. Functions of up to 1000 instructions moved to TurboFan after 1.5–4 ms of
  calls in both engines. 4000-instruction functions took up to about 50 ms.
- V8 has no on-stack replacement for wasm. A call that is already running keeps the tier it
  started in, and only the next call gets the optimized code. Each microprogram run is a separate
  call, so this does not matter for generated code. It matters for long loops in C++, such as the
  spike's own timing loops.
- For the generated VU code itself (`progsize`, best options), times are in ms, Node / Chrome:

  | VU instrs | bytes | wasm instrs | compile | instantiate | first run (Liftoff) | TurboFan (`--no-liftoff` first run) |
  | ---: | ---: | ---: | --- | --- | --- | --- |
  | 17 | 1.4 K | 467 | 0.10 / 0.11 | 0.02–0.04 | 0.08 / 0.06 | 0.57 / 0.50 |
  | 197 | 9.9 K | 3797 | 0.19 / 0.3 | 0.02–0.03 | 0.27 / 0.23 | 2.6 / 2.5 |
  | 773 | 37 K | 14453 | 0.44 / 0.48 | 0.04–0.05 | 0.96 / 0.93 | 8.5 / 12 |
  | 1805 | 86 K | 33545 | 0.83 / 0.97 | 0.03–0.04 | 2.2 / 1.9 | 24 / 32 |

  That is about 48 bytes and 19 wasm instructions per VU instruction. Liftoff takes about
  1.2 µs per VU instruction and TurboFan 13–18 µs. While the first module is alive, V8 shares the
  compiled code of byte-identical modules. Compiling the same bytes again takes 0.02–0.09 ms in
  Node and 0.05–1.2 ms in Chrome (one outlier at 4 ms), and its first run needs no compilation.

Call overhead (`calls`), in ns per call after tier-up (the first repetition, in Liftoff, is in
the log):

| Call | Node | Chrome |
| --- | ---: | ---: |
| C++ → C++, indirect, empty | 5.5 | 0.36 (inlined by speculative `call_indirect` inlining) |
| C++ → generated, through the table | 5.2–5.5 | 6.2–6.6 (never inlined: another module) |
| C++ → JS → generated export | 67–70 | 59–61 |
| JS loop → generated export | 11–13 | 18 |
| loop in generated code, no call | 0.6 | 0.6 |
| generated → C++ helper, import of a main-module export (wasm-to-wasm) | 4.3–4.7 | 6.7–7.1 |
| generated → C++ helper, `call_indirect` on the imported table | 4.9–5.4 | 7.7–8.4 |
| generated → JS import | 16.6–17.6 | 20–23 |

Many small modules or one big one (`many`, 300-instruction functions, times in µs per unit):

| | build | compile + instantiate | `addFunction` | first call | memory (Node RSS) |
| --- | ---: | ---: | ---: | ---: | --- |
| one module per unit, 10 000 modules | 10–15 | 110–140 (Chrome 140–180) | 5–6 | 30–50 | 21 KB/module on first use; 5 KB/module when freed memory is reused |
| one module of 10 000 functions | 8 | 7.5 (Chrome 5.7) | 2.6 | 18–21 | 5.2 KB/function |

- 50 000 live modules worked (1.06 GB RSS, 19.6 KB per 100-instruction module), and so did a
  50 000-slot table. V8's table limit is 10 million entries (`--wasm-max-table-size`). One
  module of 50 000 functions (15 MB) compiled in 102 ms and instantiated in 153 ms.
- Most of the per-module cost is fixed: a 100-instruction module costs as much memory as a
  300-instruction one. With `--print-wasm-offheap-memory-size`, V8 reports 3.5 KB off-heap per
  module. The rest is JS objects (module, instance, exports, dispatch tables) and code-space pages.

### 3. Throughput

The kernel is a VU1-style vertex loop with 13 VU instructions per vertex: LQI, MULAx, MADDAy,
MADDAz, MADDw, CLIPw, MAX, MINI, FCAND, IADD, SQI, ISUBIU and IBNE. It runs over 64 vertices,
which is 836 instructions per run. Results are in ns per VU instruction (median of interleaved
slices) and are bit-exact unless marked otherwise.

| Variant | Node | Chrome | × interpreter (Chrome) |
| --- | ---: | ---: | ---: |
| interpreter (replica, function-pointer dispatch) | 13.5 | 16.2 | 1 |
| native C++ with `wasm_simd128.h`, same semantics | 1.6 | 1.7 | 9.5 |
| JIT, exact, every flag | 2.9 | 1.8 | 9.1 |
| JIT, exact, clamp cache and loop hoisting | 2.0 | 1.45 | 11 |
| JIT, exact, V8-friendly shapes ("tuned") | 1.4 | 1.43 | 11 |
| JIT, exact, cache + tuned | 1.0 | 1.3 | 12.5 |
| JIT, cache + tuned + `f32x4.relaxed_madd` (**not exact**) | 1.05 | 1.26 | 12.8 |
| JIT, no clamps, no flags (**not exact**) | 0.42 | 0.51 | 31 |
| Liftoff only (`--liftoff-only`), JIT exact, with / without cache and flag elision | 2.7 / 5.9 | 3.1 / 7.1 | 17.7 / 7.7 |
| Liftoff only, interpreter | 39 | 54 | |

- The best exact variant costs about 2.5× the unclamped code. Each operand that might be dirty is
  clamped like `vuDouble` (6 SIMD ops), and each result is clamped like `VU_MAC_UPDATE`. Three
  things cut this:
  - Clean-register analysis: FMAC results are already clamped.
  - A per-register cache of clamped copies, invalidated when the register is written.
  - Clamping loop-invariant registers (the matrix) once before the loop.

  Together they removed 10 of 19 clamps in the kernel.
- Dead-flag elision computed MAC and status flags for 1 of the 4 FMAC ops. It saved 15–40% in
  Liftoff and was lost in the noise under TurboFan.
- Even Liftoff code is 2–5× faster than the interpreter at full TurboFan speed. A new program
  pays off from its first run.
- Lowering in V8 (x64, `--print-wasm-code`, `simdops`), in instructions per op:
  - **1**: `f32x4.add`, `mul`, `pmin`/`pmax`, compares, `i32x4.min/max_s/u`, `and`/`andnot`,
    `narrow`, `shr_s`, `bitmask` (`pmovmskb`/`movmskps`), and broadcasting one lane with
    `i8x16.shuffle` (`vpshufd`).
  - **2**: extract + splat to broadcast.
  - **3**: `v128.bitselect` (no `vpternlog`), `i32x4.ne` (compare then invert), and a variable
    `i8x16.swizzle`.
  - **6**: a byte shuffle reversing the bytes inside each 32-bit lane.
  - **7–8**: IEEE `f32x4.min`/`max` (NaN-propagating, 6–9× the cost of `add`). VU MAX/MINI are
    integer compares anyway: `i32x4.min_s`/`max_s` plus a bitselect.
  - `f32x4.relaxed_madd` becomes `vfmadd213ps`, which is fused.
- **Node 24's V8 (13.6) rematerialises each `v128.const` at every use in a loop.** Each costs 3
  instructions (`movq r10,imm; vmovq; vpunpcklqdq`), which makes `v128.and` with a constant 9×
  slower than with a register. Clang's output has the same problem: the native kernel has 38 such
  sequences. Chrome 155 does not: there a constant operand costs the same as a register. The
  "tuned" shapes keep constants in locals, reverse lanes with `vpshufd` and avoid `ne`. They gave
  2× in Node and 1.2× in Chrome. Node benchmarks of SIMD-heavy code are therefore pessimistic
  compared with Chrome.

### 4. Bit-exactness

The generated code matched the interpreter bit for bit on every test:

- `fuzz`: 1324 single-instruction programs (every op, dest mask, broadcast lane and fd = fs
  alias, plain and tuned) on 300 random states each with 60% special operands (denormals, ±0, ±Inf,
  NaNs, FLT_MAX/MIN, huge, tiny, random bits). That is 397 200 runs.
- `fuzz`: 2000 random 8–24-instruction programs, half of them in loops, plain and fully
  optimised, on 20 states each. That is 80 000 runs.
- `kernel`: 300 special-value states per variant.

Every run matched: VF, ACC, VI, MAC, status, clip and the whole VU memory. The fuzzer found one
real bug on the way. The interpreter sets Z as well as U for denormal results (`0x0101 << shift`),
and the first JIT set only U.

Why this works:

- The interpreter is itself wasm: clang compiles `vuDouble(a) + vuDouble(b)` to `f32.add`.
  Wasm f32 arithmetic is IEEE with round-to-nearest, no flush-to-zero and no excess precision.
  `f32x4.add`/`mul`/`sub` give the same result per lane as `f32.add`/`mul`/`sub`. The rounding
  mode and denormal handling cannot differ between them, because wasm has no way to change either.
- Copy the interpreter's operation order: `acc + (fs * ft)` as two roundings. Clang never fuses
  them, because wasm SIMD128 has no FMA. Never use `relaxed_madd`, which is fused on FMA hardware
  and differed in 53% of special-value runs.
- Copy the interpreter's integer tricks exactly. Clamps run on operands (`vuDouble`) and on
  results (`VU_MAC_UPDATE`); the result clamp is the same operation. MAX and MINI are signed
  integer compares, reversed when both values are negative. CLIP compares integers. The flags are
  Z = exponent 0, S = sign bit, U = exponent 0 and value not ±0, and O = exponent 255.
- NaNs: with the overflow clamp on (PCSX2's default, `vu1Overflow`), every operand is finite,
  so no FMAC op can produce a NaN. Only Inf results appear, and they are clamped. With the clamp
  off, the wasm spec leaves NaN payloads nondeterministic. In practice V8 on x64 gave identical
  bits for clang-compiled scalar code, generated scalar code and SIMD code (`nan`, 0 differences in
  both engines): quieted first-NaN operand, default NaN `0xffc00000` for Inf−Inf and 0×Inf.
  Payload rules could differ on arm64. Make "overflow clamp on" a condition for the JIT, or accept
  NaN-payload differences there.

### 5. Code cache

- Memory: about 20 KB per module the first time, whatever its size, and about 5 KB per module
  once freed memory is reused. One module holding many functions costs 3–5 KB per function. A
  cache of 2000–4000 microprograms is 40–80 MB in the worst case.
- Freeing: `removeFunction(slot)` nulls the table entry. Drop the JS references to the instance and
  the module, and V8 collects them at a later GC. V8 printed each NativeModule's death, and
  `jit_live_objects` dropped to 0. RSS mostly stays: after 50 000 modules, 220 MB of the 960 MB
  it grew by came back. The memory is reused, though: three rounds of 10 000 modules grew RSS by
  204, then 55, then 46 MB, and it settled at 267–279 MB after each free. The table keeps its high-water size, and
  Emscripten reuses freed slots.
- V8 shares the code of byte-identical modules while one of them is alive. Re-creating a
  module, on another thread or after a microcode reload, costs about 0.05 ms. Do not put unique
  salts in the bytes.

## Pitfalls

- Generated code lives in one thread's instance and table. Compile, register and run it on the
  same thread (MTVU). Emscripten reuses pool workers for new pthreads, and the JS state (handles,
  table slots) outlives the pthread. The cache must free its entries when its thread exits.
- With `-sALLOW_TABLE_GROWTH`, `addFunction` also records the function in
  `functionsInTableMap` and `wasmTableMirror`. Release with `removeFunction`, or the module stays
  alive.
- Never rely on traps. A trap in generated code (an out-of-bounds address or `unreachable`)
  aborts like any wasm trap. Mask every VU address (`& 0x3fff`) as the interpreter does.
- Memory growth never moves addresses in wasm, so absolute addresses of the VU state can be
  constants in the code (`i32.const 0` + the `memarg` offset, which compiles to `[rbx+offset]`).
- A CSP, if one is ever set, needs `'wasm-unsafe-eval'`, as the main module already does.
  Runtime-generated modules need nothing more.
- Emscripten warns `-pthread + ALLOW_MEMORY_GROWTH may run non-wasm code slowly`, which
  concerns JS heap views. The JIT's JS side only copies module bytes, so this costs nothing
  measurable.

## Recommended architecture

1. **Unit:** one module per microprogram, exporting one function. A microprogram is the code
   reachable from an MSCAL/MSCNT entry point until the E-bit, keyed by the entry PC and a hash of
   the micro memory it covers. Inside that function, use structured control flow: a `loop` with
   a `br_table` dispatch over the block ids for arbitrary branches, and plain loops when the
   shape allows. Per-block modules would pay the fixed 0.15–0.4 ms and 20 KB per module too
   often. Batch several programs into one module only when they arrive together (an MPG upload).
2. **Where:** compile synchronously on the MTVU thread at first execution, with Liftoff on the
   first call. A 200-instruction program costs about 0.5 ms the first time and runs 2–5× faster
   than the interpreter from then on. V8 tiers up in the background after a few ms of runs. No
   tiering of our own is needed.
3. **Interface:** C++ calls `int (*)(void)` through the table slot; the result is the exit reason
   or next PC. The VU1 register struct sits at a fixed address and is baked into the code.
   Registers, flags and pipeline state live in locals within the program and are written back at
   exits and before helpers that read state. Helpers (DIV/SQRT/RSQRT/EFU edge cases, XGKICK,
   XTOP/XITOP, I/O) are imports of main-module exports, about 5–7 ns per call.
4. **Exactness:** copy the interpreter op by op (clamps, flag words, integer MAX/MINI/CLIP, operation
   order) and its pipeline and stall model. Q/P timing, FMAC, FDIV and EFU latencies and flag
   delays should be computed statically per block from the entry state, as microVU does. Fall back to
   the interpreter for anything unsupported. Keep two development checks:
   - a lockstep mode that runs the interpreter on a copy and compares the state after each program;
   - this spike's fuzzer, per instruction and on random programs, in Node.
5. **Optimisations worth having:**
   - clean-register analysis, clamp cache and loop-invariant clamp hoisting (1.1–1.5×);
   - MAC, status and clip flag liveness (cheap; matters in Liftoff);
   - V8-friendly shapes: constants in locals, `vpshufd`-style shuffles, `eq` rather than `ne`,
     and integer compares in place of IEEE min/max.

   Never use relaxed SIMD in exact code.
6. **Cache policy:** cap the cache at a few thousand programs and evict the least recently used
   (`removeFunction` and drop the references). Expect about 20 KB per program, and nothing to
   return to the OS.

## AGENTS.md wording

The WebAssembly pitfalls section says "no recompilers, no code generation at runtime". That was
written about PCSX2's x86/arm64 recompilers. Proposed replacement for that bullet:

> - There is no `mmap` aliasing, page protection or fault handling: no fastmem, and PCSX2's
>   x86/arm64 recompilers (native code) cannot run. Memory "mappings" are heap allocations.
>   Generating *WebAssembly* at run time is possible: build a module in memory, compile it with
>   `new WebAssembly.Module`/`Instance` on the thread that runs it (each pthread has its own
>   instance and function table), and call it through a function-table slot
>   (`tools/wasm/wasm-jit-spike/`). Generated code must match the interpreter it replaces bit for
>   bit; test it against the interpreter.
