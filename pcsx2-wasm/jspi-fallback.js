// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Linked into pcsx2-wasm.js with --pre-js, so it runs in the page and in every worker before the
// module is instantiated.
//
// pcsx2-wasm is linked with -sJSPI for the WebGPU device: the GS thread suspends its stack while it
// waits for the GPU (docs/web-port/WEBGPU-DESIGN.md §3.2). Emscripten's generated code then wraps
// imports and exports in WebAssembly.Suspending and WebAssembly.promising unconditionally, so in a
// browser without JSPI the module would not start at all. There, pass-through stand-ins are put in
// their place: nothing ever suspends, and the GS keeps to the 2D path, because the WebGPU device is
// only used when the GS thread's worker has real JSPI (tst_gpu_available() in webgpu_host.js reads
// tstNoJSPI). The functions that would suspend are only called by the WebGPU device.
if (typeof WebAssembly === 'object' &&
	(typeof WebAssembly.Suspending !== 'function' || typeof WebAssembly.promising !== 'function')) {
	globalThis.tstNoJSPI = true;
	// Called with new: returning the function makes `new` return it too.
	WebAssembly.Suspending = function (fn) { return fn; };
	WebAssembly.promising = (fn) => fn;
}
