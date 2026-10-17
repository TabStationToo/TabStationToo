# Agent guidelines for TabStationToo

A file for [guiding AI coding agents](https://agents.md/) working in this repository.

## Project

TabStationToo runs PlayStation 2 emulation in the browser. It is a fork of
[PCSX2](https://github.com/PCSX2/pcsx2): PCSX2's C/C++ core compiled to WebAssembly with
Emscripten, plus a browser frontend. Read these before changing anything:

- `README.md`: what the project is and how to build and run it.
- `docs/web-port/PLAN.md`: the porting plan, measurements and the decisions behind them.
- `docs/web-port/UI-SPEC.md`: the interface design ("Cinema"); the page must follow it.
- `pcsx2-wasm/README.md`: the browser frontend, its exported API and its testing options.

Branch `wasm` is the project. `master` mirrors PCSX2 and only receives upstream updates.

### Layout

- `pcsx2-wasm/`: the browser frontend. `Main.cpp` is the emulator's entry point and the C API
  the page calls (`pcsx2web_*`); `web/` is the page (plain HTML/CSS/JS, no build step);
  `assets/` pins the page's fonts and icons (pnpm).
- `pcsx2-headless/`: a windowless runner for Node.js, used to test and benchmark without a
  browser.
- `common/Emscripten/`, `pcsx2/wasm/`, `pcsx2/CDVD/Emscripten/`, `pcsx2/DEV9/Emscripten/`,
  `pcsx2/GS/Renderers/Null/GSDeviceNull.*`: WebAssembly platform code.
- `tools/wasm/`: the dependency build script and the dev server (`serve.py`).
- Everything else is PCSX2, changed only where WebAssembly needs it.

## Build

Emscripten 6.0.12 (emsdk), CMake, Ninja and Python 3:

```sh
source ~/emsdk/emsdk_env.sh
tools/wasm/build-dependencies-wasm.sh deps-wasm      # once
emcmake cmake -B build-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF \
  -DCMAKE_PREFIX_PATH="$PWD/deps-wasm" -DCMAKE_FIND_ROOT_PATH="$PWD/deps-wasm" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
ninja -C build-wasm pcsx2-wasm pcsx2-headless
```

- `-DCMAKE_BUILD_TYPE=Devel` (for example in `build-wasm-dbg`) gives debug info and assertions;
  `-DUSE_ASAN=ON` (for example in `build-wasm-asan`) gives AddressSanitizer.
- Always build out of tree. Build directories (`build-wasm*`) and `deps-wasm*` are ignored.
- After a link, search the log for `signature mismatch`: wasm-ld turns those calls into traps.

## Test

Every change must be verified, not assumed.

- Headless (Node), with a BIOS the user provides:
  `node build-wasm/bin/pcsx2-headless.js -approot "$PWD/bin" -dataroot "$PWD/build-wasm/data" -bios <bios file> -frames 600 [-renderer sw|null] [-screenshot out.png]`.
  Use it to check that the BIOS still reaches its menu and to measure speed (median of several
  runs; report the numbers honestly).
- Browser: `python3 tools/wasm/serve.py [--port N] [--bios-dir DIR] [web root]`, then open
  `index.html` (or `index.html?bios=dev-bios/<file>` with `--bios-dir`). Test UI changes in a
  real browser at desktop and phone widths, with the keyboard.
- Performance changes must keep output pixel-identical (compare screenshots) unless the change
  is meant to alter output.

## WebAssembly pitfalls

- Null pointers do not trap in wasm: reads and writes through them silently hit static data at
  address 0. Corruption that "should crash" usually means this; use the ASan build and
  `llvm-symbolizer --obj=<file>.wasm <address>`.
- Pointers are 32-bit (`uptr` is 32 bits): no tagging or packing that assumes 64-bit pointers.
- There is no `mmap` aliasing, page protection or fault handling: no fastmem, and PCSX2's x86/arm64
  recompilers and `common/emitter` (native code) cannot run. Memory "mappings" are heap allocations,
  and code changes must be detected in software (content compares), not by write protection.
- Generating WebAssembly at run time is possible (`docs/web-port/VU1-JIT-DESIGN.md`,
  `tools/wasm/wasm-jit-spike/`): build a module in memory, compile and instantiate it with
  `new WebAssembly.Module`/`Instance` on the thread that will run it, and call it through that thread's
  function table. Each pthread has its own instance and table: compile, call and release on the same
  thread. Generated code must match the interpreter it replaces bit for bit and is tested against it
  (fuzzing in both engine tiers, and lockstep); never use relaxed SIMD in it.
- Floats always round to nearest with denormals kept; the FP control register is emulated in
  software and has no effect.
- SSE intrinsics are emulated on SIMD128 by Emscripten; some lower to slow scalar code.
- The browser's main thread must never block. The exported `pcsx2web_*` functions only touch
  atomics and lock-free queues; keep it that way.

## Working on PCSX2's code

These notes come from PCSX2's own agent guidelines, kept word for word in
`docs/upstream/PCSX2-AGENTS.md`. They apply to every change in the emulator core.

- PCSX2 recreates the PS2's hardware in software with interpreters, dynamic recompilers and a
  virtual machine that manages the console's hardware state and memory. Emulation changes can
  have subtle timing, compatibility and performance effects: preserve existing behaviour outside
  the intended fix, avoid broad refactors of hardware emulation, and add or update focused tests
  (`tests/ctest/`) where practical.
- Be skeptical of generated code. On changes that could not be fully verified, add an occasional
  short comment such as "needs proper testing", without repeating it throughout a diff.
- Keep platform-specific code and graphics backends guarded, and test changes on every
  architecture and operating system they affect: the wasm build, and the desktop builds when
  shared code changes.
- `3rdparty/` holds vendored dependencies. Avoid modifying them unless a task requires patching
  one.
- PCSX2's layout: `pcsx2/` is the emulator core (EE, IOP, VUs, GS, SPU2, input, storage,
  networking and devices), `pcsx2-qt/` the Qt desktop frontend, `common/` shared utilities and
  platform abstraction, `pcsx2-gsrunner/` the GS dump runner used for graphics testing,
  `tests/ctest/` unit tests, `cmake/` build configuration, `bin/` runtime resources, and `tools/`
  and `updater/` auxiliary programs.
- Desktop builds follow the [PCSX2 build guide](https://pcsx2.net/docs/advanced/building/). On
  Linux, dependencies come from `.github/workflows/scripts/linux/build-dependencies-qt.sh deps`.

Their contribution and communication rules apply to anything proposed to PCSX2 itself (see Git
and GitHub below). After merging a newer upstream `master`, keep this file on merge conflicts,
refresh the copy with `git show upstream/master:AGENTS.md > docs/upstream/PCSX2-AGENTS.md`, and
bring any new engineering guidance over here.

## Code

- Match the surrounding code: PCSX2's style, tabs, existing naming. Format with
  `git clang-format` on the files you changed only.
- Keep changes to PCSX2's own files small and guarded (`ARCH_WASM`, `__EMSCRIPTEN__`, or
  `EMSCRIPTEN` in CMake) so desktop builds are unaffected and upstream merges stay easy.
- Prefer fixing root causes over workarounds; explain any workaround in a short comment.
- New source files carry an SPDX header with `GPL-3.0+`, like the existing ones.
- Never commit BIOS files, game images, keys or other console or game data. Never put them in
  test fixtures, screenshots or the page.

## Web page

- Follow `docs/web-port/UI-SPEC.md`. No generic "dashboard" styling, no decoration the spec does
  not ask for, and no controls for features that do not exist yet.
- The page is cross-origin isolated and makes no requests to other origins: self-host every
  asset. Fonts and icons come from `pcsx2-wasm/assets/` (`pnpm install`, then `pnpm run sync`);
  record each asset's licence next to it.
- Use pnpm, never npm or npx.
- Accessibility is part of done: keyboard operation, visible focus, contrast, reduced motion,
  screen-reader names.

## Naming and legal

- The product is **TabStationToo** (this exact casing). Never call it "PCSX2 Web" or brand it
  with the PCSX2 name; internal identifiers (`pcsx2-wasm`, `pcsx2web_*`) may stay.
- Always credit PCSX2 ("Built on PCSX2, GPL-3.0") and state that the project is not affiliated
  with Sony or the PCSX2 project.
- No Sony trademarks or trade dress: no PlayStation logos or fonts, no imitation of the PS2's own
  interface. Do not use screenshots of the BIOS or games in the repository.
- The project is GPL-3.0. Keep upstream copyright notices.

## Git and GitHub

- Remotes: `origin` is this fork (public), `upstream` is PCSX2. Stage files explicitly; never
  `git add -A`.
- Never open pull requests, issues, discussions or comments on PCSX2's repository. In this fork,
  GitHub targets new pull requests at PCSX2 by default: do not open pull requests unless the
  owner asks, and then target this repository.
- GitHub Actions are disabled on this repository. Do not re-enable them or add workflows that run
  on push.
- Commit or push only when the owner asks.
- If code is ever proposed to PCSX2 itself, follow PCSX2's own guidelines
  (`docs/upstream/PCSX2-AGENTS.md`, current version
  [upstream](https://github.com/PCSX2/pcsx2/blob/master/AGENTS.md)): contributors declare LLM use,
  new contributors may not submit LLM-generated content, and contributions and any communication
  with their maintainers must be written by a human.
