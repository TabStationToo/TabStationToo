// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// JS side of the runtime WebAssembly spike (--js-library). Every function runs on the thread that
// calls it: each pthread is a worker with its own copy of this state, its own instance of the main
// module and its own function table. Objects (modules, instances) are kept in JIT.objs under small
// integer handles the C++ side holds.

addToLibrary({
	$JIT: {
		objs: new Map(),
		next: 1,
		put(o) { const id = JIT.next++; JIT.objs.set(id, o); return id; },
		get(id) { const o = JIT.objs.get(id); if (!o) throw new Error(`no JIT object ${id}`); return o; },
		jsHelperCalls: 0,
	},

	// Compiles [ptr, ptr + len) of wasm memory with the synchronous WebAssembly.Module constructor.
	// The bytes are copied first: WebAssembly.Module does not take views of a SharedArrayBuffer.
	// Writes the time spent in ms to *msOut. Returns a module handle, or 0 (error printed).
	jit_compile__deps: ['$JIT', '$HEAPU8', '$HEAPF64'],
	jit_compile: (ptr, len, msOut) => {
		try {
			const t0 = performance.now();
			const bytes = HEAPU8.slice(ptr, ptr + len);
			const t1 = performance.now();
			const m = new WebAssembly.Module(bytes);
			const t2 = performance.now();
			if (msOut) { HEAPF64[msOut >> 3] = t2 - t1; HEAPF64[(msOut >> 3) + 1] = t1 - t0; }
			return JIT.put(m);
		} catch (e) {
			err(`jit_compile(${len} bytes): ${e}`);
			return 0;
		}
	},

	// Instantiates a module. The import object offers everything a generated module may import:
	//   env.mem     the main module's shared memory
	//   env.table   the main module's function table (__indirect_function_table)
	//   env.h_add   jit_helper_add, a C++ function exported by the main module (wasm-to-wasm call)
	//   env.h_jmp   jit_helper_longjmp (longjmps to the jmp_buf it is given)
	//   env.js_add  a JS function with the same signature, for comparison
	jit_instantiate__deps: ['$JIT', '$HEAPF64', '$wasmMemory', '$wasmTable', 'jit_helper_add', 'jit_helper_longjmp'],
	jit_instantiate: (mod, msOut) => {
		try {
			const imports = { env: {
				mem: wasmMemory, table: wasmTable,
				h_add: _jit_helper_add, h_jmp: _jit_helper_longjmp,
				js_add: (a, b) => { JIT.jsHelperCalls++; return (a + b) | 0; },
			} };
			const t0 = performance.now();
			const inst = new WebAssembly.Instance(JIT.get(mod), imports);
			if (msOut) HEAPF64[msOut >> 3] = performance.now() - t0;
			return JIT.put(inst);
		} catch (e) {
			err(`jit_instantiate: ${e}`);
			return 0;
		}
	},

	// Puts export `name` of an instance in this thread's function table with Emscripten's
	// addFunction (needs -sALLOW_TABLE_GROWTH) and returns its index, which C++ calls as a
	// function pointer. Only valid on this thread.
	jit_add_function__deps: ['$JIT', '$addFunction', '$UTF8ToString'],
	jit_add_function: (inst, name) => {
		try {
			const f = JIT.get(inst).exports[UTF8ToString(name)];
			return addFunction(f);
		} catch (e) {
			err(`jit_add_function: ${e}`);
			return 0;
		}
	},
	jit_remove_function__deps: ['$removeFunction'],
	jit_remove_function: (index) => removeFunction(index),

	// Grows this thread's table by n slots without Emscripten's bookkeeping; returns the first one.
	jit_table_reserve__deps: ['$wasmTable'],
	jit_table_reserve: (n) => {
		try { return wasmTable.grow(n); } catch (e) { err(`jit_table_reserve(${n}): ${e}`); return -1; }
	},
	jit_table_set__deps: ['$JIT', '$wasmTable', '$UTF8ToString'],
	jit_table_set: (index, inst, name) => {
		wasmTable.set(index, inst ? JIT.get(inst).exports[UTF8ToString(name)] : null);
	},
	jit_table_size__deps: ['$wasmTable'],
	jit_table_size: () => wasmTable.length,
	// 1 if this thread's table holds a function at index, 0 if null, -1 if out of range.
	jit_table_probe__deps: ['$wasmTable'],
	jit_table_probe: (index) => (index < wasmTable.length ? (wasmTable.get(index) ? 1 : 0) : -1),

	// Calls export `f` (no arguments) of an instance n times from JS: the cost of a C++ -> JS ->
	// generated call when no table slot is used.
	jit_call_from_js__deps: ['$JIT'],
	jit_call_from_js: (inst, n) => {
		const f = JIT.get(inst).exports.f;
		for (let i = 0; i < n; i++) f();
	},

	// One C++ -> JS -> generated call (export `nop`).
	jit_call_export_once__deps: ['$JIT'],
	jit_call_export_once: (inst) => { JIT.get(inst).exports.nop(); },

	jit_release__deps: ['$JIT'],
	jit_release: (id) => { JIT.objs.delete(id); },
	jit_live_objects__deps: ['$JIT'],
	jit_live_objects: () => JIT.objs.size,

	// Full GC if the engine exposes it (node --expose-gc, or Chrome --js-flags=--expose-gc).
	jit_gc: () => {
		if (typeof gc !== 'function') return 0;
		gc(); gc();
		return 1;
	},
	// Resident set size of the process in bytes (Node only), or -1.
	jit_rss: () => (typeof process == 'object' && process.memoryUsage ? process.memoryUsage().rss : -1),

	jit_is_main_runtime_thread: () => (ENVIRONMENT_IS_PTHREAD ? 0 : 1),
	jit_engine__deps: ['$stringToNewUTF8'],
	jit_engine: () => stringToNewUTF8(typeof process == 'object' && process.versions?.v8
		? `node ${process.version}, V8 ${process.versions.v8}`
		: (typeof navigator == 'object' ? navigator.userAgent : 'unknown')),
	jit_js_helper_calls__deps: ['$JIT'],
	jit_js_helper_calls: () => JIT.jsHelperCalls,
});
