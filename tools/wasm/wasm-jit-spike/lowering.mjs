// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Compiles a module the spike dumped (dump=DIR) on its own, with a fresh shared memory, and calls
// its exports so that V8 compiles them; run it with V8's code printing to see the machine code:
//
//   node --no-liftoff --print-wasm-code lowering.mjs DIR/simdops.wasm [export...]
//   node --no-liftoff --print-wasm-code lowering.mjs DIR/kernel-11101.wasm run
//
// Exports taking one i32 get 1 (one loop iteration); others get no arguments. Memory accesses
// use the spike's addresses, so the memory is as big as the spike's initial one (64 MB).

import { readFileSync } from "node:fs";

const [file, ...names] = process.argv.slice(2);
if (!file) {
	console.error("usage: node [--print-wasm-code] lowering.mjs module.wasm [export...]");
	process.exit(2);
}
const mem = new WebAssembly.Memory({ initial: 1024, maximum: 65536, shared: true });
const table = new WebAssembly.Table({ initial: 1024, element: "anyfunc" });
const env = { mem, table, h_add: (a, b) => a + b, h_jmp: () => {}, js_add: (a, b) => a + b };
const module = new WebAssembly.Module(readFileSync(file));
const instance = new WebAssembly.Instance(module, { env });
const exports = WebAssembly.Module.exports(module).filter((e) => e.kind === "function").map((e) => e.name);
for (const name of names.length ? names : exports) {
	const f = instance.exports[name];
	console.log(`== calling ${name}`);
	f.length === 1 ? f(1) : f();
}
