# WebGPU GS device: design (M4)

*Written 2026-10-16 for the M4 milestone of `docs/web-port/PLAN.md` (sections 3.4, 4 and M4), and
revised the same day after phase 1 (the runtime spike and the utility shaders). Step 1 of §15 (the
device behind the SW renderer) is built; what it measured, and where it departs from this design, is in
"Step 1 results" at the end of §15 Step 1. The runtime architecture in §3.2 and §12 is the one the spike proved
(`tools/wasm/webgpu-spike/`, results in its `README.md`). The utility bindings in §8.2 are those of the
shaders already written in `bin/resources/shaders/webgpu/` and validated by
`tools/wasm/webgpu-shaders/validate-shaders.mjs`.*

**How to read this.** "Verified" means read in this tree (paths relative to the repository root) or in
the emdawnwebgpu package that Emscripten 6.0.12 downloads
(`~/emsdk/upstream/emscripten/cache/ports/emdawnwebgpu/emdawnwebgpu_pkg`, Dawn release
`v20260423.175430`, called "the port" below). "Measured" means observed by the spike, the shader
validation or the feature probe in headless Chrome 155 on this machine's Radeon 780M (RADV, Mesa
23.2.1), Chrome flags in §12.5; the logs stay outside the repository. "Spec" means WebGPU or WGSL
behaviour as I understand the specification; step 1 or 2 must confirm each one. "Estimate" means a
judgement nobody has measured. Only Chrome was measured; nothing here is proven for Firefox or Safari.

**Why WebGPU, given the speed profile.** The BIOS intro is bound by the EE interpreter (450 frames take
35.5 s with the Null renderer and about 36 s with the SW renderer; real time is 7.5 s). WebGPU will not
make the intro faster. It is needed for games, whose GS load the C scanline SW renderer cannot carry, and
to take drawing off the CPU threads that the interpreters need.

---

## 1. Summary

- **One new backend**, `GSDeviceWebGPU`, in `pcsx2/GS/Renderers/WebGPU/`, written against the port's
  `webgpu.h`. In API shape it follows the Vulkan backend: explicit render passes, pipeline objects,
  immutable bind groups, one command encoder per frame. Its feature profile is DX11's: no texture
  barriers, so feedback goes through RT copies; no geometry shaders, so primitives expand in the vertex
  shader (as on Metal); no framebuffer fetch; no ROV.
- **The MTGS thread keeps owning the device** (linked with `-sJSPI`, keeping wasm setjmp/longjmp) and
  suspends through JSPI in five places: adapter and device creation, synchronous readbacks, waits for
  pipeline compilation, device-loss recovery, and its idle wait for work. Nothing suspends per frame.
  The GS threading model does not change otherwise (§3.2).
- **Frames reach the page without returning to the event loop** (measured). MTGS draws into an
  OffscreenCanvas it creates itself and posts each frame as an `ImageBitmap` to a `bitmaprenderer`
  canvas on the page. No canvas is transferred and `-sOFFSCREENCANVAS_SUPPORT` is not used. This path
  also survives a GPU-process crash, which a transferred canvas does not (§12).
- **Feature profile:** `texture_barrier=0`, `multidraw_fb_copy=1`, `vs_expand=1`,
  `provoking_vertex_last=0`, `point_expand=line_expand=0`, `framebuffer_fetch=0`, `rov=0`,
  `depth_feedback=0` (depth feedback through the DS-as-RT colour clone), `test_and_sample_depth=1`,
  `prefer_new_textures=1`. `stencil_buffer`, `primitive_id` and the BC formats depend on optional
  WebGPU features.
- **Required features:** none for step 1 (core feature level). The HW renderer needs
  `dual-source-blending` until a fallback exists. Optional: `depth32float-stencil8`,
  `texture-compression-bc`, `float32-filterable`, `float32-blendable`, `primitive-index`,
  `texture-formats-tier1`, `timestamp-query`.
- **Bindings.** Utility passes use two bind groups: group 0 holds the per-draw constants (one uniform
  with a dynamic offset, replacing Vulkan's push constants), group 1 the source texture and its
  sampler (§8.2, the layout the written shaders use). TFX draws use three: per-draw uniforms plus
  vertex pulling, source textures plus sampler, feedback textures (§8.1). The base vertex of expanded
  draws travels in `firstInstance`.
- **Shaders** are hand-written WGSL in `bin/resources/shaders/webgpu/`. The utility shaders are plain
  modules, one per file, whose entry points are picked by name with a few pipeline-overridable
  constants; they need no preprocessing and are written and validated (§9.3). TFX (step 2) goes
  through a small C++ `#if` preprocessor fed the same macro list the Vulkan backend uses, with one
  module per selector; pipelines are cached by a `PipelineSelector` modelled on Vulkan's.
- **Three steps:**
  1. Present, merge, interlace, shadeboost and ImGui on the GPU behind the SW renderer, with the
     runtime layer from the spike. In the browser this replaces GSDeviceNull's CPU paths and renders
     the BIOS.
  2. The HW renderer with the TFX paths the BIOS uses.
  3. The rest of TFX, readbacks, device loss, performance.
  
  In total that is about 6–7k lines of C++ and 3.5–4k lines of WGSL (estimate). The steps are sized M,
  L and XL on the PLAN.md scale.

---

## 2. What the GS expects of a device, and where WebGPU differs

| PCSX2 assumes | WebGPU / the port | Consequence for the design |
|---|---|---|
| Synchronous readback: `GSDownloadTexture::Flush()` then `Map()` (`GSTexture.h:263-327`) | Only `mapAsync`. The result arrives on the worker's event loop. | JSPI suspend inside `Flush()`: 2.3–3.0 ms median per readback (measured, §11) |
| Device creation inside `GSopen` on MTGS, synchronous (`MTGS.cpp:172-175`) | `requestAdapter` and `requestDevice` are async | JSPI waits in `Create()`: about 50 ms and 15 ms (measured) |
| Present is a swap-chain call | A transferred canvas shows a frame only when its worker returns to its event loop (measured). `wgpuSurfacePresent` aborts in the port (verified, `library_webgpu.js:2620-2623`). | Never call it. Draw into a GS-owned OffscreenCanvas and post `transferToImageBitmap()` to the page: no yield (§12). |
| Threads sleep in blocking waits (`WorkSema`, `Atomics.wait` underneath) | Submitted commands reach the GPU while the worker blocks (measured). Callbacks (device lost, maps) are delivered only while the worker's event loop runs (measured). | The idle wait suspends instead of blocking (§3.2) |
| Texture barriers or framebuffer fetch for SW blending, DATE, tex-is-fb, fbmask | A texture cannot be an attachment and be sampled in the same pass (spec; measured) | DX11 copy model: copy the RT to a clone, which ends the pass (§10.2) |
| Vulkan's 96 B of push constants and push descriptors (`GSDeviceVK.cpp:4037-4100`) | None. The port has a `maxImmediateSize` limit but no setter (verified by grep). | Dynamic-offset uniforms and cached bind groups (§8) |
| Geometry shaders (DX11/GL expansion) | None | `vs_expand` with storage-buffer vertex pulling, as on Metal |
| Last-vertex provoking (GL) | `@interpolate(flat)` takes the first vertex (spec) | `provoking_vertex_last=0`. `GSRendererHW::HandleFlatShadedVertices` fixes the colours on the CPU, as for DX, VK and Metal. |
| `gl_PrimitiveID` | Only with the `primitive-index` feature | PrimID DATE is optional |
| Wide lines, large points | 1 px only | `point_expand=line_expand=0`, so upscaled points and lines use VS expansion (`GSRendererHW.cpp:5557-5690`) |
| Partial depth copies (VK, GL, DX12) | Copies of depth/stencil formats must cover the whole subresource (spec) | Depth feedback through DS-as-RT (an R32F colour clone that can be copied partially). Full copies otherwise, like DX11 (`GSDevice11.cpp:1541-1597`). |
| Copy within one texture (VK `Layout::CopySelf`) | Source and destination subresources must differ (spec) | Bounce through a temporary texture |
| Read-only depth chosen per draw (layouts, DSVs) | `depthReadOnly` is a property of the render pass | Sampling the bound depth buffer splits the pass |
| GLSL/HLSL/MSL with a preprocessor; a shader compiler on the host | WGSL only: no preprocessor, strict typing, uniformity analysis, no multi-component swizzle assignment. No compiler in the page. | Hand-written WGSL; entry points and override constants for the utility shaders, a C++ preprocessor for TFX (§9) |
| A VRAM budget and synchronous allocation failure | No memory query. Out of memory is reported asynchronously (error scope or device loss). | The device keeps its own budget in `CreateSurface` (§6.5) |
| Device loss followed by a synchronous `GSreopen(true, …)` (`GSRenderer.cpp:547-567`) | Loss is reported asynchronously; after a GPU-process crash, 130–630 ms later (measured). Recovery needs a new adapter and a new device, both async, and a new canvas. | Set a flag; recover under JSPI (§13) |
| Upload order is command order (VK init buffer vs draw buffer) | `queue.writeTexture` runs on the queue timeline, before the not-yet-submitted encoder | Split uploads into early and late (§6.3) |

---

## 3. Classes, files and plug-in points

### 3.1 Classes and what each is modelled on

| File (under `pcsx2/GS/Renderers/WebGPU/` unless noted) | Class | Modelled on | Role |
|---|---|---|---|
| `GSDeviceWebGPU.{h,cpp}` | `GSDeviceWebGPU final : public GSDevice` | Structure from **GSDeviceVK**: `Create`→`CheckFeatures`→`CreateBuffers`→`CreatePipelineLayouts`→`Compile*Pipelines`, dirty-flag state cache, `RenderImGui`, `BeginPresent`/`EndPresent` (`GSDeviceVK.cpp:2204-2562, 4720-4795, 5809-5975`). `RenderHW` feedback handling from **GSDevice11** (`RenderHW`, `SendHWDraw`, `FeedbackCopyAndBind`, `GSDevice11.cpp:3072-3441`). Binding grouping from the **GSDevice12** root signature (`GSDevice12.cpp:2818-2852`). | The device |
| `GSTextureWebGPU.{h,cpp}` | `GSTextureWebGPU final : public GSTexture` | **GSTextureVK**: clear state, use serial, early vs late upload (`GSTextureVK.cpp:352-363`); 256-byte staging pitch as in **GSTexture12** (`GSTexture12.cpp:592-705`) | Textures, views, uploads, mip generation |
| same file | `GSDownloadTextureWebGPU final : public GSDownloadTexture` | **GSDownloadTexture12** (256-byte transfer pitch, `GSTexture12.cpp:1161-1320`); the flush logic of **GSDownloadTextureVK** (`GSTextureVK.cpp:944-966`) | Readbacks with `mapAsync` |
| `WebGPUStreamBuffer.{h,cpp}` | `WebGPUStreamBuffer` | **VKStreamBuffer** / **D3D12StreamBuffer** | Vertex, index, uniform and upload rings: a CPU shadow plus `writeBuffer` |
| `WebGPUCaches.{h,cpp}` | `WebGPUBindGroupCache`, `WebGPUSamplerCache` | VK `GetSampler` (`GSDeviceVK.cpp:3848-3873`), DX12 descriptor tables | Caches of immutable objects |
| `WebGPUShaderCache.{h,cpp}`, `WGSLPreprocessor.{h,cpp}` (step 2) | `WebGPUShaderCache`, `WGSLPreprocessor` | The keys of **VKShaderCache**; the macro lists of `GSDeviceVK::GetTFXVertexShader`/`GetTFXFragmentShader` (`GSDeviceVK.cpp:4981-5085`) | TFX WGSL generation, shader-module and pipeline caches. Step 1 loads the utility modules directly. |
| `WebGPUHost.{h,cpp}` + `webgpu_host.js` (`--js-library`) | `WebGPUHost` (namespace) | `tools/wasm/webgpu-spike/spike.cpp` | Platform layer, the spike's primitives made reusable: timed waits, suspending sleep, the GS-owned OffscreenCanvas and its frame port, the surface-size feed, the lost flag (§12). Swappable for the fallbacks (§14). |
| `WebGPUBuilders.h` | descriptor helpers | **VKBuilders** | Brevity |
| `bin/resources/shaders/webgpu/*.wgsl` | — | `vulkan/*.glsl` (structure, macro names); `dx11/tfx.fx` (D3D clip space, depth-as-RT output `tfx.fx:195-229`); `Metal/tfx.metal` (first-vertex provoking, VS expansion) | Shaders |

`GSDeviceWebGPU` is built from the following parts (sizes are estimates):

- **Context.** Instance, adapter, device, queue and surface (on the GS-owned OffscreenCanvas). Adapter
  info is cached for `GetDriverInfo()` and for the settings UI.
- **Encoder and pass manager (§7).** One `WGPUCommandEncoder` per submit and at most one open
  `WGPURenderPassEncoder`. The pass is described by its attachments, load ops and read-only flags.
- **State cache (§7.3).** Pipeline, bind groups with their dynamic offsets, vertex and index buffers,
  viewport, scissor, blend constant and stencil reference. All of it is invalidated whenever a pass
  starts.
- **Rings:** `m_vertex_ring` (32 MiB, VERTEX | STORAGE | COPY_DST), `m_index_ring` (16 MiB),
  `m_expand_index_ring` (AA1), `m_uniform_ring` (4 MiB, in units of 256 bytes, the
  `minUniformBufferOffsetAlignment`: utility constants and the 48-byte VS buffer take one unit, the
  272-byte `PSConstantBuffer` two) and `m_upload_ring` (64 MiB, COPY_SRC, for late uploads). Plus the
  fixed `m_expand_ib` filled by `GSDevice::GenerateExpansionIndexBuffer`.
- **Pipelines.** `m_convert[ShaderConvertSelector::NUM_TOTAL_SHADERS]`, `m_present[PresentShader::Count]`
  (per target format), `m_merge[2]`, `m_interlace[5]`, `m_shadeboost`, `m_fxaa`, `m_cas[2]`, `m_imgui`,
  `m_date_setup[4]`, `m_primid_init[2][4]` and `m_tfx_pipelines` (an unordered_map keyed by
  `PipelineSelector`).
- **Layouts:** `m_tfx_bgl[3]`, `m_tfx_layout`; `m_utility_cb_bgl` (group 0), `m_utility_tex_bgl[2]`
  (group 1: float or unfilterable source), `m_utility_layout[2]`; `m_cas_bgl`, `m_cas_layout`.
- **Null textures** for unused bindings (1×1 float, 1×1 r32float) and the cached "null feedback" bind
  group.
- **Pending-destruction list** (§6.1) and the GPU memory counter (§6.5).

### 3.2 Threading and ownership

- **All `wgpu*` calls happen on the MTGS thread.** Verified: `GSopen` → `OpenGSDevice` → `Create` runs
  on MTGS (`MTGS.cpp:144-200`, `GS.cpp:117-185`). The SW raster threads (GS-SW-n) never touch the device;
  `GSRendererSW` uploads its output on MTGS (`GSRendererSW.cpp:116,180`). FullscreenUI and ImGui also run
  on MTGS. WebGPU JS objects live in the worker that created them (the port keeps a per-worker
  `jsObjects` table, `library_webgpu.js:120-123`), so no other thread may call the device. The adapter
  information the settings UI wants is cached when the device is created. A debug request from the page
  (for example "destroy the device" for tests) is an atomic flag that MTGS polls in `BeginPresent`.
- **JSPI on the MTGS pthread works as is** (measured): with `-sJSPI` on the `-sPROXY_TO_PTHREAD`
  layout, a pthread created by another pthread (as the CPU thread creates MTGS) can suspend from its
  start routine down. No `JSPI_EXPORTS`/`JSPI_IMPORTS` lists and no trampoline are needed; other exports
  (`pcsx2web_*`) stay synchronous, because `-sJSPI` wraps only `main` and thread entries by default
  (verified, `src/settings.js:924-950`).
- **Timed waits.** `WebGPUHost::Wait(future, timeout)` calls `wgpuInstanceWaitAny` with a timeout. In
  the port that path needs ASYNCIFY, which `-sJSPI` provides; without it, `emwgpuWaitAny` aborts
  (verified, `library_webgpu.js:689-718`). The instance must be created with
  `WGPUInstanceFeatureName_TimedWaitAny`, which the port grants only with JSPI or Asyncify, or timed
  waits return an error (verified, `webgpu.cpp:1705-1714`). Callbacks waited on this way use
  `WaitAnyOnly` mode. A timeout of 0 never suspends (`webgpu.cpp:1706`), so polling is free. Without
  JSPI or Asyncify there is no way to wait from C++: 4.9 million `wgpuInstanceProcessEvents` polls over
  2 s never saw the adapter (measured).
- **Suspending sleep.** `WebGPUHost::SleepSuspending(ms)` is `emscripten_atomic_wait_suspending` on a
  private word that nobody notifies. It is used for the adapter retry (§4) and the bounded event-loop
  turn below.

| Where | Waits for | Duration |
|---|---|---|
| `Create()` | `requestAdapter` (retried while it answers null), `requestDevice`, utility pipelines | adapter 46–67 ms, device 12–18 ms (measured); the first request in a fresh Chrome profile answered null after ~340 ms and a retry 0.5 s later worked (measured, every run). Pipelines: not measured. |
| MTGS idle (`WorkSema::WaitForWork`) | new ring-buffer work | Wake-up 0.045–0.06 ms median, against 0.03 ms for a blocking wait (measured) |
| `GSDownloadTextureWebGPU::Flush()` | `mapAsync` | 2.3–2.5 ms median for 256×256 RGBA8, 2.8–3.0 ms for 1024×1024, worst 8.7 ms (measured) |
| Pipeline miss (`GetTFXPipeline`) | `createRenderPipelineAsync` | 1–50 ms on a cold cache (estimate) |
| Device-loss recovery | a new adapter and device | 12–16 ms after `destroy()`, 0.56–0.72 s after a GPU-process crash (measured) |
| `EndPresent()`, only when MTGS has not idled for 100 ms | `SleepSuspending(1)`, an event-loop turn | 1 ms per 100 ms (design; not measured) |

- **The idle wait suspends.** A blocked worker never runs its event loop, so no callback reaches it:
  during a blocking `Atomics.wait` an `AllowSpontaneous` work-done callback did not fire, and one
  `MessageChannel` yield afterwards was not enough either; during `emscripten_atomic_wait_suspending`
  it fired (measured). Device loss after a GPU-process crash, early map completions (§11.3) and the
  uncaptured-error callback all need event-loop turns. So on wasm, `MTGS::MainLoop`'s
  `s_sem_event.WaitForWork()` (`MTGS.cpp:322-340`) must suspend instead of blocking. Proposed
  mechanism (not built yet; needs proper testing): in `common/Semaphore.cpp`, under `__EMSCRIPTEN__`,
  `KernelSemaphore` becomes a futex counter (`emscripten_atomic_wait_u32`, `emscripten_atomic_notify`)
  with a second wait, `WaitSuspending()`, built on `emscripten_atomic_wait_suspending`.
  `WorkSema::WaitForWork()` (and `WaitForWorkWithSpin()`) use it when the calling thread has opted in
  (`Threading::SetWaitsSuspend(true)`, set by `GSDeviceWebGPU::Create` on MTGS and cleared in
  `Destroy`). Only these idle waits suspend: `WaitForEmpty` and plain semaphore waits stay blocking, so
  the SW renderer's many syncs with its raster threads inside a frame do not pay the extra wake-up
  latency. `MTGS.cpp` is not changed. `Atomics.notify` wakes both blocking and asynchronous waiters,
  so `Post()` needs no mode. `pcsx2-headless` never opts in (it has no JSPI, and the suspending wait
  aborts without it, `libatomic.js:86`).
- **When MTGS never idles** (GPU- or GS-bound), `EndPresent` gives the event loop a 1 ms
  `SleepSuspending` turn if more than 100 ms have passed since MTGS last idled or waited, so that loss
  is still noticed. A bare yield is not enough (see above); whether 1 ms is, is a step-1 test.
- **Constraints the spike found** (measured):
  - Never suspend inside a WebGPU callback: JSPI throws `SuspendError: trying to suspend without
    WebAssembly.promising` (Asyncify appears to work, then corrupts memory). Callbacks only set flags
    and store results; recovery runs on MTGS's own stack.
  - No JS frames between the thread entry and a suspension. With `-sSUPPORT_LONGJMP=emscripten`, JSPI
    throws `SuspendError: trying to suspend JS frames`; with `-sSUPPORT_LONGJMP=wasm`, a `longjmp`
    back across a suspension 8 frames deep works. The build already uses wasm SjLj and
    `-fno-exceptions` (verified, `cmake/BuildParameters.cmake:88-89, 177`), so there are no `invoke_*`
    frames. No EM_JS or library function on MTGS's stack may call back into wasm code that waits.
  - `wgpuDeviceDestroy` calls the lost callback synchronously, on the caller's stack.
- **Re-entrancy hazard.** While MTGS is suspended, its event loop runs tasks that call into wasm on
  the same thread: WebGPU callbacks, and Emscripten's mailbox (`checkMailbox`) for work proxied *to*
  that thread. PCSX2 proxies nothing to the GS thread today (verified: no `emscripten_proxy_*`,
  `emscripten_dispatch_to_thread` or `MAIN_THREAD_*EM_ASM` in `common/`, `pcsx2/` or `pcsx2-wasm/`).
  Add a debug assertion (a thread-local "suspended" flag checked in the proxy entry) so a future change
  cannot re-enter GS code in the middle of a draw. The idle wait is the safe point: `MainLoop` has
  released its lock and holds no draw state there.
- **Locks held across a suspend.** `MainLoop` holds `s_mtx_RingBufferBusy2` while it processes packets
  and releases it around `WaitForWork` (verified, `MTGS.cpp:336-339`). A readback or pipeline wait
  suspends while holding it, which only makes other threads wait in `Atomics.wait` in their own
  workers. That cannot deadlock, because resuming MTGS depends only on its own event loop.

### 3.3 Plug-in points

All changes to PCSX2's own files are small and guarded, as `AGENTS.md` requires. "Step" is the plan
step (§15) that makes the change.

| Place | Change | Step |
|---|---|---|
| `pcsx2/GS/GS.h:16-24` | Add `RenderAPI::WebGPU`, at the end of the enum | 1 |
| `pcsx2/GS/GS.cpp:81-115` `GetAPIForRenderer` | Under `__EMSCRIPTEN__`: `SW` → `RenderAPI::WebGPU` if `WebGPUHost::IsAvailable()`, else `None` (the SW renderer presents through a GPU device, as on desktop). `Null` → `None`. From step 2 also `WebGPU` → `RenderAPI::WebGPU` and `Auto` → the preferred renderer. | 1, 2 |
| `pcsx2/GS/GS.cpp:117-185` `OpenGSDevice` | `case RenderAPI::WebGPU: g_gs_device.reset(MakeGSDeviceWebGPU());`, using the same factory pattern as Metal's `MakeGSDeviceMTL()` (`GS.cpp:133`). This keeps the backend's headers out of `GS.cpp`. | 1 |
| `pcsx2/GS/Renderers/Common/GSDevice.cpp:257-275` | `RenderAPIToString`: `CASE(WebGPU)` | 1 |
| `common/WindowInfo.h:12-19` | Add `Type::Web` (guarded): "present to the page through the frame port". It carries the surface size and scale from the page's size feed; `window_handle` is unused, because `WebGPUHost` creates the OffscreenCanvas itself (§12.1). | 1 |
| `common/Threading.h`, `common/Semaphore.cpp` | Under `__EMSCRIPTEN__`: the futex `KernelSemaphore` with `WaitSuspending()`, used by `WorkSema::WaitForWork` on threads that called `Threading::SetWaitsSuspend(true)` (§3.2) | 1 |
| `pcsx2/CMakeLists.txt` (EMSCRIPTEN branch) | Add the `GS/Renderers/WebGPU/*` sources, `--use-port=emdawnwebgpu` (compile, and link as an interface option) and `--js-library GS/Renderers/WebGPU/webgpu_host.js`, so both executables link them. | 1 |
| `pcsx2-wasm/CMakeLists.txt` (frontend owner) | `-sJSPI` (no export or import lists). No `-sOFFSCREENCANVAS_SUPPORT`. Add `shaders/webgpu` to `PCSX2_WASM_RESOURCES`, so the shaders are preloaded at `/resources/shaders/webgpu/` like the other resources (`BAKE_SHADERS_IN_CPP` stays off, and `cmake/ShaderToCpp.cmake` only accepts `.glsl`/`.fx`). `pcsx2-headless` links the port (the device is in the core library) but not JSPI, and never selects WebGPU, because Node has no `navigator.gpu`. | 1 |
| `pcsx2-wasm/Main.cpp` and `web/app.js` (frontend owners) | `Host::AcquireRenderWindow` returns `Type::Web` in WebGPU mode. A new lock-free export, `pcsx2web_surface_size(w, h, scale)`, feeds the canvas's backing size. The page gives its display canvas a `bitmaprenderer` context in WebGPU mode and defines `Module.tstFramePort(port)`, which receives the frame port once and then shows every posted bitmap (§12.1). The 2D blit and `pcsx2web_frame` stay for the Null path. A canvas keeps its first context type, so the page decides the mode before it creates a context, and the repeated-loss fallback (§13) replaces the element. | 1 |
| `pcsx2/Config.h:271-281` | Add `GSRendererType::WebGPU = 32`. The value stays clear of upstream's numbering (3, 11–15, 17) so that a future upstream renderer cannot take it. It is stored as an integer in the INI. | 2 |
| `pcsx2/Pcsx2Config.cpp:~696` | `GetRendererName`: `"WebGPU"` | 2 |
| `pcsx2/GS/GS.cpp:~640-690` `GSGetAdapterInfo` | `case GSRendererType::WebGPU`: return the cached adapter name and `maxTextureDimension2D`, so the UI can offer upscale multipliers | 2 |
| `pcsx2/GS/GSUtil.cpp:276-311` `GetPreferredRenderer` | `#elif defined(__EMSCRIPTEN__)`: WebGPU if available, else SW | 2 |
| `pcsx2/ImGui/FullscreenUI_Settings.cpp:2860-2900, 3028-3036` | Under `__EMSCRIPTEN__` the renderer list is WebGPU, Software, Null, and `is_hardware` includes WebGPU | 2 |
| `pcsx2/GS/Renderers/Common/GSRenderer.cpp:547-567` | Under `__EMSCRIPTEN__`, replace the two `pxFailRel` calls on repeated device loss with a host callback that switches to the SW renderer on GSDeviceNull (§13). Aborting would kill the page. | 3 |

### 3.4 Settings on the web

The frontend's base settings (`pcsx2-wasm/Main.cpp`) set these. There are no changes to core defaults.

| Key (`EmuCore/GS`) | Web value | Why |
|---|---|---|
| `Renderer` | Step 1: `13` (SW), which presents through GSDeviceWebGPU when WebGPU is available and through GSDeviceNull otherwise. From step 2: `32` (WebGPU) when `navigator.gpu` gives an adapter, else `13`. `?renderer=webgpu\|sw\|null` (new) overrides it for tests, and `?gpu=0` (new) keeps the SW renderer on GSDeviceNull for comparisons. | Selection |
| `HWDownloadMode` | `Unsynchronized` (proposal; open question 6) | EE FIFO reads come from GS local memory without a GPU round trip (`MTGS.cpp:283-297`). Each synchronous readback costs at least 2.3 ms (measured, §11). |
| `OverrideTextureBarriers` | `-1` (default). `0` turns the copy model off for comparison runs. | `multidraw_fb_copy = OverrideTextureBarriers != 0`, as in DX11 (`GSDevice11.cpp:710`) |
| `DepthFeedbackMode` | `Auto` → DS-as-RT. `Depth` → full depth copies. | §10.4 |
| `UpscaleMultiplier` | 1 by default; capped by `maxTextureDimension2D` and by the memory budget (§6.5) | Tab memory |
| `CASMode`, `FXAA` | Off until step 3 | Their shaders are not ported before then |
| `OsdShowGPU` | Off unless `timestamp-query` is present | `SetGPUTimingEnabled` returns false without it |

---

## 4. Device creation, features and limits

**Create sequence** (`GSDeviceWebGPU::Create`, after `GSDevice::Create`):

1. `wgpuCreateInstance` with `requiredFeatures = {TimedWaitAny}` (the default `timedWaitAnyMaxCount`,
   64, is enough; the spike set only the feature).
2. `wgpuInstanceRequestAdapter` with `powerPreference = HighPerformance` and core feature level (never
   compatibility mode, which has 0 vertex-stage storage buffers), then `Wait` (5 s). If the adapter is
   null, `SleepSuspending(500)` and retry, up to 10 times: in every headless run the first request
   answered null ("Failed to create WebGPU Context Provider") and the second succeeded (measured).
   Whether a windowed Chrome does this too is not known.
3. Read the adapter's features and limits. `wgpuAdapterRequestDevice` with: the wanted features that are
   available; `maxTextureDimension2D`, `maxBufferSize` and `maxStorageBufferBindingSize` raised to the
   adapter's values; `deviceLostCallbackInfo` and `uncapturedErrorCallbackInfo` in `AllowSpontaneous`
   mode. Then `Wait`.
4. Surface (for `WindowInfo::Type::Web`): `WebGPUHost::CreateCanvas(w, h)` creates an `OffscreenCanvas`
   in the MTGS worker and registers it in `specialHTMLTargets` under `"#tst-gs-canvas"`; then
   `wgpuInstanceCreateSurface` with that selector. The port resolves selectors through
   `findCanvasEventTarget`, which looks in `specialHTMLTargets` (verified,
   `library_webgpu.js:1984-2015`; measured working in the worker without `OFFSCREENCANVAS_SUPPORT`).
   Configure it with the surface's first capability format (`rgba8unorm` here, measured), `RenderAttachment`
   usage, alpha mode `Opaque` and present mode `Fifo` (the port asserts Fifo, verified
   `library_webgpu.js:2545-2547`). The first time, `WebGPUHost` also sends the page its frame port
   (§12.1).
5. `CheckFeatures()` (below), `CreateBuffers()`, `CreatePipelineLayouts()`, `CreateNullTextures()`,
   `CompileUtilityPipelines()` (all async, then wait for all), `CreateImGuiPipeline()`.
6. In dev builds, push a validation error scope around steps 5–6 and fail `Create()` if it reports an
   error.

**`FeatureSupport`** (`GSDevice.h:1399-1426`):

| Flag | Value | Reason, and the fallback the HW renderer already has |
|---|---|---|
| `broken_point_sampler` | 0 | That workaround is for an AMD GL driver issue |
| `vs_expand` | `!DisableVertexShaderExpand` | Read-only storage buffers in the vertex stage are core (spec) |
| `primitive_id` | `primitive-index && float32-blendable` | The PrimID DATE init blends r32float with `min` (`GSDevice11.cpp:3214-3217`). Without it, DATE uses stencil or full copies (`GSRendererHW.cpp:6073-6188`). |
| `texture_barrier` | 0 | Not in WebGPU |
| `multidraw_fb_copy` | `OverrideTextureBarriers != 0` | The DX11 copy model |
| `provoking_vertex_last` | 0 | WGSL flat interpolation takes the first vertex |
| `point_expand`, `line_expand` | 0 | 1 px only, so VS expansion is used when upscaling |
| `prefer_new_textures` | 1 | Reusing a texture already used in the open encoder forces a late upload or a pass split (§6.3), as on VK, DX12 and Metal |
| `dxt_textures`, `bptc_textures` | `texture-compression-bc` | Without it, replacement BC textures are rejected (`GSTextureReplacementLoaders.cpp:467-497`). There is no CPU decoder. |
| `framebuffer_fetch` | 0 | Not on the web (Dawn's experimental fetch extension is not exposed) |
| `stencil_buffer` | `depth32float-stencil8` | Else `depth32float`; DATE falls back as above |
| `cas_sharpening` | 0 until CAS is ported (step 3), then 1 | Checked at `GSRenderer.cpp:673`; needs a compute pass and rgba8unorm storage |
| `test_and_sample_depth` | 1 | Possible with a `depthReadOnly` pass (measured). Every switch splits the pass; measure, and set it to 0 if the splits cost more than copies. |
| `depth_feedback` | `multidraw_fb_copy && DepthFeedbackMode == Depth` | Otherwise DS-as-RT, the DX11 rule (`GSDevice11.cpp:720`) |
| `aa1` | `HWAA1 && vs_expand && feedback_loops()` | Same rule as every backend |
| `rov`, `copyless_ds_as_rt` | 0 | No fragment shader interlock |

**New flag, step 3:** `dual_source_blend`, proposed in PLAN M4 task 11. Until a fallback exists, the HW
renderer requires `dual-source-blending`, and `Create()` fails with a clear message for the WebGPU
renderer when it is missing. Every WebKit browser lacks it (PLAN §2). The SW-renderer device of step 1
does not need it.

**Optional WebGPU features requested** (all exist in the port's `WGPUFeatureName`, verified
`webgpu.h:411-435`): `Depth32FloatStencil8`, `TextureCompressionBC`, `Float32Filterable`,
`Float32Blendable`, `DualSourceBlending`, `PrimitiveIndex`, `TextureFormatsTier1`, `TimestampQuery`.

**Limits that matter** (spec defaults; the device requests the adapter's values when they are larger):

| Limit | Default | Use |
|---|---|---|
| `maxTextureDimension2D` | 8192 | `m_max_texture_size`, which drives the maximum upscale (`GS.cpp:200-216`) |
| `maxBindGroups` | 4 | TFX uses 3 (§8.1) |
| `minUniformBufferOffsetAlignment` | 256 | Uniform ring slots |
| `maxDynamicUniformBuffersPerPipelineLayout` | 8 | TFX uses 2, utility passes 1 |
| `maxStorageBufferBindingSize` | 128 MiB | Bound to the 32 MiB vertex ring |
| `maxColorAttachments` / `maxColorAttachmentBytesPerSample` | 8 / 32 | RT plus DS-as-RT is at most 8 + 4 bytes |
| `maxInterStageShaderVariables` | 16 | TFX passes 5 (t, ti, c, inv_cov, interior) |
| `maxVertexAttributes` | 16 | 7 used |

---

## 5. Formats

| `GSTexture::Format` (`GSTexture.h:49-66`) | WebGPU format | Requirement and notes |
|---|---|---|
| `Color` | `rgba8unorm` | Core: render, blend, filter, write-only storage (CAS, ShaderWrite) |
| `ColorHQ` | `rgb10a2unorm` | Core render and blend |
| `ColorHDR` | `rgba16float` | Core render, blend, filter |
| `ColorClip` | `rgba16unorm` with `texture-formats-tier1`, else `rgba16float` | Core has no `rgba16unorm`. The fallback is the one DX11 uses when RGBA16 UNORM cannot be blended (`GSDevice11.cpp:3118`). |
| `DepthStencil` | `depth32float-stencil8`, else `depth32float` | Sampled through a depth-only view as `unfilterable-float` with `textureLoad`. Partial copies are not allowed (§2). |
| `DepthColor` | `r32float` | Renderable. Filtering needs `float32-filterable`, blending `float32-blendable`; it is always bound as `unfilterable-float`. Write-only storage for ShaderWrite usage is core. |
| `UNorm8` | `r8unorm` | Palettes, OSD |
| `UInt16` | `r16uint` | Render target for 16-bit readback conversions; copy to buffer |
| `UInt32` | `r32uint` | The same for 24/32-bit |
| `PrimID` | `r32float` | The DATE init blend needs `float32-blendable` |
| `BC1`, `BC2`, `BC3`, `BC7` | `bc1-rgba-unorm`, `bc2-rgba-unorm`, `bc3-rgba-unorm`, `bc7-rgba-unorm` | `texture-compression-bc`. Level 0 must be a multiple of 4×4 (spec): reject replacements that are not. |
| present surface (the OffscreenCanvas) | the surface's first capability format, `getPreferredCanvasFormat()` (`rgba8unorm` here, measured; `bgra8unorm` on other platforms) | Only drawn into, never copied |

`LookupNativeFormat()` follows the Vulkan pattern (`GSDeviceVK.cpp:2948-2972`), with the substitutions
above resolved once in `CheckFeatures()`. `CheckFeatures()` also verifies each format's capabilities,
as Vulkan does (`GSDeviceVK.cpp:2847-2875`), and fails clearly if one is missing.

---

## 6. Resources

### 6.1 `GSTextureWebGPU`

- **Members:** `WGPUTexture m_texture`; `WGPUTextureView m_view` (all mips, colour or depth aspect, for
  sampling); `WGPUTextureView m_rt_view` (mip 0, for attachments); `std::vector<WGPUTextureView>
  m_mip_views` (created lazily for mip generation); `u64 m_use_serial` (the last encoder that referenced
  it); `u32 m_bytes` (for the budget).
- **Usage mapping:**
  - `Texture` → TextureBinding | CopyDst | CopySrc, plus RenderAttachment when `levels > 1` and the
    format is renderable (mip generation).
  - `RenderTarget` and `FeedbackTarget` → RenderAttachment | TextureBinding | CopySrc | CopyDst, plus
    StorageBinding for `Color` when `cas_sharpening` (`m_cas` is created as a RenderTarget,
    `GSDevice.cpp:1364`).
  - `DepthStencil` → RenderAttachment | TextureBinding | CopySrc | CopyDst.
  - `ShaderWrite*` → adds StorageBinding.
  
  WebGPU has no image layouts, so there is none of GSTextureVK's transition code.
- **`GetNativeHandle()`** returns `this`, as GSTextureVK does. ImGui uses it as `ImTextureID`
  (`GSDevice.cpp:568-645`, `GSDeviceVK.cpp:4780`).
- **Destruction.** If the texture was referenced by the encoder that is still open (`m_use_serial ==
  current`), push it to `m_pending_destroy`. That list is destroyed right after the next submit; calling
  `wgpuTextureDestroy` before the submit would make the submit invalid (spec). Otherwise call
  `wgpuTextureDestroy` and release immediately. WebGPU keeps resources that submitted work still uses
  alive (spec), so no fence tracking is needed, unlike VK's `DeferImageDestruction`.

### 6.2 Clear state

`GSDevice::ClearRenderTarget`/`ClearDepth` only record a clear (`GSDevice.cpp:518-526`). The device
honours it in three ways:

- **When the texture is next an attachment:** `loadOp = Clear` with the clear value. `Invalidated` also
  gets `Clear` (with 0), because WebGPU has no "don't care" load (spec). `Dirty` gets `Load`.
- **`CommitClear(t)`**, used before sampling or copying a cleared texture: open a pass that only clears
  it, then end it.
- **Clear carry-forward in `CopyRect`:** `GSDevice::ProcessClearsBeforeCopy` (`GSDevice.cpp:528-561`),
  as in DX11 and VK.

### 6.3 Uploads: `Update`, `Map`, `Unmap`

WebGPU has two upload mechanisms with different ordering, which correspond to Vulkan's two command
buffers:

- **Early upload:** `wgpuQueueWriteTexture`. It runs on the queue timeline immediately, before the
  encoder that is still being recorded (spec), so it plays the role of VK's "init command buffer". There
  is no `bytesPerRow` alignment rule (spec). The source is a view of the shared wasm memory, which the
  spec's `AllowSharedBufferSource` permits. **Allowed only if** the texture has not been referenced by
  the open encoder (`m_use_serial != current`) and has no pending late upload.
- **Late upload:** copy the rows into `m_upload_ring` with a 256-byte pitch (`writeBuffer` from a CPU
  shadow), then record `copyBufferToTexture` in the encoder. This ends any open pass. It plays the role of
  VK's in-command-buffer upload and is needed for render targets and for textures already used in this
  encoder (`GSTextureVK.cpp:352-363`).

`Map(rect, layer)` returns a pointer into a per-device CPU scratch buffer, with the pitch rounded to 256
bytes so that a late upload needs no second copy. `Unmap()` performs the upload. `Update()` with
`layer > 0` writes that mip level. Both mark `m_needs_mipmaps_generated`, as the base class expects.

### 6.4 Mipmaps

`GenerateMipmap()`: for each level n ≥ 1, run a render pass on `m_mip_views[n]` that draws the
`COPY` convert pipeline with a linear sampler from level n−1. WebGPU has no `generateMipmap` (spec). This
applies only to renderable formats; BC textures upload their mips.

### 6.5 Memory budget

WebGPU cannot report free video memory, and allocation failure arrives asynchronously. `CreateSurface`
therefore adds the texture's size to `m_gpu_bytes` and returns `nullptr` when that would exceed
`m_gpu_budget` (default 1.5 GiB on desktop and 512 MiB on mobile, configurable; estimate). The existing
`FetchSurface` path then purges the pool and retries (`GSDevice.cpp:722-737`), and the texture cache
treats a null result as a normal failure. In dev builds, an OutOfMemory error scope around texture
creation logs whatever the budget missed. `GSClampUpscaleMultiplier` (`GS.cpp:200-216`) also uses a
`m_max_texture_size` lowered so that two upscaled 1024×1024 targets fit in the budget.

### 6.6 Stream buffers

`WebGPUStreamBuffer` is a GPU buffer plus a same-size CPU shadow in the wasm heap.

- `ReserveMemory(size, align)` and `CommitMemory(size)` behave like VKStreamBuffer.
- The committed bytes are uploaded with **one** `wgpuQueueWriteBuffer` per ring, just before each
  submit. That is one JS call per frame rather than one per draw. Offsets and sizes are multiples of 4
  (spec); index data is padded.
- **Wrapping.** `queue.writeBuffer` is ordered after earlier submits on the queue timeline (spec), so
  work already submitted needs no fences. The only rule is that a region referenced by the *open* encoder
  must not be written twice. When a ring would wrap onto data the open encoder uses, the device submits
  first (`ExecuteCommandBuffer("ring wrap")`), as VK's `IASetVertexBuffer` does (`GSDeviceVK.cpp:3612-3627`).
- **Draws use base offsets, not rebinding.** The vertex ring is bound once per pass, and draws pass
  `baseVertex = m_vertex.start`. The index ring is bound once per pass, and draws pass
  `firstIndex = m_index.start`.

### 6.7 Samplers

`GetSampler(SamplerSelector)` is cached in a map with up to 256 keys, as in VK (`GSDeviceVK.cpp:3848-3873`):

- mag, min and mip filters from `IsMagFilterLinear()`, `IsMinFilterLinear()` and `IsMipFilterLinear()`;
- `tau`/`tav` → `Repeat`, else `ClampToEdge`;
- `lodMaxClamp = 0.25` when `lodclamp || !UseMipmapFiltering()`, else 32;
- `maxAnisotropy = 1` (anisotropic filtering is done in the shader, `ps.sw_aniso`).

Utility passes use point and linear samplers, plus a non-filtering point sampler for unfilterable
sources. `ClearSamplerCache()` submits, then drops the samplers and every bind group that references them.

---

## 7. Command recording

### 7.1 Encoder lifecycle

- `m_encoder` is created on first use after a submit. `m_encoder_serial` is incremented at each submit.
- **Submit** (`ExecuteCommandBuffer(reason)`): end the pass, flush the ring shadows (`writeBuffer`),
  `wgpuCommandEncoderFinish`, `wgpuQueueSubmit`, destroy `m_pending_destroy`, start async maps for
  readbacks recorded in this submit (§11.3), and invalidate the cached state.
- **When it submits:**
  - in `EndPresent` (once per frame);
  - in `GSDownloadTextureWebGPU::Flush` when the copy is still unsubmitted;
  - on a ring wrap;
  - in `ClearSamplerCache`;
  - in `Destroy`;
  - optionally every N passes to bound latency (measure).

### 7.2 Render-pass manager

This follows VK's `OMSetRenderTargets`/`BeginRenderPass`/`EndRenderPass` (`GSDeviceVK.cpp:3658-3846, 5635-5740`)
without the layout transitions.

- `OMSetRenderTargets(rt, ds_as_rt, ds, scissor, depth_read_only)` records the *wanted* attachments. If
  they differ from the open pass (views, read-only flag), the pass ends. The pass begins lazily at the
  next draw, so the load ops can come from texture state (§6.2) at that moment.
- **Pass description:**
  - colour 0 is the RT (or the colclip RT, or the PrimID image);
  - colour 1 is the DS-as-RT clone (r32float), when `ps.IsFeedbackLoopDepth()` and `!depth_feedback`;
  - the depth attachment has `depthReadOnly` when `test_and_sample_depth` is in use. Store ops are
    always `Store`.
  - The stencil load op is `Clear(0)` for `SetupDATE` and `Clear(1)` for StencilOne with a barrier,
    which DX11 does with `ClearDepthStencilView` (`GSDevice11.cpp:3276-3277`). Otherwise `Load`.
- **Avoid restarts, as VK and DX11 do** (`GSDeviceVK.cpp:6305-6326`, `GSDevice11.cpp:3227-3244`). When a
  draw needs only the RT and the open pass has RT+DS of the same size, keep the DS bound and set
  `PipelineSelector.ds = 1` (a pipeline with depth compare `Always` and no writes), and the reverse.
- **What ends a pass:**
  - any copy (`CopyRect`, feedback clones, `CopyFromTexture`, late uploads);
  - a change of attachment, of `depthReadOnly` or of the stencil load;
  - `CommitClear`;
  - mip generation;
  - submit.
- **Counters:** `GSPerfMon::RenderPasses` counts every begin and `TextureCopies` every copy, so
  feedback-heavy titles can be measured (PLAN M4 exit).

### 7.3 State cache

WebGPU resets all pass state at every `beginRenderPass` (spec): pipeline, bind groups, vertex and index
buffers, viewport, scissor, blend constant, stencil reference. The device keeps VK-style dirty flags
(`GSDeviceVK.h` `DIRTY_FLAG_*`) and sets every flag when a pass begins. `ApplyTFXState()` and
`ApplyUtilityState()` issue only what changed. The scissor is always clamped to the attachment, because
WebGPU validates it (spec; PLAN M4 task 4). The viewport is the full attachment size.

### 7.4 Debug markers

`PushDebugGroup` and `PopDebugGroup` map to pass or encoder debug groups only in dev builds. WebGPU
requires groups to be balanced within a pass (spec), and PCSX2's `GL_PUSH` scopes can span passes. The
device therefore keeps its own stack and re-pushes it whenever a pass begins.
`InsertDebugMessage` maps to `insertDebugMarker`.

---

## 8. Pipelines and bindings

### 8.1 TFX pipeline layout (step 2)

Three groups, out of the four allowed. Grouping by change frequency follows DX12's root signature
(`GSDevice12.cpp:2833-2852`), and the group numbers follow the utility layout (§8.2): constants in
group 0, source texture and sampler in group 1.

| Group | Binding | Resource (WGSL type) | Visibility | Changes |
|---|---|---|---|---|
| 0 draw | 0 | `uniform` VSConstantBuffer (48 B) — **dynamic offset** | VS | When `cb_vs` changes |
| | 1 | `uniform` PSConstantBuffer (272 B, verified from `GSDevice.h:1123-1142`) — **dynamic offset** | FS | When `cb_ps` changes |
| | 2 | `read-only-storage` vertex ring (GSVertex[]) | VS | Never (the whole ring is bound) |
| | 3 | `read-only-storage` expand index ring (AA1 triangles) | VS | Never |
| 1 source | 0 | `texture_2d<f32>` Texture (`float`, filterable) | FS | Per draw |
| | 1 | `texture_2d<f32>` Palette (`float`) | FS | Per draw |
| | 2 | `texture_2d<f32>` TextureRaw (`unfilterable-float`), for depth sources (`ps.depth_fmt` 1 or 2) and `DepthColor` sources, read with `textureLoad`. A depth view cannot be bound to a filterable float slot (PLAN M4 task 7). | FS | Per draw |
| | 3 | `sampler` (`filtering`, which also accepts point samplers) | FS | Per draw |
| 2 feedback | 0 | `texture_2d<f32>` RtTexture (`unfilterable-float`, read with `textureLoad`): the RT clone | FS | Feedback draws |
| | 1 | `texture_2d<f32>` PrimMinTexture (`unfilterable-float`, r32float) | FS | PrimID DATE |
| | 2 | `texture_2d<f32>` DepthTexture (`unfilterable-float`): the DS-as-RT clone or the depth clone | FS | Depth feedback |

- **One fixed layout for every TFX pipeline.** Each generated module declares only the bindings it
  uses; an explicit pipeline layout may contain more entries than the shader references (spec), so all
  TFX pipelines share `m_tfx_layout` and every cached bind group fits every pipeline. The shaders
  report proposed instead a few layout variants chosen by an "interface key" (texture needed,
  depth-format source, feedback kind, DATE class). That is rejected for now: WebGPU checks at each draw
  that every bound group matches the pipeline's layout, so variants would force rebinding on every
  variant switch and a layout in the bind-group cache key. Depth-format sources therefore use
  TextureRaw rather than a second Texture layout. Revisit only if a measurement shows a cost.
- **Sample types.** Every `float` format can also be bound as `unfilterable-float` (spec), and depth
  formats can be bound as `unfilterable-float` (spec; measured: a depth-only view sampled with
  `textureLoad`). A filtering sampler may coexist with unfilterable textures in one layout as long as
  the shader never pairs them in `textureSample*` (spec); TextureRaw and group 2 are only read with
  `textureLoad`, as `tfx.glsl` reads them with `texelFetch`.
- **Group 0 is static.** It is created with the rings and recreated only when they are. Each draw calls
  `setBindGroup(0, bg, 2 offsets)`, and only when an offset changed. The PS buffer takes two 256-byte
  units of the uniform ring. A buffer bound as `VERTEX` and as read-only storage in the same pass is
  valid, because both usages are reads (spec). 8 storage buffers are allowed in the vertex stage here
  (measured, core adapter).
- **Groups 1 and 2 come from `WebGPUBindGroupCache`,** keyed by the tuple of view pointers and the
  sampler pointer. Each entry also hangs off its textures, so a texture's destructor drops its entries.
  The cache is LRU with a cap of about 4096 entries. Unused slots get the 1×1 null textures. Every group
  in a layout must be set before a draw (spec), so non-feedback draws bind a cached null group 2 once per
  pass.
- **Hazard rule.** A texture that is an attachment of the current pass is never put in a bind group:
  sampling the bound render target in the same pass fails validation ("includes writable usage and
  another usage in the same synchronization scope", measured). `RenderHW` substitutes the clone
  (`TEX_HAZARD_RT/DEPTH`, `GSHWDrawConfig::tex_hazard`), as DX11's `FeedbackCopyAndBind` does. A depth
  attachment with `depthReadOnly` may be sampled in the same pass (measured).

### 8.2 Utility and compute layouts

The utility layout is the one the written shaders declare (`bin/resources/shaders/webgpu/*.wgsl`,
header comment of each file) and the one the validation page builds
(`tools/wasm/webgpu-shaders/validate.js`); 82 pipelines are created with it and drawn without a
validation error (measured).

| Group | Binding | Resource | Visibility | Layout entry |
|---|---|---|---|---|
| 0 constants | 0 | `uniform`, **dynamic offset**, bound with size 96 (`CONVERT_PUSH_CONSTANTS_SIZE`, `GSDeviceVK.h:387`) | VERTEX \| FRAGMENT | `m_utility_cb_bgl`, one for every utility pipeline |
| 1 source | 0 | `texture_2d<f32>` (`samp0_tex`; `Texture` in `imgui.wgsl`) | FRAGMENT | `float` in `m_utility_tex_bgl[Float]`, `unfilterable-float` in `m_utility_tex_bgl[Unfilterable]` |
| | 1 | `sampler` (`samp0`; `Sampler` in `imgui.wgsl`) | FRAGMENT | `filtering` with the float layout, `non-filtering` with the unfilterable one |

- **Group 0** replaces Vulkan's push constants. Each shader declares its own struct, laid out like what
  `GSDeviceVK` pushes, so the device can copy the same bytes: `DisplayConstantBuffer` (96 B) for
  present; `BGColor` (16 B) for merge, as VK pushes it (`GSDeviceVK.cpp:3444-3445`); `ZrH` (16 B) for
  interlace; `{EMODA, EMODC}` for YUV; the CLUT, indexed and downsample `Uniforms` structs
  (`GSDeviceVK.cpp:3367-3420`; field offsets checked against the WGSL); `uScale`/`uTranslate` for
  ImGui (VERTEX only, which the shared VERTEX | FRAGMENT entry covers). Shaders that need no constants
  still get group 0 bound, because the layout has it. Each draw takes one 256-byte unit of the uniform
  ring.
- **Group 1 kind.** The `[f]` entry points of `convert.wgsl` (`HasFloat32Input()`: depth and r32float
  sources; their list is in the file's header) use the unfilterable layout with a depth-only view or
  the r32float view; everything else uses the float layout.
- **Bind groups.** Group 0 is created once per uniform ring. Group 1 comes from a small cache keyed by
  (view, sampler); ImGui textures and the present source change every frame, so it is cheap to create
  and drop.
- **Vertex input:** `GSVertexPT1` for convert, present, merge, interlace and shadeboost; `ImDrawVert`
  for ImGui (§8.3).
- **CAS compute (step 3):** binding 0 a uniform (12 u32 of constants), binding 1 the source texture,
  binding 2 a write-only `rgba8unorm` storage texture.

### 8.3 Vertex input and VS expansion

Vertex attributes are those of VK (verified, `GSDeviceVK.cpp:5145-5154`):

| Struct | Stride | Attributes |
|---|---|---|
| `GSVertex` | 32 | ST `float32x2`@0, RGBA `uint8x4`@8, Q `float32`@12, XY `uint16x2`@16, Z `uint32`@20, UV `uint16x2`@24, FOG `unorm8x4`@28 |
| `GSVertexPT1` (utility) | 32 | pos `float32x4`@0, tex `float32x2`@16 |
| `ImDrawVert` | 20 | pos `float32x2`, uv `float32x2`, col `unorm8x4` |

**Expanded draws need no per-draw uniform.** Vulkan passes `BaseVertex` and `BaseIndex` in push
constants (`tfx.glsl:114-120`, `GSDeviceVK.cpp:2912-2927`). WebGPU allows a non-zero `firstInstance` on
direct draws in core (spec), and `@builtin(instance_index)` includes it, so:

- **Point, sprite and line expansion** (fixed `m_expand_ib`, or CPU-expanded line indices):
  `drawIndexed(count, 1, firstIndex, 0, firstInstance = m_vertex.start)`. The shader uses
  `vertex_buffer[instance_index + (vertex_index >> 2)]`, or the equivalent for each expansion shape.
- **AA1 triangles** (`UseVSExpandIndexBuffer`): `draw(count * 13, 1, firstVertex = (m_index.start +
  offset) * 13, firstInstance = m_vertex.start)`. The shader computes `index_buffer[vertex_index / 13]`
  and the corner `vertex_index % 13`.

If the spike finds that `instance_index` misbehaves in a browser, fall back to a third dynamic uniform
(`base_vertex`, `base_index`) in group 0.

### 8.4 Pipeline cache keyed by the existing selectors

```
struct PipelineSelectorWebGPU            // modelled on GSDeviceVK::PipelineSelector (GSDeviceVK.h:339)
{
	GSHWDrawConfig::PSSelector ps;       // 16 B, normalised: no_color1 when blending is not effective
	u32 topology : 2;                    // point, line, triangle
	u32 rt_format : 3;                   // none, Color, ColorClip (native format), PrimID, DepthColor
	u32 ds : 1;                          // depth attachment present (format fixed per device)
	u32 ds_as_rt : 1;                    // second colour target r32float
	u32 prim_init : 1;                   // DATE PrimID init pass (min blend, red only)
	GSHWDrawConfig::BlendState bs;
	GSHWDrawConfig::VSSelector vs;
	GSHWDrawConfig::DepthStencilSelector dss;
	GSHWDrawConfig::ColorMaskSelector cms;
};
```

**The descriptor**, translated from `GSDeviceVK::CreateTFXPipeline` (`GSDeviceVK.cpp:5087-5220`):

- **Depth:** compare is `{Never, Always, GreaterEqual, Greater}[ztst]`; `depthWriteEnabled = zwe`.
- **Stencil** when `dss.date`: compare `Equal`, `passOp = date_one ? Zero : Keep`, read and write masks 1.
  The reference (1) is set per pass.
- **Blend factors** `SRC_COLOR…CONST_ZERO` → `Src, OneMinusSrc, Dst, OneMinusDst, Src1, OneMinusSrc1,
  SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha, Src1Alpha, OneMinusSrc1Alpha, Constant,
  OneMinusConstant, One, Zero`. Ops are `Add`, `Subtract` and `ReverseSubtract`. The PrimID init uses
  `Min`, which WebGPU allows only with factors `One`/`One` (spec); DX11 already uses exactly that.
- **Write mask** = `cms.wrgba`. Blending is disabled when `!bs.IsEffective(cms)`, as in VK.
- **No vertex buffers** for expand pipelines.
- **Primitive:** cull none.
- **Blend constant:** `SetBlendConstants(u8)` sets the same normalised value as VK.

**Creation policy.** Avoid `createRenderPipeline` (synchronous): if it fails, the pipeline is invalid and
using it invalidates the whole command encoder, which loses the frame. Vulkan only skips the draw
(`GSDeviceVK.cpp:5233-5242`; PLAN M4 task 9). Instead, on a miss:

1. Generate the WGSL if the shader module is missing.
2. Call `createRenderPipelineAsync` (callback mode `WaitAnyOnly`) and store the future.
3. Poll it with `Wait(future, 0)`.
4. If it is not ready, **wait with JSPI** (up to 2 s). This hitches, like desktop PCSX2's synchronous
   compiles, but stays correct.
5. A failed pipeline is cached as null, and its draws are skipped with one log line (in dev builds, the
   WGSL and `GetCompilationInfo` go to the console).

Skipping the draw instead of waiting can be offered as an option ("async shader compilation").

**Other pipelines** are keyed the same way:

- convert pipelines by `ShaderConvertSelector::Index()` and output format;
- present pipelines by (`PresentShader`, target format);
- ImGui pipelines by canvas format.

Each `ShaderConvertSelector` gives its output kind: `OutputFormat()`, `DepthOutput()` (frag_depth with a
depth attachment), `StencilOutput()` (DATM: no colour target, stencil op `Replace`, compare `Always`,
fragments discarded where alpha fails) and `IntegerOutputBpp()` (`u32` output to r16uint or r32uint).
That is the same split VK makes (`GSDeviceVK.cpp:4192-4368`).

---

## 9. Shaders

### 9.1 Strategy: hand-written WGSL; a C++ preprocessor for TFX only

PLAN §4 proposed offline translation for the utility shaders (glslang → SPIR-V → Tint or naga) and a
hand-maintained template for TFX. The decision is **hand-written WGSL for everything**, and it is
already applied to the utility shaders. The translators were evaluated in a scratch folder, nothing
installed (glslang 16.6.0 and Tint from Dawn v20260423.175430, both official release assets with
matching SHA-256 digests; naga-wasm 30.2.0 from npm):

- glslang + Tint translated 22 of 24 tested entry points, but the output is not usable as source:
  push constants become `var<immediate>` (Chrome-only, not core WebGPU), Vulkan's Y flip stays, each
  entry point becomes its own module, the code is machine-style, and the Lottes CRT filter fails the
  uniformity check. Using it at build time would add a WebGPU define to PCSX2's own GLSL (a merge
  conflict on every upstream update) and two Linux-only native tools (about 37 MB) to a build that
  otherwise needs only emsdk.
- naga-wasm's GLSL reader rejects `sampler2D`, which every PCSX2 Vulkan shader uses (5 of 7 tested
  shaders failed). naga itself needs cargo, which this machine does not have.
- The utility GLSL needs WebGPU-specific edits anyway: no push constants, separate texture and sampler,
  no Y flip (WebGPU uses D3D-style clip space; measured: the copy is not flipped), integer and depth
  outputs. PCSX2 itself hand-maintains these shaders in GLSL, HLSL and Metal; upstream changed the five
  utility files 10 times in 12 months, mostly by 1–26 lines.
- The texts stay reviewable next to `vulkan/*.glsl`: each file keeps the GLSL's order, names and
  comments and records the upstream commit it was synced with, and
  `tools/wasm/webgpu-shaders/check-upstream.mjs` lists the upstream commits not yet ported. Tint and
  glslang remain useful as porting references only.

**Utility shaders need no preprocessor.** Where the GLSL is compiled once per selector with `HAS_*`
macros, the WGSL files instead have one entry point per output kind, picked by name, and
pipeline-overridable constants (`override`): `HAS_BILN` for `ShaderConvertSelector::Biln()`, and
`PRIMID_MAX`/`PRIMID_MIN`, whose defaults match `GSShaderEnums.h`. One module per file is created once;
a pipeline is (module, entry point, constants, target format, blend). The preprocessor below is needed
only for TFX, in step 2.

**`WGSLPreprocessor`** (estimate: ~400 lines):

- Object-like `#define NAME <integer>` (from the macro list or in the file).
- `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`, with integer expressions (`defined()`, `!`,
  `&&`, `||`, comparisons, `+ - * / & | << >>`).
- No function-like macros and no token pasting.
- Macro names used *in code* are not substituted. Instead the generator emits `const PS_X: u32 = N;` at
  the top of the module, and the code reads those constants. `#if` is used wherever the choice changes
  bindings, entry-point interfaces or types, because WGSL type-checks dead `const` branches and counts
  the resources they reference as statically used.

**Macro lists.** They are copied from `GSDeviceVK::GetTFXVertexShader` and `GetTFXFragmentShader`:
6 VS macros and 62 PS macros (`GSDeviceVK.cpp:4981-5085`). Add WebGPU-only ones:

- `PS_DEPTH_FEEDBACK_SUPPORT` = 2 (DS-as-RT) or 1 (copy), from DX11 (`GSDevice11.cpp:2051`);
- `HAS_DUAL_SOURCE`;
- `HAS_PRIMITIVE_INDEX`.

A unit test (`tests/ctest/`) compares the PS macro names in `tfx.wgsl` with those in `vulkan/tfx.glsl`
(82 distinct `PS_` identifiers today, counted), so an upstream merge that adds a selector bit fails
loudly instead of silently mis-rendering.

### 9.2 Translating `tfx` to WGSL

Port the structure of `vulkan/tfx.glsl` (1990 lines). Where the two differ, follow `dx11/tfx.fx`
(2039 lines) for clip space and for the DS-as-RT output struct (`tfx.fx:195-229`). The rules:

- Types: `vec4` → `vec4f`, `uvec4` → `vec4u`, and so on. Conversions are always explicit. `f32` → `u32`
  saturates in WGSL (spec); GLSL leaves negative input undefined.
- There is no multi-component swizzle assignment (`C.rgb = x` is not WGSL). Write
  `C = vec4f(x, C.a)`. There is no `?:`; use `select(f, t, cond)`. There are no `out`/`inout`
  parameters; use `ptr<function, T>`.
- `texture(Texture, uv)` → `textureSample`; `textureLod` → `textureSampleLevel`; `texelFetch` →
  `textureLoad`; `dFdx`/`dFdy` → `dpdx`/`dpdy`. The module starts with
  `diagnostic(off, derivative_uniformity);`, because tfx samples inside non-uniform control flow
  (anisotropy loops, discards). Without it Chrome rejects such a module; with it the module compiles
  (measured, feature probe). The results there are as undefined as in GLSL, and the other backends
  already live with that. Also emit `enable dual_source_blending;` and `enable primitive_index;` when
  the selector uses them (both measured working here), and write `DISCARD`/`DISCARD_COLOR`/`DISCARD_DEPTH`
  as inline WGSL, since their ROV forms are compiled out.
- Built-ins: `gl_FragCoord` → `@builtin(position)`; `gl_FragDepth` → `@builtin(frag_depth)`
  (WebGPU has no conservative depth, so `depth_less` is dropped); `gl_VertexIndex` →
  `@builtin(vertex_index)`; `gl_PrimitiveID` → `@builtin(primitive_index)` behind
  `enable primitive_index;`.
- `flat` → `@interpolate(flat)` (first vertex).
- Dual-source output: `enable dual_source_blending;` with `@location(0) @blend_src(0)` and
  `@blend_src(1)`. It is valid only with exactly one colour target (spec), so DS-as-RT draws must have
  `no_color1`; the HW renderer already asserts that they do not blend (`GSRendererHW.cpp:9658-9667`).
- ROV paths (`PS_ROV_*`) are compiled out (`rov = 0`).

### 9.3 Utility shader inventory

Status on 2026-10-16. "Validated" means: the module compiles with no error or warning, and each listed
pipeline is created with the explicit layout of §8.2 and drawn once without a validation error, on the
hardware adapter and on SwiftShader, and offline through Tint (measured, `validate-shaders.mjs`).

| File | Entry points | GLSL lines (vulkan/) | Status | Step |
|---|---|---|---|---|
| `convert.wgsl` | VS + 37 fragment: the 29 `ShaderConvert` values (`DEPTH32_TO_16_BITS` shares `ps_convert_depth32_32bits`, `DEPTH32_TO_RGB8` shares `ps_convert_depth32_rgba8` with write mask 0x7), a `_ds` twin writing `frag_depth` for each of the 7 that can output depth, and `ps_primid_image_init_0..3` | 664 | Written, validated; pixel checks: copy (no flip), rgba8 → depth24 → rgb8 through r32float and through `frag_depth`, RGB5A1 packing, YUV luma | 1 (COPY, TRANSPARENCY_FILTER, YUV used); 2 (the rest used) |
| `present.wgsl` | VS + 8 (`PresentShader`) | 446 | Written, validated (the Lottes `Fetch` samples first and then selects, for the uniformity rule) | 1 |
| `merge.wgsl` | VS + 2 (`ps_main0/1`, by `PMODE.MMOD`) | 46 | Written, validated; pixel check: alpha from BGColor | 1 |
| `interlace.wgsl` | VS + 5 (weave, bob, blend, MAD buffer, MAD reconstruct) | 227 | Written, validated; pixel check: bob | 1 |
| `imgui.wgsl` | VS + FS | 42 | Written, validated; pixel check: orientation with `uScale = (2/w, −2/h)`, `uTranslate = (−1, 1)` | 1 |
| `shadeboost.wgsl` | 1 | 77 | Written, validated; pixel checks in `validate.js`, and in the device within 1 LSB of the formula | 1 |
| `fxaa.wgsl` | 1 | 498 (`common/fxaa.fx`) | Not written | 3 |
| `cas.wgsl` | 2 compute | 60 + the parts of `ffx_cas.h`/`ffx_a.h` (3.9k) that are used | Not written | 3 |
| `tfx.wgsl` | VS + FS | 1990 | Not written | 2 (whole template; BIOS paths validated), 3 (all paths) |

- **Choosing a convert pipeline:** entry point `ShaderEntryPoint(shader) + (selector.DepthOutput() ?
  "_ds" : "")`, constant `HAS_BILN = selector.Biln()`, the group-1 layout from `Float32Input()`, and the
  target from `OutputFormat()`/`IntegerOutputBpp()`/`StencilOutput()` (§8.4). The DATM entry points
  have no colour output and are used with a stencil-only pass.
- **Not yet proven by pixels:** the 8 present filters, the depth-copy, colclip, RTA, DATM, CLUT,
  indexed and downsample paths compile and draw, but their output has not been compared with Vulkan's.
  The uniform layouts of CLUT, indexed and downsample were checked field by field against
  `GSDeviceVK.cpp:3367-3420`. Step 1 compares present and interlace output with GSDeviceNull's (§15);
  step 2's census covers the convert paths.
- **The checks catch faults:** a deliberately broken set (a uniformity error, a wrong output type, a
  uniform struct larger than the binding) failed at module, pipeline and draw time respectively
  (measured).

### 9.4 Compilation and caching

- **Utility modules** are created once per device from the preloaded files
  (`/resources/shaders/webgpu/*.wgsl`, read with `GSDevice::ReadShaderSource` like every backend,
  §3.3); their pipelines are created in `Create()` with `createRenderPipelineAsync` and waited for
  together. Their creation time is not measured yet (step 1 logs it).
- **TFX** (step 2): WGSL generation (string assembly plus preprocessing) runs in wasm, on MTGS.
  Estimate: under 1 ms per selector. Modules are cached by `VSSelector.key` and by normalised
  `PSSelector`. Measuring module creation against pipeline compilation on real GS dumps is the first
  TFX task; if parsing dominates, value-only `#if` blocks can become WGSL `if` statements on override
  constants (the shaders report's alternative).
- Chrome caches compiled shaders and pipelines on disk, keyed by source and state, so a second session is
  faster with no work from us (spec of the implementation; measure).
- **Prewarm** (PLAN M5 task 6): persist the `PipelineSelectorWebGPU` keys used per game serial (OPFS or
  IndexedDB, through the frontend), and at boot issue `createRenderPipelineAsync` for them without
  waiting.

---

## 10. Every `GSDevice` virtual, mapped to WebGPU

Virtuals are listed in `GSDevice.h:1509-1719`. "Step" is the plan step (§15) that implements each one.

| Virtual | WebGPU implementation | Step |
|---|---|---|
| `Create(vsync, throttle)` | §4 sequence. Returns false (and `OpenGSDevice` fails cleanly) on no adapter or a missing required format. | 1 |
| `Destroy()` | `GSDevice::Destroy()`, submit, unconfigure the surface, release every object, `wgpuDeviceDestroy` (its lost callback runs synchronously with reason `Destroyed`, measured; ignore it), drop the OffscreenCanvas, `Threading::SetWaitsSuspend(false)` | 1 |
| `GetRenderAPI()` | `RenderAPI::WebGPU` | 1 |
| `HasSurface()` | The surface is configured | 1 |
| `DestroySurface()` | `wgpuSurfaceUnconfigure` + release | 1 |
| `UpdateWindow()` | `AcquireWindow(false)`, then a new OffscreenCanvas and surface (§12.1) | 1 |
| `ResizeWindow(w, h, scale)` | Set the OffscreenCanvas's `width`/`height` from the worker and reconfigure the surface. The page's size feed (§3.3) triggers it from `BeginPresent`. | 1 |
| `SupportsExclusiveFullscreen()` | false | 1 |
| `BeginPresent(frame_skip)` | §12.2: end the pass; `DeviceLost` if the lost flag is set (the only reliable test: on a lost device `getCurrentTexture` still reports success, measured); on frame skip, submit and return `FrameSkipped`; `wgpuSurfaceGetCurrentTexture` (reconfigure on `Outdated`/`Lost`, skip on `Timeout`); begin the present pass with `Clear` to black | 1 |
| `EndPresent()` | `RenderImGui()`, end the pass, submit, **no** `wgpuSurfacePresent`, `WebGPUHost::PostFrame()` (`transferToImageBitmap()` and a post on the frame port; no yield), the bounded event-loop turn if MTGS has not idled for 100 ms (§3.2), invalidate state | 1 |
| `SetVSyncMode(mode, throttle)` | Store only. The page shows each posted bitmap at its next rendering update; the frame limiter paces emulation. | 1 |
| `GetDriverInfo()` | `wgpuAdapterGetInfo` (vendor, architecture, device, description, backend type), plus features and key limits | 1 |
| `SetGPUTimingEnabled` / `GetAndResetAccumulatedGPUTime` | With `timestamp-query`: pass `timestampWrites` on the first and last pass of a frame, `resolveQuerySet` into a buffer, read it asynchronously one or two frames later (no stall). Browsers quantise timestamps (spec). Without the feature: `false` / `0`. | 3 |
| `SetGPUPipelineStatisticsEnabled` / `GetAndResetAccumulatedGPUPipelineStatistics` | `false` / `{}`: WebGPU has no pipeline statistics | 1 |
| `PushDebugGroup` / `PopDebugGroup` / `InsertDebugMessage` | §7.4 (dev builds) | 1 |
| `CreateSurface(usage, w, h, levels, fmt)` | `GSTextureWebGPU::Create` (§6.1) with the budget check (§6.5) | 1 |
| `CreateDownloadTexture(w, h, fmt)` | `GSDownloadTextureWebGPU::Create` (§11.2) | 1 (used for screenshots), 3 (hardened) |
| `CopyRect(s, d, r, x, y)` | Clear carry-forward (`ProcessClearsBeforeCopy`); `CommitClear` of the source; commit the destination clear when the copy is partial; end the pass; `copyTextureToTexture`. **Depth formats:** full copy when the sizes match, otherwise a `DEPTH_COPY` convert draw into the destination region, which is more general than DX11's early return (`GSDevice11.cpp:1557-1562`). **Same texture:** copy through a temporary texture. Counts `TextureCopies`. | 1 (colour), 2 (depth) |
| `DoStretchRect(s, sRect, d, dRect, ShaderConvertSelector, filter)` | Utility draw: 4 `GSVertexPT1` as a strip; convert pipeline `Index()`; pass on `d` (colour, depth or stencil per selector, §8.4); `Load`, or `Clear` when `dRect` covers a cleared or invalidated target (VK's `BeginRenderPassForStretchRect`, `GSDeviceVK.cpp:3228-3267`) | 1 |
| `DoStretchRect(s, sRect, dRect, PresentShader, filter)` | Present pipeline into the open present pass | 1 |
| `PresentRect(s, sRect, d, dRect, shader, time, filter)` | `d == nullptr`: the OffscreenCanvas's current texture (above), with `DisplayConstantBuffer` in the utility uniform. Otherwise a `DoStretchRect` with the present pipeline for `d`'s format. | 1 |
| `DrawMultiStretchRects(rects, n, d, shader)` | One draw per run of rects with the same source and filter, as VK's `DoMultiStretchRects` (`GSDeviceVK.cpp:3110-3226`) | 1 |
| `DoMerge(sTex[3], sRect, d, dRect, PMODE, EXTBUF, c, filter)` | A copy of VK's pass sequence (`GSDeviceVK.cpp:3438-3557`): clear to the background colour, copy circuit 2, YUV feedback write to `sTex[2]`, blend circuit 1 with `m_merge[MMOD]`, `CommitClear(d)` | 1 |
| `DoInterlace(s, sRect, d, dRect, shader, filter, cb)` | `m_interlace[shader]` with `InterlaceConstantBuffer`. MAD uses the two-bank buffer exactly as the GPU backends do. | 1 |
| `DoShadeBoost(s, d, params)` | `m_shadeboost` with a 16-byte uniform; `shadeboost.wgsl` is still to write | 1 |
| `DoFXAA(s, d)` | `m_fxaa` once `fxaa.wgsl` exists; until then a plain copy, with FXAA off by default | 3 |
| `DoCAS(s, d, sharpen_only, consts)` | Compute pass with `m_cas[sharpen_only]`, a storage-texture destination, dispatch over 16×16 tiles as in VK (`GSDeviceVK.cpp:4820-4844`). Returns false until ported (`cas_sharpening = 0`). | 3 |
| `UpdateCLUTTexture(s, scale, x, y, d, off, size)` | `CLUT_4`/`CLUT_8` convert with its uniform (VK `GSDeviceVK.cpp:3363-3385`) | 2 |
| `ConvertToIndexedTexture(...)` | `RGBA_TO_8I`/`RGB5A1_TO_8I` with its uniform (`GSDeviceVK.cpp:3387-3411`) | 2 |
| `FilteredDownsampleTexture(...)` | `DOWNSAMPLE_COPY` with its uniform (`GSDeviceVK.cpp:3413-3436`) | 2 |
| `RenderHW(config)` | §10.1 | 2 (core), 3 (feedback, DATE, colclip, DS-as-RT, AA1) |
| `ClearSamplerCache()` | Submit, drop samplers and dependent bind groups | 2 |
| `BeginDSAsRT(ds, area)` (non-pure) | Keep the base implementation (`GSDevice.cpp:1234-1301`). It uses `DrawMultiStretchRects(DEPTH_COPY)` into an r32float feedback target. | 3 |
| **GSTexture** `GetNativeHandle()` | `this` | 1 |
| `Update(r, data, pitch, layer)` | §6.3 | 1 |
| `Map(m, r, layer)` / `Unmap()` | §6.3 | 1 |
| `GenerateMipmap()` | §6.4 | 2 |
| `SetDebugName(name)` | `wgpuTextureSetLabel` (dev builds) | 1 |
| **GSDownloadTexture** `CopyFromTexture` / `Map` / `Unmap` / `Flush` / `SetDebugName` | §11.2 | 1, 3 |

The non-virtual helpers in `GSDevice` (`StretchRect*`, `Merge`, `Interlace`, `ShadeBoost`, `FXAA`, `CAS`,
`FetchSurface`, `Recycle`, pool ageing, ImGui texture management) are used unchanged.

### 10.1 `RenderHW`

The order is DX11's `GSDevice11::RenderHW` (`GSDevice11.cpp:3072-3326`), with VK's pass-reuse logic:

1. **Colclip** (`ColClipMode`): `EarlyResolve` blits the colclip RT back with `COLCLIP_RESOLVE`.
   Otherwise create a `ColorClip` feedback target (native or fallback format, §5), initialise it with
   `COLCLIP_INIT` over `drawarea` (or the full RT for `ConvertOnly`), and draw into it. Resolve it after
   the draws for `ResolveOnly`/`ConvertAndResolve`.
2. **DATE setup** (§10.3).
3. **Upload** the vertices and indices to the rings. Expand draws use the fixed expand IB or the VS
   index ring.
4. **Pipeline:** build `PipelineSelectorWebGPU` and get it or create it (§8.4). A null pipeline skips
   the draw.
5. **Feedback clones** (§10.2) when `(require_one_barrier || (require_full_barrier &&
   multidraw_fb_copy))` and the PS reads the RT or depth.
6. **Attachments:** `OMSetRenderTargets(draw_rt, ds_as_rt, draw_ds, …, depth_read_only)`, where
   `depth_read_only = tex == ds && test_and_sample_depth && !zwe && !feedback`, as DX11's
   `read_only_dsv` (`GSDevice11.cpp:3201-3207`).
7. **Bind groups:** group 1 (texture, palette, raw texture, sampler), group 2 (clone, PrimID, depth
   clone) or the null group.
8. **`SendHWDraw`:** a single draw, a one-barrier copy then the draw, or the full-barrier loop (§10.2).
9. **Blend multipass and alpha second pass:** the same pass with a new pipeline (and the PS uniform for
   AREF), as DX11 does (`GSDevice11.cpp:3282-3305`).
10. **Colclip resolve,** then recycle the temporary textures.

### 10.2 Feedback through copies (no texture barriers)

- **One barrier.** End the pass; `copyTextureToTexture(rt → rt_clone)` over `ProcessCopyArea(drawarea ∪
  samplearea)` or the two areas separately, whichever is smaller (`GSDevice11.cpp:3348-3378`). Bind the
  clone in group 2 (and as group 1 `Texture` for `TEX_HAZARD_RT`). Resume the pass with `Load`. The DS
  clone works the same way, from the DS-as-RT texture or, in Depth mode, a full depth copy.
- **Full barrier.** For each `drawlist` entry: end the pass, copy that entry's `drawlist_bbox ∩
  drawarea`, resume, rebind all state, draw the sub-range (`GSDevice11.cpp:3380-3425`).
  `ComputeDrawlistGetSize` saves the boxes because `!texture_barrier && multidraw_fb_copy`
  (`GSRendererHW.cpp:11232-11240`).
- **Cost.** One full-barrier draw with n primitive groups costs n copies plus n pass restarts. Each
  restart re-sends the pipeline, the bind groups, the buffers, the viewport and the scissor, because pass
  state does not survive a restart (spec). This is the main GPU-side performance risk (§16).
  Mitigations:
  1. Prefer the stencil DATE paths (§10.3).
  2. Copy only the bounding boxes, as now.
  3. Measure `RenderPasses` and `TextureCopies` per frame on feedback-heavy titles.
  4. Later, coalesce entries whose boxes do not touch pixels drawn since the last copy.

### 10.3 DATE

Destination alpha test modes and how WebGPU supports each (selection in
`GSRendererHW::EmulateDATESelectMethod`, `GSRendererHW.cpp:6073-6188`):

| Mode | WebGPU | Needs |
|---|---|---|
| `Stencil` | `SetupDATE`: a pass on the DS with stencil `Clear(0)` and depth `Load`, drawing the `DATM_*` convert pipeline (stencil `Replace` where alpha passes). Then the main draw with stencil compare `Equal` against reference 1. | `depth32float-stencil8` |
| `StencilOne` | Without a barrier: as `Stencil`. With a barrier: the main pass starts with stencil `Clear(1)` and `passOp Zero`, which forces a new pass. | `depth32float-stencil8` |
| `PrimIDTracking` | PrimID init pass into an r32float target with blend `Min` (`m_primid_init`), then the main draw reads `PrimMinTexture` and compares `primitive_index` | `primitive-index` + `float32-blendable` |
| `Full` | `ps.date = 5 + DATM`: reads the RT clone; full barrier, so per-group copies | nothing (slow) |

On a typical desktop adapter with `depth32float-stencil8`, most DATE draws take the cheap stencil paths.
Adapters without it fall back to `Full`, which is correct but slow.

### 10.4 Depth: DS-as-RT, read-only sampling, clipping

- **DS-as-RT** (`depth_feedback = 0`). When the PS reads and writes depth
  (`IsFeedbackLoopDepth`), `GSRendererHW` calls `BeginDSAsRT` (`GSRendererHW.cpp:9654-9668`). The pass
  then has a second colour target, the r32float clone, which the shader writes together with
  `frag_depth`. The clone's copies (`draw_ds_clone`) are colour copies, so they can be partial.
- **Depth mode** (`DepthFeedbackMode = Depth`, `depth_feedback = 1`): the DS clone is a full
  `depth32float(-stencil8)` copy every time. Simpler shaders, more bandwidth; measure both.
- **Sampling the bound depth while testing** uses a `depthReadOnly` pass with a depth-only view in group
  1 binding 2 (measured: a depth-only view sampled with `textureLoad` while the same texture is the
  read-only depth attachment validates and draws). With `depth32float-stencil8`, the stencil aspect may
  stay writable in the same pass (separate aspects, spec; **verify in step 2**, the probe did not test
  it).
- **Clipping.** WebGPU clips z to [0, 1] (spec). PCSX2's z is `Z * 2^-32` and never leaves that range;
  VK and Metal also clip, so `unclippedDepth` (`depth-clip-control`) is not needed.

### 10.5 Blending without dual-source blending (later)

Seventeen of the 81 `m_blendMap` entries use SRC1 factors (`GSDevice.cpp:2083-2165`, counted in PLAN
§2), plus `AFAIL_RGB_ONLY_DSB` and PABE. The fallback is new upstream-facing logic: with
`dual_source_blend = 0`, `EmulateBlending` routes those cases to SW blending (`blend_a/b/d` in the
shader, reading the RT clone). That needs `require_one_barrier` or `require_full_barrier`, and so
copies. It is the existing `UseDebugBlend` path (`GSRendererHW.cpp:7173-7193`) made automatic for those
entries. It is correct but slow, and it is the only route to Safari and iOS. It is out of steps 1–3 and
would be a separate task.

---

## 11. Readbacks and synchronisation

### 11.1 Call sites that need GPU results synchronously

| Call site | Purpose | Gated by `HWDownloadMode`? | Strategy |
|---|---|---|---|
| `MTGS::InitAndReadFIFO` → `GSTextureCache::InvalidateLocalMem` → `Read(Target)` (`MTGS.cpp:283-297`, `GSTextureCache.cpp:4982-5115, 5262-5290, 7406-7529`) | The EE reads GS memory (local→host transfer) | Yes. `Unsynchronized` copies from GS local memory with no GPU access; `NoReadbacks`/`Disabled` skip it. Depth readbacks are skipped above `EnabledForceFull` (`:5002, :5272`). | Web default `Unsynchronized` (open question 6). With `Enabled`: synchronous `Flush` under JSPI while the EE waits in `WaitGS`. |
| `GSRendererHW::InvalidateLocalMem` for local→local transfers whose source is a target (`GSRendererHW.cpp:2311-2340`) | Keep local memory coherent for CPU-side work | Partly | Synchronous |
| `SwSpriteRender` (`GSRendererHW.cpp:2490-2506`; 318 `cpuSpriteRenderBW` GameDB entries, PLAN §2) and CPU CLUT render (53 `cpuCLUTRender` entries) | The CPU draws from target data | No | Synchronous. These titles will stall; measure them. |
| A PSMT4/PSMT8H target used as a source (`GSTextureCache.cpp:1750-1765`) | No GPU shader exists for these formats | No | Synchronous |
| A target used as a source with a format mismatch (`GSRendererHW.cpp:9960-9970`); a texture copied back (`:10720-10730`) | Correctness hacks | No | Synchronous |
| `GSRendererHW::Reset` → `ReadbackAll` (`GSRendererHW.cpp:80-90`); `GSreopen` with `ReadTCOnClose` (`GS.cpp:272-275`) | CSR reset, renderer switch | No / a setting | Synchronous (rare) |
| `GSRenderer::SaveSnapshotToMemory` (`GSRenderer.cpp:1061-1130`) | Screenshots, save-state thumbnails | No | Synchronous (user action). Could become deferred like capture later. |
| `GSTexture::Save` (`GSTexture.cpp:47-90`) | Debug dumps | No | Synchronous |
| `GSCapture` (`GSCapture.cpp:880-930`) | Video capture, three frames behind | — | Already deferred. Not built on the web (`GSCaptureNull`). |

Every synchronous readback is a full round trip: submit, the GPU finishes all queued work, the map
completes, MTGS resumes. Measured with `copyTextureToBuffer`, `mapAsync` and `WaitAny` (2 s timeout)
on an otherwise idle queue: 2.3–2.5 ms median (p90 2.5–2.7 ms) for 256×256 RGBA8 and 2.8–3.0 ms (worst
8.7 ms) for 1024×1024, with correct data in all 280 maps. Waiting through callbacks and yields took
the same time, so the floor is the browser's GPU-process round trip, not the wait. Desktop PCSX2 pays
less (Vulkan even spins on a timestamp queue to shave it, `ReadbackSpinManager`). A title that reads
back several times per frame loses about 2.5 ms each time; hence `Unsynchronized` as the web default
(§3.4) and the measurement of the ungated call sites above in step 3.

### 11.2 `GSDownloadTextureWebGPU`

- **`Create`:** a `WGPUBuffer` with usage MapRead | CopyDst and size
  `GetBufferSize(w, h, fmt, 256)`, plus a CPU shadow of the same size in the wasm heap.
- **`CopyFromTexture(drc, tex, src, level, use_transfer_pitch)`:** commit the texture's pending clear;
  end the pass; `copyTextureToBuffer` with `bytesPerRow = GetTransferPitch(use_transfer_pitch ?
  drc.width : m_width, 256)` (256-byte rule, spec; DX12 code `GSTexture12.cpp:1212-1268`). The copied
  aspect is colour; depth targets are converted to UInt32/UInt16 first by the texture cache
  (`GSTextureCache.cpp:7421-7489`). Record `m_copy_serial = current encoder serial` and set
  `m_needs_flush`.
- **`Flush()`:**
  1. Return if `!m_needs_flush`.
  2. If `m_copy_serial` is still the open encoder, `ExecuteCommandBuffer("readback")`.
  3. If no map is in flight, call `wgpuBufferMapAsync(Read, 0, size)`.
  4. `WebGPUHost::Wait(map_future, 2 s)`. A timeout or failure means the device was lost: set the
     lost flag and leave the shadow untouched.
  5. `wgpuBufferReadMappedRange(buffer, 0, shadow, size)`. The port copies straight into the wasm
     pointer (verified, `library_webgpu.js:1064-1078`; the spike used `GetConstMappedRange`, so this
     call's cost is not measured). Then `wgpuBufferUnmap`, so the buffer can be reused at once.
  6. Clear `m_needs_flush`.
- **`Map(rc)`:** flush if needed, then `m_map_pointer = shadow`. **`Unmap()`:** `m_map_pointer =
  nullptr`; nothing is held mapped, because WebGPU forbids using a mapped buffer in a copy (spec).
- **Do not use `GetConstMappedRange`.** In the port it allocates a new heap copy on every call and
  frees it at unmap (verified, `library_webgpu.js:992-1016`). `ReadMappedRange` into the persistent
  shadow avoids that.

### 11.3 Asynchronous pre-mapping

At each submit, any download texture whose copy was in that submit starts `mapAsync` with an
`AllowSpontaneous` callback that sets a "ready" flag. The callback runs only when MTGS's event loop
runs, that is during its suspending idle wait or another JSPI wait (§3.2). A later `Flush()` then
waits (with `WaitAny` on the same future) only if the map is still in flight. This makes deferred
consumers (capture, a future deferred screenshot, GPU timestamp readback) stall-free, and it overlaps
part of the synchronous round trip.

### 11.4 Waits and timeouts

- `WebGPUHost::Wait(WGPUFuture, u64 timeout_ns)` returns ok, timed out or error. It calls
  `wgpuInstanceWaitAny` (JSPI) and never blocks the event loop of any other thread.
- Timeouts: readback 2 s, pipeline 2 s, adapter 5 s per attempt (up to 10 attempts, §4), device 5 s.
  A readback or pipeline timeout is treated as device loss (§13), not a hang.
- MTGS waits; the EE thread blocks in `Atomics.wait` on its own worker as it does today (`WaitGS`). The
  page thread never waits.

---

## 12. Runtime architecture: presentation, waits and pacing

This section is the architecture the spike proved (`tools/wasm/webgpu-spike/`: `spike.cpp` models the
CPU thread and MTGS; `run-headless.mjs` drives headless Chrome, reads every canvas back, follows the
workers' consoles and crashes the GPU process on request). The chosen combination (variant
`jspi-bitmap`: JSPI, wasm SjLj, no `OFFSCREENCANVAS_SUPPORT`) was rebuilt from the current source and
re-run during the review, with the same results.

### 12.1 Decided: a GS-owned OffscreenCanvas, frames posted as ImageBitmaps

```
page (main thread)                        MTGS worker (instance, adapter, device)
<canvas> bitmaprenderer  <-- frame port -- OffscreenCanvas "#tst-gs-canvas" (webgpu)
  transferFromImageBitmap(bitmap)           draw, submit, transferToImageBitmap(),
                                            port.postMessage(bitmap, [bitmap])
pcsx2web_surface_size()  --- atomics -->   read in BeginPresent, resize the canvas
```

- **MTGS creates its own OffscreenCanvas** (`WebGPUHost::CreateCanvas`, `new OffscreenCanvas(w, h)`
  registered in `specialHTMLTargets`) and makes the surface from it (§4). Nothing is transferred from
  the page, so there is no `-sOFFSCREENCANVAS_SUPPORT`, no `emscripten_pthread_attr_settransferredcanvases`
  and no hook in `MTGS::StartThread`. (The transferred canvas also works: the page's canvas reaches the
  PROXY_TO_PTHREAD main thread at startup and can be passed on to a thread it creates, with no `$GL`
  force-include. It is the fallback below.)
- **Each frame** ends with `transferToImageBitmap()` on the OffscreenCanvas and a post of the bitmap,
  in the transfer list, to the page, which shows it with `transferFromImageBitmap` on a
  `bitmaprenderer` canvas. The thread never returns to its event loop: in 60 paced frames with blocking
  sleeps between them and no yield, the page saw 59–60 distinct frames, and the transfer and post took
  0.075–0.09 ms per frame (measured, core and compatibility adapters). A frame posted just before a
  1 s blocking wait was on screen during the wait (measured).
- **The frame port.** The spike posts every frame through Emscripten's internal worker command
  `CMD_CALL_HANDLER` (9 in 6.0.12, `src/lib/libpthread.js:42, 348-349`), which calls `Module[handler]`
  on the page. The device uses it **once**: `WebGPUHost` creates a `MessageChannel`, sends one port to
  `Module.tstFramePort` with that command (the port in the transfer list), and posts every frame on
  the other port. Only the handoff depends on Emscripten internals; `webgpu_host.js` should write the
  command as `{{{ CMD_CALL_HANDLER }}}` so a renumbering cannot break it silently (to check: a
  `--js-library` file can read that compile-time constant), and the page logs an error if no port
  arrives within 5 s of boot.
- **Why not the transferred canvas.** It shows nothing until the worker's task ends: after 120 frames
  and a 1.5 s blocking wait, the page still showed the frame from before the loop. It needs one yield
  per present; a `MessageChannel` round trip costs 0.32–0.40 ms with a frame and shows 54–58 of 60
  frames, `setTimeout(0)` costs 4.1 ms (Chrome's nesting clamp), worker `requestAnimationFrame` waits
  for vsync (5–14 ms) and stops in hidden tabs, and a microtask shows nothing (measured). Above all,
  after a GPU-process crash it stays blank even with a new device configured, in plain JS too, while a
  new OffscreenCanvas through the bitmap path recovers (measured; possibly specific to headless).
- **Commands do not need a yield either.** A 29–40 ms compute dispatch submitted before a 100 ms
  blocking wait was finished when the wait ended (measured): Chrome flushes submitted work without the
  worker returning to its event loop. The design's earlier assumption that it did not is withdrawn.

### 12.2 Present sequence

**`BeginPresent`:**

1. If the lost flag is set, return `DeviceLost` (§13). If the page asked for a debug device destroy,
   do it now.
2. Read the page's size feed; if it changed, call `ResizeWindow`.
3. `wgpuSurfaceGetCurrentTexture` on the OffscreenCanvas's surface.
4. Begin the present pass: `Clear` to black, viewport and scissor on the full surface.

**`GSRenderer` then** draws the frame with `PresentRect(nullptr, …)` using the configured
`PresentShader` and filter, `DisplayConstantBuffer` and the time, followed by `FullscreenUI::Render()`
and `ImGuiManager::RenderOSD()` (`GSRenderer.cpp:570-578`).

**`EndPresent`:**

1. `RenderImGui()`, ported from VK (`GSDeviceVK.cpp:4720-4793`): `ImGui::Render()` and
   `UpdateImGuiTextures()`, then per command list: vertices and indices into the rings, padded to 4
   bytes, a clamped scissor per command, the command's texture (`GetTexID()` is a `GSTextureWebGPU*`)
   with the linear sampler, and `drawIndexed`. The vertex shader gets `uScale = (2/w, −2/h)` and
   `uTranslate = (−1, 1)`, not Vulkan's `(2/w, 2/h, −1, −1)`, because WebGPU's clip space has +Y up
   (measured with the ImGui pixel check). ImGui texture updates are early uploads unless the texture
   was used in this encoder; in that case the present pass is split around a late upload.
2. End the pass, submit, `WebGPUHost::PostFrame()`.
3. The bounded event-loop turn if MTGS has not idled for 100 ms (§3.2).

**Surfaceless** (a test page with no canvas): render offscreen, submit, return `FrameSkipped`, as VK
does without a swap chain (`GSDeviceVK.cpp:2469-2474`).

### 12.3 Fallback: the transferred canvas

If the bitmap path fails in a browser (for example `transferToImageBitmap` on a WebGPU OffscreenCanvas
in a worker is missing), `WebGPUHost` switches to the transferred canvas: build with
`-sOFFSCREENCANVAS_SUPPORT`, the CPU thread passes `#canvas` to MTGS with
`emscripten_pthread_attr_settransferredcanvases` (a guarded hook in `MTGS::StartThread`,
`MTGS.cpp:118-127`), and `EndPresent` ends with one `MessageChannel` yield (about 0.35 ms). A
GPU-process crash then needs a new canvas element from the page. This is the reason `WebGPUHost` hides
the present path behind `CreateCanvas`/`PostFrame`.

### 12.4 Pacing and what the frontend gains

- **Pacing.** The canvas has no vsync control: `SetVSyncMode` only records the mode. The idle loop's
  `GSThrottlePresentation` (`MTGS.cpp:327-333`) and the existing frame limiter stay in charge. The page
  shows the newest bitmap at its next rendering update; bitmaps that arrive faster replace each other.
  The page thread must stay responsive, because every frame passes through it. Latency, jitter and
  tearing of this hop are not measured (step 1 measures them), and the spike ran 60-frame bursts only,
  so long-run memory behaviour is also a step-1 check.
- **ImGui and FullscreenUI work for the first time on the web.** GSDeviceNull renders nothing for them
  (PLAN §3.4). The frontend must preload `fullscreenui/` and `icons/` (FullscreenUI fails without
  `fullscreenui/placeholder.png`, `FullscreenUI.cpp:336`) and forward pointer input to ImGui. That is
  frontend work, outside this device.
- **Hidden tabs.** The bitmap path needs neither `requestAnimationFrame` nor timers, so it should keep
  running in a hidden tab; not tested. The rAF cadence measurement for `surface_refresh_rate` (PLAN M3
  task 6) applies unchanged.

### 12.5 Test environment (headless Chrome on Linux)

Measured with Chrome 155 on the Radeon 780M (RADV, Mesa 23.2.1). The built-in browser pane has no
WebGPU adapter. The smallest working flag set for a hardware core adapter is
`--headless=new --enable-unsafe-webgpu --enable-features=Vulkan --use-angle=vulkan
--disable-vulkan-surface`; removing any one of the four leaves no hardware adapter
(`--ignore-gpu-blocklist`, which the spike runner also passes, is harmless and not needed).

- Without `--disable-vulkan-surface`, Chrome's own Vulkan fails to start (`vkCreateInstance() failed:
  -7`, surface extensions missing in headless), WebGPU offers only SwiftShader, and canvases fail
  (`Could not find SharedImageBackingFactory ... WebgpuSwapChainTexture`). `--use-vulkan=native` changes
  nothing; `--disable-software-compositing-fallback` stops the page from running.
- With the core flags, Dawn's Vulkan backend runs on the GPU (`amd`/`rdna-3`, core features,
  dual-source blending, `depth32float-stencil8`, `texture-formats-tier1`, `primitive-index`,
  `float32-blendable`, BC), but Chrome composites in software: screenshots do not show WebGPU canvases,
  so tests read them back from the page; `bitmaprenderer` canvases do show.
- `--use-webgpu-adapter=opengles` plus `featureLevel: "compatibility"` gives Dawn's OpenGL ES backend
  through ANGLE with GPU compositing. It has no vertex-stage storage buffers, so it only tests mechanics.
- In a fresh profile the first `requestAdapter` answers null; retry (§4).
- Runners: `tools/wasm/webgpu-spike/run-headless.mjs` (`--gpu core|compat`, `--profile-root` for the
  throwaway profile, `SHOT`/`GPUCRASH`/`DONE` console commands) and
  `tools/wasm/webgpu-shaders/validate-shaders.mjs`. Both use throwaway profiles and stop Chrome and the
  server at exit.

---

## 13. Device loss, errors and the fallback chain

- **Detection.** The `deviceLostCallbackInfo` callback (`AllowSpontaneous`) sets `m_device_lost`
  (atomic) and does nothing else. `wgpuDeviceDestroy` calls it synchronously (reason `Destroyed`); a
  GPU-process crash reports reason `Unknown` ("A valid external Instance reference no longer exists.")
  127–628 ms later, and only while MTGS's event loop runs (measured): during its suspending idle wait,
  another JSPI wait, or the bounded event-loop turn (§3.2). Nothing else detects it: on the lost device
  `getCurrentTexture` still reports success and no error is raised (measured). Reason `Destroyed` during
  our own `Destroy()` is ignored. A readback or pipeline wait that times out also sets the flag.
- **Recovery.** The next `BeginPresent` returns `PresentResult::DeviceLost`. `GSRenderer` then calls
  `GSreopen(true, false, …)` (`GSRenderer.cpp:547-567`) on MTGS's own stack, never inside a callback,
  which purges the texture cache and the pool (`GS.cpp:265-271`), destroys the device and calls
  `Create()` again. `Create()` requests a **new adapter** (an adapter cannot create a second device,
  spec) and device under JSPI, and creates a **new OffscreenCanvas** and surface; the page keeps its
  `bitmaprenderer` canvas and the frame port, so it notices nothing. Measured: 12–16 ms after
  `destroy()`, 0.56–0.72 s after a GPU-process crash on the core adapter (0.06 s on compatibility), and
  the first frame afterwards was on screen. Lost texture-cache contents are re-uploaded from GS local
  memory as on desktop; targets drawn only on the GPU are lost, as on desktop. Calls made between the
  loss and the reopen are harmless: WebGPU objects of a lost device accept calls and do nothing (spec,
  and no uncaptured error was raised, measured), maps fail, and `Flush()` returns with stale data.
- **Repeated loss.** Upstream aborts on two losses within 15 s, or when the reopen fails
  (`GSRenderer.cpp:553-567`). On the web that aborts the page. Instead, under `__EMSCRIPTEN__`, ask the
  host to switch to `Renderer = SW` on GSDeviceNull. The display canvas has a `bitmaprenderer` context
  and a canvas keeps its first context type, so the page swaps in a new `<canvas>` element and uses the
  existing 2D blit path (open question 5).
- **Uncaptured errors** (validation and internal) are logged, with the first 10 messages printed in
  full, and counted in `stats()`. Step exit criteria require a count of 0.

---

## 14. Fallbacks if part of the runtime is missing in a browser

JSPI on the MTGS pthread works in Chrome, directly from the thread's start routine (measured), so the
trampoline the first version of this design kept in reserve is not needed. What remains are fallbacks
for browsers that lack a piece; Firefox and Safari have not been tested (§17).

1. **No bitmap path** (`transferToImageBitmap` on a WebGPU OffscreenCanvas in a worker): the
   transferred canvas with one yield per present (§12.3).
2. **No JSPI, but Asyncify-sized code is acceptable:** `-sASYNCIFY` also works with
   `-sSUPPORT_LONGJMP=wasm` (measured), but it made the spike's wasm 72 % bigger (151 KB against 88 KB)
   and instruments every function that may suspend; its speed cost on PCSX2's interpreters is not
   measured and is likely large (the spike's microbenchmark ran about twice as slow under Asyncify, on a
   loaded machine, so inconclusive). Suspending inside a callback crashes it later with `memory access
   out of bounds` (measured), so the callback rule matters even more.
3. **An event-loop-driven GS thread with no synchronous readbacks**, when no JSPI is available at all.
   `MTGS::MainLoop` becomes re-entrant: it processes ring-buffer packets until the ring is empty or a
   VSync completes, then returns to the worker's event loop (`emscripten_unwind_to_js_event_loop`; the
   spike's `none` variant got the adapter in 54 ms and the device in 69 ms this way, while polling
   from C++ never did, measured). It is woken by `Atomics.waitAsync` on the ring semaphore. That is a
   guarded change to `MTGS.cpp` (estimate: ~200 lines, M).
   - Device creation happens before `GSopen` returns, through the event loop.
   - `HWDownloadMode` is forced to `Unsynchronized`. `GSDownloadTexture::Flush()` cannot wait, so
     ungated readbacks (§11.1) return the previous contents, and screenshots are delivered one frame
     late through a callback. Titles that depend on GPU→CPU readbacks render incorrectly, and the
     support list must say so.
4. **A GPU-owner worker with a command stream** (PLAN's original plan B). MTGS serialises WebGPU calls
   into shared memory, a dedicated worker executes them, and MTGS blocks with `Atomics.wait` for
   results. This restores synchronous readbacks, but it amounts to a private WebGPU wire protocol with
   one more copy of all upload data (estimate: L–XL). Do it only if option 3's readback losses prove
   unacceptable on the target titles.

---

## 15. Incremental plan

Size estimates compare with the existing backends (lines counted with `wc -l`):

| Backend | C++ (device + texture + helpers) | Shaders |
|---|---|---|
| DX11 | ≈ 5.5k (`GSDevice11.cpp` 3441, `.h` 433, texture 388, shader cache 536, `D3D.cpp` 709) | HLSL shared with DX12 |
| DX12 | ≈ 9.2k | (shared HLSL) |
| Vulkan | ≈ 12.5k (`GSDeviceVK.cpp` 6745, `.h` 786, texture 1125, builders 1611, shader cache 814, stream 366, swap chain 792, loader 232) | GLSL 3.5k |
| Metal | ≈ 4.0k | MSL 3.2k |
| OpenGL | ≈ 4.7k | GLSL |

WebGPU has no layouts, no memory allocator, no swap-chain management and no descriptor heaps, so it
should come in near DX11 plus the copy-model share of VK: **≈ 6–7k lines of C++ and ≈ 3.5–4k lines of
WGSL in total** (estimate). Efforts use PLAN.md's scale for one engineer who knows PCSX2.

### Step 1: present and utility passes on the GPU, SW renderer (size M; ≈ 2.4k C++, 0.3k JS; WGSL written except shadeboost)

**Order of work** (each part is checked before the next starts):

1. **Build plumbing and the JSPI gate.** Link `pcsx2-wasm` with `-sJSPI` and the port while the device
   is still GSDeviceNull. Check the link log for `signature mismatch`, the wasm and JS size change, and
   the speed: the BIOS intro (450 frames, Null renderer, median of 3 runs) must be within noise of the
   build without `-sJSPI`. JSPI does not instrument code, so no cost is expected, but the spike's
   microbenchmark was inconclusive on a loaded machine; run this only when the interpreter benchmarks
   are not running, and coordinate with that work.
2. **Suspending idle waits** (`common/Semaphore.cpp`, §3.2). Check that `pcsx2-headless` and the page
   still boot the BIOS at unchanged speed with the opt-in off, then with it on in the page.
3. **`WebGPUHost` and the page**, ported from `spike.cpp`: instance with `TimedWaitAny`, adapter retry,
   device, `Wait`, `SleepSuspending`, the OffscreenCanvas, the frame port, `PostFrame`, the size feed,
   the lost flag, the debug destroy request. The page: mode choice before any context, the
   `bitmaprenderer` canvas, `Module.tstFramePort`, `pcsx2web_surface_size`.
4. **The device:** `Create`, `Destroy`, surface and present sequence (§12.2); `GSTextureWebGPU`
   (create, views, early and late uploads, `Map`/`Unmap`, clear state); `GSDownloadTextureWebGPU`
   (basic, for screenshots); the rings; the pass manager; the utility layouts (§8.2); samplers;
   `RenderImGui`.
5. **Passes:** `DoStretchRect` with every convert pipeline that has a colour or r32float target (all are
   validated); `PresentRect` with the 8 present shaders; `DoMerge`; `DoInterlace` (5); `DoShadeBoost`
   (write `shadeboost.wgsl` and add it to `validate.js` first); `DrawMultiStretchRects`; colour
   `CopyRect`.
6. **Stubs:** `RenderHW`, CLUT, indexed and downsample, depth `CopyRect`. HW features are reported
   off, because the HW renderer cannot be selected yet.

**Files.** New (all with SPDX `GPL-3.0+` headers):

| File | Contents |
|---|---|
| `pcsx2/GS/Renderers/WebGPU/WebGPUHost.{h,cpp}`, `webgpu_host.js` | Platform layer (§12) |
| `pcsx2/GS/Renderers/WebGPU/GSDeviceWebGPU.{h,cpp}` | The device: create, present, pass manager, utility pipelines, ImGui |
| `pcsx2/GS/Renderers/WebGPU/GSTextureWebGPU.{h,cpp}` | `GSTextureWebGPU`, `GSDownloadTextureWebGPU` |
| `pcsx2/GS/Renderers/WebGPU/WebGPUStreamBuffer.{h,cpp}` | Vertex, index, uniform and upload rings |
| `pcsx2/GS/Renderers/WebGPU/WebGPUCaches.{h,cpp}` | Sampler cache, utility bind-group cache |
| `bin/resources/shaders/webgpu/shadeboost.wgsl` | Port of `vulkan/shadeboost.glsl`, same header conventions |

Changed (guarded, §3.3 rows marked step 1): `common/Threading.h`, `common/Semaphore.cpp`,
`common/WindowInfo.h`, `pcsx2/GS/GS.h`, `pcsx2/GS/GS.cpp`, `pcsx2/GS/Renderers/Common/GSDevice.cpp`,
`pcsx2/CMakeLists.txt`; frontend: `pcsx2-wasm/CMakeLists.txt`, `pcsx2-wasm/Main.cpp`,
`pcsx2-wasm/web/app.js` (and `index.html` if the canvas element changes), `pcsx2-wasm/README.md` (the
WebGPU mode and the new query switches); tools: `tools/wasm/webgpu-shaders/validate.js` (shadeboost
cases), `tools/wasm/webgpu-spike/run-headless.mjs` (a timed `GPUCRASH` and a frame-counter readout
for the app, or a sibling runner for the app).

**Exit criteria:**

- In headless Chrome on the RADV adapter (§12.5), the BIOS boots with `Renderer=SW` on
  GSDeviceWebGPU, shows the intro and reaches the OSDSYS menu, and takes keyboard input. BIOS
  screenshots stay outside the repository (`AGENTS.md`).
- 0 uncaptured WebGPU errors over 600 frames. Bitmaps received by the page per second equal VPS, and
  the frame port arrives on every boot.
- **Pixel check:** with nearest presentation at 1:1 and no TV shader, the presented frame read back
  through `SaveSnapshotToMemory` is identical to GSDeviceNull's frame for the same frame number (`?gpu=0`;
  both are copies of the SW output), or every difference is explained.
- The ImGui OSD (FPS and VPS) is visible when enabled.
- The debug destroy request recovers within 1 s, and a GPU-process crash (`GPUCRASH`) during the intro
  within 2 s; the BIOS continues and no error follows. Repeat the crash with the frame limiter off, so
  that MTGS rarely idles, to test the bounded event-loop turn (§3.2).
- 10 minutes at the menu: the page's JS heap and the wasm memory stay flat, frames keep arriving.
- The JSPI gate of part 1 holds.
- MTGS-thread CPU time for merge, interlace and present is at least 5× lower than with GSDeviceNull
  (measure with the browser profiler). Latency from `PostFrame` to the page's `transferFromImageBitmap`
  is logged (median, p99).
- `pcsx2-headless` (Node, Null and SW) still builds and boots.
- The 2D-canvas path still works when WebGPU is unavailable (`?gpu=0`, and a browser without
  `navigator.gpu`).
- Checked once by the owner in a windowed Chrome: the canvas shows frames (not only read back), and a
  tab hidden for 30 s keeps running.

#### Step 1 results (2026-10-16)

Measured in headless Chrome on the RADV adapter (§12.5) with `tools/wasm/run-app-headless.mjs`, the
BIOS SCPH-39001; logs and screenshots stay outside the repository. Runs compared side by side were
pinned to separate physical cores and swapped between rounds.

| Exit criterion | Result |
|---|---|
| BIOS on GSDeviceWebGPU, menu, keyboard | The intro, the first-run screens and the OSDSYS menu (Browser, System Configuration) render; Cross (K) steps through the first-run screens. |
| 0 uncaptured errors | 0 in every run, from 600 frames to 28,000 (the 10-minute run). A build with a deliberately invalid buffer shows the detection works. Exception, fixed in round 1: a canvas larger than the adapter's largest texture. |
| Bitmaps per second = VPS; frame port | Bitmaps shown = bitmaps posted (no page drops); shown per second = VPS less the vsyncs PCSX2 skips presenting as duplicates (5 of 28,134 in the 10-minute run). Frames arrive over the port on every boot, including Stop then Boot again (the first 95 ms after Boot). |
| Pixel check | At 1:1 with nearest presentation, Weave, Off or Bob deinterlacing: identical to GSDeviceNull's frame for the same vsync (every one of 62 vsyncs compared in two runs, up to vsync 800), and the `SaveSnapshotToMemory` PNGs are byte-identical. Explained differences: Blend differs by at most 1 LSB (rounding of the 1-2-1 filter); Automatic (MAD for this BIOS) differs only on the reconstructed field's lines, because GSDeviceNull's MAD has no motion detection; with the default bilinear presentation the GPU matches a float bilinear resample of the 1:1 frame within 1 LSB, and the 2D path matches a nearest resample exactly. |
| ImGui OSD | FPS, VPS, speed and OSD messages render when enabled. |
| Device loss | `destroy()`: first frame of the new device on the page 90–210 ms after the request (35–50 ms on the GS thread). GPU-process crash: 0.83–1.45 s; twice in one page when 120 s or more apart, or with Chrome's domain blocking off. With the idle wait kept blocking (`?gsidle=block`, frame limiter at 10x) the loss is noticed in the bounded event-loop turn and recovered in 0.83 s. Two failed recoveries in a row switch the page to the 2D path with a notice; frames stay identical to the 2D path's. |
| JSPI gate | 450 BIOS frames: 47.93 s and 47.65 s without JSPI, 47.39 s and 48.30 s with it (+0.1 % on average). |
| GS-thread CPU time, merge + interlace + present | **Not met: 3.1–3.4x lower, not 5x.** WebGPU 0.94–1.03 ms per vsync against 3.1–3.5 ms for GSDeviceNull (CPU profiles, intro and menu). These are sampled times, not CPU time; see the round-1 row below. |
| Latency post to `transferFromImageBitmap` | Measured until `transferFromImageBitmap()` returns on the page, not until the frame is on screen. In `stats().latency`: median 0.57 ms, p99 3.7 ms, max 5.1 ms (last 600 frames of the 10-minute run). Runs that read frames back for tests see a p99 up to 50 ms, because the read-backs block the page. |
| 10 minutes at the menu | 40.1 bitmaps shown per second for 40.1 vsyncs per second, none dropped (28,129 posted and shown); wasm memory 745 MB throughout; JS heap 11–17 MB after the first collection, with no upward trend; 0 errors. Not measured then, found in round 1: the GPU process grew by 5.5 KB a frame and the GS worker's JS heap by 41 B a frame (fixed, see below). |
| `pcsx2-headless`, the 2D path, no WebGPU | Headless SW and Null boot as before (identical screenshots, time within noise). `?gpu=0`, a page and workers with `navigator.gpu` hidden, and a browser whose `requestAdapter` answers null all run the 2D path; the last starts 0.75 s later and shows no notice. |
| Windowed Chrome, hidden tab | Not done (owner check). |

**Departures from the design, and why:**

- **Frames are identified by the vsync they show** (`PerformanceMetrics::GetFrameNumber() + 1`), on
  both paths and in the test captures, not by a count of presented frames. `SkipDuplicateFrames`
  (on by default) skips presenting frames that the internal-FPS detector, which works on 0.5 s
  wall-clock windows, considers repeats, so how many frames were presented by a given vsync depends
  on timing: the two paths' counts drifted apart by one in some runs.
- **The event-loop check uses a microtask, not `onSubmittedWorkDone`.** After a GPU-process crash that
  promise rejects, emdawnwebgpu leaves the rejection unhandled ("assumed not to reject"), and
  Emscripten's pthread runtime turns an unhandled rejection into a fatal error. A microtask queued at
  each present runs only when the GS thread suspends; while it is pending its age is how long the
  event loop has not run.
- **Large uploads go through mapped staging buffers** (`UploadTextureStaged`: MapWrite buffers
  mapped again after the submit that used them). `queue.writeTexture` and `writeBuffer` copied the
  SW renderer's 0.9–1.1 MB output at about 1.5 GB/s (0.67–0.78 ms a frame); filling a mapped range
  takes 0.13–0.19 ms.
- **Device loss:** a loss within 15 s of the last recovery counts as a failed recovery (upstream
  aborts on the first); after two in a row, or when a recovery cannot create a device twice, the GS
  gives WebGPU up for the page. At boot, a page that never had a device gives up after one attempt
  (four adapter requests over 0.75 s, not ten over 5 s), so a browser without an adapter starts at
  most 0.75 s later. Chrome refused adapters to the page after a second GPU-process crash 93 s after
  the first, not 121 s or more after it (and never with `--disable-domain-blocking-for-3d-apis`): a
  second crash soon after the first falls back to the 2D path in practice.
- **OSD messages are off by default on both paths** (`OsdMessagesPos=None`): the 2D path cannot show
  them, and the page has its own status line (UI-SPEC: no toasts). The FPS/VPS overlay and messages
  work when enabled.
- `GSDeviceNull` does no shadeboost, EXTBUF feedback or MAD motion detection, so those can only be
  checked against formulas, not against the 2D path.

**Known gaps:** the 5x target; the GPU memory budget of §6.5 is not implemented (the round-1 fixes
removed a byte count nothing read); debug groups are no-ops; mip generation, depth copies, depth and
stencil convert targets and the HW paths are stubs until step 2; only Chrome on Linux was tested,
headless; the owner's windowed check is open.

#### Step 1, round-1 verification fixes (2026-10-16)

Two verifications of step 1 found the problems below; all are fixed, and the exit criteria were
measured again on the fixed build (same setup; the machine was shared with other builds, load 6–22).

- **GPU-process memory grew 5.5 KB a frame.** Chrome frees the GPU-process side of a WebGPU object
  (the frame's encoder, passes, command buffer, canvas texture and view) only when the JS wrapper is
  garbage collected in a major collection, and the GS worker allocates so little JS that V8 ran none
  for minutes: the GPU process grew from 209 to 252 MB in 7,400 frames, and a forced collection gave
  it back (measured). Destroying the canvas texture after each frame did not help (measured).
  `tst_post_frame` now allocates and drops a 64 MB `ArrayBuffer` every 300 frames: V8 counts it as
  external memory and starts a major collection, and the buffer is never touched, so nothing is
  committed (0.05–4 ms per allocation in a loaded worker, measured). Over 10 minutes (20,080 frames)
  the GPU process stayed at 209–216 MB.
- **`WebGPU.Internals.futures` gained an entry a frame** (the staging buffers' `AllowSpontaneous`
  `mapAsync`; emdawnwebgpu removes a future's entry only in a timed wait on it). The staging buffers
  are now mapped, written and unmapped by `webgpu_host.js` on the `GPUBuffer` itself (through
  emdawnwebgpu's public `WebGPU.getJsObject`), after their first use through `webgpu.h`. The table
  holds 1 entry (the device-lost future) and the GS worker's heap stays at 1.5–2.3 MB over 10 minutes.
- **Timed-out waits left callbacks pointing into dead stack frames.** A `WaitAnyOnly` callback whose
  wait timed out still runs later, at the latest when `wgpuInstanceRelease` completes it as
  cancelled. Every waited-for result (adapter, device, error scope, pipelines, readback) now lives in
  a heap `WebGPUHost::RequestResult` that the callback frees when its waiter has given up. Tested
  with a `requestAdapter` that never answers: the wait timed out after 5 s, the next attempt got a
  device, and the abandoned callback ran safely at the next instance release.
- **Timeouts as device loss (§11.4, §13).** A readback that times out (2 s) cancels its map, and it
  and a pipeline that times out (10 s) set the lost flag; the device is recreated at the next
  present (tested: a readback that never completes, recovered in 85 ms).
- **No 2D fallback without JSPI.** With `-sJSPI` the generated JS calls `new WebAssembly.Suspending`
  unconditionally. `pcsx2-wasm/jspi-fallback.js` (a `--pre-js`) puts pass-through stand-ins in place
  where the browser lacks JSPI, and `tst_gpu_available()` then reports no WebGPU, so the page runs the
  2D path (tested with both hidden in the page and every worker; the round-0 build died there).
- **Canvas larger than the largest texture.** The device presents at most 4096 pixels on a side and
  never more than `maxTextureDimension2D`, keeping the aspect ratio; the page scales the frame. A
  17000x12750 canvas presents at 4096x3072 with 0 errors (clamping to 16384 alone gave 800 MB textures
  that stalled the page).
- **Blank frames after a GPU-process crash.** Frames are no longer posted once the loss is known.
  Frames drawn between the crash and Chrome reporting the loss (0.2–0.6 s) are still blank, and they
  cannot be told apart: on that device, maps still resolve before the loss is reported. Keeping the
  last good frame on screen is not possible cheaply either: a clone of a pre-crash frame (confirmed
  by a value read back from the GPU after its present) showed fully transparent after the crash,
  because its memory was in the crashed process (measured); a CPU copy would cost a readback on the
  page every few frames, for a 1–2 s event.
- **A boot that gets no device is no longer a permanent give-up.** That boot uses the 2D path
  (`WebGPUHost::NoteUnavailable`) and the next boot tries WebGPU again (tested: four refused
  adapters, then Stop and Boot gave a device). Repeated loss still gives WebGPU up for the page.
- **The staged upload copied the frame twice.** Rows whose pitch is a multiple of 256 (the SW
  renderer's 4096-byte rows) are copied into the staging buffer once, padding included, with that
  pitch as `bytesPerRow`. The device's own wasm time per vsync fell from 0.23–0.29 ms to 0.09–0.10 ms.
- Smaller fixes: utility passes no longer set the viewport and scissor that a pass starts with;
  `WindowInfo.h`, `GS.h` and the two `Semaphore.cpp` call sites leave the upstream lines as they were
  (desktop objects and preprocessed sources identical); the driver-info label names the device
  actually created; desktop packages leave out `shaders/webgpu/`; a CMake warning when Emscripten is
  not 6.0.12; the suspending futex wait uses `HEAP32` directly with `addr >>> 2`; a debug assertion
  (§3.2) that no GS code runs while the GS thread is suspended in a WebGPU wait; dead code removed;
  `run-app-headless.mjs` fails a run with errors, enables `Log` in workers, samples the GPU process
  and the GS worker (`--sample-every`), reads the display canvas back (`--display-at`) and no longer
  hangs on a hung page.

| Exit criterion (round 1) | Result |
|---|---|
| 0 uncaptured errors | 0 in every run, including the oversize canvas, OSD, `destroy()` and crash runs and 10 minutes at the menu. |
| Pixel check | At 1:1 nearest, Weave, Off and Bob: 17 of 17 vsyncs (100–900) identical to the 2D path, snapshot PNGs at vsyncs 300, 450 and 900 byte-identical; also after two `destroy()` recoveries and after the fallback to the 2D path. |
| Device loss | `destroy()`: 85–164 ms on the GS thread, 219–240 ms to the first frame on the page (at 10 % speed under load). GPU-process crash: 0.76 s alone; 1.55–2.05 s with two crash runs at once on a loaded machine. `?gsidle=block` with the frame limiter at 10x: loss noticed in the bounded turn, recovered. Three losses close together: the page switches to the 2D path with the notice. |
| Frames = VPS, 10 minutes | 20,080 posted, 20,080 shown, for 20,086 vsyncs; GPU process 209–216 MB, GS worker heap 1.5–2.3 MB, page heap 11–17 MB, wasm 711 MiB, futures table 1 entry. Latency median 0.63 ms, p99 5.0 ms. |
| JSPI gate (Null renderer, median of 3 pinned pairs) | 41.50 s without JSPI, 41.58 s with it (+0.2 %, the pairs within 0.25 s of each other). The final build adds 4.6 KB of wasm and 6.7 KB of JS for JSPI and the fallback. |
| GS-thread CPU time | **Not met: about 3x, not 5x.** In CPU profiles, merge + interlace + present took 0.82–0.83 ms per vsync with WebGPU against 2.72–2.80 ms with GSDeviceNull (3.3–3.4x; two pinned, swapped pairs at the menu). A profile's sampled time is not CPU time: the gaps around the GS thread's JSPI suspensions are charged to the last call that ran, so the per-call split measured that way (posting the bitmap, submit, `transferToImageBitmap`, `getCurrentTexture`, the staging writes) is not a CPU breakdown and is withdrawn, and the ratio is taken as about 3x. CPU time is measured from the kernel instead: the on-CPU time of the GS worker's OS thread (`/proc/<pid>/task/<tid>/schedstat`, sampled over the same window), which covers the whole thread rather than one call. Reaching 5x would likely take presenting without posting a bitmap (the transferred canvas, §12.3), a design change. |
| `pcsx2-headless` | SW and Null: screenshots identical to the round-0 build's (md5), same time (concurrent runs, 61.7–62.8 s for 600 frames). |
| No WebGPU, no JSPI | `?gpu=0`, `navigator.gpu` hidden, `--disable-features=WebGPU` and no JSPI all run the 2D path; the page logs why. |

### Step 2: the HW renderer with the TFX paths the BIOS uses (size L; +≈ 2.4k C++, +2.1k WGSL)

**Scope:**

- **Census first.** Run the same BIOS on a desktop build with the matching feature profile and
  `SaveHWConfig` dumps (`GSRendererHW.cpp:9671`, `GSHWDrawConfig::DumpConfig`) over the intro and menu, and list the distinct
  `PSSelector`, `VSSelector`, blend and depth keys.
  - The exact reference is **DX11** (same copy model). Vulkan or GL with `OverrideTextureBarriers=0`
    turns feedback off instead of copying, which is a different profile.
  - Dumps and captures stay outside the repository (`AGENTS.md`).
- **Shaders:** `tfx.wgsl`, the whole template ported from `vulkan/tfx.glsl` with the `dx11/tfx.fx`
  clip-space and output conventions, validated on the census set first. Selectors outside the set log
  once.
- **Plumbing:** `GSRendererType::WebGPU` and the rows of §3.3 marked step 2 (renderer name, adapter
  info, preferred renderer, FullscreenUI list).
- **Shader generation:** `WGSLPreprocessor` (§9.1) and its unit test against `vulkan/tfx.glsl`'s macro
  names.
- **Pipelines and bindings:** the TFX layout (§8.1), the bind-group cache, `PipelineSelectorWebGPU` with
  async creation and the wait policy (the first measured `createRenderPipelineAsync` waits through
  `WaitAny`), expansion with `firstInstance`.
- **`RenderHW` core path:** no feedback, no DATE, no colclip; render-pass reuse; the PS and VS uniforms.
- **Remaining device pieces:**
  - every convert entry point, including the depth conversions and the DATM shaders;
  - `UpdateCLUTTexture`, `ConvertToIndexedTexture`, `FilteredDownsampleTexture`;
  - depth formats with clears;
  - depth `CopyRect` (full copy or `DEPTH_COPY` draw);
  - `GenerateMipmap`, `ClearSamplerCache`.
- **Flags:** `dual-source-blending` becomes required for `Renderer=WebGPU`. Also implement the
  `stencil_buffer` DATE paths if the census shows DATE.

**Exit criteria:**

- At native resolution, the BIOS intro and menu render with `Renderer=WebGPU` and match the DX11
  reference: under 0.5 % of pixels differ by more than 2 LSB per channel, and every residual is
  explained (for example DX11's primitive-ID DATE against WebGPU's stencil DATE).
- 0 uncaptured errors.
- No skipped draws after warm-up (counter).
- `GSPerfMon` draw calls, render passes, texture copies and readbacks are logged per frame.
- Intro wall time with the HW renderer is no worse than with the SW renderer (both are EE-bound), and
  MTGS plus SW-thread CPU time is lower.

### Step 3: the rest of TFX, readbacks, device loss, performance (size XL; +≈ 1.8k C++, +0.7k WGSL)

**Scope:**

- **Feedback:** the copy model with one-barrier and full-barrier loops (§10.2); DS-as-RT and Depth mode
  (§10.4); `test_and_sample_depth` read-only passes.
- **DATE:** every mode (§10.3), with PrimID behind its features.
- **Remaining HW paths:** colclip HW (with the format fallback); AA1 (expand index ring); point, line and
  sprite expansion when upscaling; mip generation and LOD paths; BC replacements.
- **Readbacks:** synchronous `Flush` hardened with timeouts, plus pre-mapping (§11).
- **Device loss:** the full chain (§13), including the SW + GSDeviceNull fallback.
- **Post-processing and timing:** CAS (compute), FXAA, timestamp queries.
- **Performance:**
  - state-cache hit rates;
  - one `writeBuffer` per ring per submit;
  - bind-group cache sizing;
  - fewer pass restarts;
  - pipeline prewarm;
  - a profile of per-draw JS-boundary cost.
- **Later decision:** whether to start the dual-source-blend fallback (§10.5).

**Exit criteria (PLAN M4 exit, restated):**

- 20 titles render correctly against a desktop reference forced to the same profile (DX11 or GL with
  `OverrideTextureBarriers=0`), at 1× and 3×, with identical blend-accuracy settings; residuals are
  documented.
- `GSPerfMon` reports readbacks, barriers (copies) and render passes for each title.
- 0 validation errors.
- Gameplay survives `device.destroy()` and a GPU-process crash (the spike runner's `GPUCRASH` command).
- On the reference laptop, MTGS time per frame stays under 8 ms on two named light 3D titles at 1×
  (target to confirm with the owner).

Recommended alongside step 2: **GS-dump replay in the page** (PLAN M2 task 3). Dumps exercise the
renderer at full speed without the EE interpreter, which runs the BIOS intro at about a fifth of real
time. They are the efficient test bed for steps 2 and 3.

---

## 16. Hardest problems

1. **Synchronous GS semantics on an asynchronous API.** Device creation, readbacks, pipeline readiness
   and loss recovery all happen in the middle of synchronous PCSX2 call stacks. In Chrome this is
   solved: JSPI suspends the MTGS pthread from its start routine, frames reach the page without a yield,
   and commands reach the GPU while the thread blocks (all measured). What remains: every synchronous
   readback costs at least 2.3 ms; loss detection and early map completions need MTGS's event loop to
   run, hence the suspending idle wait and its change to `common/`; the frame hop through the page
   thread is unmeasured for latency; and none of it is tested outside Chrome. If a browser lacks a
   piece, the fallbacks cost a yield per frame, code size, correctness (no synchronous readbacks) or a
   large IPC layer (§14).
2. **No texture barriers.** Every feedback draw ends the render pass for a copy. Full-barrier draws
   become n copies plus n pass restarts. Because WebGPU resets all pass state, each restart re-binds
   everything. Depth copies must be whole-texture, which forces the DS-as-RT path. This is likely the
   dominant GPU cost on blend-heavy titles.
3. **The per-call cost of WebGPU from wasm.** Each `wgpu*` call crosses into JS, which looks up the
   object and calls the GPU*Encoder method, and Chrome serialises it to the GPU process. Estimate: 0.3–1
   µs per call, about 8 calls per draw, hundreds to thousands of draws per PS2 frame. The design
   minimises calls with base offsets instead of rebinding, cached bind groups, dirty-flag state and one
   `writeBuffer` per ring per submit. It is still a risk that the backend ends up CPU-bound on MTGS;
   step 3 must profile it.
4. **WGSL.** There is no preprocessor, typing is strict, the uniformity analysis is strict, there is no
   multi-component swizzle assignment and there are no `?:` expressions. The 2k-line TFX shader is a real
   rewrite, and it must track upstream's `tfx.glsl` and `tfx.fx` on every merge (hence the macro-name
   test, §9.1).
5. **Pipeline compilation.** There are dozens to hundreds of TFX pipelines per title. Async creation
   with JSPI waits trades hitches for correctness. An invalid pipeline would poison a whole frame if it
   were ever created synchronously.
6. **Optional formats and features.** `rgba16unorm` (colclip), r32float filtering and blending, stencil
   and BC are all optional, and each has its own fallback. Dual-source blending is missing on every
   WebKit browser, and its fallback needs new HW-renderer logic.
7. **Upload ordering.** `queue.writeTexture` and `writeBuffer` take effect before the encoder that is
   being recorded. Getting the early/late split wrong produces subtle, content-dependent corruption.
8. **Memory without a VRAM query.** The device keeps its own budget, and out-of-memory is reported
   asynchronously.

---

## 17. Open questions

1. **Suspending waits in `common/`.** The idle wait must suspend (§3.2), which the proposal does
   with a wasm-only futex `KernelSemaphore` in `common/Semaphore.cpp` and a per-thread opt-in. Accept
   that guarded change to shared code, or prefer a WebGPU-only wait inside `MTGS::MainLoop`?
2. **Renderer enum.** Accept `RenderAPI::WebGPU` and `GSRendererType::WebGPU = 32`, or prefer another
   value?
3. **Dual-source blending.** Is "WebGPU HW renderer requires dual-source blending" acceptable for v1,
   which leaves Safari and iOS on the SW renderer? Or should the fallback (§10.5) be scheduled?
4. **Shader toolchain.** Decided for the utility shaders (hand-written, already done, §9.1). For TFX:
   hand-written template with the C++ preprocessor (recommended), unless you prefer otherwise.
5. **Repeated device loss.** Is falling back to SW on GSDeviceNull with a replaced `<canvas>` element
   the behaviour you want, or should the page show an error and offer a reload?
6. **Default `HWDownloadMode` on the web.** `Unsynchronized` (fast, EE reads local memory) or upstream's
   `Enabled` (accurate, one GPU round trip per EE readback)?
7. **GPU memory budget and maximum upscale** on desktop and on mobile.
8. **Pipeline-wait policy.** Wait (hitch) or skip the draw (pop-in) on a cold pipeline? This design
   waits by default.
9. **Keep GSDeviceNull and the 2D blit permanently** as the path for browsers without WebGPU? This
   design assumes yes.
10. **Closure.** emdawnwebgpu recommends `--closure=1` for size. Does the frontend's
    `EXPORTED_RUNTIME_METHODS` usage survive Closure, or is the larger unminified glue accepted?
11. **Reference platform.** DX11 is the exact feature-profile match for the comparisons. Is a Windows
    machine available for reference captures, or should references come from GL with
    `OverrideTextureBarriers=0` and an accepted list of differences?
12. **Firefox and Safari.** PLAN targets Firefox 153+ and Safari 27, which both have JSPI. JSPI on a
    worker, WebGPU in workers, `transferToImageBitmap` on a WebGPU OffscreenCanvas in a worker, the
    bitmap path's cost, and the optional features listed in §4 must be checked there before they are
    promised. Is a Mac (Safari) or a Firefox run on this machine available for that check?
13. **Emscripten internals.** The frame port's one-time handoff uses Emscripten's internal
    `CMD_CALL_HANDLER` worker command (§12.1). Acceptable while Emscripten is pinned (6.0.12), with a
    check at boot?

---

## 18. Code pointer index

- **Interface:** `pcsx2/GS/Renderers/Common/GSDevice.h`: `ShaderConvertSelector` :266-456,
  `GSHWDrawConfig` :678-1348 (`VSSelector` :692, `PSSelector` :718, `SamplerSelector` :937,
  `DepthStencilSelector` :996, `ColorMaskSelector` :1020, constant buffers :1044-1176, `BlendState`
  :1178, DATE modes :1237), `FeatureSupport` :1399-1426, virtuals :1509-1719.
  `GSTexture.h` (formats :49-66, `GSDownloadTexture` :263-327).
- **Base behaviour:** `GSDevice.cpp`: `Create`/`AcquireWindow` :453-478, clear state :518-566, ImGui
  textures :568-669, `FetchSurface` :681-764, `Recycle`/pools :766-839, `StretchRect*` :913-998,
  `BeginDSAsRT` :1234-1325, `CAS` :1353-1389, `m_blendMap` :2083-2165.
  `GSTexture.cpp` (usage and format rules :20-45, feedback and shader-write formats :205-214).
- **Selection:** `pcsx2/GS/GS.cpp` (:81-115 `GetAPIForRenderer`, :117-185 `OpenGSDevice`, :200-216 upscale
  clamp, :218-243 renderer creation, :255-330 `GSreopen`); `GS/GSUtil.cpp:276-311`; `GS/GS.h:16-24`;
  `Config.h:271-281, 399-406, 900-901`; `ImGui/FullscreenUI_Settings.cpp:2860-2900, 3028-3036`;
  `common/WindowInfo.h:12-19`.
- **Threading:** `pcsx2/MTGS.cpp` (:118-127 `StartThread`, :144-200 thread entry and `GSopen`, :283-297
  `InitAndReadFIFO`, :322-340 `MainLoop`).
- **Present and loss:** `GS/Renderers/Common/GSRenderer.cpp` (:523-568 `BeginPresentFrame` and device
  loss, :570-578 `EndPresentFrame`, :665-680 CAS gate, :1061-1130 snapshots).
- **HW renderer requirements:** `GS/Renderers/HW/GSRendererHW.cpp` (:5503-5555 flat shading, :5557-5690
  `SetupIA` and expansion, :6073-6295 DATE selection and config, :6889+ `EmulateBlending`, :7173-7193
  `UseDebugBlend`, :8476-8545 hazards, :9640-9688 DS-as-RT and `RenderHW` call, :11232-11246 drawlist and
  AA1); readbacks at :80-90, :2311-2340, :2490-2506, :9960-9970, :10720-10730;
  `GS/Renderers/HW/GSTextureCache.cpp` (:69-80, :1750-1765, :4982-5115, :5262-5290, :7406-7551).
- **Templates:**
  - DX11: `GSDevice11.cpp` (features :63-82, :707-748; `CopyRect` :1541-1597; `RenderHW` :3072-3326;
    `FeedbackCopyAndBind` :3328-3378; `SendHWDraw` :3380-3425).
  - Vulkan: `GSDeviceVK.cpp` (`BeginPresent`/`EndPresent` :2458-2560; `CheckFeatures` :2787-2892;
    `LookupNativeFormat` :2948-2972; `CopyRect` :2993-3078; `DoMerge` :3438-3557; `GetSampler` :3848;
    buffers and layouts :3986-4100; convert pipelines :4192-4368; `RenderImGui` :4720-4793; TFX shaders
    and pipelines :4981-5242; `RenderHW` :6116-6590). `GSTextureVK.cpp:352-363` (early/late upload),
    `:944-966` (readback flush). `GSDeviceVK.h:339-370` (`PipelineSelector`).
  - DX12: `GSDevice12.cpp:2818-2852` (root signatures); `GSTexture12.cpp:592-705, 1161-1320` (pitch,
    download).
  - Metal: `GSDeviceMTL.mm:1049-1067` (feature profile without geometry shaders); `Metal/tfx.metal`.
  - Null (today's web device): `GS/Renderers/Null/GSDeviceNull.{h,cpp}`.
- **Shaders:** `bin/resources/shaders/vulkan/{tfx,convert,present,merge,interlace,shadeboost,imgui,cas}.glsl`,
  `dx11/tfx.fx` (:176-229 inputs and outputs, :1732-1733 VS-expand buffers), `common/{fxaa.fx,ffx_cas.h,ffx_a.h}`;
  the WebGPU ports in `bin/resources/shaders/webgpu/` (bindings in each file's header).
- **Phase-1 tools:** `tools/wasm/webgpu-spike/` (`spike.cpp`, `build.sh`, `index.html`, `run-headless.mjs`,
  `README.md` with the results); `tools/wasm/webgpu-shaders/` (`validate-shaders.mjs`, `validate.html`,
  `validate.js`, `check-upstream.mjs`).
- **Emscripten 6.0.12:** `src/lib/libpthread.js` (worker commands :34-42, `CMD_CALL_HANDLER` dispatch
  :348-349), `src/runtime_pthread.js:85-99` (proxied handlers), `src/settings.js:924-950` (JSPI),
  `system/include/emscripten/atomic.h:304-309` (`emscripten_atomic_wait_suspending`),
  `src/lib/libatomic.js:86` (its abort without JSPI or Asyncify).
- **The port** (`~/emsdk/upstream/emscripten/cache/ports/emdawnwebgpu/emdawnwebgpu_pkg/`, v20260423.175430):
  `webgpu/include/webgpu/webgpu.h` (features :411-435, limits :1928-1962, entry points :2614-2860);
  `webgpu/src/library_webgpu.js` (futures :129-141, `emwgpuWaitAny` :689-718, mapped ranges :992-1078,
  surface creation :1984-2015, configure :2540-2560, present abort :2620-2623); `webgpu/src/webgpu.cpp`
  (`WaitAny` :552-590, :1703-1731).
