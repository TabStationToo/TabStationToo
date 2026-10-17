# WebGPU runtime spike (M4)

A standalone Emscripten program that answers the platform questions of M4's opening task
(`docs/web-port/PLAN.md`) before any `GSDeviceWebGPU` code is written: can a pthread that behaves
like MTGS (plain loops, blocking waits) own a WebGPU device through emdawnwebgpu, show frames, read
textures back synchronously and survive device loss? It does not touch the emulator.

- `spike.cpp`: `main()` runs on a pthread (`-sPROXY_TO_PTHREAD`), like the CPU thread, and starts a
  "GS" pthread that owns the device. Tests: `present`, `bitmap`, `idle`, `readback`, `flush`,
  `longjmp`, `bench`, `lost` (all of these are `all`), plus `crash` and `cbsuspend` on request.
- `build.sh`: builds the variants (`jspi`, `jspi-bitmap`, `asyncify`, `none`, `jspi-sjlj-js`) into
  `build-wasm/webgpu-spike/`, one `em++` at a time.
- `index.html`: shows `#canvas` (transferred to the GS thread) and `#present` (ImageBitmaps the GS
  thread posts), and reads back what they show (`drawImage` of the canvas, centre pixel).
- `run-headless.mjs`: drives headless Chrome over the DevTools protocol with a throwaway profile,
  prints the console of the page and its workers, takes screenshots on `SHOT`, crashes the GPU
  process on `GPUCRASH` (`Browser.crashGpuProcess`).

## Run

```sh
source ~/emsdk/emsdk_env.sh
tools/wasm/webgpu-spike/build.sh                      # all variants
python3 tools/wasm/serve.py --port 8872 build-wasm/webgpu-spike &
node tools/wasm/webgpu-spike/run-headless.mjs --gpu core --out /tmp/spike \
  --url 'http://127.0.0.1:8872/index.html?build=jspi&tests=all'
node tools/wasm/webgpu-spike/run-headless.mjs --gpu compat --out /tmp/spike-compat \
  --url 'http://127.0.0.1:8872/index.html?build=jspi&tests=present,bitmap,lost&adapter=compat'
```

`?tests=crash` and `?tests=cbsuspend` run alone (the second one kills the GS thread on purpose).

## Headless Chrome on Linux

Measured with Chrome 155 on a Radeon 780M (RADV, Mesa 23.2.1).
All runs use `--headless=new --enable-unsafe-webgpu --enable-features=Vulkan --use-angle=vulkan
--ignore-gpu-blocklist`. Then:

- `--gpu core` adds `--disable-vulkan-surface`. Dawn's Vulkan backend runs on the real GPU
  (`amd`/`rdna-3`, core features, dual-source blending), but Chrome composites in software, so
  screenshots do not show WebGPU canvases. `#present` (ImageBitmaps) does show, and the page reads
  `#canvas` back.
- `--gpu compat` adds `--use-webgpu-adapter=opengles`, and the page asks for
  `featureLevel: "compatibility"`. Dawn's OpenGL ES backend runs through ANGLE on the real GPU.
  Chrome composites on the GPU, so screenshots show both canvases. The adapter has no
  vertex-stage storage buffers, so it is only good for testing the mechanics.

Without `--disable-vulkan-surface`, Chrome's own Vulkan fails to start (`vkCreateInstance() failed:
-7`, missing surface extension under headless). WebGPU then offers only SwiftShader, and canvases
fail (`Could not find SharedImageBackingFactory ... WebgpuSwapChainTexture`). In a fresh profile,
the first `requestAdapter` can also return null ("Failed to create WebGPU Context Provider"); a
retry 0.5 s later succeeds.

## Results (2026-10-16, Emscripten 6.0.12, emdawnwebgpu v20260423.175430)

| Question | Result |
| --- | --- |
| Device on a pthread | Works with `-sJSPI`. `wgpuInstanceWaitAny` with a timeout needs the `TimedWaitAny` instance feature, which emdawnwebgpu only grants with JSPI or Asyncify. `requestAdapter` takes 50–60 ms, `requestDevice` 13–17 ms. Without either, polling never sees a promise settle (4.9 M `ProcessEvents` in 2 s, no adapter): the thread has to return to the event loop and continue in callbacks. |
| Canvas on the GS thread | `-sOFFSCREENCANVAS_SUPPORT` plus `emscripten_pthread_attr_settransferredcanvases(&attr, "#canvas")` from the PROXY_TO_PTHREAD main thread works. No `$GL` force-include is needed. An `OffscreenCanvas` the thread creates itself and registers in `specialHTMLTargets` also works as a surface, without `OFFSCREENCANVAS_SUPPORT`. |
| Frames from a loop that never yields | A transferred canvas shows nothing until the worker's task ends (grey during a 1.5 s blocking wait, red after one yield). Per-frame yields: a microtask shows nothing (1 of 60 frames); `setTimeout(0)` shows 58–60 of 60 but costs 4.1 ms (nesting clamp); a `MessageChannel` round trip shows 54–58 of 60 for 0.3–0.4 ms; worker `requestAnimationFrame` waits for vsync. Posting `transferToImageBitmap()` frames to a `bitmaprenderer` canvas shows 59–60 of 60 with **no yield at all**, for 0.08 ms per frame. |
| Suspending idle wait | `emscripten_atomic_wait_suspending` wakes in 0.06 ms median (blocking `Atomics.wait`: 0.03 ms), and WebGPU callbacks fire while it waits; they do not during a blocking wait. |
| Submit without yielding | A 30–40 ms compute dispatch submitted before a 100 ms blocking wait is finished when the wait ends: commands reach the GPU without a yield. |
| Readback | `mapAsync` + `WaitAny`: 2.3–2.5 ms median for 256×256 RGBA8, 2.8–3.0 ms for 1024×1024, data correct. Callbacks plus yields take the same time. |
| Flags together | `-pthread -sPROXY_TO_PTHREAD -sSUPPORT_LONGJMP=wasm -sJSPI -sALLOW_MEMORY_GROWTH` work together, including `longjmp` across a suspension. With `-sSUPPORT_LONGJMP=emscripten`, JSPI traps ("trying to suspend JS frames"). Asyncify also works but makes the wasm 72 % bigger (151 KB vs 88 KB). Suspending inside a WebGPU callback traps under JSPI ("without WebAssembly.promising") and corrupts memory under Asyncify. |
| Device loss | `wgpuDeviceDestroy` calls the lost callback synchronously (reason Destroyed). A GPU process crash reports reason Unknown 130–630 ms later, only through the event loop. A new adapter and device take 0.06–0.7 s to get. After a GPU process crash, the transferred canvas stays blank in Chrome, even in plain JS; a new `OffscreenCanvas` through the ImageBitmap path recovers. |
