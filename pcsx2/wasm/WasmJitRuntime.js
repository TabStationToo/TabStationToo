// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// JS side of runtime WebAssembly code generation (--js-library of pcsx2-wasm and pcsx2-headless, see
// pcsx2/CMakeLists.txt). Every function runs on the thread that calls it: each pthread is a worker with its own
// copy of this state, its own instance of the main module and its own function table. So a module is compiled,
// put in the table, called and released on one thread (docs/web-port/VU1-JIT-DESIGN.md section 3.5). Objects
// (modules, instances) are kept under small integer handles that the C++ side holds (pcsx2/wasm/WasmJitRuntime.h).
// Started from tools/wasm/wasm-jit-spike/jit_runtime.js.

addToLibrary({
	$WasmJit: {
		objs: new Map(),
		next: 1,
		put(o) {
			const id = WasmJit.next++;
			WasmJit.objs.set(id, o);
			return id;
		},
		get(id) {
			const o = WasmJit.objs.get(id);
			if (!o)
				throw new Error(`no WasmJit object ${id}`);
			return o;
		},
	},

	// Compiles [ptr, ptr + len) of the wasm memory with the synchronous WebAssembly.Module constructor (allowed on
	// workers for any size). The bytes are copied first: WebAssembly.Module does not take views of a
	// SharedArrayBuffer. Returns a module handle, 0 if the bytes do not compile (the error is printed), or -1 if the
	// engine is out of executable memory (RangeError), so that the caller can flush its cache and retry.
	wasmjit_compile__deps: ['$WasmJit', '$HEAPU8'],
	wasmjit_compile: (ptr, len) => {
		try {
			return WasmJit.put(new WebAssembly.Module(HEAPU8.slice(ptr, ptr + len)));
		} catch (e) {
			if (e instanceof RangeError)
				return -1;
			err(`wasmjit_compile(${len} bytes): ${e}`);
			return 0;
		}
	},

	// 1 if [ptr, ptr + len) is a valid module (WebAssembly.validate), else 0. For debugging generated code.
	wasmjit_validate__deps: ['$HEAPU8'],
	wasmjit_validate: (ptr, len) => (WebAssembly.validate(HEAPU8.slice(ptr, ptr + len)) ? 1 : 0),

	// Instantiates a module. The import object offers everything generated code may import:
	//   env.mem          the shared memory (declared shared, min 0, max 65536 pages in the module)
	//   env.h_selftest   vu1jit_selftest_helper (pcsx2/wasm/VU1Jit.cpp)
	// Helpers are the main module's own exports, so generated code calls them wasm to wasm, without JS in between.
	// Returns an instance handle, 0 on a link error, -1 on RangeError.
	wasmjit_instantiate__deps: ['$WasmJit', '$wasmMemory', 'vu1jit_selftest_helper'],
	wasmjit_instantiate: (mod) => {
		try {
			const imports = {
				env: {
					mem: wasmMemory,
					h_selftest: _vu1jit_selftest_helper,
				},
			};
			return WasmJit.put(new WebAssembly.Instance(WasmJit.get(mod), imports));
		} catch (e) {
			if (e instanceof RangeError)
				return -1;
			err(`wasmjit_instantiate: ${e}`);
			return 0;
		}
	},

	// Puts export `name` of an instance in this thread's function table (addFunction, -sALLOW_TABLE_GROWTH) and
	// returns its index, which C++ calls as a function pointer. Only valid on this thread. 0 on failure.
	wasmjit_add_function__deps: ['$WasmJit', '$addFunction', '$UTF8ToString'],
	wasmjit_add_function: (inst, name) => {
		try {
			return addFunction(WasmJit.get(inst).exports[UTF8ToString(name)]);
		} catch (e) {
			err(`wasmjit_add_function: ${e}`);
			return 0;
		}
	},

	// Frees a table slot from wasmjit_add_function. removeFunction also drops Emscripten's own references
	// (functionsInTableMap, wasmTableMirror), without which the module would stay alive.
	wasmjit_remove_function__deps: ['$removeFunction'],
	wasmjit_remove_function: (index) => removeFunction(index),

	// Drops the handle of a module or instance; the engine collects it later.
	wasmjit_release__deps: ['$WasmJit'],
	wasmjit_release: (id) => {
		WasmJit.objs.delete(id);
	},

	// Handles alive on this thread.
	wasmjit_live_objects__deps: ['$WasmJit'],
	wasmjit_live_objects: () => WasmJit.objs.size,

	// The engine, for logs: a malloc'd string the caller frees.
	wasmjit_engine__deps: ['$stringToNewUTF8'],
	wasmjit_engine: () => stringToNewUTF8(typeof process == 'object' && process.versions && process.versions.v8
		? `node ${process.version}, V8 ${process.versions.v8}`
		: (typeof navigator == 'object' ? navigator.userAgent : 'unknown engine')),
});
