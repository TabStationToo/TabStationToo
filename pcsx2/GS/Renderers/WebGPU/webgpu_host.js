// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// JS side of WebGPUHost (WebGPUHost.cpp), linked with --js-library. It runs in the GS thread's
// worker, which owns the WebGPU device (docs/web-port/WEBGPU-DESIGN.md §12).
//
// The GS draws into an OffscreenCanvas of its own and posts each frame to the page as an
// ImageBitmap over a MessagePort (the "frame port"). The port reaches the page once, through
// Emscripten's "call a Module handler on the main thread" worker command (Module.tstFramePort);
// every frame then goes over the port, without returning to the worker's event loop.

addToLibrary({
  $tstGs: {
    canvas: null,
    port: null,
    staging: null, // WGPUBuffer pointer -> { buffer, mapped } (see tst_staging_map)
    frames: 0,
  },

  // The module is linked with -sJSPI; jspi-fallback.js stands in for JSPI where the browser has none,
  // and then the GS must not use WebGPU, whose waits need it.
  tst_gpu_available__deps: [],
  tst_gpu_available: () => (typeof navigator !== 'undefined' && !!navigator.gpu && !globalThis.tstNoJSPI) ? 1 : 0,

  tst_canvas_create__deps: ['$tstGs', '$specialHTMLTargets', '$UTF8ToString'],
  tst_canvas_create: (selector, width, height) => {
    if (typeof OffscreenCanvas !== 'function') return 0;
    const canvas = new OffscreenCanvas(width, height);
    tstGs.canvas = canvas;
    specialHTMLTargets[UTF8ToString(selector)] = canvas;
    if (!tstGs.port) {
      // The command number is Emscripten's (src/lib/libpthread.js); the page's handler takes the
      // other end of the channel.
      const channel = new MessageChannel();
      tstGs.port = channel.port1;
      postMessage({ cmd: {{{ CMD_CALL_HANDLER }}}, handler: 'tstFramePort', args: [channel.port2] }, [channel.port2]);
    }
    return 1;
  },

  tst_canvas_destroy__deps: ['$tstGs', '$specialHTMLTargets', '$UTF8ToString'],
  tst_canvas_destroy: (selector) => {
    delete specialHTMLTargets[UTF8ToString(selector)];
    tstGs.canvas = null;
  },

  // Returns the milliseconds taken, or -1 when there was nothing to post.
  //
  // Chrome frees the GPU-process side of a WebGPU object (encoders, passes, command buffers, the
  // canvas texture and its view: 5-6 KB a frame here) only when the JS wrapper is garbage collected,
  // in a major collection. The GS worker allocates so little JS that V8 went minutes without one
  // while the GPU process grew (measured: 5.5 KB a frame, given back by a forced collection). So
  // every 300 frames this allocates a 64 MB ArrayBuffer and drops it: V8 counts it as external
  // memory, which makes it start a major collection; the buffer is never touched, so no memory is
  // committed for it. Needs checking in other browsers. When the allocation fails (RangeError: the
  // address space or the engine's limit is short) that nudge is skipped, and logged once.
  tst_post_frame__deps: ['$tstGs'],
  tst_post_frame: (frame, device, vsync) => {
    if (!tstGs.canvas || !tstGs.port) return -1;
    const t0 = performance.now();
    const bitmap = tstGs.canvas.transferToImageBitmap();
    tstGs.port.postMessage({ type: 'frame', frame, device, vsync, bitmap, postedAt: performance.timeOrigin + t0 }, [bitmap]);
    if (++tstGs.frames % 300 === 0) {
      try {
        let pressure = new ArrayBuffer(64 * 1024 * 1024);
        pressure = null;
      } catch (e) {
        if (!tstGs.pressureFailed) {
          tstGs.pressureFailed = true;
          err(`WebGPU: skipping the collection nudge (${e})`);
        }
      }
    }
    return performance.now() - t0;
  },

  // Tells the page that the GS draws through the 2D path: from now on (permanent, it gave up
  // WebGPU after repeated loss) or for this boot (no device could be created).
  tst_post_fallback__deps: ['$tstGs', '$UTF8ToString'],
  tst_post_fallback: (reason, permanent) => {
    const message = { type: 'fallback', reason: UTF8ToString(reason), permanent: !!permanent };
    if (tstGs.port) tstGs.port.postMessage(message);
    else postMessage({ cmd: {{{ CMD_CALL_HANDLER }}}, handler: 'tstGpuFallback', args: [message.reason, message.permanent] });
  },

  // A sleep that suspends the calling thread's stack (JSPI) instead of blocking its worker, so the
  // worker's event loop runs meanwhile. Atomics.waitAsync with a timeout on a word nobody notifies:
  // unlike setTimeout it is not clamped when nested, and unlike emscripten_atomic_wait_suspending()
  // it allocates nothing that outlives the wait.
  tst_sleep_suspending__async: true,
  tst_sleep_suspending__deps: ['$tstGs'],
  tst_sleep_suspending: async (ms) => {
    if (!tstGs.sleepWord) tstGs.sleepWord = new Int32Array(new SharedArrayBuffer(4));
    const wait = Atomics.waitAsync(tstGs.sleepWord, 0, 0, ms);
    if (wait.async) await wait.value;
  },

  // A microtask runs only once the GS thread's JS stack is empty, which happens only when its wasm
  // stack suspends (JSPI); then the worker's event loop runs too. While one is pending, its age is how
  // long the loop has not run. Queues one (and returns 0) if none is pending.
  tst_event_loop_stalled_ms__deps: ['$tstGs'],
  tst_event_loop_stalled_ms: () => {
    const now = performance.now();
    if (tstGs.loopPostedAt)
      return now - tstGs.loopPostedAt;
    tstGs.loopPostedAt = now;
    queueMicrotask(() => { tstGs.loopPostedAt = 0; });
    return 0;
  },

  // Staging buffers (WebGPUHost::Staging*): mapped, written and unmapped here on the GPUBuffer itself
  // (WebGPU.getJsObject is emdawnwebgpu's public JS interop). Through webgpu.h every mapAsync would
  // leave an entry in WebGPU.Internals.futures, which only a timed wait on that future removes.
  // Each map has its own entry object, so a map that ends after its buffer was forgotten (destroyed,
  // or replaced by a buffer at the same address) changes nothing.
  tst_staging_map__deps: ['$tstGs', '$WebGPU'],
  tst_staging_map: (ptr) => {
    if (!tstGs.staging) tstGs.staging = new Map();
    const entry = { buffer: WebGPU.getJsObject(ptr), mapped: false };
    tstGs.staging.set(ptr, entry);
    entry.buffer.mapAsync(GPUMapMode.WRITE).then(() => { entry.mapped = true; }, () => {});
  },

  tst_staging_is_mapped__deps: ['$tstGs'],
  tst_staging_is_mapped: (ptr) => {
    const entry = tstGs.staging && tstGs.staging.get(ptr);
    return entry && entry.mapped ? 1 : 0;
  },

  tst_staging_write__deps: ['$tstGs'],
  tst_staging_write: (ptr, data, size) => {
    const entry = tstGs.staging && tstGs.staging.get(ptr);
    if (!entry || !entry.mapped) return 0;
    entry.mapped = false;
    try {
      new Uint8Array(entry.buffer.getMappedRange(0, size)).set(HEAPU8.subarray(data, data + size));
      entry.buffer.unmap();
      return 1;
    } catch (e) {
      return 0;
    }
  },

  tst_staging_forget__deps: ['$tstGs'],
  tst_staging_forget: (ptr) => {
    if (tstGs.staging) tstGs.staging.delete(ptr);
  },

  tst_now: () => performance.timeOrigin + performance.now(),
});
