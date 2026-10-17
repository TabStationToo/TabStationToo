> **Status update (2026-10-15, after this plan was written).** The "wasm\*" browser
> frontend it describes as in progress is the same effort: `pcsx2-wasm/` now boots the
> BIOS in Chrome (built-in browser), renders the intro and menus in the canvas at
> 15-20 % speed during the intro and 60-100 % in menus, and takes keyboard input
> (quick taps are held for two vsyncs so menus see them). GSDeviceNull's merge,
> interlace (incl. FastMAD) and present paths now follow the GPU shaders. M2's
> "first pixels" exit is met in the browser; the remaining items below still apply.

> **Status update (2026-10-16): speed.** The emulated CPU is the limit, not drawing: the
> BIOS intro is EE-interpreter-bound. Under Node on the 7840U, 450 frames of the
> SCPH-39001 intro (7.5 s in real time) took 35.5 s with `-renderer null` and about 37 s
> with `-renderer sw`. Three interpreter changes taken from the recompilers, wasm-only
> (`ARCH_WASM`) and checked on the BIOS only (the EE event test runs only once an event is
> due, the IOP skips idle loops, the BIOS boot hooks are tested at branch ends), bring that
> to about 25 s (null) and 27 s (SW): the intro now runs at about 25-30 % of full speed
> (it was about 20 %), the menus near full speed. The SW renderer adds under 10 % to the
> intro's time, so WebGPU (M4) matters for games, not for the BIOS; full speed needs a
> wasm JIT (M5).

# PCSX2 in the browser: porting plan (Emscripten → WebAssembly, WebGPU)

*Lead-architect synthesis for the downstream fork `pcsx2-web`. Upstream base: commit 685637d30 (2026-10-14). Written 2026-10-15, revised after a critic pass.*

**How to read this.** "Verified" means I or a skeptic reviewer opened the file or ran the command. "Estimate" means a judgement nobody has measured. Mapper claims that were refuted have been dropped or corrected. Paths are relative to the repository root. Upstream line numbers refer to `pcsx2-web` (685637d30). `wasm:` is committed work on the user's port branch in the sibling worktree `/home/nassim/Desktop/Startups/pcsx2-web-wasm`. `wasm*:` is uncommitted or untracked work in that worktree, which another session is still editing.

**Most important finding.** The port is no longer at zero. Branch `wasm` in the sibling worktree has two commits (verified with `git log`):
- `458b6baddb` "Build the core for WebAssembly with Emscripten" (44 files)
- `a9639c0a2e` "Boot the PS2 BIOS headless in WebAssembly" (14 files, +1250)

With these, a Release wasm32 build (`build-wasm/bin/pcsx2-headless.wasm`, 9.9 MB) boots SCPH-39001 and SCPH-70004 to the OSDSYS menu under Node. It runs interpreters only, with the SW renderer, a new `GSDeviceNull`, and a screenshot at the end.

Measured in `build-wasm/boot-rel.log` (Release, SW renderer, default 2 SW threads, Ryzen 7 PRO 7840U, limiter target speed 1, `:122`):
- **BIOS logo intro (a light 3D load, frames 6–449):** median 20.1 % of real time over 63 samples, interquartile range 19.8–20.4 %, full range 14.7–60.6 %.
- **Static OSDSYS menu (frames 489–579):** 100 %. That is the frame limiter's ceiling, so the real headroom is unknown.
- The run does not separate CPU cost from SW-renderer cost. `boot-70004.log` ends at 71–75 %. A later split (2026-10-16, status update at the top) shows the EE interpreter is the limit: with `-renderer null` the intro takes under 10 % less time.

**A browser frontend is in progress (wasm\*, another session, uncommitted):**
- `pcsx2-wasm/` holds `Main.cpp` (831 lines), `CMakeLists.txt` and `web/{index.html,app.js,style.css}`.
- `tools/wasm/serve.py` is a COOP/COEP dev server that binds only to 127.0.0.1. It has an optional `--bios-dir`.
- The root `CMakeLists.txt` has an uncommitted `add_subdirectory(pcsx2-wasm)`, and `GSDeviceNull.{h,cpp}` have uncommitted changes that add a present callback.
- A Release build exists in `build-wasm-web/bin/`: `pcsx2-wasm.wasm` 9.9 MB, `.data` 4.3 MB.

What the frontend does today:
- Frames go from the GSDeviceNull present callback into a shared descriptor (`pcsx2-wasm/Main.cpp:215,384,479`), and the page blits them to a 2D canvas on each rAF (`web/app.js:118,676`).
- It has a real CPU-thread host queue (`Main.cpp:498`).
- It sets `SPU2/Output Backend=Null` using the correct key (`Main.cpp:193-194`).
- It uses the legacy FS (`-sFORCE_FILESYSTEM`, BIOS in MEMFS) and `PTHREAD_POOL_SIZE=8`. It does not use MODULARIZE, EXPORT_ES6 or `ALLOW_BLOCKING_ON_MAIN_THREAD=0`.
- It preloads only `GameIndex.yaml`, `game_controller_db.txt` and four fonts.

I have not verified whether it boots in a browser. **Status:** M0 is done. M1 is done under Node, and the browser half is in progress. **Everything below builds on that branch and that frontend, not on master.**

---

## 1. TL;DR

**Verdict: feasible.** Compiling the core is done. Browser correctness work is substantial but bounded. Speed is the open question.

- **Useful build (estimate: a few months).** Interpreter-only, SW renderer, 2D and light titles, no HW renderer.
- **WebGPU HW renderer (estimate: 6–9 months for one engineer).** It needs a JSPI or GPU-owner-worker design and a large backend.
- **Playable build for most 3D games.** Needs a wasm JIT, a multi-month effort with no PCSX2 precedent. Every non-x86 recompiler so far (ARMSX2, LRPS2) was a full rewrite.

**Realistic performance (estimates unless marked measured):**

| Configuration | Desktop (recent laptop/desktop, Chrome) | Mobile |
|---|---|---|
| Interpreters, SW renderer (today) | **Measured** under Node on a 7840U: light-3D BIOS intro about 20 %; idle menu capped at 100 % by the limiter. **Measured** split (2026-10-16): EE-interpreter-bound; 450 intro frames (7.5 s in real time) took 35.5 s with `-renderer null` and about 37 s with `-renderer sw`. 3D games: roughly 5–20 % (estimate) until the M1 profile exists. Upstream's only number is about 8 fps native on a 3.9 GHz Haswell (`pcsx2/Interpreter.cpp:157-160`), and wasm adds roughly 1.5–2× (Jangda 2019). | Not usable. |
| Interpreters plus cheap wins (decode caches, event-test gating, native SIMD conversions) | Perhaps 1.5–2× the above, so about 30–40 % on the measured intro (estimate). **Measured** for the first ones (event-test gating, an IOP idle-loop skip, BIOS hooks at branch ends): about 1.3–1.4×, so 25–30 % on the intro (about 25 s null, 27 s SW for the 450 frames). **Hypothesis to test in M1/M2:** 2D and FMV-heavy titles may reach full speed. Test it with one named 2D title and one IPU/FMV title. MTVU is not a cheap win (see M3 task 10). | Light titles only, if any. |
| Wasm JIT for VU1 + EE, WebGPU HW renderer | Full speed on light and medium titles needs roughly 5× over today. Plausible but unproven (estimate; no prior art for an LLE PCSX2 JIT in wasm). Heavy titles are likely below full speed. | Light titles at best. Every iOS browser is WebKit: no dual-source blending, and JSPI only from Safari 27. |

*Not comparable:* pcsx2_web reports 0.32× in Chrome and 0.63× in gameplay, but it ran with EECycleRate=-2, EECycleSkip=2, vu1Instant, WaitLoop, IntcStat and fastCDVD, and the gameplay figure was measured under Node (`web/core-worker.js:183-192`, README:135).

**Recommended strategy:**
1. **Continue on the `wasm` branch, and finish the in-progress `pcsx2-wasm` frontend instead of forking a new target.** Settings: wasm32 with fixed memory ≤ 2 GB, interpreters, the SSE4.1-emulation SIMD route, the C scanline SW renderer. Keep `pcsx2-headless` (Node, `-sNODERAWFS`) as the CI and regression harness. **Decide legacy FS vs WasmFS now.** With pthreads, the legacy FS proxies every syscall to the page thread, including console output and ISO reads.
2. **Run every C++ thread off the page thread.** `-sPROXY_TO_PTHREAD` is already in the frontend. The CPU, MTGS, MTVU, SW-raster and CDVD-reader threads are pthreads. The page thread does only DOM, input capture, AudioContext, pickers and the frame blit.
3. **Graphics in two steps, the SW-playable product first.** First pixels need neither WebGPU nor JSPI: GSDeviceNull's present callback fills a SharedArrayBuffer descriptor and the page blits it on rAF (M2; mostly done in wasm\*). The WebGPU work comes after the SW-playable milestone, as M4. It opens with a JSPI + emdawnwebgpu spike, then a present-only device, then `GSDeviceWebGPU` on the DX11 copy-feedback and DX12 descriptor-grouping templates. The MTGS thread owns the device and yields through JSPI; a GPU-owner worker is the fallback.
4. **Browser targets.** Primary: desktop Chrome/Edge 137+. Secondary: Firefox 153+ on Windows and Apple Silicon, and Safari 27, which needs a no-dual-source fallback. iOS support means "Safari 27+ only, no Lockdown Mode (which reportedly disables WebAssembly), no fullscreen or pointer lock on iPhone, no lightgun". Require `dual-source-blending` until the fallback exists.
5. **Performance order:** interpreter wins first. Then a wasm JIT built on an IR: microVU1 first (self-contained, no SMC, no MMU), then the EE with software SMC detection. Defer the IOP JIT. **Profile before committing.**
6. **Legal and process.** Users supply their own BIOS and discs. Publish the GPL source offer, including the exact dependency builds. Record the terms of any `patches.zip` you serve. Use a distinct product name. Any upstream contact stays human-to-human, per `AGENTS.md`.

---

## 2. Browser constraints mapped onto PCSX2

| Constraint | PCSX2 code affected | Solution | Milestone |
|---|---|---|---|
| No native code generation | x86 recompilers `pcsx2/x86/**` (35.4k lines) and `common/emitter` (7.6k). GS SW JIT `GS/Renderers/SW/GSDrawScanline.cpp:13` (`#define ENABLE_JIT_RASTERIZER`). VIF unpack JIT `Vif_Dynarec.h:46`; `VifUnpackSSE_Init` called unconditionally at `VMManager.cpp:2715`; the mode-0 path calls `nVifUpk[]` at `Vif_Unpack.cpp:477-496`. | Interpreters are already selected when `_M_X86` is undefined (`VMManager.cpp:2700-2760`). JIT rasterizer off on wasm (C `CDrawScanline` path). `newVifDynaRec=false`, with mode 0 routed through `VIFfuncTable` (`Vif_Unpack.cpp:163-169`). `pcsx2/wasm/RecStubs.cpp`. **Done in `wasm:`**. Later: a wasm-module JIT. | M0 ✔, M5 |
| No mmap/mprotect/signals | `SysMemory::AllocateMemoryMap` (shm + MAP_FIXED, `Memory.cpp:90-112`). `vtlb_Core_Alloc` always reserves 4 GB of fastmem and installs the fault handler (`vtlb.cpp:1325-1356`). `FASTMEM_AREA_SIZE` wraps to 0 on wasm32 (`vtlb.cpp:78`). Emscripten's `mmap(len 0)` *succeeds*, after which `vtlb_RemoveFastmemMapping` wrote 0xFFFFFFFF over linear memory [0, 4 MB). GS VRAM 4× aliased mirror (`GS.cpp:1039-1080`). EE-rec SMC detection by page protection (`vtlb.cpp:1451-1496`). | Malloc-backed SysMemory, no code region, fastmem early-returns, `CHECK_EEREC/IOPREC/FASTMEM` constexpr false (`wasm: pcsx2/Config.h:1509`): **done**. VRAM is 16 MB of *non-coherent* copies: **done, needs audit** (mask inside `PAHelper`, `GSLocalMemory.h:248-251`). A future JIT needs software SMC detection. | M0–M1 ✔, M2 audit, M5 |
| wasm32 pointers | vtlb sign-bit tagging `vtlb.h:130-177`. `IsoReader.cpp:296` `static_assert(sizeof(size_t)==8)`. Raw-pointer savestate freezes: `Gif_Path` holds a `u8*` (`Gif_Unit.cpp:206`), and `SPU2Savestate::FreezeIt` memcpys all `Cores`, whose `V_Core` has `u16* DMAPtr/DMARPtr` and whose `V_Voice` has `s16* SBuffer` (`SPU2/spu2freeze.cpp:49,116`; `SPU2/defs.h:264,469-470`). So the SPU2 DataBlock layout and size differ from x86-64. `BASEBLOCK` 8-byte assert (`x86/BaseblockEx.h:249`, JIT only). | u64 vtlb entries on wasm (**done** in `a9639c0a2e`). IsoReader fixed (**done**). Savestates are not desktop-compatible today. If they must be: serialize Gif_Path and SPU2 Cores/Voices field by field, or pad the pointer fields to 8 bytes under `ARCH_WASM`, plus an automated desktop→wasm load test (open question 4). | M0–M1 ✔, M3 |
| One linear memory (≤ 4 GiB on wasm32) | About 210 MB of static data. IOP `Main[TotalRam]` = 128 MB, which looks like a slip from commit e47b32b2e (`MemoryTypes.h:56`); `iopMemReset` memsets all of it. GIF paths 3×9 MB plus GS packet queues 3×4 MB at static init. Gzip cache up to 200 MB. `CdvdPrecache` copies the whole image. | Fixed `INITIAL_MEMORY = MAXIMUM_MEMORY` (start at 1 GB, measure, keep ≤ 2 GB; `wasm: cmake/BuildParameters.cmake:92` still grows 512 MB→2 GB). Size IOP Main to `TotalIopRam`. Force precache off and cap the gzip cache. No memory64 (flag-only in Safari; 10–100 % slower in SpiderMonkey). The wasm heap is not the whole footprint (§3.2). | M1, M3 |
| Threads are Workers; the page thread must never block | CPU loop (`VMManager::Execute` → `Cpu->Execute`, returns only at an interrupted VSync). MTGS blocks in `WaitForWork` (`MTGS.cpp:168,338`). Throttle busy-spins (`VMManager.cpp:2306-2347`). Plain `std::thread` with Emscripten's 64 KB default stack (gsrunner CPU thread, save-state and screenshot threads). | `-sPROXY_TO_PTHREAD`, `-sSTACK_SIZE=4MB`, `-sDEFAULT_PTHREAD_STACK_SIZE=2MB` (**done** for headless and the frontend). `STACK_OVERFLOW_CHECK` is 0 by default, so an overflow silently corrupts memory; enable it in dev builds. Pre-size `PTHREAD_POOL_SIZE` (frontend: 8). Use WasmFS: the legacy FS proxies every syscall synchronously to the page thread (`libcore.js:2654-2665`). | M1 |
| Page lifecycle: hidden tabs, discard, mobile kill | rAF does not run in hidden tabs (per spec; engine behaviour for worker rAF unverified). The EE blocks on `s_sem_Vsync` after two queued frames (`MTGS.cpp:274-280`, `VsyncQueueSize=2` at `Config.h:849`). Host commands drain only at guest vsync (`VMManager.cpp:2966-2971`). Chrome Memory Saver can discard inactive tabs; iOS suspends or kills background pages. | Never let MTGS wait only on rAF; use `Atomics.waitAsync` or setTimeout as a fallback. Pause through the host queue on `visibilitychange`. Flush memcards, NVRAM and INI on `pagehide`/`freeze`. Decide whether audible background play is allowed (Chrome does not throttle audible tabs). The frontend today only releases keys on visibilitychange (`web/app.js:1472`). | M3 |
| WebGPU needs the owning worker's event loop | MTGS never yields (`MTGS.cpp:144-168, 322-340`). `GSopen` and device creation run inside it (`MTGS.cpp:172-175`). Device loss: `PresentResult::DeviceLost` triggers a synchronous `GSreopen(true, …)`, and `pxFailRel` if that fails (`GSRenderer.cpp:552-567`). On the web, `device.lost` settles only on the worker's event loop, and recovery needs a new `requestAdapter`/`requestDevice` (both async). | `-sJSPI`: MTGS suspends at present, when idle and in readbacks. Fallback: a GPU-owner worker fed by a command stream, with `Atomics.wait` for results. Present without yielding via `transferToImageBitmap` (spec-defined; engine behaviour unverified). Device loss: watch `device.lost`, return DeviceLost from BeginPresent, re-acquire asynchronously inside GSreopen under JSPI. Texture-cache targets on the GPU are lost. | M4 |
| Readback is async only (`mapAsync`) | `GSDownloadTexture::Flush/Map` are synchronous (`GSTexture.h:291-304`). Callers: `GSTextureCache.cpp:7476-7549`. Several are **not** gated by `HWDownloadMode`: `GSTextureCache.cpp:1761`, `GSRendererHW.cpp:84, 9969, 10728`, screenshots `GSRenderer.cpp:1114-1120`, `GSTexture.cpp:81-82`. The EE waits in `MTGS.cpp:283-297`. GameDB: 318 `cpuSpriteRenderBW` and 53 `cpuCLUTRender` entries take readback paths. | JSPI-suspended timed `wgpuInstanceWaitAny` in `Flush`. Web default `HWDownloadMode=Unsynchronized`. Deferred screenshots on the GSCapture model (3 frames in flight). Feature-detect `mapSync` (Chrome flag only). | M4 |
| No geometry shaders | — | Already handled: `vs_expand` (storage buffers in the VS). Compatibility mode has 0 vertex-stage storage buffers, so require core mode. | M4 |
| No framebuffer fetch; cannot sample an attachment | Feedback hazards (`GSRendererHW.cpp`, `tex_is_fb`, SW blend, DATE) | Copy model as in DX11: `texture_barrier=0, multidraw_fb_copy=1` (`GSDevice11.cpp:710, 3249-3412`). Each copy ends the pass. `depthReadOnly` is per pass, so read-only DSV switches also end the pass. | M4 |
| No push constants or push descriptors | Vulkan requires `VK_KHR_push_descriptor` and 96 B of push constants (`GSDeviceVK.cpp:143-145, 4053, 4068`) | Dynamic-offset UBOs and a bind-group cache, following DX12's table grouping (`GSDevice12.cpp:4179-4231`). Immediates only where available (Chrome-only today). | M4 |
| WGSL only; no dlopen | shaderc loaded with `DynamicLibrary` (`VKShaderCache.cpp:135-150`). TFX is permuted with 62 PS macros. | Build-time translation of the utility shaders. Generated WGSL text per `PSSelector` for TFX. No translator in the browser. | M4 |
| Dual-source blending is optional | 17 of 81 `m_blendMap` entries use SRC1 (`GSDevice.cpp:2083-2165`), plus `AFAIL_RGB_ONLY_DSB` (`GSRendererHW.cpp:9181-9191, 9263-9290`) and PABE (`:7384-7386`). Chrome 130+ and Firefox have it; **WebKit does not**, and every iOS browser is WebKit. | Require the feature at first. Then add `FeatureSupport::dual_source_blend` with a fallback built on the existing `UseDebugBlend` all-SW-blend path (`GSRendererHW.cpp:7173-7193`). | M4 |
| SIMD128 only; SSE emulation partly scalar | Per-pixel conversions `GSVector.h:113-120` (`_mm_cvttps_epi32` is a scalar loop), Z `GSVector4.h:814-817` (`_mm_cvt(t)pd_epi32` scalar loops). `noopt()` `+x` asm (`GSVector4.h:195-198`). `sra64` uses AVX-512 (`GSVector4i.h:835-838`). xxhash pulled in inside a namespace (`GSXXH.cpp:12-15`). | SSE4.1 emulation (**done**; compile breakers fixed). Override the scalarized ops with native `wasm_*` intrinsics under `ARCH_WASM`, but only after the SW reference diff passes (trunc_sat saturates where x86 `cvttps` returns 0x80000000). No AVX2: Emscripten 6.0.12 emulates it, with no gain. | M0 ✔, M3, M5 |
| No MXCSR rounding/FTZ/DAZ | `FPControl.h:23-193`. Defaults are ChopZero+DAZ+FTZ for the EE FPU and VU0/1 **for every game** (`Pcsx2Config.cpp:31-41`), applied by the VU interpreters (`VU0microInterp.cpp:251`, `VU1microInterp.cpp:261`). | Thread-local bitmask, no-op SetCurrent (**done**). DAZ and FTZ are mostly emulated in software already (`FPU.cpp:79-90,168-183`, `VUops.cpp:440-456`). Rounding becomes nearest. Measure the loss (M1); optional software chop later. | M0 ✔, M1, M3 |
| setjmp/longjmp | `FastJmp.cpp` (asm only), interpreter exit and cancel (`Interpreter.cpp:569,583,590,597`), libpng (`Image.cpp:252-368`), rapidyaml (`YAML.cpp`) | Macro over `setjmp` with `-sSUPPORT_LONGJMP=wasm`, deps built in the same mode (**done**). **Open bug:** cancel jumps with 0 (`Interpreter.cpp:590`), which longjmp turns into 1, so every TLB miss or address error leaves `Execute`. | M0 ✔ / M1 fix |
| No dlopen of native libs | shaderc, FFmpeg capture (`GSCapture.cpp:38-40` includes headers unconditionally), cubeb backends | Compile-time stubs (`GSCaptureNull.cpp`, pcap stub, null HTTP): **done**. Cubeb compiles with no backend and falls back to Null at runtime, so change `DEFAULT_BACKEND` (`Config.h:981`). The frontend already sets `Backend=Null` with the correct key; `pcsx2-headless/Main.cpp:105` still writes the stale `OutputModule` key. | M0 ✔, M1 |
| File access through JS | `FileSystem` stdio everywhere. `VMManager` refuses paths `FileExists` cannot see (`VMManager.cpp:1259,1430`). Memcards opened `r+b` (`MemoryCardFile.cpp:309-313`), written by buffered `fwrite` with no `fflush` or `fsync` in Save/EraseBlock; `fflush` appears only at creation (`:118,148`). libzip writes a temp file and renames it. The DEV9 HDD needs a multi-GB image (tens of GB by default) and its creation runs on its own thread (`FullscreenUI_Settings.cpp:105`). | WasmFS with an OPFS backend for `/data` (created off the page thread). The frontend currently uses legacy-FS MEMFS, so nothing persists. A disc-I/O worker reads user `File`s. WORKERFS **does not work** with PROXY_TO_PTHREAD (verified from Emscripten code). Memcards: `fflush`+`fsync` after each Save/EraseBlock (or once per vsync), with fsync mapped to `FileSystemSyncAccessHandle.flush()`. OPFS takes one exclusive handle per `r+b` file, so add a single-instance guard. Safari deletes OPFS data after 7 days without interaction unless the site is installed, so make export/import mandatory. Firefox's best-effort quota is min(10 %, 10 GiB). DEV9 HDD is out of scope for v1. | M1, M2, M3 |
| No raw sockets | DEV9 pcap and sockets, curl (`SearchForStuff.cmake:41-42`) | Stubbed (**done**). A fetch-based `HTTPDownloader` for achievements later (CORS unverified). | M0 ✔, M3 |
| Gamepad and AudioContext are Window-only | `SDLInputSource::PollEvents` on the CPU thread (`SDLInputSource.cpp:790`). `SPU2::Open` creates the stream on the CPU thread (`spu2.cpp:103-134`). Chrome exposes at most 4 gamepads, only after a button press while focused. The standard mapping reports face buttons as digital, so DualShock 2 pressure buttons are unavailable (unverified against current Chromium source). | Prototype: SDL3 3.4.18's Emscripten drivers already proxy to the page thread (`MAIN_THREAD_EM_ASM`, ScriptProcessorNode). Production: a Wasm Audio Worklet calling `AudioStream::ReadFrames`, and a gamepad snapshot in a SAB. Show "press a button to connect". Map extra multitap ports to the keyboard. With `ALLOW_BLOCKING_ON_MAIN_THREAD=0`, a page-thread `pthread_join`/`cond_wait` aborts. Whether SDL's page-thread audio callback ever waits on a condvar is unverified, so test it under ASSERTIONS first. | M3 |
| USB peripherals | `pcsx2/USB/{usb-lightgun,usb-mic,usb-eyetoy,usb-pad,usb-msd,usb-hid,usb-printer}`. The mic goes through cubeb, which has no Emscripten backend (`3rdparty/cubeb/CMakeLists.txt:60-106`). EyeToy is cam-noop only (`pcsx2/CMakeLists.txt:731-735`). Wheels expect SDL haptic force feedback. | Default every port to None. Support GunCon 2 through InputManager pointer events (no pointer lock on iPhone). Mic (getUserMedia, Window-only), camera and wheel force feedback are out of v1. | M3 |
| Cross-origin isolation | Hosting | COOP `same-origin` plus COEP `require-corp` (credentialless is not in Safari), CSP `wasm-unsafe-eval`. `tools/wasm/serve.py` (wasm\*) does this locally. coi-serviceworker only for GitHub Pages demos. It cannot coexist with a separate caching service worker in the same scope, so merge the two if needed. | M1 (browser), M3 |
| `call_indirect` checks signatures | Function-pointer casts, e.g. `AudioStream::ParseBackendName` (`Pcsx2Config.cpp:1290`, worked around in `a9639c0a2e`) | Treat every wasm-ld "function signature mismatch" warning as an error. | M0 ✔ / ongoing |
| Coarse timers; no pause instruction | `MultiPause` is empty on wasm (`wasm: common/HostSys.cpp:43-44`), so `MeasurePauseTime` (`HostSys.cpp:48-68`) can overflow `int testcnt` or divide by zero, and `ShortSpin` stops spinning. | Fixed `PAUSE_TIME` on wasm, or a non-elidable spin on `emscripten_get_now`. | M1 |

---

## 3. Target architecture

### 3.1 Threads and workers

```
 Browser page thread (never runs blocking C++)
 ┌──────────────────────────────────────────────────────────────────────┐
 │ HTML shell: library, BIOS/ISO pickers, settings, About/licences,     │
 │   in-game controls (pause, save/load state, perf overlay) until M4   │
 │ input capture (KeyboardEvent.code, pointer, Gamepad poll in rAF)     │
 │   → SAB mailboxes                                                    │
 │ frame blit on rAF from the SAB frame descriptor (M2–M3)              │
 │ lifecycle: visibilitychange → pause; pagehide → flush request        │
 │ AudioContext creation/resume on user gesture                         │
 │ Emscripten runtime "main" (services pthread_create, proxied calls)   │
 └───────────────┬──────────────────────────────────────────────────────┘
                 │ SharedArrayBuffer (one wasm32 linear memory, fixed size)
 ┌───────────────┴──────────────────────────────────────────────────────┐
 │ pthread "CPU" = main() (PROXY_TO_PTHREAD, 4 MB stack)                 │
 │   EmuThread state machine (copy of QtHost EmuThread::run)             │
 │   EE interp + IOP interp + VU0 + VU1 + VIF/IPU/SPU2/CDVD state        │
 │   Host::RunOnCPUThread queue drained at VSync and while paused        │
 │   InputManager reads SAB mailbox at PollInputOnCPUThread              │
 │   SPU2 → AudioStream ring (+ soundtouch on the writer side) ─┐        │
 ├──────────────────────────────────────────────┐               │        │
 │ pthread "MTGS"                               │               │        │
 │   M2–M3: GSRendererSW → GSDeviceNull present │               │        │
 │          callback → SAB frame descriptor     │               │        │
 │   M4: owns GPUDevice + OffscreenCanvas,      │               │        │
 │       JSPI-suspendable; GSRendererHW →       │               │        │
 │       GSDeviceWebGPU; ImGui OSD/pause menu   │               │        │
 │   ├─ pthreads "GS-SW-n" (C scanline, 0..N)   │               │        │
 ├──────────────────────────────────────────────┘               │        │
 │ pthread "MTVU" (idle on non-x86; VU1 worker only after an     │        │
 │                 MTVU-safe VU1 interpreter or the VU1 JIT)     │        │
 │ pthread "CDVD reader" (ThreadedFileReader)  ⇄ disc-I/O JS worker      │
 │        (SAB request queue; the JS worker holds the user's File and    │
 │         reads with FileReaderSync, writing straight into wasm memory) │
 │ WasmFS OPFS proxy worker (/data: bios, memcards, sstates, inis)       │
 │ transient pthreads: save-state zip, screenshot (one each per action!) │
 ├───────────────────────────────────────────────────────────────┴──────┤
 │ AudioWorklet (Wasm Audio Worklet): AudioStream::ReadFrames per        │
 │ 128-frame quantum                                                     │
 └──────────────────────────────────────────────────────────────────────┘
```

Peak thread count is about 8–10: CPU, MTGS, MTVU, 2 SW workers, CDVD, WasmFS proxy, transient threads and audio. Under PROXY_TO_PTHREAD a pool is optional, because the page thread is free to spawn Workers. Pre-size one anyway (the frontend uses 8; the plan suggests about 10) so thread creation does not stall on worker startup.

**Hidden tabs.** No thread may depend on rAF alone to make progress. Today the CPU and MTGS threads do not wait on the page, so a hidden tab only stops the display. The M4 JSPI design must suspend on `Atomics.waitAsync`/setTimeout, not only on rAF.

### 3.2 Memory

- **wasm32 with fixed memory.** Start with `INITIAL_MEMORY = MAXIMUM_MEMORY = 1 GB`. Measure the peak, then move toward a desktop profile of up to 2 GB and a smaller mobile profile.
- The branch currently grows from 512 MB to 2 GB (`wasm: cmake/BuildParameters.cmake:92`), and the frontend inherits that. With pthreads, growth makes JS heap views slow (Emscripten warns about this in `build-wasm/headless.log`). `GROWABLE_ARRAYBUFFERS` is not usable everywhere until Firefox 154.
- vtlb entries are u64 on wasm (done), so the handler tag bit no longer limits host addresses. Keep ≤ 2 GB anyway for device reasons. iOS jetsam kills tabs at thresholds nobody has documented.
- Footprint estimate for the wasm heap, after removing code caches and fastmem: about 370–480 MB, or about 250–360 MB once IOP Main is sized to `TotalIopRam`. Add the SW texture cache (unmeasured, perhaps 20–130 MB) and about 39 MB of static-init heap (gifUnit).
- **The wasm heap is not the whole tab.** The JS heap also holds MEMFS/legacy-FS data, including the `--preload-file` `.data` blob (4.3 MB today) and any shm buffers (MEMFS ftruncate allocates a JS `Uint8Array`, `libmemfs.js:128-134`). Add 8–10 worker JS contexts. From M4 on, GPU allocations come on top: upscaled render targets and the texture cache, which share system RAM on integrated GPUs. WebGPU gives no VRAM query; out of memory shows up as `GPUOutOfMemoryError` or device loss. Wrap texture creation in error scopes and map failures to the existing texture-creation-failure paths. Cap the upscale multiplier on the web, lower on mobile. Measure total tab memory (`performance.measureUserAgentSpecificMemory` or the browser task manager) in M3 and M4.
- `__pagesize` stays 4 KiB through `OVERRIDE_HOST_PAGE_SIZE`. musl's 64 KiB `PAGESIZE` must not leak into it.

### 3.3 CPU execution

- Interpreters only, through the existing non-`_M_X86` wiring. VU1 runs synchronously on the CPU thread (`THREAD_VU1` false, `Config.h:1500-1506`); the MTVU thread is opened but idle.
- **MTVU with the VU1 interpreter is not free.** XGKICK bypasses the MTVU path (`VUops.cpp:1862-1873`). D/T-bit interrupts write `VPU_STAT` and call `hwIntcIrq` directly (`VU1microInterp.cpp:40-60`). `InterpVU1::Execute` reads `cpuRegs.cycle` (`:278`). `THREAD_VU1` is tied to `REC_VU1`. Upstream never runs this combination. It is an optional M3 task (M–L effort), not a cheap win.
- Execution exits at VSync (`Counters.cpp:546-547`). `Execute()` can be re-entered, so frame slicing is possible later. Upstream has a latent hazard here: `intSafeExitExecution` jumps immediately when called outside the event test (`Interpreter.cpp:573-585`), so host stop requests must stay inside the event test.
- M5 adds a wasm JIT per owning thread. Modules are compiled and instantiated in the thread that runs them, with function-table slots per worker, and linked with `return_call_indirect` through link slots in linear memory.

### 3.4 Rendering

- **M2–M3 (no WebGPU).** The SW renderer de-swizzles into a 4 MiB CPU buffer (`GSRendererSW.cpp:32,116,180`). GSDeviceNull's CPU merge, interlace and stretch produce the presented frame. Its present callback (wasm\*) copies the frame into a SAB descriptor keyed by a sequence number, and the page blits it on rAF. A 2D canvas `putImageData` is used today (`web/app.js:676`); move to WebGL2 or page-thread WebGPU only if the blit cost shows up in profiles.
- **No ImGui before M4.** The ImGui OSD and FullscreenUI pause menu are only drawn by a GS device in `GSRenderer::EndPresentFrame` (`GSRenderer.cpp:570-578`). GSDeviceNull renders nothing for them, so the whole SW-playable phase has no in-game OSD or pause menu.
- **M4.** `GSDeviceWebGPU` on emdawnwebgpu (`--use-port=emdawnwebgpu`; Emscripten 6.0.12 pins Dawn `v20260423`). The present-only device uploads frames with `queue.writeTexture`, which has no 256-byte `bytesPerRow` rule. The 256-byte rule applies to the buffer copies used for readback (`copyTextureToBuffer`, DX12 template).
- M4 feature profile: `texture_barrier=0, multidraw_fb_copy=1, framebuffer_fetch=0, rov=0, vs_expand=1, point_expand=0, line_expand=0, provoking_vertex_last=0, prefer_new_textures=1, depth_feedback=0` (then measure Depth mode). `stencil_buffer` from `depth32float-stencil8`; `primitive_id` only with `primitive-index` plus `float32-blendable`.

### 3.5 I/O

- **`/resources`.** Preloaded or fetched; `bin/resources` is about 9.5 MB. The frontend preloads only six files (4.3 MB `.data`). Define the resource set per feature:
  - FullscreenUI needs `fullscreenui/` (it fails without `fullscreenui/placeholder.png`, `FullscreenUI.cpp:336`) and `icons/`.
  - The game list needs `RedumpDatabase.yaml`.
  - Sounds are optional.
  - Lazy-fetch fonts, GameDB and patches.
- **`patches.zip`** (widescreen, no-interlace, built-in patches) is not in the repo. CI downloads it from the PCSX2/pcsx2_patches releases (`.github/workflows/linux_build_qt.yml:129`). `Patch::OpenPatchesZip` looks for it in Resources and shows an OSD error when it is missing (`Patch.cpp:117, 288-317`). Fetch a pinned copy at build time, record its licence (§8), and support importing user cheats and pnach files into `/data`.
- **`/data`.** WasmFS OPFS. Call `navigator.storage.persist()`. Save export/import is a hard requirement because of Safari's 7-day eviction; show install-to-home-screen guidance on Safari. Check `navigator.storage.estimate()` before any OPFS import. Add a single-instance guard (Web Locks or BroadcastChannel) before opening any file, because OPFS gives one exclusive handle per `r+b` file and only Chrome has `readwrite-unsafe`.
- **Durability.** Memcard writes are buffered `fwrite` with no flush (§2). Flush per write or per vsync, and map fsync to `SyncAccessHandle.flush()`.
- **Disc images.** User `File` → disc-I/O worker → `ThreadedFileReader` subclass. Plan B: import into OPFS (quota permitting), then the WasmFS read-only Blob path. A disc path must exist in the FS namespace, or `VMManager`'s `FileExists` checks have to learn a `web://` scheme.
- **BIOS.** Uploaded into `/data/PCSX2/bios` (MEMFS in the frontend today). NVRAM is written next to it (`CDVD.cpp:160-169`).

### 3.6 Audio, input, UI

- **Audio.** M3a uses `AudioBackend::SDL`, which works from the pthread through page-thread proxies. M3b adds `AudioBackend::WebAudio`, a Wasm Audio Worklet calling `ReadFrames`. JS must not read the ring directly, because the writer also moves `m_rpos` in `StretchOverrun` (`AudioStream.cpp:769-777`). At low emulation speed, time stretch runs soundtouch on the CPU thread inside the write path (`AudioStream.cpp:595-632`) at extreme tempo (`MIN_TEMPO` 0.05, `AudioStream.cpp:672`), which costs EE time and sounds broken. Below a speed threshold, mute or disable stretch.
- **Input.** M3a uses SDL3's Emscripten joystick plus the built-in keyboard/pointer sources fed via `InputManager::InvokeEvents`. M3b adds a `WebGamepadInputSource` reading a SAB snapshot. Limits: 4 pads in Chrome, no pressure-sensitive buttons, a connect prompt.
- **UI.** An HTML shell for everything outside a running game. During M2–M3 it also hosts the in-game controls (pause, save/load state, settings, performance overlay), because there is no ImGui output. From M4, ImGui provides the OSD and in-game pause menu (`ShouldPreferHostFileSelector=true`). `FullscreenUI` is already in the core (`pcsx2/CMakeLists.txt:840-875`).

---

## 4. Key decisions

| Decision | Recommendation | Reason |
|---|---|---|
| **wasm32 vs memory64** | **wasm32**, fixed memory ≤ 2 GB | Memory64 is flag-only in Safari and costs 10–100 % in SpiderMonkey. The footprint is under 500 MB without code caches. The ILP32 issues found so far (vtlb, IsoReader, FASTMEM wrap) are fixed. Memory64 would not have avoided the fastmem rewrite: a 4 GB anonymous mmap becomes a 4 GB memalign plus memset. Remaining cost: savestates are not byte-compatible with desktop (Gif_Path and SPU2 Cores/Voices pointers). |
| **Interpreter-first vs JIT** | **Interpreter first**, then cheap interpreter wins (M3), then JIT (M5) | The interpreter build boots now. The JIT has no reusable code: there is no IR, and the x86 emitter cannot be retargeted. A decode-cached interpreter needs no runtime codegen, per-thread tables or SMC machinery. QEMU took the same path upstream (TCI interpreter for Emscripten; no wasm TCG backend merged). |
| **First JIT target** | **microVU1**, then EE, IOP last or never | VU1 programs are self-contained and cached by content compare (`x86/microVU.cpp:216-254`). They need no MMU, exceptions or mprotect. ps2-web-recomp's author reports VU1 at about a quarter of CPU in God of War II and the IOP at about 2 %. A VU1 JIT can also move VU1 onto the MTVU thread (`THREAD_VU1` is tied to `REC_VU1`); the VU1 *interpreter* cannot run there as-is (§3.3). **Profile before committing**: the CPU researcher ranked EE first on code structure alone. |
| **SW vs HW renderer first** | **SW first, shipped as a product** (M2 page blit, M3 SW-playable). WebGPU HW renderer is M4. | SW already works headless, and the present path needs no WebGPU or JSPI: GSDeviceNull already merges, interlaces and stretches on the CPU. This keeps GS correctness separate from the large WebGPU backend and keeps the biggest platform risk off the path to a first usable release. It is not a gameplay fallback for heavy 3D titles: the C scanline path on emulated SSE is slow (unmeasured), and `CSetupPrim`/`CDrawEdge` have never run in a shipped build (a CSetupPrim bug was fixed 2026-08-16, commit 3dd7028302). Validate the C path against a desktop build with `ENABLE_JIT_RASTERIZER` undefined. |
| **WGSL strategy** | Utility shaders (convert, present, merge, interlace, shadeboost, CAS, FXAA, imgui): **offline** glslang→SPIR-V→Tint (or naga), behind a `WEBGPU` define that drops the Y flip and push constants, embedded via the `BAKE_SHADERS_IN_CPP` mechanism (`cmake/ShaderToCpp.cmake`). TFX: **a hand-maintained `tfx.wgsl` template**, text-generated per `PSSelector` for bindings, outputs and expansion shape. `override` constants only for value-like switches. Cache keyed by selector hash, as `VKShaderCache` does. | WGSL has no preprocessor. Overrides cannot change bindings, types or outputs, and override-dead branches must still type-check and pass uniformity analysis. Shipping a translator in wasm adds MBs (unmeasured). For TFX, HLSL `tfx.fx` (separate Texture2D/SamplerState, D3D clip space) and `tfx.metal` (101 function constants, first-vertex provoking) are closer models than the Vulkan GLSL. |
| **SIMD strategy** | Keep **SSE4.1 emulation** (`-msimd128 -msse4.1`). Add **targeted native overrides** under `ARCH_WASM`: `GSVector4i(GSVector4)` → `wasm_i32x4_trunc_sat_f32x4` / nearest+trunc_sat, `f64toi32` → `wasm_i32x4_trunc_sat_f64x2_zero`, `allfalse`/`eq` → `wasm_v128_any_true`. Apply them only after the M2 C-path baseline diff passes. Full `GSVector*_wasm.h` (modelled on `_arm64.h`) only if profiles demand it. No AVX2. Relaxed SIMD only as an optional build. | The headers compile today. Only a few ops are pathological: the scalar conversion loops on the per-pixel path (`GSDrawScanline.cpp:661-676, 738-739`). Register-count shifts, loadl/storel and blendv are already native or cheap; `_mm_rcp_ps` has no callers. Saturating conversion matches shipped ARM64 behaviour but differs from x86 on out-of-range input. |
| **UI** | **HTML shell** for library, BIOS/ISO pick, settings, licences, and (until M4) in-game controls. **ImGui** for OSD and pause menu once a GPU device draws it (`ShouldPreferHostFileSelector=true`). | File pickers, fullscreen and pointer lock need user activation on the page thread. Accessibility, IME and clipboard are easier in HTML. FullscreenUI comes for free inside the core, but only renders through a real GS device. |
| **Where the GPU device lives** | **MTGS pthread + JSPI**. Plan B: a GPU-owner worker with a command stream and `Atomics.wait` on results. | It keeps PCSX2's model of one thread owning the device. JSPI ships in Chrome 137, Firefox 153 and Safari 27 (2026-09-17). JSPI traps if any JS frame sits between the promising entry and the suspend point, which is why wasm SjLj (not JS `invoke_*` trampolines) is mandatory. JSPI inside Emscripten pthreads is untested here; that is the M4 opening spike. Device-loss recovery (async re-acquisition) is designed in from the start. |
| **Filesystem** | Decide now: **WasmFS + OPFS** for `/data`, a disc-I/O worker for discs. The frontend uses the legacy FS (`-sFORCE_FILESYSTEM`, MEMFS). | With pthreads, the legacy FS proxies every syscall (console output, ISO reads, memcard writes) to the page thread, and it offers no persistence. WasmFS's OPFS backend must be created off the page thread, which PROXY_TO_PTHREAD allows. |
| **Dependencies** | See the next table. | — |
| **Emscripten** | Pin **6.0.12** (installed at `~/emsdk`; both commits and the frontend built with it). Use emsdk's toolchain file, not CMake 4.2+'s native platform, whose empty `CMAKE_SYSTEM_PROCESSOR` breaks cpuinfo. | Flags below. |

**Dependencies: keep, port or stub**

| Keep (vendored, builds as-is) | Build for wasm (`tools/wasm/build-dependencies-wasm.sh`, all `-pthread -sSUPPORT_LONGJMP=wasm`) | Stub or disable |
|---|---|---|
| fmt, fast_float, rapidjson, simpleini, lzma, libchdr, libzip, soundtouch (scalar), imgui, rcheevos, freesurround, demangler, ccc, cpuinfo (Emscripten backend; keep `CMAKE_SYSTEM_PROCESSOR=x86`), cubeb (inert), discord-rpc (inert), xxhash (pre-include `<arm_neon.h>` in `GSXXH.cpp`) | zlib 1.3.1, libpng 1.6.58, libjpeg-turbo 3.2.0, FreeType 2.13.3, libwebp 1.6.0 (+ `libsharpyuv`; `FindWebP.cmake` misses it), SDL3 3.4.18 (audio and joystick only), zstd, lz4, plutovg, plutosvg, rapidyaml. Later: emdawnwebgpu (port). | Qt, KDDockWidgets, FFmpeg (`GSCaptureNull.cpp`), libcurl (null or fetch HTTPDownloader), libpcap (stub), DBus, X11, Wayland, libbacktrace, udev, Vulkan, shaderc, GL, D3D, Metal, xbyak, zydis, vixl, x86 emitter, CDVD IOCtlSrc (`CDVD/Emscripten/IOCtlSrc.cpp`, also `GetValidDrive`) |

**Flags.** ✔ = committed on the branch; ✔\* = in the uncommitted `pcsx2-wasm` frontend; ✱ = to add; ? = combination untested.

- **Compile:** ✔ `-msimd128 -msse4.1 -sSUPPORT_LONGJMP=wasm`, ✔ `-pthread`, existing `-fno-exceptions`. Use Release builds for any timing (Devel defines `PCSX2_DEVBUILD`, so `__ri` expands to nothing).
- **Link (browser target):**
  - Threads and stacks: ✱ `-pthread` explicitly (today it arrives only through SDL3's INTERFACE options), ✔ `-sPROXY_TO_PTHREAD=1`, ✔ `-sSTACK_SIZE=4MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB`, ✔\* `-sPTHREAD_POOL_SIZE=8` (measure; maybe 10).
  - Environment and output: ✔\* `-sENVIRONMENT=web,worker`, ✔\* `-sEXIT_RUNTIME=0`, ✱ `-sMODULARIZE -sEXPORT_ES6`.
  - Main thread: ✱ `-sALLOW_BLOCKING_ON_MAIN_THREAD=0`. Validate first with SDL audio under ASSERTIONS, and keep page-thread `pcsx2web_*` exports lock-free, as they are today.
  - Memory: ✱ `-sINITIAL_MEMORY=1GB -sMAXIMUM_MEMORY=1GB` (no growth; the branch has 512 MB→2 GB growth).
  - Filesystem: ✱ `-sWASMFS` (still marked experimental) with an OPFS backend mounted from `main()`, replacing ✔\* `-sFORCE_FILESYSTEM`.
  - Later milestones: ✱ (M3) `-sAUDIO_WORKLET -sWASM_WORKERS`; ✱ (M4) `-sJSPI` ?, `--use-port=emdawnwebgpu`, `-sOFFSCREENCANVAS_SUPPORT` ? (GL-coupled; may need `$GL` force-included or a custom transfer); ✱ (M5) `-sALLOW_TABLE_GROWTH`.
  - Dev builds: `-sASSERTIONS -sSTACK_OVERFLOW_CHECK=1 --profiling-funcs` (the frontend already keeps `--profiling-funcs`).
- Legacy wasm exceptions remain the default, for Safari < 18.4. Use `-sWASM_LEGACY_EXCEPTIONS=0` only if the floor rises.

---

## 5. Milestones

Effort scale for one engineer who knows PCSX2: **S** ≤ 1 week, **M** 1–3 weeks, **L** 1–2 months, **XL** > 2 months.

**Order and calendar (estimates).** The milestones are ordered so that a usable SW-rendered product ships before the WebGPU backend:
- M0 done; M1 nearly done.
- M2 + M3 (first pixels, then SW-rendered playable): about 4–6 months serial for one engineer, judging by the effort tags. Less if parts run in parallel.
- M4 (WebGPU HW renderer): plausibly 6–9 months alone, given two XL and several L items.
- M5 (JIT): 6+ months, starting with microVU1.

M4 and M5 can overlap with M3 once its exit criteria hold.

### M0 — compiles ✔ (commit `458b6baddb`)

Done: `ARCH_WASM`/`SIMD_SSE` (`common/Pcsx2Defs.h`, `cmake/BuildParameters.cmake:83-94`), the `SearchForStuff` gates, `common/Emscripten/{EmHostSys,EmMisc,EmHTTPDownloader}.cpp`, LnxThreads `__EMSCRIPTEN__` arms, FPControl, FastJmp, `pcsx2/wasm/RecStubs.cpp`, the VIF C path, JIT-rasterizer guards, GSVector compile fixes, xxhash, IsoReader, AdapterUtils `const`, CDVD/pcap/capture stubs, and the dependency script.

**Remaining M0 hygiene (S):**
- Add `-pthread` to link options explicitly.
- Make wasm-ld signature-mismatch warnings fatal.
- Gate `VU0.cpp:55`, which reads `EnableEE` directly.
- Suppress the "recompiler not enabled" OSD warnings (`VMManager.cpp:3450-3485`).
- Change `if (newVifDynaRec)` call sites to `if constexpr` (`VMManager.cpp:2720`, `Vif_Unpack.cpp:316/358`, `MTVU.cpp:39`).
- Fix the stale `OutputModule` key in `pcsx2-headless/Main.cpp:105` (use `Backend`, as the frontend does).

**Exit:** `pcsx2-headless`, `pcsx2-wasm` and `pcsx2-gsrunner` link in Release and Devel with zero signature warnings.

### M1 — boots BIOS (✔ under Node, commit `a9639c0a2e`; browser in progress in wasm\*)

**Tasks:**
1. **Fix the fastjmp cancel contract** (S). `Interpreter.cpp:590` jumps with 0, which longjmp turns into 1. Cancel with 2 and return from `intExecute` only on ==1 (`:597`). This also works for the native asm.
2. **MultiPause calibration** (S). Use a fixed `PAUSE_TIME` or a time-based spin on wasm (`common/HostSys.cpp:48-113`). Node's high-resolution clock hides the bug; browser clocks expose it.
3. **Memory trims** (S). IOP `Main` → `TotalIopRam` (`MemoryTypes.h:56`; confirm with an upstream human that it is a slip). Force `CdvdPrecache` off. Cap `GZFILE_CACHE_SIZE_MB`. Switch to fixed memory.
4. **Finish the browser harness `pcsx2-wasm`** (M, in progress, other session; coordinate before touching it). It already has `-sENVIRONMENT=web,worker`, PROXY_TO_PTHREAD, the `serve.py` COOP/COEP dev server, BIOS upload into MEMFS, and the frame descriptor. Remaining work:
   - Decide legacy FS vs WasmFS, then move `/data` to WasmFS OPFS.
   - Add MODULARIZE/EXPORT_ES6 if the shell needs them.
   - Set fixed memory.
   - Keep folder roots AppRoot `/`, Resources `/resources`, DataRoot `/data` (`CustomDataPath` appends `PCSX2`).
5. **Real host command queue** (S, in progress at `pcsx2-wasm/Main.cpp:498`). `Host::RunOnCPUThread` is reached in normal boots: `Patch.cpp:689`, `SDLInputSource.cpp:881`, `ImGuiManager.cpp:1117,1445`. gsrunner's version aborts (`pcsx2-gsrunner/Main.cpp:295-298`). Drain the queue while paused too.
6. **Native accuracy baseline** (S). x86 build, all interpreters, EE/VU Round=Nearest, DAZ/FTZ off. FTZ is read from the `DenormalsAreZero` key (`Pcsx2Config.cpp:618-621`). Run on a 20-game set and record regressions. This prices the missing MXCSR.
7. **CPU profile** (S). On native interpreters, split time across EE, VU1, IOP, GS, **IPU, CDVD read and SPU2**, including soundtouch and freesurround on the CPU thread. This settles which JIT comes first and where FMV time goes.
8. **Wasm speed rerun** (S). Release build under Node with the frame limiter off (the headless runner has no flag for this yet, so it needs a small runner change) and with `-renderer sw` vs `-renderer null`, to separate CPU from SW-GS cost. Put both numbers in §1. Done on 2026-10-16 for the intro (status update at the top): it is EE-interpreter-bound. The frame limiter stayed on, which does not matter for the intro (it never reaches full speed); the menu's headroom above full speed is still unmeasured.

**Exit:**
- The BIOS reaches OSDSYS in desktop Chrome and Firefox from an uploaded BIOS.
- Node CI boots two BIOS versions and hashes a frame in the idle menu. It runs with `ManuallySetRealTimeClock` and fixed `Rtc*` values (`Config.h:1374,1408`; `CDVD.cpp:957-966`) and a pristine NVRAM copied in before each run (`CDVD.cpp:160-169`).
- The same BIOS run twice gives identical hashes before the check is adopted.

### M2 — first pixels (SW renderer, no WebGPU, no JSPI)

**Tasks:**
1. **Frame path** (S, mostly done in wasm\*). GSDeviceNull present callback → SAB descriptor keyed by sequence number → page blit on rAF (2D canvas today). Move to WebGL2 or page-thread WebGPU only if the blit shows up in profiles.
2. **Disc I/O** (M). The disc-I/O worker plus a `ThreadedFileReader` subclass registered in `GetFileReader` (`CDVD/InputIsoFile.cpp:40-57`). Prove a 4.5 GB ISO and a CHD. Benchmark per-sector latency and sustained streaming throughput from a user `File` in Chrome, Firefox and Safari. `ThreadedFileReader`'s waits block the CPU thread (`ThreadedFileReader.cpp:343,379,390`), so round-trip latency adds directly to wall time.
3. **GS-dump replay in the page** (S). gsrunner path; no EE needed.
4. **VRAM wrap audit** (M). Mask in `GSOffset::PAHelper::value` (`GSLocalMemory.h:248-251`) and `loopPixels` (`:268-281`). Diff SW frames on wrap-heavy dumps.
5. **SW raster threading** (S). `SWExtraThreads=0` first, then 2.
6. **C-scanline reference build** (S). Build desktop with `ENABLE_JIT_RASTERIZER` undefined, so it uses the same `CSetupPrim`/`CDrawScanline`/`CDrawEdge` path as wasm. Diff desktop C against desktop JIT as a separate test. Native SIMD overrides come only after this baseline passes.

**Exit:**
- A commercial game reaches in-game rendering with the SW renderer in Chrome.
- One FMV-heavy title plays its FMV in the page (record the speed).
- Twenty GS dumps match the desktop C-path SW output bit-for-bit, or with explained differences.

### M3 — SW-rendered playable: I/O, audio, input, shell

**Tasks:**
1. **SDL audio and pad first** (S). Fix `DEFAULT_BACKEND` on wasm (`Config.h:981`). Run with `ALLOW_BLOCKING_ON_MAIN_THREAD=0` and ASSERTIONS to check that SDL's page-thread audio callback never blocks.
2. **`AudioBackend::WebAudio`** (M). Enum `Host/AudioStreamTypes.h:11-17`, factory `AudioStream.cpp:107-128`, Wasm Audio Worklet calling `ReadFrames`, AudioContext resumed on gesture. Below a speed threshold, mute or disable stretch.
3. **`WebGamepadInputSource`** (M). `Input/InputSource.h`, registered in `InputManager.cpp:1871-1877`. Keyboard via `KeyboardEvent.code` (`ConvertHostKeyboard*`). Show a "press a button" prompt. Map extra multitap ports to the keyboard. Document the 4-pad and no-pressure limits.
4. **Persistence** (M).
   - File memcards with `fflush`+`fsync` after each Save/EraseBlock, or a per-vsync flush of dirty cards (`MemoryCardFile.cpp:441-525`), with fsync mapped to `SyncAccessHandle.flush()`.
   - Save states (the libzip temp file plus rename works in WasmFS OPFS) and the INI via `CommitBaseSettingChanges`.
   - A single-instance guard (Web Locks/BroadcastChannel) before any file opens.
   - Mandatory save export/import, `storage.persist()`, Safari install guidance, `storage.estimate()` before imports.
   - Folder memcards later.
5. **HTML shell** (L). Library, pickers, settings, About and licences, **plus the in-game controls**: pause, save/load state, settings, performance overlay. The ImGui OSD and pause menu do not render until M4.
6. **Frame pacing** (M). The web has no refresh-rate API. Measure the rAF cadence and pass it as `WindowInfo::surface_refresh_rate` (`common/WindowInfo.h:43`) for `SyncToHostRefreshRate` (`VMManager.cpp:2248, 2830`). Handle 120/144 Hz and VRR, 59.94 vs 60 Hz drift, 50 Hz PAL, and the latency from `VsyncQueueSize=2`. Keep Throttle's sleep and remove the busy-spin.
7. **Page lifecycle** (S–M). Pause on `visibilitychange` through the host queue. Flush memcards, NVRAM and INI on `pagehide`/`freeze`. Decide on audible background play. Make no wait depend on rAF alone.
8. **Patches, cheats, widescreen** (S). A pinned `patches.zip` fetched at build time and served with the resources (licence recorded in §8). pnach and cheat import into `/data`. A per-feature resource set: `fullscreenui/`, `icons/`, `RedumpDatabase.yaml`.
9. **USB scope** (S). All ports default to None. GunCon 2 through InputManager pointer events. Mic, camera and wheel force feedback are out of v1.
10. **Interpreter speedups** (M).
    - Gate `intEventTest` on `nextEventCycle` (`Interpreter.cpp:270-275`, as the recompiler does at `iR5900.cpp:1401-1412`; timing-sensitive, needs testing). Done for wasm (`ARCH_WASM`, `intBranchTest()`) on 2026-10-16, together with an IOP idle-loop skip and the BIOS hooks tested at branch ends; checked on the BIOS only, needs testing with games.
    - An EE decoded-block cache (code-word compare on entry).
    - A VU decode cache via the `InterpVU::Clear` hooks.
    - Native SIMD conversions (after the M2 baseline).
11. **Optional: MTVU-safe VU1 interpreter** (M–L; upstream never runs this combination).
    - Route XGKICK through the MTVU path (`VUops.cpp:1862-1873`).
    - Send D/T-bit interrupts through `vu1Thread.mtvuInterrupts` instead of direct `hwIntcIrq`/`VPU_STAT` writes (`VU1microInterp.cpp:40-60`).
    - Stop reading `cpuRegs.cycle` (`:278`).
    - Gate `THREAD_VU1` independently of `REC_VU1`.
    - Validate with golden hashes.
12. **Hosting** (M).
    - Headers, Brotli, cache-busted names.
    - A payload budget, with measured cold and warm load times.
    - Lazy fetch of fonts, GameDB and patches.
    - One service worker if both the COOP/COEP shim and caching are needed.
    - A deploy whitelist of Release artifacts only (Devel/ASan wasm are 71/97 MB, and `build-wasm/data` holds a BIOS).
    - The source offer and licences.

**Exit:**
- Five titles playable end to end on desktop Chrome with the SW renderer: boot, play with a pad, save to memcard, reload the page, load.
- Durability: save in-game, kill the tab within one second, reload; the save is present.
- Frame-time variance and judder check passes, including on a 120 Hz display.
- Audio has no sustained underruns, measured only on titles running at ≥ 90 % speed.
- Payload budget met.

### M4 — WebGPU HW renderer

**Tasks:**
1. **Opening spike** (M; retires the largest platform risk; the rest of M4 depends on it). JSPI + emdawnwebgpu on the MTGS pthread: device acquisition, per-VSync suspend in `WaitForWork` and the idle-present loop (`MTGS.cpp:322-340`), and a timed `WaitAny` on `mapAsync`. Add `WindowInfo::Type::Web` (`common/WindowInfo.h:12-19`) and get the canvas to MTGS through a hook in `MTGS::StartThread` (`MTGS.cpp:118-127`) using `emscripten_pthread_attr_settransferredcanvases`, or a custom transfer. If JSPI fails, switch to the GPU-owner worker.
2. **Enums and selection** (S). `RenderAPI::WebGPU` (`GS/GS.h:16-24`), `GSRendererType` (`Config.h:271-281`), `GetAPIForRenderer`/`OpenGSDevice` (`GS.cpp:79-150`), `GSUtil::GetPreferredRenderer`.
3. **Offline utility-shader pipeline** (L). Needed before the present-only device: convert, present, merge, interlace, imgui, then shadeboost, CAS and FXAA.
4. **`GSDeviceWebGPU` present-only** (M).
   - `CreateSurface`, `GSTexture::Update` via `writeTexture`.
   - `DoMerge`/`DoInterlace`/`PresentRect`/`CopyRect`.
   - `RenderImGui`, which brings back the ImGui OSD and pause menu. Clamp ImGui scissor rects to the attachment (the Vulkan path only clamps at ≥ 0, `GSDeviceVK.cpp:4777`).
   - Stub the HW-only virtuals.
5. **Resource layer** (L).
   - Format map with fallbacks: D32S8 → D32F with `stencil_buffer=0`; ColorClip → RGBA16F; BC only with `texture-compression-bc`, otherwise replacements are *rejected*, since CPU BC decoders exist only for alpha min/max.
   - Streaming rings via `writeBuffer`, dynamic UBOs, a bind-group cache (DX12 grouping), samplers.
   - Early-vs-late upload ordering (Vulkan `GSTextureVK.cpp:353-363`).
   - Texture creation in error scopes; capped upscale multiplier.
6. **Render-pass manager** (L). Clear carry-forward. A pass split on every feedback copy, every `depthReadOnly` change and every StencilOne DATE clear.
7. **`RenderHW` port** (XL). From `GSDevice11::RenderHW`/`SendHWDraw`/`FeedbackCopyAndBind`, including DS clones, DATE paths, colclip and multi-pass blend. Use separate depth-texture bindings, since a depth view cannot bind to a `float` + filtering slot.
8. **`tfx.wgsl` generator** (XL).
9. **Pipelines** (M). `createRenderPipelineAsync` everywhere. In WebGPU an invalid pipeline invalidates the whole encoder, unlike Vulkan's skip-the-draw (`GSDeviceVK.cpp:5233-5236`). Skip the draw while a pipeline is pending.
10. **Readbacks** (L). JSPI `Flush`. Web default `HWDownloadMode=Unsynchronized`. Deferred screenshots and thumbnails.
11. **Dual-source gate** (M). `FeatureSupport::dual_source_blend`. Fail clearly without it, then design the `UseDebugBlend`-based fallback for Safari and iOS.
12. **Device loss** (M). Watch `device.lost`, return `PresentResult::DeviceLost` from BeginPresent, and implement async re-acquisition (`requestAdapter` + `requestDevice`; an adapter is consumed once a device exists) inside GSreopen under JSPI, or in the GPU-owner worker for plan B (`GSRenderer.cpp:552-567`).

**Exit:**
- 20 titles render correctly against a desktop reference **forced to the same feature profile**: Vulkan or GL with `OverrideTextureBarriers=0` (`Config.h:900`), or DX11, with identical blend-accuracy settings. Residual differences are documented, such as primitive-ID DATE on DX11 (`GSDevice11.cpp:67`) vs full-barrier DATE without `primitive-index` (`GSRendererHW.cpp:6172-6182`).
- GSPerfMon reports Readbacks, Barriers and TextureCopies for each title.
- No WebGPU validation errors.
- Gameplay survives `device.destroy()` and `chrome://gpucrash`.

### M5 — performance: JIT

**Tasks:**
1. **JIT runtime** (L).
   - A wasm module builder (Play!'s `WasmModuleBuilder`, BSD-2, or a new one).
   - An async compile queue with an adaptive sync budget.
   - Multi-function batching (flycast: about 20 µs per block batched vs about 1 ms per module).
   - A per-thread table and instance registry with caps.
   - Imports bound to raw `wasmExports`, not JS thunks.
   - Bytes copied with `slice()` before compile (Firefox 138–150 rejects SAB views).
   - A hotness threshold.
2. **microVU1 → wasm, v1** (L). Keep the program cache, `microVU_Analyze.inl` and the flag-instance logic. Emit SIMD128, and fall back to the interpreter per op for anything not yet covered.
3. **microVU1 full coverage** (XL). The x86 microVU is about 10.6k lines including macro ops, and clamping, flag instances and pipeline-state-keyed blocks must all be re-emitted. LRPS2's full AArch64 rewrite still reports VU1 at about 70 % coverage. Then enable MTVU (`THREAD_VU1` tied to `REC_VU1`).
4. **EE JIT** (XL).
   - Reuse `recRecompile` block scanning and the `iR5900Analysis` passes.
   - v1 is call-threaded (`REC_FUNC`/`recCall` pattern, `x86/iR5900.h:36-62`) with native branches; then inline ALU and softmem.
   - vmap entries are i64, so the ARM64 softmem code does not carry over.
   - SMC: `ProtMode_Manual` (`iR5900.cpp:2062-2075`) as the bootstrap, then page-generation bits on every write path into EE RAM, including DMA (`Sif0.cpp:43` and `sif2.cpp:69` have `Cpu->Clear` commented out).
   - Preserve the `Cpu == &intCpu` behaviours (`vtlb.cpp:525-534`, `Interpreter.cpp:232-262`).
5. **`return_call_indirect` linking** through link slots (M). Region sizes respect the ~7 MB per-function limit.
6. **Pipeline prewarm** (M). Persist `PipelineSelector` keys per game in OPFS.
7. **Optional precompiled scanline specializations** (M). `template<u64 KEY>` around `CDrawScanline(…, sel)` with explicit `always_inline`.

**Exit:**
- ≥ 90 % speed on N named light and medium titles on the reference desktop. That implies roughly 5× or more over today's measured interpreter speed, consistent with the §1 JIT row.
- Golden framebuffer hashes unchanged against the interpreter (shadow-differential test).

---

## 6. Risk register

| Risk | Likelihood | Impact | Mitigation | Early test that retires it |
|---|---|---|---|---|
| JSPI does not work (or is unsafe) for a long-lived suspended Emscripten pthread with WebGPU | Medium | High: no readback, no present (M4 only; M2–M3 do not depend on it) | Plan B: GPU-owner worker with a command stream; `transferToImageBitmap` presentation | M4 opening spike (and the week-1 throwaway): MTGS pthread requests a device, clears the canvas each VSync and suspends in `WaitForWork`. Chrome, Firefox 153, Safari 27. |
| Interpreter speed stays far below playable even with decode caches | High | High | Ship the interpreter as the accuracy baseline; publish per-game expectations; JIT in M5 | M1 native profile, M1 limiter-off/null-renderer wasm rerun, and a Release browser run of 3 games at default settings |
| No dual-source blending in Safari and all iOS browsers | Certain (today) | Medium | Require the feature, then a `UseDebugBlend` fallback | Count SRC1 draws per frame on desktop dumps |
| WebGPU device loss (GPU-process crash, driver reset, mobile backgrounding, OOM) | Medium | High: today's sync `GSreopen` path cannot work; `pxFailRel` on failure | Async re-acquisition under JSPI or in the GPU-owner worker; error scopes on texture creation | M4: `device.destroy()` and `chrome://gpucrash` during gameplay |
| Readback-heavy titles stall (`cpuSpriteRenderBW` and others) | Medium | Medium–High | Unsynchronized default; per-game overrides; deferred readbacks | M4: GSPerfMon Readbacks per frame on 10 titles with `gsrunner` |
| Wrong rendering from non-coherent VRAM mirrors | Medium | Medium (silent) | `PAHelper` masking; dump diffs | M2 VRAM audit with wrap-heavy dumps |
| C scanline path (`CSetupPrim`/`CDrawEdge`) has latent bugs | Medium | Medium | Desktop C-vs-JIT frame diff; use the desktop C build as the wasm reference | Native build with `ENABLE_JIT_RASTERIZER` undefined over a dump corpus (M2 task 6) |
| Accuracy loss from forced round-to-nearest | Certain | Low–Medium (game-specific) | Measure; optional software chop on the default path | M1 native Nearest/no-FTZ baseline on 20 games |
| Silent memory corruption (64 KB stacks, wasm32 wraps, null-page writes) | Medium | High | Fixed big stacks; `STACK_OVERFLOW_CHECK=1` and ASan builds in CI | Already found twice (FASTMEM wrap, null vector). Keep the ASan Node boot in CI |
| Thread or worker startup deadlock (pool exhaustion, transient save-state threads) | Medium | Medium | Pool size ≥ peak; make thread creation non-blocking | Save 5 states rapidly while running, in Chrome |
| Tab hidden, discarded or killed: stalls or lost saves | High | Medium–High | Pause on visibilitychange; flush on pagehide/freeze; memcard flush per write; no rAF-only waits | M3 exit: kill the tab within 1 s of an in-game save; background the tab mid-game and return |
| Silent save loss from storage eviction (Safari 7-day rule) or quota (Firefox ≤ 10 GiB) | Medium | High (user data) | Mandatory export/import; `storage.persist()`; install guidance on Safari; `storage.estimate()` before imports | Safari: verify eviction behaviour for installed vs non-installed sites; Firefox: OPFS import of a 4.5 GB ISO |
| Second tab cannot open memcards/NVRAM/INI (exclusive OPFS handles) | Medium | Low | Single-instance guard via Web Locks or BroadcastChannel | Open the app in two tabs |
| Disc I/O too slow or quota-limited for 4–8 GB images | Medium | Medium | Disc-I/O worker (no import); OPFS import as fallback | 4.5 GB ISO random-read and streaming benchmark from `File` in Chrome, Firefox, Safari (M2 task 2) |
| JIT module compile storms or per-thread table limits | High (M5) | Medium | Hotness threshold, batching, async compile, caps | Micro-benchmark: 10k modules per worker, compile latency, memory, in 3 engines |
| Memory kills on iOS or low-RAM devices; tab memory beyond the wasm heap | High | Medium | Mobile profile ≤ 768 MB–1 GB; trim IOP and caches; count JS heap, workers and GPU memory | Real-device test of fixed 1 GB shared memory on iPhone/iPad; total tab memory in M3/M4 |
| iOS: Lockdown Mode reportedly disables WebAssembly (per-site exceptions reportedly ineffective since iOS 17.4); all browsers are WebKit; no fullscreen or pointer lock on iPhone | Certain (for those users) | Medium | Detect `typeof WebAssembly === 'undefined'` and show a specific message; state iOS limits up front | Test on an iPhone with Lockdown Mode on |
| Browser WebGPU reach (Firefox Linux/Android, Chrome Linux non-Intel-Gen12) | Certain | Medium | Supported-browser matrix; the SW renderer with page blit stays available | Feature-detect telemetry in the shell (if open question 9 allows) |
| Legal exposure (BIOS or game hosting, trademarks, patches.zip terms) | Low if disciplined | High | User-supplied only; never deploy `build-wasm/data` (it contains a BIOS); distinct name; record patches.zip licence | Deployment script whitelist; review the asset list before each deploy |
| Upstream friction or `AGENTS.md` violations | Medium | Medium | Keep the fork downstream; humans author any upstream PRs | Decide the upstreaming policy now (open question 1) |
| Parallel sessions editing the same worktree (frontend, GSDeviceNull) | High (today) | Medium | Stage only your own files; agree ownership of `pcsx2-wasm/` and `GSDeviceNull` | Check `git status` before each change |

---

## 7. Prior art and what to borrow

| Project | What it is | Borrow | Caveat |
|---|---|---|---|
| **Play!** (BSD-2) — https://github.com/jpd002/Play- , https://playjs.purei.org , CodeGen https://github.com/jpd002/Play--CodeGen | HLE-BIOS PS2 emulator; Emscripten build with 2 workers, WebGL2 and a wasm JIT (since 2021) | `Jitter_CodeGen_Wasm.cpp` and `WasmModuleBuilder` as a reference or library; COOP/COEP deployment (S3 + CloudFront); Node CodeGen test suite | One module per block, synchronous compile, no block linking (`BasicBlock.cpp:300-433`), no SMC invalidation (`EeExecutor.cpp:260`). Helpers wrapped in JS thunks; compiles from a SAB subarray. Disc reads busy-wait on the page thread. |
| **ps2-web-recomp** (MIT) — https://github.com/snuri00/ps2-web-recomp | Per-game AOT EE recompilation plus a VU1 runtime JIT and a WGSL compute GS | `vu_jit_web.cpp` (slice plus raw `wasmExports`), AudioWorklet SAB ring, MessageChannel yielding, HTTP Range disc reads, compute-GS techniques, bit-exact software PS2 floats | HLE kernel; not cycle-accurate; its 60 fps figures do not transfer to an LLE PCSX2 |
| **Shubin123/pcsx2_web** (GPL-3) — https://github.com/Shubin123/pcsx2_web | Browser shell around a private PCSX2 wasm build | coi-serviceworker setup, WORKERFS-in-worker layout, OPFS import cache, Puppeteer tests | Speed numbers are not comparable: heavy speedhacks (EECycleRate=-2, EECycleSkip=2, vu1Instant, WaitLoop, IntcStat, fastCDVD), the 0.63× gameplay figure was measured under Node, and its 2.1× gain combines MTVU with a VU1 decode cache. It links an Internet Archive ISO catalog (**do not copy**). |
| **flycast-wasm** (GPL-2) — https://github.com/nasomers/flycast-wasm | SH4→wasm JIT in production | Async compile queue with interpreter bridge, batching, storm mode, import-crossing costs (30–40 ns), page-generation SMC, shadow-differential testing | Different guest; GPL-2-only code needs a licence check before copying |
| **v86** — https://github.com/copy/v86 | x86→wasm JIT | Hotness threshold, multi-page modules, fixed table-slot pool, TLB "has code" bit, engine limits (~7 MB per function, br_table issues) | — |
| **qemu-wasm** — https://github.com/ktock/qemu-wasm | TCG→wasm backend | Per-thread instance lists, instance caps, FinalizationRegistry accounting, `HEAP8.slice` for Firefox | Not upstream; QEMU merged the interpreter-only Emscripten build instead |
| **ARMSX2** — https://github.com/ARMSX2/ARMSX2 , **LRPS2** — https://github.com/libretro/ps2 | AArch64 EE/IOP/VU recompilers for PCSX2 | Order of work, per-op interpreter fallback, golden-framebuffer hashes (LRPS2 commit 5f2eea7bf), softmem inline lookup design | Assumes 64-bit host pointers and fastmem first; LRPS2 VU1 still about 70 % covered |
| **Upstream ARM64 port** — https://github.com/PCSX2/pcsx2/pull/11364 | Interpreter-only non-x86 template | `pcsx2/arm64/RecStubs.cpp`, CMake arch hooks, `_arm64.h` 128-bit GSVector | Its "2–3×" figure is native recompilers vs Rosetta, not vs interpreters |
| **parallel-gs** (LGPL-3) — https://github.com/Arntzen-Software/parallel-gs | Vulkan compute LLE GS | An alternative GS design to evaluate before committing to the raster HW port | LGPL; Vulkan-only |
| **.NET jiterpreter** — https://github.com/dotnet/runtime/blob/main/docs/design/mono/jiterpreter.md | Interpreter-first trace JIT in wasm | Tiering model | Disabled when threaded |
| **Feature matrix** — https://github.com/WebAssembly/website/blob/main/features.json ; **coi-serviceworker** — https://github.com/gzuidhof/coi-serviceworker | — | Gate optional features; demo hosting | coi-serviceworker occupies the service-worker scope; merge it with any caching worker |

---

## 8. Legal, licensing and project rules

*Not legal advice; have counsel review before a public launch.*

- **GPLv3 applies to a hosted build.** Serving wasm/JS is conveying object code (`COPYING.GPLv3:99-101`; GPL FAQ #UnreleasedMods). Under §6(d) (`:275-286`), provide equivalent access to the Corresponding Source next to the app for as long as it is served. That source must include build scripts (`:134-137`): the exact `tools/wasm/build-dependencies-wasm.sh`, the pinned dependency tarballs and patches (zlib, FreeType, libjpeg-turbo, libpng with the APNG patch, libwebp, SDL3, lz4, zstd, plutovg, plutosvg, rapidyaml), the Emscripten version, and the frontend sources (`pcsx2-wasm/`, its `web/` page and `tools/wasm/serve.py`).
- **The licence is effectively GPL-3.0-only.** About 1,028 files are GPL-3.0+, but `pcsx2/USB/usb-pad/lg/lg_ff.cpp` is GPL-3.0-only and in the unconditional USB list (`pcsx2/CMakeLists.txt:379-412`).
- **Notices.** Show Appropriate Legal Notices (GPL, no warranty) in an About page. Extend `bin/docs/ThirdPartyLicenses.html` (58 third-party entries; **SDL is missing**) with Emscripten (MIT/UIUC), musl, libc++ (Apache-2.0 with LLVM exception), Dawn/emdawnwebgpu (BSD-3), SDL3 (zlib), mimalloc if used, and coi-serviceworker if used.
- **BIOS and games.** Never host, link or bundle them. Users dump their own BIOS (point to PCSX2's dumping docs). Files are read locally and never uploaded. `AGENTS.md:29-30` forbids committing BIOS, game images or keys. `build-wasm/data/PCSX2/bios` holds a BIOS, so deploy only whitelisted Release artifacts (for example from `build-wasm-web/bin`), never by copying a build directory. `serve.py --bios-dir` is for local automated runs only. Do not reproduce pcsx2_web's archive catalog.
- **Legal background.** Sony v. Connectix (2000) supports reverse-engineered emulators. Nintendo v. Yuzu (2024) shows the cost of crossing into key or game distribution.
- **Naming.** Do not use "PlayStation", "PS2" logos or Sony trade dress. Use a distinct product name ("X, based on PCSX2"). Whether "PCSX2" is a registered mark, and the team's stance on web forks, are unverified; ask through human channels.
- **Bundled data.** The terms for redistributing `GameIndex.yaml`, `RedumpDatabase.yaml` and the externally sourced `patches.zip` (PCSX2/pcsx2_patches releases) on a public site are unverified beyond the main repo licence. Record the pcsx2_patches licence before serving it.
- **`AGENTS.md` rules** (verified, `AGENTS.md:113-160`; included by `CLAUDE.md`, so they bind agents in this checkout):
  - Agents must not open or update PRs, issues or discussions, or post comments via any GitHub UI, API or CLI.
  - AI-generated PR descriptions, commit messages, review responses and issue text must not be published to GitHub.
  - Contributors must declare LLM use. New contributors may not use LLMs for contribution content, and nobody may use them for full code generation. Contributors must be able to explain every change without LLM help.
  - Before code changes, agents must check the human's understanding, give guidance rather than solutions, and confirm the human can explain the change.
  - **Consequence:** the `wasm` branch, with large LLM-assisted commits, **cannot be upstreamed as-is**. Upstreaming means human-written, human-explained patches. Good first candidates: the IOP `TotalRam` sizing, the X11-link inconsistency, the `Pcsx2Config.cpp:618-621` key bug, the gsrunner `OutputModule` key. Whether to keep `AGENTS.md` in the fork is the owner's call (open question 1).

---

## 9. Week-1 starter checklist

Builds on the existing worktree. Run from `/home/nassim/Desktop/Startups/pcsx2-web-wasm`. Another session owns uncommitted work there (`pcsx2-wasm/`, `tools/wasm/serve.py`, `GSDeviceNull`, root `CMakeLists.txt`): coordinate before editing, and stage only your own files.

**Day 1: reproduce the toolchain, the Node boot and the browser build.**
```bash
cd ~/emsdk && ./emsdk install 6.0.12 && ./emsdk activate 6.0.12
source ~/emsdk/emsdk_env.sh && emcc --version          # expect 6.0.12
cd /home/nassim/Desktop/Startups/pcsx2-web-wasm
tools/wasm/build-dependencies-wasm.sh deps-wasm        # static, -pthread, -sSUPPORT_LONGJMP=wasm
emcmake cmake -S . -B build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_FIND_ROOT_PATH="$PWD/deps-wasm" -DCMAKE_PREFIX_PATH="$PWD/deps-wasm" \
  -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DENABLE_GSRUNNER=OFF
# The EMSCRIPTEN branches already force USE_OPENGL/USE_VULKAN/X11_API/WAYLAND_API/USE_BACKTRACE off.
cmake --build build-wasm --target pcsx2-headless
node build-wasm/bin/pcsx2-headless.js -approot "$PWD/bin" -dataroot "$PWD/build-wasm/data" \
  -bios "$PWD/build-wasm/data/PCSX2/bios/<your-dump>.bin" -renderer sw -frames 600 \
  -screenshot "$PWD/build-wasm/shots/boot.png"
# Same run with -renderer null to split CPU cost from SW-GS cost.

# Browser frontend (same configure flags, separate build dir):
emcmake cmake -S . -B build-wasm-web -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_FIND_ROOT_PATH="$PWD/deps-wasm" -DCMAKE_PREFIX_PATH="$PWD/deps-wasm" \
  -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DENABLE_GSRUNNER=OFF
cmake --build build-wasm-web --target pcsx2-wasm pcsx2-wasm-web
python3 tools/wasm/serve.py build-wasm-web/bin    # 127.0.0.1:8080, COOP/COEP headers
```
Also make a Devel and an ASan (`-DUSE_ASAN=ON`) build directory. Record wall time and VPS per BIOS, separately for the intro and the menu.

**Day 2: understand, then fix the known latent bugs.** Per `AGENTS.md`, the owner should first be able to explain each one:
- the fastjmp cancel value (`Interpreter.cpp:587-598`)
- MultiPause calibration (`common/HostSys.cpp:48-113`)
- `VU0.cpp:55`
- explicit `-pthread` link
- IOP `Main` sizing (`MemoryTypes.h:56`)
- fixed memory (`-sINITIAL_MEMORY=1GB -sMAXIMUM_MEMORY=1GB`, drop `ALLOW_MEMORY_GROWTH`)
- the stale `OutputModule` key in `pcsx2-headless/Main.cpp:105`

Re-run the Node boot and the ASan boot.

**Day 3: finish the browser harness (do not create a second target).**
- Open the `pcsx2-wasm` page from `serve.py` in Chrome and Firefox and boot an uploaded BIOS. Goal: OSDSYS frames in the canvas and console output in the page.
- **Decide legacy FS vs WasmFS.** If WasmFS: replace `-sFORCE_FILESYSTEM` with `-sWASMFS` and mount OPFS at `/data` from `main()`. Add `-sALLOW_BLOCKING_ON_MAIN_THREAD=0` and check it under ASSERTIONS.
- Record which resources are missing for FullscreenUI and patches (`fullscreenui/`, `icons/`, `RedumpDatabase.yaml`, `patches.zip`).

**Day 4: the early risk spikes, as throwaway prototypes.** These de-risk M4 and M2; they are not on the first-pixels path.
- (a) **JSPI + emdawnwebgpu on a pthread:** `requestDevice`, clear the canvas each frame, suspend between frames, timed `WaitAny` on `mapAsync`, `device.destroy()` recovery. Test in Chrome, Firefox 153 and Safari 27.
- (b) **Disc-I/O worker:** read random 2 KB sectors and a sustained stream from a 4.5 GB `File` into SAB from a blocked pthread; measure latency and throughput.
- (c) **Compile and instance limits:** compile 10k tiny modules from a SAB view inside a worker; measure time and memory per engine.

**Day 5: baselines on desktop.**
- Native x86 build, all interpreters, Round=Nearest, DAZ/FTZ off: 20-game accuracy sheet.
- Per-subsystem time split: EE, VU1, IOP, GS, IPU, CDVD read, SPU2 (including soundtouch/freesurround).
- A desktop build with `ENABLE_JIT_RASTERIZER` undefined, for the SW reference diffs.
- `gsrunner` readback counts on a dump set.

These feed the M2 exit, the M4 design and the M5 order.

---

## 10. Open questions for the owner

1. **Upstream policy.** Is this a permanent downstream product, or do you want parts upstreamed? Upstreaming needs human-authored, explained patches and no agent GitHub activity. Do you want `AGENTS.md` kept in the fork, with its rules binding agents here?
2. **Product name and hosting.** Pick a domain and a host with header control: Cloudflare/Netlify `_headers`. Cloudflare Pages has a 25 MiB per-asset cap; the Release wasm is 9.9 MB plus a 4.3 MB `.data` today. Set a payload budget.
3. **Browser floor.** Is "Chrome/Edge 137+ primary; Firefox 153+ (Windows, Apple Silicon); Safari 27 later" acceptable? Is mobile a goal, or explicitly out of scope for v1? iOS would mean Safari 27+, no Lockdown Mode, no fullscreen or pointer lock on iPhone.
4. **Savestates.** Must browser savestates load in desktop PCSX2 and vice versa? If yes, `Gif_Path`, SPU2 `Cores`/`Voices` and any other raw-pointer freezes need field-wise serialization (or 8-byte padding) on wasm32, plus an automated cross-load test.
5. **Accuracy vs speed.** Is forced round-to-nearest acceptable, with software chop only where measured regressions justify the cost?
6. **JIT investment.** Is a multi-month JIT effort (microVU1, then EE) in scope, or should the product launch interpreter-only and SW-rendered for 2D and light titles first? The milestone order now allows the latter.
7. **Disc import UX.** Read in place from the user's `File` (no copy; re-pick each session unless you use the Chromium-only file-handle persistence), or import into OPFS (quota-limited, especially Firefox's 10 GiB; slow first load; persistent)?
8. **Networking features.** Achievements (fetch, CORS unknown), PINE, DEV9 online: needed for v1?
9. **Telemetry.** May the shell report adapter features and speed anonymously, to drive the supported-device list?
10. **Benchmark set.** Which 10–20 titles do you own and can dump legally for the regression and golden-hash suite? Include at least one 2D title and one FMV-heavy title.
11. **Filesystem.** Confirm the switch from the frontend's legacy FS/MEMFS to WasmFS + OPFS (agree it with the session building the frontend).
12. **Peripherals and HDD.** OK to default all USB ports to None, support only GunCon 2 via pointer, and leave mic, camera, wheel force feedback and the DEV9 HDD out of v1?
13. **Background play.** Should emulation pause when the tab is hidden, or keep running audibly (Chrome does not throttle audible tabs)?
14. **patches.zip.** Is serving PCSX2/pcsx2_patches' `patches.zip` acceptable under its licence, or should widescreen and other patches be user-imported only?

---

## 11. Code pointer index

**M0 (done; for review and hygiene)**
- `cmake/Pcsx2Utils.cmake:12-22`, `cmake/BuildParameters.cmake:21-36,83-94`, `cmake/SearchForStuff.cmake:20-74`, `cmake/FindWebP.cmake:61-96`, `CMakeLists.txt:63-73`
- `common/Pcsx2Defs.h:26-56`, `common/VectorIntrin.h`, `common/SingleRegisterTypes.h:191-195`, `common/FPControl.h`, `common/FastJmp.{h,cpp}`, `common/HostSys.{h,cpp}` (wasm MultiPause `wasm: :43-44`), `common/Linux/LnxThreads.cpp:15-27,341-347`, `common/Emscripten/*`, `common/CMakeLists.txt:157-213`
- `pcsx2/CMakeLists.txt:483-493,1050-1080,1117-1146,1200-1204`, `pcsx2/wasm/RecStubs.cpp`, `pcsx2/Vif_Dynarec.h:46`, `pcsx2/Vif_Unpack.cpp:477-496`, `pcsx2/Config.h:1500-1517`, `pcsx2/VU0.cpp:55`, `pcsx2/VMManager.cpp:2700-2785,3450-3485`
- `pcsx2/GS/GSVector.h:87-185`, `GSVector4.h:195-198`, `GSVector4i.h:835-838`, `GSXXH.{h,cpp}`, `GS/Renderers/SW/GSDrawScanline.{h,cpp}`, `GS/Renderers/Common/GSVertex.h:22-32`
- `pcsx2/IPU/IPU_MultiISA.cpp:1377-1409`, `IPU/yuv2rgb.h`, `pcsx2/CDVD/IsoReader.cpp:296`, `pcsx2/DEV9/AdapterUtils.cpp:521`, `pcsx2/GS/GSCapture.cpp`, `pcsx2/Pcsx2Config.cpp:1290`

**M1**
- `pcsx2/Interpreter.cpp:559-598` (cancel/exit), `pcsx2/vtlb.{h,cpp}` (u64 entries, fastmem guards `:78,972-1010,1228,1325-1356`), `pcsx2/Memory.{h,cpp}:42-122`, `pcsx2/MemoryTypes.h:56`, `pcsx2/IopMem.cpp:95`
- `common/HostSys.cpp:48-113`, `common/Semaphore.cpp`
- Browser frontend (wasm\*): `pcsx2-wasm/Main.cpp` (`:193-194` audio key, `:215` PresentFrame, `:384` present callback, `:479` `pcsx2web_frame`, `:498` RunOnCPUThread queue), `pcsx2-wasm/CMakeLists.txt` (preload list, link options), `pcsx2-wasm/web/app.js` (`:118,676` 2D blit, `:1472` visibilitychange), `tools/wasm/serve.py`
- `pcsx2-headless/Main.cpp` (`:105` stale key, `:146-152` renderer switch), `pcsx2-gsrunner/Main.cpp:295-298,813,842-897`, `pcsx2-qt/QtHost.cpp:372-424` (EmuThread template)
- `common/FileSystem.cpp:2574-2650`, `pcsx2/Pcsx2Config.cpp:2156-2299` (EmuFolders), `pcsx2/ps2/BiosTools.cpp`, `pcsx2/CDVD/CDVD.cpp:160-169` (NVRAM), `:957-966` (RTC), `pcsx2/Config.h:1374,1408` (manual RTC)
- `pcsx2/GS/Renderers/Null/GSDeviceNull.{h,cpp}` (present callback, wasm\*), `pcsx2/GS/GS.cpp:79-150`, `pcsx2/ImGui/ImGuiManager.cpp:150-153,490-520`
- Logs: `build-wasm/boot-rel.log:122,174-460`, `build-wasm/boot-70004.log`

**M2**
- `pcsx2/GS/Renderers/SW/GSRendererSW.cpp:32,116,180`, `GSRasterizer.cpp:1602-1630`, `GSJobQueue.h:35-77`, `pcsx2/GS/Renderers/Common/GSRenderer.cpp:237-250,530-579,778-845`
- `pcsx2/GS/GSLocalMemory.{h,cpp}` (`:53`, `PAHelper` `:225-281`), `pcsx2/GS/GS.cpp:1039-1080`, `GSLocalMemoryMultiISA.cpp:772-1040`
- `pcsx2/CDVD/ThreadedFileReader.{h,cpp}` (`:343,379,390`), `CDVD/InputIsoFile.cpp:40-57`, `CDVD/FlatFileReader.cpp`, `pcsx2/GSDumpReplayer.cpp`
- `GS/Renderers/SW/GSDrawScanline.cpp:13-14,169-173` (C-path reference build), `GSDrawScanlineCodeGenerator.all.cpp:339-341`, commit 3dd7028302
- `pcsx2/IPU/IPU_MultiISA.cpp`, `IPU/yuv2rgb.h` (FMV path on the EE thread)

**M3**
- `pcsx2/Host/AudioStream.{h,cpp}` (`:107-128,230-357,595-632,672-701,763-777`), `AudioStreamTypes.h:11-17`, `SDLAudioStream.cpp`, `pcsx2/SPU2/spu2.cpp:103-134`, `pcsx2/Config.h:981-982`
- `pcsx2/Input/InputSource.h`, `InputManager.{h,cpp}:1871-1877`, `SDLInputSource.cpp:790,881`
- `pcsx2/SIO/Memcard/MemoryCardFile.cpp:118,148,309-313,441-525`, `pcsx2/SaveState.cpp:1037`, `pcsx2/SPU2/spu2freeze.cpp:41-156`, `pcsx2/SPU2/defs.h:264,469-470`, `pcsx2/Gif_Unit.cpp:206`, `pcsx2/INISettingsInterface.cpp:90-117`
- `pcsx2/VMManager.cpp:1259,1430,1990,2248,2306-2347,2830,2966-2971`, `pcsx2/MTGS.cpp:274-280`, `pcsx2/Config.h:849`, `common/WindowInfo.h:43`, `pcsx2/Host.h`, `pcsx2/ImGui/FullscreenUI*.cpp` (`FullscreenUI.cpp:336`)
- `pcsx2/Patch.cpp:117,288-317`, `.github/workflows/linux_build_qt.yml:129`, `bin/resources/{fullscreenui,icons,RedumpDatabase.yaml}`
- `pcsx2/USB/*` (`pcsx2/CMakeLists.txt:731-735`), `3rdparty/cubeb/CMakeLists.txt:60-106`, `pcsx2/ImGui/FullscreenUI_Settings.cpp:105` (HDD creation)
- `pcsx2/Interpreter.cpp:156-182,270-286`, `pcsx2/R5900.cpp:362-459`, `pcsx2/VU1microInterp.cpp:40-60,64-160,278`, `pcsx2/VUops.cpp:1862-1873` (MTVU-safety), `common/HTTPDownloader.h`
- `bin/docs/ThirdPartyLicenses.html`, `bin/docs/GPL.html`, `COPYING.GPLv3`

**M4**
- `common/WindowInfo.h:12-19`, `pcsx2/MTGS.cpp:118-175,283-297,322-340,758-779,876-923`
- `pcsx2/GS/Renderers/Common/GSDevice.{h,cpp}` (`FeatureSupport :1399-1426`, `PresentResult::DeviceLost :1386`, virtuals `:1509-1719`, blend map `:2083-2165`, ImGui textures `:568-640`), `GSTexture.{h,cpp}:105-308`, `GSRenderer.cpp:552-578` (device loss, ImGui render)
- `pcsx2/GS/Renderers/HW/GSRendererHW.cpp` (expansion `:5503-5690`, DATE `:6099-6295`, blend `:6946-7193,7384`, AFAIL `:9181-9290`, readbacks `:84,2501,9969,10728`), `HW/GSTextureCache.cpp:1761,5002-5282,7476-7549`, `HW/GSTextureCache.h:439-446` (memory counters), `HW/GSTextureReplacementLoaders.cpp:468-497`
- Templates: `DX11/GSDevice11.cpp:67,710-720,1557-1586,3201-3412`, `DX12/GSDevice12.cpp:1395,4179-4231`, `DX12/GSTexture12.cpp` (256-B pitch), `Vulkan/GSDeviceVK.cpp:2464,2950-2971,3612-3627,3850-3866,4777,4981-5236,6332-6371`, `Vulkan/GSTextureVK.cpp:353-363,944-965`, `Metal/tfx.metal`, `dx11/tfx.fx`, `vulkan/tfx.glsl`
- `pcsx2/GS/GS.h:16-24`, `pcsx2/Config.h:271-281,399-406,763,900-901`, `pcsx2/GS/GSUtil.cpp:291-306`, `pcsx2/GS/GSCapture.cpp:139-140,880-930` (deferred-readback model)
- `bin/resources/shaders/vulkan/{convert,present,merge,interlace,imgui}.glsl`, `cmake/ShaderToCpp.cmake`, `tools/shader_to_cpp.py`

**M5**
- `pcsx2/x86/ix86-32/iR5900.cpp` (`:375-379,1266-1270,1401-1414,2039-2120`), `x86/iR5900.h:36-62`, `x86/iR5900Analysis.cpp`, `x86/BaseblockEx.h:14-20,200-249`, `x86/ix86-32/recVTLB.cpp:154,371,559,684`
- `pcsx2/x86/microVU.cpp:216-260,315-319,412-420`, `x86/microVU_Analyze.inl`, `x86/microVU_IR.h`, `pcsx2/MTVU.cpp`, `pcsx2/Config.h:1500-1506`
- `pcsx2/vtlb.cpp:525-549,1451-1496`, `pcsx2/Sif0.cpp:43`, `pcsx2/sif2.cpp:69`, `pcsx2/COP0.cpp:260-331`, `pcsx2/IopMem.cpp:288,331,404`, `pcsx2/Vif_Codes.cpp:342-362`
- `pcsx2/arm64/Vif_Dynarec.cpp`, `arm64/Vif_UnpackNEON.cpp` (second-backend patterns), `pcsx2/GS/Renderers/Common/GSFunctionMap.{h,cpp}` (scanline specialization seam), `pcsx2/GS/Renderers/SW/GSDrawScanline.cpp:42-60,489+`