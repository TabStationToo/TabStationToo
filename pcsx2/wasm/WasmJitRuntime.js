// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// JS side of runtime WebAssembly code generation (--js-library of pcsx2-wasm and pcsx2-headless, see
// pcsx2/CMakeLists.txt). Every function runs on the thread that calls it: each pthread is a worker with its own
// copy of this state, its own instance of the main module and its own function table. So a module is compiled,
// put in the table, called and released on one thread (the EE JIT, pcsx2/wasm/EEJit.cpp; the VU1 JIT, docs/web-port/VU1-JIT-DESIGN.md section 3.5). Objects
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

	// One previous byte stream per length. A length match alone never selects code.
	$VU1Modules: { entries: new Map(), bytes: 0 },
	wasmjit_clear_vu1_modules__deps: ['$VU1Modules'],
	wasmjit_clear_vu1_modules: () => {
		VU1Modules.entries.clear();
		VU1Modules.bytes = 0;
	},
	wasmjit_compile_vu1__deps: ['$VU1Modules', '$WasmJit', '$HEAPU8', 'wasmjit_compile', 'wasmjit_clear_vu1_modules'],
	wasmjit_compile_vu1: (ptr, len) => {
		if (len > 65536)
			return _wasmjit_compile(ptr, len);
		try {
			const heap = HEAPU8;
			const cache = VU1Modules.entries;
			const old = cache.get(len);
			if (old) {
				const bytes = old.bytes;
				let i = 0;
				while (i < len && bytes[i] === heap[ptr + i])
					i++;
				cache.delete(len);
				if (i === len) {
					cache.set(len, old);
					return WasmJit.put(old.module);
				}
				VU1Modules.bytes -= len;
			}
			const bytes = heap.slice(ptr, ptr + len);
			const module = new WebAssembly.Module(bytes);
			while (cache.size >= 256 || VU1Modules.bytes + len > 4 * 1024 * 1024) {
				const oldest = cache.keys().next().value;
				cache.delete(oldest);
				VU1Modules.bytes -= oldest;
			}
			cache.set(len, { bytes, module });
			VU1Modules.bytes += len;
			return WasmJit.put(module);
		} catch (e) {
			_wasmjit_clear_vu1_modules();
			if (e instanceof RangeError)
				return -1;
			err(`wasmjit_compile_vu1(${len} bytes): ${e}`);
			return 0;
		}
	},

	// 1 if [ptr, ptr + len) is a valid module (WebAssembly.validate), else 0. For debugging generated code.
	wasmjit_validate__deps: ['$HEAPU8'],
	wasmjit_validate: (ptr, len) => (WebAssembly.validate(HEAPU8.slice(ptr, ptr + len)) ? 1 : 0),

	// Instantiates a module. The import object offers everything generated code may import:
	//   env.mem     the shared memory (declared shared, min 0, max 65536 pages in the module)
	//   env.table   this thread's function table: generated code calls C++ helpers and interpreter functions with
	//               call_indirect, their function pointers (table indices) being constants in the code (EE JIT)
	//   env.h_selftest   vu1jit_selftest_helper (pcsx2/wasm/VU1Jit.cpp)
	//   env.h_kick, env.h_upper, env.h_lower, env.h_upper0, env.h_lower0   the VU JITs' helpers
	//               (pcsx2/wasm/VU1JitEmit.cpp), main module exports called wasm to wasm
	// Returns an instance handle, 0 on a link error, -1 on RangeError.
	wasmjit_instantiate__deps: ['$WasmJit', '$wasmMemory', '$wasmTable', 'vu1jit_selftest_helper', 'vu1jit_h_kick', 'vu1jit_h_upper', 'vu1jit_h_lower', 'vu1jit_h_upper0', 'vu1jit_h_lower0'],
	wasmjit_instantiate: (mod) => {
		try {
			const imports = {
				env: {
					mem: wasmMemory,
					table: wasmTable,
					h_selftest: _vu1jit_selftest_helper,
					h_kick: _vu1jit_h_kick,
					h_upper: _vu1jit_h_upper,
					h_lower: _vu1jit_h_lower,
					h_upper0: _vu1jit_h_upper0,
					h_lower0: _vu1jit_h_lower0,
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
