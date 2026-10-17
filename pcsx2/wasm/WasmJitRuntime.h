// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// C++ side of runtime WebAssembly code generation (pcsx2/wasm/WasmJitRuntime.js). A generated module is compiled,
// instantiated, put in the calling thread's function table and called through that slot, all on one thread: each
// pthread has its own instance of the main module and its own table (docs/web-port/VU1-JIT-DESIGN.md 3.5).

#pragma once

#include "common/Pcsx2Types.h"

#include <cstddef>
#include <vector>

extern "C" {
int wasmjit_compile(const void* ptr, size_t len);
int wasmjit_validate(const void* ptr, size_t len);
int wasmjit_instantiate(int module);
int wasmjit_add_function(int instance, const char* name);
void wasmjit_remove_function(int index);
void wasmjit_release(int handle);
int wasmjit_live_objects();
char* wasmjit_engine();
}

namespace WasmJit
{
	enum class LoadResult
	{
		Ok,
		CompileError, // the bytes did not compile or link (a generator bug, or an unsupported engine)
		OutOfMemory, // RangeError: the engine has no executable memory left
		TableError,
	};

	// One generated function, alive in the calling thread's table.
	struct LoadedFunction
	{
		int module = 0;
		int instance = 0;
		int slot = 0; // table index: the function pointer C++ calls

		bool IsLoaded() const { return slot != 0; }
	};

	// Compiles bytes, instantiates them and puts export `name` in this thread's table.
	inline LoadResult Load(const std::vector<u8>& bytes, const char* name, LoadedFunction* out)
	{
		*out = {};
		const int module = wasmjit_compile(bytes.data(), bytes.size());
		if (module <= 0)
			return (module < 0) ? LoadResult::OutOfMemory : LoadResult::CompileError;

		const int instance = wasmjit_instantiate(module);
		if (instance <= 0)
		{
			wasmjit_release(module);
			return (instance < 0) ? LoadResult::OutOfMemory : LoadResult::CompileError;
		}

		const int slot = wasmjit_add_function(instance, name);
		if (slot == 0)
		{
			wasmjit_release(instance);
			wasmjit_release(module);
			return LoadResult::TableError;
		}

		out->module = module;
		out->instance = instance;
		out->slot = slot;
		return LoadResult::Ok;
	}

	// Frees the table slot and drops the handles. Must run on the thread that loaded the function.
	inline void Unload(LoadedFunction* f)
	{
		if (f->slot)
			wasmjit_remove_function(f->slot);
		if (f->instance)
			wasmjit_release(f->instance);
		if (f->module)
			wasmjit_release(f->module);
		*f = {};
	}
} // namespace WasmJit
