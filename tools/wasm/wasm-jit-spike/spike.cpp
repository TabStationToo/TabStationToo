// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Runtime WebAssembly code generation spike for the VU1 JIT (phase 1). It does not touch the
// emulator. main() runs on a pthread (-sPROXY_TO_PTHREAD) like PCSX2's CPU thread; most tests run
// on another pthread, like the MTVU thread. See README.md for the questions and the answers.
//
//   spike <tests> [key=value ...]
//   tests: comma-separated, or "all": basic grow mainsize latency calls many kernel progsize fuzz nan simdops
//   keys:  sizes=50,200,... reps=N modules=N verts=N dump=DIR seconds=S

#include "vu_model.h"
#include "wasm_emitter.h"

#include <emscripten.h>
#include <emscripten/heap.h>
#include <emscripten/threading.h>
#include <pthread.h>
#include <setjmp.h>
#include <wasm_simd128.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace wasmjit;

extern "C" {
// jit_runtime.js
int jit_compile(const void* ptr, size_t len, double* ms);
int jit_instantiate(int mod, double* ms);
int jit_add_function(int inst, const char* name);
void jit_remove_function(int index);
int jit_table_reserve(int n);
void jit_table_set(int index, int inst, const char* name);
int jit_table_size();
int jit_table_probe(int index);
void jit_call_from_js(int inst, int n);
void jit_call_export_once(int inst);
void jit_release(int id);
int jit_live_objects();
int jit_gc();
double jit_rss();
int jit_is_main_runtime_thread();
char* jit_engine();
int jit_js_helper_calls();

// C++ helpers generated code calls (exported, and imported by the generated modules).
EMSCRIPTEN_KEEPALIVE int jit_helper_add(int a, int b)
{
	return a + b;
}

EMSCRIPTEN_KEEPALIVE void jit_helper_longjmp(void* buf)
{
	longjmp(*static_cast<jmp_buf*>(buf), 7);
}
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Utilities

static std::map<std::string, std::string> g_args;

static std::string Arg(const char* key, const char* def)
{
	auto it = g_args.find(key);
	return it == g_args.end() ? def : it->second;
}

static long ArgInt(const char* key, long def)
{
	auto it = g_args.find(key);
	return it == g_args.end() ? def : strtol(it->second.c_str(), nullptr, 0);
}

static std::vector<long> ArgList(const char* key, const char* def)
{
	std::vector<long> out;
	std::string s = Arg(key, def);
	for (size_t p = 0; p < s.size();)
	{
		size_t q = s.find(',', p);
		if (q == std::string::npos)
			q = s.size();
		out.push_back(strtol(s.substr(p, q - p).c_str(), nullptr, 0));
		p = q + 1;
	}
	return out;
}

static double Now()
{
	return emscripten_get_now();
}

static double Median(std::vector<double> v)
{
	if (v.empty())
		return 0;
	std::sort(v.begin(), v.end());
	return v.size() & 1 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}

static double Min(const std::vector<double>& v)
{
	return v.empty() ? 0 : *std::min_element(v.begin(), v.end());
}

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		g_failures++;
}

static void* ThreadEntry(void* p)
{
	(*static_cast<std::function<void()>*>(p))();
	return nullptr;
}

// Runs fn on a new pthread and waits for it.
static void OnPthread(std::function<void()> fn)
{
	pthread_t th;
	pthread_create(&th, nullptr, ThreadEntry, &fn);
	pthread_join(th, nullptr);
}

static std::function<void()>* g_main_fn;
static void MainTrampoline()
{
	(*g_main_fn)();
}

// Runs fn on the main runtime thread (the page's thread, or Node's main thread) and waits.
static void OnMainRuntimeThread(std::function<void()> fn)
{
	g_main_fn = &fn;
	emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, MainTrampoline);
	g_main_fn = nullptr;
}

static const char* ThreadName()
{
	if (jit_is_main_runtime_thread())
		return "main runtime thread";
	return pthread_self() == emscripten_main_runtime_thread_id() ? "main runtime thread" : "pthread";
}

struct Loaded
{
	int mod = 0, inst = 0;
	double compile_ms = 0, copy_ms = 0, instantiate_ms = 0;
	explicit operator bool() const { return inst != 0; }
	void Release()
	{
		if (inst)
			jit_release(inst);
		if (mod)
			jit_release(mod);
		inst = mod = 0;
	}
};

static Loaded Load(const void* bytes, size_t len)
{
	Loaded l;
	double t[2] = {};
	l.mod = jit_compile(bytes, len, t);
	l.compile_ms = t[0];
	l.copy_ms = t[1];
	if (!l.mod)
		return l;
	l.inst = jit_instantiate(l.mod, &l.instantiate_ms);
	return l;
}

static Loaded Load(const std::vector<u8>& bytes)
{
	return Load(bytes.data(), bytes.size());
}

static void Dump(const char* name, const std::vector<u8>& bytes)
{
	const std::string dir = Arg("dump", "");
	if (dir.empty())
		return;
	const std::string path = dir + "/" + name;
	if (FILE* fp = fopen(path.c_str(), "wb"))
	{
		fwrite(bytes.data(), 1, bytes.size(), fp);
		fclose(fp);
		printf("  dumped %s (%zu bytes)\n", path.c_str(), bytes.size());
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// The test module: imports env.mem, env.table, env.h_add, env.h_jmp, env.js_add.

static volatile u32 g_probe[4];

struct TestTypes
{
	u32 v_v, ii_i, i_v, ii_v, i_i;
};

static std::vector<u8> BuildTestModule()
{
	Module m;
	TestTypes t;
	t.v_v = m.type({}, {});
	t.ii_i = m.type({ValType::I32, ValType::I32}, {ValType::I32});
	t.i_v = m.type({ValType::I32}, {});
	t.ii_v = m.type({ValType::I32, ValType::I32}, {});
	t.i_i = m.type({ValType::I32}, {ValType::I32});
	m.import_memory("env", "mem", 0, 65536, true);
	m.import_table("env", "table", 0);
	const u32 h_add = m.import_function("env", "h_add", t.ii_i);
	const u32 h_jmp = m.import_function("env", "h_jmp", t.i_v);
	const u32 js_add = m.import_function("env", "js_add", t.ii_i);
	const s32 add_ptr = static_cast<s32>(reinterpret_cast<intptr_t>(&jit_helper_add));
	const u32 probe = static_cast<u32>(reinterpret_cast<uintptr_t>(&g_probe[0]));

	{ // f: g_probe[0] = 0x12340000 + h_add(1, 2)
		Function f(0);
		f.i32_const(0);
		f.i32_const(1);
		f.i32_const(2);
		f.call(h_add);
		f.i32_const(0x12340000);
		f.i32_add();
		f.i32_store(probe);
		m.export_function("f", m.add_function(t.v_v, f));
	}
	{ // add_tbl(a, b): jit_helper_add through call_indirect on the imported table
		Function f(2);
		f.local_get(0);
		f.local_get(1);
		f.i32_const(add_ptr);
		f.call_indirect(t.ii_i, 0);
		m.export_function("add_tbl", m.add_function(t.ii_i, f));
	}
	{ // store(addr, value)
		Function f(2);
		f.local_get(0);
		f.local_get(1);
		f.i32_store(0);
		m.export_function("store", m.add_function(t.ii_v, f));
	}
	{ // load(addr)
		Function f(1);
		f.local_get(0);
		f.i32_load(0);
		m.export_function("load", m.add_function(t.i_i, f));
	}
	{ // jmp(buf): calls jit_helper_longjmp(buf), which longjmps through this frame
		Function f(1);
		f.local_get(0);
		f.call(h_jmp);
		m.export_function("jmp", m.add_function(t.i_v, f));
	}
	{ // nop
		Function f(0);
		m.export_function("nop", m.add_function(t.v_v, f));
	}
	auto loop_calls = [&](const char* name, int how) {
		// (n) -> acc: acc = helper(acc, 1) n times; how 0 = import, 1 = table, 2 = JS, 3 = inline add
		Function f(1);
		const u32 acc = f.local(ValType::I32);
		f.block();
		f.local_get(0);
		f.i32_eqz();
		f.br_if(0);
		f.loop();
		f.local_get(acc);
		f.i32_const(1);
		if (how == 0)
			f.call(h_add);
		else if (how == 1)
		{
			f.i32_const(add_ptr);
			f.call_indirect(t.ii_i, 0);
		}
		else if (how == 2)
			f.call(js_add);
		else
			f.i32_add();
		f.local_set(acc);
		f.local_get(0);
		f.i32_const(1);
		f.i32_sub();
		f.local_tee(0);
		f.br_if(0);
		f.end();
		f.end();
		f.local_get(acc);
		m.export_function(name, m.add_function(t.i_i, f));
	};
	loop_calls("loop_imp", 0);
	loop_calls("loop_tbl", 1);
	loop_calls("loop_js", 2);
	loop_calls("loop_none", 3);
	return m.finish();
}

using VoidFn = void (*)();
using IntIntFn = int (*)(int, int);
using IntFn = int (*)(int);
using StoreFn = void (*)(u32, u32);
using LoadFn = u32 (*)(u32);
using JmpFn = void (*)(void*);

template <typename T>
static T TableFn(int index)
{
	return reinterpret_cast<T>(static_cast<intptr_t>(index));
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// basic: compile, instantiate, register and call generated code on this thread.

static bool BasicHere(const char* where)
{
	const int fail0 = g_failures;
	printf("[basic] on the %s (%s)\n", where, ThreadName());
	const std::vector<u8> bytes = BuildTestModule();
	Loaded l = Load(bytes);
	Check(l.mod != 0, "new WebAssembly.Module on the module's bytes");
	Check(l.inst != 0, "new WebAssembly.Instance with env.mem (shared memory) and env.table");
	if (!l)
		return false;
	printf("  %zu bytes: compile %.3f ms (+ copy %.3f ms), instantiate %.3f ms\n", bytes.size(), l.compile_ms, l.copy_ms, l.instantiate_ms);

	const int table0 = jit_table_size();
	const int f_idx = jit_add_function(l.inst, "f");
	Check(f_idx > 0, "addFunction(instance.exports.f) gives a table index");
	g_probe[0] = 0;
	TableFn<VoidFn>(f_idx)();
	Check(g_probe[0] == 0x12340003, "C++ calls it through the function pointer; it called jit_helper_add (import) and wrote memory");

	const int add_idx = jit_add_function(l.inst, "add_tbl");
	Check(TableFn<IntIntFn>(add_idx)(40, 2) == 42, "call_indirect from generated code into C++ through the imported table");

	const int jmp_idx = jit_add_function(l.inst, "jmp");
	jmp_buf buf;
	volatile int stage = 0;
	const int r = setjmp(buf);
	if (r == 0)
	{
		stage = 1;
		TableFn<JmpFn>(jmp_idx)(&buf);
		stage = 2;
	}
	Check(stage == 1 && r == 7, "longjmp from a C++ helper through a generated frame back to setjmp (SUPPORT_LONGJMP=wasm)");

	const int base = jit_table_reserve(2);
	jit_table_set(base, l.inst, "f");
	g_probe[0] = 0;
	TableFn<VoidFn>(base)();
	Check(g_probe[0] == 0x12340003, "wasmTable.grow + wasmTable.set without addFunction also works");
	printf("  table: %d entries before, %d after (each thread has its own)\n", table0, jit_table_size());

	jit_table_set(base, 0, nullptr);
	jit_remove_function(f_idx);
	jit_remove_function(add_idx);
	jit_remove_function(jmp_idx);
	l.Release();
	return g_failures == fail0;
}

static void TestBasic()
{
	BasicHere("proxied main thread (CPU thread)");
	OnPthread([] { BasicHere("second pthread (VU thread)"); });
	OnMainRuntimeThread([] { BasicHere("main runtime thread"); });

	// A table index is only meaningful on the thread that set it.
	int idx = 0, inst_here = 0;
	Loaded l = Load(BuildTestModule());
	idx = jit_add_function(l.inst, "nop");
	inst_here = jit_table_probe(idx);
	int there = -2;
	OnPthread([&] { there = jit_table_probe(idx); });
	printf("[basic] slot %d: %s here, %s on another pthread\n", idx, inst_here == 1 ? "set" : "empty",
		there == 1 ? "set" : there == 0 ? "null" : "out of range");
	Check(inst_here == 1 && there != 1, "a slot filled on one thread is not filled on another");
	jit_remove_function(idx);
	l.Release();
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// grow: memory growth by another thread while generated code is live.

static void TestGrow()
{
	OnPthread([] {
		printf("[grow] heap %zu MB at start\n", emscripten_get_heap_size() >> 20);
		const std::vector<u8> bytes = BuildTestModule();
		Loaded l = Load(bytes);
		const int store_idx = jit_add_function(l.inst, "store");
		const int load_idx = jit_add_function(l.inst, "load");
		const size_t before = emscripten_get_heap_size();

		// Another thread grows the memory (like the emulator allocating while VU1 runs).
		u8* big = nullptr;
		const size_t big_size = static_cast<size_t>(ArgInt("growmb", 768)) << 20;
		OnPthread([&] {
			big = static_cast<u8*>(malloc(big_size));
		});
		const size_t after = emscripten_get_heap_size();
		printf("[grow] another pthread allocated %zu MB: heap %zu -> %zu MB, block at 0x%08" PRIxPTR "\n", big_size >> 20,
			before >> 20, after >> 20, reinterpret_cast<uintptr_t>(big));
		Check(big && after > before, "the memory grew");
		if (!big)
			return;
		u32* high = reinterpret_cast<u32*>(big + big_size - 64);
		const u32 addr = static_cast<u32>(reinterpret_cast<uintptr_t>(high));
		Check(addr >= before, "the probe address is above the old memory size");
		TableFn<StoreFn>(store_idx)(addr, 0xc0ffee);
		Check(*high == 0xc0ffee, "an instance created before the growth writes above the old size");
		*high = 0xbeef;
		Check(TableFn<LoadFn>(load_idx)(addr) == 0xbeef, "and reads what C++ wrote there");

		// Compile from bytes that are themselves above the old size: this worker's heap views must
		// have been refreshed (Emscripten's GROWABLE_HEAP_* wrappers in library code).
		u8* code = big + big_size - (bytes.size() + 4096);
		memcpy(code, bytes.data(), bytes.size());
		Loaded l2 = Load(code, bytes.size());
		Check(l2.inst != 0, "a module compiled from bytes above the old size instantiates");
		const int f_idx = jit_add_function(l2.inst, "f");
		g_probe[0] = 0;
		TableFn<VoidFn>(f_idx)();
		Check(g_probe[0] == 0x12340003, "and runs");
		OnMainRuntimeThread([&] {
			Loaded l3 = Load(code, bytes.size());
			Check(l3.inst != 0, "the main runtime thread compiles the same bytes above the old size");
			l3.Release();
		});
		jit_remove_function(store_idx);
		jit_remove_function(load_idx);
		jit_remove_function(f_idx);
		l.Release();
		l2.Release();
		free(big);
	});
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Straight-line VU-like code of about n instructions: 8 v128 registers loaded from memory,
// groups of r[a] = r[b] * r[c] + r[d], stored back. seed makes each module's bytes unique.

static void EmitStraightLine(Function& f, u32 n, u32 seed, u32 state)
{
	u32 r[8];
	for (int i = 0; i < 8; i++)
		r[i] = f.local(ValType::V128);
	for (int i = 0; i < 8; i++)
	{
		f.i32_const(0);
		f.v128_load(state + i * 16);
		f.local_set(r[i]);
	}
	f.local_get(r[0]);
	f.v128_const_i32x4(seed, seed * 3 + 1, seed * 5 + 2, seed * 7 + 3);
	f.simd(Simd::V128Xor);
	f.local_set(r[0]);
	for (u32 g = 0; f.instructions() + 6 + 24 <= n; g++)
	{
		f.local_get(r[(g + 1) & 7]);
		f.local_get(r[(g + 2) & 7]);
		f.simd(Simd::F32x4Mul);
		f.local_get(r[(g + 5) & 7]);
		f.simd(Simd::F32x4Add);
		f.local_set(r[g & 7]);
	}
	for (int i = 0; i < 8; i++)
	{
		f.i32_const(0);
		f.local_get(r[i]);
		f.v128_store(state + i * 16);
	}
}

static std::vector<u8> BuildStraightLine(u32 n, u32 seed, u32 state, u32 funcs = 1, u32* instrs = nullptr)
{
	Module m;
	const u32 t = m.type({}, {});
	m.import_memory("env", "mem", 0, 65536, true);
	for (u32 i = 0; i < funcs; i++)
	{
		Function f(0);
		EmitStraightLine(f, n, seed * 977 + i, state);
		if (instrs)
			*instrs = f.instructions();
		char name[16];
		snprintf(name, sizeof(name), "f%u", i);
		m.export_function(name, m.add_function(t, f));
	}
	return m.finish();
}

alignas(16) static u32 g_scratch_state[64];

//////////////////////////////////////////////////////////////////////////////////////////////////
// mainsize: synchronous compilation of big modules on the main runtime thread vs a worker.

static void TestMainSize()
{
	const u32 state = static_cast<u32>(reinterpret_cast<uintptr_t>(g_scratch_state));
	for (long kb : ArgList("mainsizes", "4,5,1024,7000,8100,8300,16384"))
	{
		// Functions of at most ~1 MB each (V8 caps a function body at 7654321 bytes).
		const u32 target = static_cast<u32>(kb * 1024);
		u32 funcs = std::max<u32>(1, target / (1u << 20));
		u32 per = target / funcs;
		// 14 bytes per group of 6 instructions in this code
		std::vector<u8> bytes = BuildStraightLine(std::max<u32>(40, per * 3 / 7), 1234 + kb, state, funcs);
		bool main_ok = false, worker_ok = false;
		double main_ms = 0, worker_ms = 0;
		OnMainRuntimeThread([&] {
			Loaded l = Load(bytes);
			main_ok = l.inst != 0;
			main_ms = l.compile_ms;
			l.Release();
		});
		{
			Loaded l = Load(bytes);
			worker_ok = l.inst != 0;
			worker_ms = l.compile_ms;
			l.Release();
		}
		printf("[mainsize] %8zu bytes: main runtime thread %s (%.2f ms), pthread %s (%.2f ms)\n", bytes.size(),
			main_ok ? "ok" : "REFUSED", main_ms, worker_ok ? "ok" : "REFUSED", worker_ms);
	}
}

struct Timing
{
	double early_ns = 0, late_ns = 0; // per call: first batches, last batches
	long calls = 0;
	double tier_ms = -1; // time from the first call to the first fast batch
	double tier_calls = -1; // calls made by then
};

// Calls fn repeatedly for `seconds`, in batches; per-call time of the first and last batches.
template <typename Fn>
static Timing TimeCalls(Fn&& fn, double seconds, int batch)
{
	Timing t;
	std::vector<double> per, when;
	const double start = Now();
	while (Now() - start < seconds * 1000)
	{
		const double b0 = Now();
		for (int i = 0; i < batch; i++)
			fn();
		const double b1 = Now();
		per.push_back((b1 - b0) * 1e6 / batch);
		when.push_back(b1 - start);
		t.calls += batch;
	}
	const size_t k = std::min<size_t>(per.size(), 3);
	t.early_ns = Median(std::vector<double>(per.begin(), per.begin() + k));
	t.late_ns = Median(std::vector<double>(per.end() - std::min<size_t>(per.size(), 20), per.end()));
	for (size_t i = 0; i < per.size(); i++)
		if (per[i] < 0.6 * t.early_ns)
		{
			t.tier_ms = when[i];
			t.tier_calls = double(batch) * (i + 1);
			break;
		}
	return t;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// latency: compile/instantiate/first call/tier-up vs module size.

static void TestLatency()
{
	OnPthread([] {
		const u32 state = static_cast<u32>(reinterpret_cast<uintptr_t>(g_scratch_state));
		for (int i = 0; i < 32; i++)
			g_scratch_state[i] = 0x3f800000 + i;
		const double seconds = ArgInt("seconds", 1000) / 1000.0;
		printf("[latency] engine: %s\n", jit_engine());
		printf("[latency] %6s %7s %8s %8s %8s %9s %9s %10s %10s %8s %9s %8s\n", "instrs", "bytes", "compile", "inst", "table",
			"1st call", "2nd call", "early/call", "late/call", "tier-up", "at call", "after ms");
		static u32 seed = 1;
		for (long n : ArgList("sizes", "50,200,1000,4000,16000,64000"))
		{
			const int reps = static_cast<int>(ArgInt("reps", n >= 16000 ? 4 : 10));
			std::vector<double> comp, inst, tab, first, second, early, late, tier_ms, tier_calls;
			u32 instrs = 0;
			size_t size = 0;
			for (int rep = 0; rep < reps; rep++)
			{
				std::vector<u8> bytes = BuildStraightLine(static_cast<u32>(n), seed++, state, 1, &instrs);
				size = bytes.size();
				Loaded l = Load(bytes);
				if (!l)
				{
					printf("  load failed\n");
					return;
				}
				double t0 = Now();
				const int idx = jit_add_function(l.inst, "f0");
				double t1 = Now();
				VoidFn fn = TableFn<VoidFn>(idx);
				fn();
				double t2 = Now();
				fn();
				double t3 = Now();
				comp.push_back(l.compile_ms);
				inst.push_back(l.instantiate_ms);
				tab.push_back(t1 - t0);
				first.push_back(t2 - t1);
				second.push_back(t3 - t2);
				// Batches of calls; per-call time over time. Tier-up = first batch whose per-call
				// time is below 60% of the early batches'.
				const int batch = std::max(1, static_cast<int>(ArgInt("lbatch", 200000) / n));
				const Timing tm = TimeCalls(fn, seconds, batch);
				early.push_back(tm.early_ns);
				late.push_back(tm.late_ns);
				if (tm.tier_ms >= 0)
				{
					tier_ms.push_back(tm.tier_ms);
					tier_calls.push_back(tm.tier_calls);
				}
				jit_remove_function(idx);
				l.Release();
			}
			char tier[32] = "none", tcall[32] = "-", tms[32] = "-";
			if (!tier_calls.empty())
			{
				snprintf(tier, sizeof(tier), "%zu/%d", tier_calls.size(), reps);
				snprintf(tcall, sizeof(tcall), "%.0f", Median(tier_calls));
				snprintf(tms, sizeof(tms), "%.1f", Median(tier_ms));
			}
			printf("[latency] %6u %7zu %8.3f %8.3f %8.3f %9.3f %9.3f %10.1f %10.1f %8s %9s %8s\n", instrs, size, Median(comp), Median(inst), Median(tab),
				Median(first), Median(second), Median(early), Median(late), tier, tcall, tms);
		}
		printf("[latency] times in ms (median of reps); early/late per call in ns (first 5 batches / last 20 batches over %.1f s)\n", seconds);
	});
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// calls: call overhead between C++ and generated code.

__attribute__((noinline)) static void CppNop()
{
}

// Each measured loop is its own non-inlined function, called once per repetition: V8 has no
// on-stack replacement for wasm, so a loop that runs inside one long call keeps the code it
// started with (Liftoff), and only later calls run the optimized code.
static VoidFn volatile g_cpp_fn = CppNop;
static VoidFn volatile g_gen_fn;
static int g_calls_inst;
__attribute__((noinline)) static void LoopCpp(int n) { for (int i = 0; i < n; i++) g_cpp_fn(); }
__attribute__((noinline)) static void LoopGen(int n) { for (int i = 0; i < n; i++) g_gen_fn(); }
__attribute__((noinline)) static void LoopViaJs(int n) { for (int i = 0; i < n; i++) jit_call_export_once(g_calls_inst); }

static void TestCalls()
{
	OnPthread([] {
		Loaded l = Load(BuildTestModule());
		g_calls_inst = l.inst;
		g_gen_fn = TableFn<VoidFn>(jit_add_function(l.inst, "nop"));
		IntFn loops[4] = {TableFn<IntFn>(jit_add_function(l.inst, "loop_imp")), TableFn<IntFn>(jit_add_function(l.inst, "loop_tbl")),
			TableFn<IntFn>(jit_add_function(l.inst, "loop_js")), TableFn<IntFn>(jit_add_function(l.inst, "loop_none"))};
		const int n = static_cast<int>(ArgInt("calls", 5000000));
		volatile int sink = 0;
		auto measure = [&](const char* what, std::function<void()> body) {
			std::vector<double> t;
			for (int rep = 0; rep < 9; rep++)
			{
				const double t0 = Now();
				body();
				t.push_back((Now() - t0) * 1e6 / n);
			}
			printf("[calls] %-62s first %6.2f ns, then median %6.2f ns (min %.2f)\n", what, t[0], Median(std::vector<double>(t.begin() + 1, t.end())), Min(t));
		};
		measure("C++ -> C++ (indirect call, empty function)", [&] { LoopCpp(n); });
		measure("C++ -> generated (indirect call through the table, empty)", [&] { LoopGen(n); });
		measure("C++ -> JS -> generated (library function calling the export)", [&] { LoopViaJs(n); });
		measure("JS loop -> generated export (no C++)", [&] { jit_call_from_js(l.inst, n); });
		measure("generated loop, no call (i32.add)", [&] { sink = loops[3](n); });
		measure("generated -> C++ helper, import of the main module's export", [&] { sink = loops[0](n); });
		measure("generated -> C++ helper, call_indirect on the imported table", [&] { sink = loops[1](n); });
		measure("generated -> JS function import", [&] { sink = loops[2](n); });
		(void)sink;
		l.Release();
	});
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// many: many small modules vs one module with many functions; memory and freeing.

static void TestMany()
{
	OnPthread([] {
		const u32 state = static_cast<u32>(reinterpret_cast<uintptr_t>(g_scratch_state));
		const long count = ArgInt("modules", 2000);
		const long n = ArgInt("modsize", 300);
		const bool has_gc = jit_gc();
		auto mb = [](double b) { return b / (1024.0 * 1024.0); };
		printf("[many] %ld units of ~%ld instructions; gc() %s\n", count, n, has_gc ? "available" : "not exposed");

		// A: one module per unit; twice, to see whether freed memory is reused.
		for (int round = 1; round <= static_cast<int>(ArgInt("rounds", 2)); round++)
		{
			printf("[many] A round %d\n", round);
			jit_gc();
			const double rss0 = jit_rss();
			const int table0 = jit_table_size();
			std::vector<Loaded> mods;
			std::vector<int> slots;
			double t_build = 0, t_load = 0, t_table = 0, t_call = 0;
			size_t bytes_total = 0;
			for (long i = 0; i < count; i++)
			{
				double t0 = Now();
				std::vector<u8> bytes = BuildStraightLine(static_cast<u32>(n), 100000 + i, state);
				double t1 = Now();
				Loaded l = Load(bytes);
				double t2 = Now();
				if (!l)
				{
					printf("[many] module %ld failed\n", i);
					break;
				}
				const int idx = jit_add_function(l.inst, "f0");
				double t3 = Now();
				TableFn<VoidFn>(idx)();
				double t4 = Now();
				t_build += t1 - t0;
				t_load += t2 - t1;
				t_table += t3 - t2;
				t_call += t4 - t3;
				bytes_total += bytes.size();
				mods.push_back(l);
				slots.push_back(idx);
			}
			const double rss1 = jit_rss();
			printf("[many] A one module each: emit %.1f us, compile+instantiate %.1f us, addFunction %.1f us, first call %.1f us per unit; %.0f bytes/module\n",
				t_build * 1e3 / count, t_load * 1e3 / count, t_table * 1e3 / count, t_call * 1e3 / count, double(bytes_total) / count);
			if (rss0 > 0)
				printf("[many] A RSS %.1f -> %.1f MB: %.1f KB per module (table %d -> %d)\n", mb(rss0), mb(rss1), (rss1 - rss0) / 1024 / count, table0, jit_table_size());
			const double tf0 = Now();
			for (int idx : slots)
				jit_remove_function(idx);
			for (Loaded& l : mods)
				l.Release();
			mods.clear();
			const double tf1 = Now();
			jit_gc();
			const double tf2 = Now();
			const double rss2 = jit_rss();
			printf("[many] A freed: removeFunction+release %.1f us/unit, gc %.1f ms; RSS %.1f MB (%.1f MB returned), live JS handles %d\n",
				(tf1 - tf0) * 1e3 / count, tf2 - tf1, mb(rss2), mb(rss1 - rss2), jit_live_objects());
			// Some of V8's wasm code memory goes back only after later GCs: look again.
			emscripten_thread_sleep(500);
			jit_gc();
			printf("[many] A after 0.5 s and another gc: RSS %.1f MB\n", mb(jit_rss()));
		}
		// B: one module holding every unit.
		{
			jit_gc();
			const double rss0 = jit_rss();
			double t0 = Now();
			std::vector<u8> bytes = BuildStraightLine(static_cast<u32>(n), 777, state, static_cast<u32>(count));
			double t1 = Now();
			Loaded l = Load(bytes);
			double t2 = Now();
			std::vector<int> slots;
			char name[16];
			for (long i = 0; i < count; i++)
			{
				snprintf(name, sizeof(name), "f%ld", i);
				slots.push_back(jit_add_function(l.inst, name));
			}
			double t3 = Now();
			for (int idx : slots)
				TableFn<VoidFn>(idx)();
			double t4 = Now();
			const double rss1 = jit_rss();
			printf("[many] B one module, %ld functions (%zu bytes): emit %.1f ms, compile %.1f ms + instantiate %.1f ms, addFunction %.1f us/unit, first calls %.1f us/unit\n",
				count, bytes.size(), t1 - t0, l.compile_ms, l.instantiate_ms, (t3 - t2) * 1e3 / count, (t4 - t3) * 1e3 / count);
			if (rss0 > 0)
				printf("[many] B RSS %.1f -> %.1f MB: %.1f KB per function\n", mb(rss0), mb(rss1), (rss1 - rss0) / 1024 / count);
			for (int idx : slots)
				jit_remove_function(idx);
			l.Release();
			jit_gc();
			emscripten_thread_sleep(500);
			jit_gc();
			printf("[many] B freed: RSS %.1f MB\n", mb(jit_rss()));
		}
	});
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// The vertex-transform microprogram, its state, and the native SIMD reference.

static std::vector<vu::Insn> KernelProgram(int verts, int unroll = 1)
{
	using vu::Insn;
	using vu::Op;
	std::vector<Insn> p;
	auto I = [&](Op op) -> Insn& { p.push_back(Insn{op}); return p.back(); };
	// VI05 input (qwords), VI02 output, VI03 count, VI04 clipped vertices (FCAND writes VI01);
	// VF10-13 matrix, VF20/21 bounds.
	{ Insn& i = I(Op::IADDIU); i.it = 5; i.is = 0; i.imm = 0; }
	{ Insn& i = I(Op::IADDIU); i.it = 2; i.is = 0; i.imm = 512; }
	{ Insn& i = I(Op::IADDIU); i.it = 3; i.is = 0; i.imm = verts; }
	const int loop = static_cast<int>(p.size());
	for (int u = 0; u < unroll; u++)
	{
	{ Insn& i = I(Op::LQI); i.ft = 1; i.is = 5; }
	{ Insn& i = I(Op::MULAbc); i.fs = 10; i.ft = 1; i.bc = 0; }
	{ Insn& i = I(Op::MADDAbc); i.fs = 11; i.ft = 1; i.bc = 1; }
	{ Insn& i = I(Op::MADDAbc); i.fs = 12; i.ft = 1; i.bc = 2; }
	{ Insn& i = I(Op::MADDbc); i.fd = 2; i.fs = 13; i.ft = 1; i.bc = 3; }
	{ Insn& i = I(Op::CLIP); i.fs = 2; i.ft = 2; }
	{ Insn& i = I(Op::MAX); i.fd = 3; i.fs = 2; i.ft = 20; }
	{ Insn& i = I(Op::MINI); i.fd = 3; i.fs = 3; i.ft = 21; }
	{ Insn& i = I(Op::FCAND); i.imm = 0x3f; }
	{ Insn& i = I(Op::IADD); i.id = 4; i.is = 4; i.it = 1; }
	{ Insn& i = I(Op::SQI); i.fs = 3; i.it = 2; }
	{ Insn& i = I(Op::ISUBIU); i.it = 3; i.is = 3; i.imm = 1; }
	}
	{ Insn& i = I(Op::IBNE); i.is = 3; i.it = 0; i.imm = loop; }
	I(Op::END);
	return p;
}

static float RandFloat(std::mt19937& rng, float lo, float hi)
{
	return std::uniform_real_distribution<float>(lo, hi)(rng);
}

static u32 F2U(float f)
{
	u32 v;
	memcpy(&v, &f, 4);
	return v;
}

// A float bit pattern: mostly ordinary values, with a share of specials when special > 0.
static u32 RandomBits(std::mt19937& rng, int special)
{
	const u32 r = rng();
	if (special && (r % 100) < static_cast<u32>(special))
	{
		const u32 sign = rng() & 0x80000000;
		switch (rng() % 9)
		{
			case 0: return sign; // +-0
			case 1: return sign | (rng() & 0x007fffff); // denormal
			case 2: return sign | 0x7f800000; // inf
			case 3: return sign | 0x7f800000 | (1 + rng() % 0x7fffff); // NaN
			case 4: return sign | 0x7f7fffff; // FLT_MAX
			case 5: return sign | 0x00800000; // FLT_MIN
			case 6: return sign | ((0xf0 + rng() % 15) << 23) | (rng() & 0x7fffff); // huge
			case 7: return sign | ((1 + rng() % 12) << 23) | (rng() & 0x7fffff); // tiny
			default: return rng(); // any bits
		}
	}
	return F2U(RandFloat(rng, -1000.f, 1000.f));
}

static void KernelState(vu::State& s, int verts, u32 seed, int special)
{
	std::mt19937 rng(seed);
	vu::ResetState(s);
	for (int r = 1; r < 32; r++)
		for (int l = 0; l < 4; l++)
			s.vf[r][l] = RandomBits(rng, special);
	for (int c = 10; c <= 13; c++)
		for (int l = 0; l < 4; l++)
			s.vf[c][l] = special ? RandomBits(rng, special) : F2U(RandFloat(rng, -2.f, 2.f));
	for (int l = 0; l < 4; l++)
	{
		s.vf[20][l] = F2U(-2000.f);
		s.vf[21][l] = F2U(2000.f);
	}
	for (int l = 0; l < 4; l++)
		s.acc[l] = RandomBits(rng, special);
	for (int v = 0; v < verts; v++)
		for (int l = 0; l < 4; l++)
			reinterpret_cast<u32*>(s.mem)[v * 4 + l] = l == 3 && !special ? F2U(1.f) : RandomBits(rng, special);
	s.mac = rng() & 0xffff;
	s.status = rng() & 0xf;
	s.clip = rng() & 0xffffff;
}

// The kernel in C++ with wasm_simd128.h: the semantics of the JIT's exact mode with dead flags
// elided (only the last MADD's flags are kept), for comparing generated code with clang's.
static inline v128_t Clamp(v128_t x)
{
	v128_t a = wasm_u32x4_min(wasm_v128_and(x, wasm_i32x4_splat(0x7fffffff)), wasm_i32x4_splat(0x7f7fffff));
	a = wasm_v128_and(a, wasm_i32x4_gt(a, wasm_i32x4_splat(0x007fffff)));
	return wasm_v128_or(a, wasm_v128_and(x, wasm_i32x4_splat(0x80000000)));
}

__attribute__((noinline)) static void KernelNative(vu::State* s, int verts)
{
	v128_t m0 = Clamp(wasm_v128_load(s->vf[10])), m1 = Clamp(wasm_v128_load(s->vf[11]));
	v128_t m2 = Clamp(wasm_v128_load(s->vf[12])), m3 = Clamp(wasm_v128_load(s->vf[13]));
	const v128_t lo = wasm_v128_load(s->vf[20]), hi = wasm_v128_load(s->vf[21]);
	v128_t v1 = wasm_v128_load(s->vf[1]), v2 = wasm_v128_load(s->vf[2]), v3 = wasm_v128_load(s->vf[3]);
	v128_t acc = wasm_v128_load(s->acc);
	u32 vi1 = s->vi[1], vi5 = 0, vi2 = 512, vi3 = static_cast<u32>(verts), vi4 = s->vi[4];
	u32 clip = s->clip, mac = s->mac, status = s->status;
	do
	{
		v1 = wasm_v128_load(s->mem + ((vi5 * 16) & 0x3fff));
		vi5 = (vi5 + 1) & 0xffff;
		const v128_t c = Clamp(v1);
		acc = Clamp(wasm_f32x4_mul(m0, wasm_i32x4_shuffle(c, c, 0, 0, 0, 0)));
		acc = Clamp(wasm_f32x4_add(acc, wasm_f32x4_mul(m1, wasm_i32x4_shuffle(c, c, 1, 1, 1, 1))));
		acc = Clamp(wasm_f32x4_add(acc, wasm_f32x4_mul(m2, wasm_i32x4_shuffle(c, c, 2, 2, 2, 2))));
		const v128_t r = wasm_f32x4_add(acc, wasm_f32x4_mul(m3, wasm_i32x4_shuffle(c, c, 3, 3, 3, 3)));
		{
			// flags of the MADD only (the MULA/MADDA flags are dead)
			const v128_t ex = wasm_v128_and(r, wasm_i32x4_splat(0x7f800000));
			const v128_t z = wasm_i32x4_eq(ex, wasm_i32x4_splat(0)); // zero or denormal
			const v128_t sg = wasm_i32x4_lt(r, wasm_i32x4_splat(0));
			const v128_t u = wasm_v128_andnot(z, wasm_i32x4_eq(wasm_v128_and(r, wasm_i32x4_splat(0x7fffffff)), wasm_i32x4_splat(0)));
			const v128_t o = wasm_i32x4_eq(ex, wasm_i32x4_splat(0x7f800000));
			v128_t b = wasm_i8x16_narrow_i16x8(wasm_i16x8_narrow_i32x4(z, sg), wasm_i16x8_narrow_i32x4(u, o));
			b = wasm_i8x16_shuffle(b, b, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
			mac = wasm_i8x16_bitmask(b);
			status = wasm_i32x4_bitmask(wasm_i32x4_ne(b, wasm_i32x4_splat(0)));
		}
		v2 = Clamp(r);
		// CLIP v2, v2.w
		const v128_t w = wasm_i32x4_shuffle(v2, v2, 3, 3, 3, 3);
		const v128_t value = wasm_v128_bitselect(wasm_v128_and(w, wasm_i32x4_splat(0x7fffffff)), wasm_i32x4_splat(0x007fffff),
			wasm_i32x4_ne(wasm_v128_and(w, wasm_i32x4_splat(0x7f800000)), wasm_i32x4_splat(0)));
		const v128_t pn = wasm_i16x8_narrow_i32x4(wasm_i32x4_gt(v2, value), wasm_i32x4_gt(wasm_v128_xor(v2, wasm_i32x4_splat(0x80000000)), value));
		clip = (((clip << 6) | (wasm_i8x16_bitmask(wasm_i8x16_shuffle(pn, pn, 0, 8, 2, 10, 4, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)) & 0x3f)) & 0xffffff);
		// MAX v3, v2, lo; MINI v3, v3, hi
		const v128_t mx = wasm_v128_bitselect(wasm_i32x4_min(v2, lo), wasm_i32x4_max(v2, lo), wasm_i32x4_shr(wasm_v128_and(v2, lo), 31));
		v3 = wasm_v128_bitselect(wasm_i32x4_max(mx, hi), wasm_i32x4_min(mx, hi), wasm_i32x4_shr(wasm_v128_and(mx, hi), 31));
		vi1 = (clip & 0x3f) ? 1 : 0; // FCAND
		vi4 = (vi4 + vi1) & 0xffff;
		wasm_v128_store(s->mem + ((vi2 * 16) & 0x3fff), v3);
		vi2 = (vi2 + 1) & 0xffff;
		vi3 = (vi3 - 1) & 0xffff;
	} while (vi3 != 0);
	wasm_v128_store(s->vf[1], v1);
	wasm_v128_store(s->vf[2], v2);
	wasm_v128_store(s->vf[3], v3);
	wasm_v128_store(s->acc, acc);
	s->vi[1] = vi1;
	s->vi[2] = vi2;
	s->vi[3] = vi3;
	s->vi[4] = vi4;
	s->vi[5] = vi5;
	s->clip = clip;
	s->mac = mac;
	s->status = status;
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// kernel: throughput of the microprogram, interpreter vs generated code vs native C++, and
// bit-exactness against the interpreter.

alignas(16) static vu::State g_ref, g_work, g_init;

static bool SameState(const vu::State& a, const vu::State& b, bool print)
{
	if (memcmp(&a, &b, sizeof(vu::State)) == 0)
		return true;
	if (print)
	{
		const u32* x = reinterpret_cast<const u32*>(&a);
		const u32* y = reinterpret_cast<const u32*>(&b);
		int shown = 0;
		for (size_t i = 0; i < sizeof(vu::State) / 4 && shown < 6; i++)
			if (x[i] != y[i])
			{
				const size_t off = i * 4;
				const char* what = off < offsetof(vu::State, acc) ? "vf" : off < offsetof(vu::State, vi) ? "acc" : off < offsetof(vu::State, mac) ? "vi" : off < offsetof(vu::State, mem) ? "flags" : "mem";
				printf("    differs at +%zu (%s): interp 0x%08x, other 0x%08x\n", off, what, x[i], y[i]);
				shown++;
			}
	}
	return false;
}

static void TestKernel()
{
	OnPthread([] {
		const int verts = static_cast<int>(ArgInt("verts", 64));
		const double warm = ArgInt("warm", 400) / 1000.0;
		const double slice = ArgInt("slice", 150) / 1000.0;
		const int rounds = static_cast<int>(ArgInt("rounds", 7));
		const std::vector<vu::Insn> prog = KernelProgram(verts);
		const u32 state = static_cast<u32>(reinterpret_cast<uintptr_t>(&g_work));
		KernelState(g_init, verts, 1, 0);
		g_ref = g_init;
		const u32 executed = vu::interp::Run(&g_ref, prog.data());
		printf("[kernel] %zu instructions, %d vertices: %u VU instructions per run (%.1f per vertex)\n", prog.size(), verts, executed,
			double(executed) / verts);
		const int batch = std::max(1, 4000 / verts);

		struct Runner
		{
			std::string name;
			std::function<void()> run;
			std::string info;
			Timing first;
			std::vector<double> late;
			int batch = 1;
		};
		std::vector<Runner> runners;
		runners.push_back({"interpreter (replica)", [&] { vu::interp::Run(&g_work, prog.data()); }, ""});
		{
			g_work = g_init;
			KernelNative(&g_work, verts);
			runners.push_back({"native C++ SIMD (clang)", [&] { KernelNative(&g_work, verts); }, SameState(g_ref, g_work, true) ? "bit-exact" : "DIFFERS"});
		}

		struct Variant
		{
			const char* name;
			vu::CompileOptions opt;
		};
		const Variant variants[] = {
			{"jit exact, all flags", {true, true, false, false, false, false}},
			{"jit exact, dead flags elided", {true, true, true, false, false, false}},
			{"jit exact, all flags, cached", {true, true, false, false, true, false}},
			{"jit exact, all flags, tuned", {true, true, false, false, false, true}},
			{"jit exact, all flags, cache+tune", {true, true, false, false, true, true}},
			{"jit exact, elided, cache+tune", {true, true, true, false, true, true}},
			{"jit e+c+t, relaxed madd", {true, true, true, true, true, true}},
			{"jit no clamps, no flags, tuned", {false, false, false, false, false, true}},
		};
		std::vector<Loaded> loaded;
		std::vector<int> slots;
		for (const Variant& v : variants)
		{
			vu::CompileStats cs;
			vu::Compiler c(prog, state, v.opt);
			const std::vector<u8> bytes = c.Build(&cs);
			char dname[64];
			snprintf(dname, sizeof(dname), "kernel-%d%d%d%d%d%d.wasm", v.opt.exact, v.opt.flags, v.opt.elide_dead_flags, v.opt.relaxed_madd,
				v.opt.cache_clamps, v.opt.tuned);
			Dump(dname, bytes);
			Loaded l = Load(bytes);
			if (!l)
			{
				printf("[kernel] %s: load failed\n", v.name);
				g_failures++;
				continue;
			}
			const int idx = jit_add_function(l.inst, "run");
			VoidFn fn = TableFn<VoidFn>(idx);
			loaded.push_back(l);
			slots.push_back(idx);
			g_work = g_init;
			const double t0 = Now();
			fn();
			const double t1 = Now();
			const bool exact_mode = v.opt.exact && !v.opt.relaxed_madd;
			const bool ok = SameState(g_ref, g_work, exact_mode);
			// Special values: denormals, infinities, NaNs, huge and tiny numbers, in every input.
			int bad = 0;
			const int seeds = static_cast<int>(ArgInt("seeds", 300));
			for (int seed = 0; seed < seeds; seed++)
			{
				KernelState(g_work, verts, 1000 + seed, 30);
				g_ref = g_work;
				vu::interp::Run(&g_ref, prog.data());
				fn();
				if (!SameState(g_ref, g_work, bad == 0 && exact_mode))
					bad++;
			}
			g_ref = g_init;
			vu::interp::Run(&g_ref, prog.data());
			if (exact_mode)
				Check(ok && bad == 0, (std::string(v.name) + ": bit-exact with the interpreter").c_str());
			char info[512];
			snprintf(info, sizeof(info), "%zu B, %u wasm instrs, compile %.3f + inst %.3f + 1st run %.3f ms; clamps %u/%u elided, flags %u/%u; %s, special %d/%d differ",
				bytes.size(), cs.wasm_instructions, l.compile_ms, l.instantiate_ms, t1 - t0, cs.clamps_elided, cs.clamps + cs.clamps_elided,
				cs.fmac_with_flags, cs.fmac_total, ok ? "exact" : "DIFFERS", bad, seeds);
			runners.push_back({v.name, [fn] { fn(); }, info});
		}

		// Warm up (and see the tier-up), then interleave short slices so that load from other
		// processes affects every variant alike; report the median of the slices.
		// Batches of ~2 ms: performance.now() is coarse in browsers (5 us when cross-origin isolated).
		for (Runner& r : runners)
		{
			g_work = g_init;
			r.first = TimeCalls(r.run, warm, batch);
			r.batch = std::max(1, static_cast<int>(2e6 / std::max(1.0, r.first.late_ns)));
		}
		for (int round = 0; round < rounds; round++)
			for (Runner& r : runners)
			{
				g_work = g_init;
				r.late.push_back(TimeCalls(r.run, slice, r.batch).late_ns);
			}
		const double interp_ns = Median(runners[0].late);
		printf("[kernel] %-34s %9s %9s %9s %7s %6s %10s %8s\n", "variant", "ns/run", "ns/vert", "ns/instr", "min", "vs int", "first ns", "tier ms");
		for (const Runner& r : runners)
		{
			const double ns = Median(r.late);
			printf("[kernel] %-34s %9.1f %9.2f %9.3f %7.3f %5.1fx %10.0f %8.0f  %s\n", r.name.c_str(), ns, ns / verts, ns / executed, Min(r.late) / executed,
				interp_ns / ns, r.first.early_ns, r.first.tier_ms, r.info.c_str());
		}
		for (size_t i = 0; i < slots.size(); i++)
		{
			jit_remove_function(slots[i]);
			loaded[i].Release();
		}
	});
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// progsize: compile cost of generated VU code vs microprogram length (the kernel's body
// repeated n times in the loop; VU1 micro memory holds at most 2048 instruction pairs).

static void TestProgSize()
{
	OnPthread([] {
		const u32 state = static_cast<u32>(reinterpret_cast<uintptr_t>(&g_work));
		printf("[progsize] %7s %8s %8s %9s %9s %9s %10s %10s %11s\n", "VU ins", "bytes", "wasm ins", "compile", "inst", "1st run", "2nd run",
			"interp run", "speedup");
		for (long unroll : ArgList("unrolls", "1,4,16,64,150"))
		{
			const int verts = static_cast<int>(unroll) * 2;
			const std::vector<vu::Insn> prog = KernelProgram(verts, static_cast<int>(unroll));
			std::vector<double> comp, inst, first, second;
			size_t bytes_size = 0;
			u32 instrs = 0;
			static u32 salt = 1;
			for (int rep = 0; rep < 6; rep++)
			{
				KernelState(g_init, verts, 1, 0);
				vu::CompileStats cs;
				// Unique bytes per module, except the last one: a copy of the previous module's
				// bytes, to see V8's cache of identical modules.
				vu::CompileOptions opt{true, true, true, false, true, true};
				opt.salt = rep == 5 ? salt - 1 : salt++;
				vu::Compiler c(prog, state, opt);
				const std::vector<u8> bytes = c.Build(&cs);
				bytes_size = bytes.size();
				instrs = cs.wasm_instructions;
				Loaded l = Load(bytes);
				const int idx = jit_add_function(l.inst, "run");
				g_work = g_init;
				const double t0 = Now();
				TableFn<VoidFn>(idx)();
				const double t1 = Now();
				TableFn<VoidFn>(idx)();
				const double t2 = Now();
				if (rep == 5)
					printf("[progsize]   same bytes again: compile %.3f, instantiate %.3f, first run %.3f ms\n", l.compile_ms, l.instantiate_ms, t1 - t0);
				else
				{
					comp.push_back(l.compile_ms);
					inst.push_back(l.instantiate_ms);
					first.push_back(t1 - t0);
					second.push_back(t2 - t1);
				}
				if (rep == 4)
				{
					jit_remove_function(idx);
					continue; // keep module 4 alive while its copy is compiled
				}
				jit_remove_function(idx);
				l.Release();
			}
			std::vector<double> it;
			for (int r = 0; r < 50; r++)
			{
				g_work = g_init;
				const double i0 = Now();
				vu::interp::Run(&g_work, prog.data());
				it.push_back(Now() - i0);
			}
			printf("[progsize] %7zu %8zu %8u %9.3f %9.3f %9.3f %10.3f %10.3f %10.1fx\n", prog.size(), bytes_size, instrs, Median(comp), Median(inst),
				Median(first), Median(second), Min(it), Min(it) / std::max(1e-6, Median(second)));
		}
		printf("[progsize] ms, median of 5 modules with unique bytes; exact code, dead flags elided, clamp cache, tuned; a run is 2 x unroll vertices; interp = best of 50 runs\n");
	});
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// fuzz: every instruction of the model, every dest mask and broadcast lane, special operands.

alignas(16) static vu::State g_fz;

static void TestFuzz()
{
	OnPthread([] {
		using vu::Op;
		const u32 state = static_cast<u32>(reinterpret_cast<uintptr_t>(&g_fz));
		const int trials = static_cast<int>(ArgInt("trials", 300));
		const Op ops[] = {Op::MULAbc, Op::MADDAbc, Op::MADDbc, Op::MULbc, Op::ADD, Op::SUB, Op::MUL, Op::MADD, Op::MAX, Op::MINI, Op::CLIP};
		std::mt19937 rng(42);
		int programs = 0, runs = 0, bad = 0;
		double compile_ms = 0;
		for (Op op : ops)
		{
			const bool has_bc = op <= Op::MULbc;
			const int dests = op == Op::CLIP ? 1 : 15;
			for (int d = 1; d <= dests; d++)
				for (int bc = 0; bc < (has_bc ? 4 : 1); bc++)
					for (int variant = 0; variant < 4; variant++)
					{
						const int alias = variant & 1, tuned = variant >> 1;
						vu::Insn in{op};
						in.dest = op == Op::CLIP ? 0xf : static_cast<u8>(d);
						in.bc = static_cast<u8>(bc);
						in.fs = 1;
						in.ft = 2;
						in.fd = alias ? 1 : 3; // fd == fs
						std::vector<vu::Insn> prog = {in, vu::Insn{Op::END}};
						vu::Compiler c(prog, state, {true, true, false, false, false, tuned != 0});
						const std::vector<u8> bytes = c.Build();
						Loaded l = Load(bytes);
						compile_ms += l.compile_ms + l.instantiate_ms;
						const int idx = jit_add_function(l.inst, "run");
						VoidFn fn = TableFn<VoidFn>(idx);
						programs++;
						int prog_bad = 0;
						for (int t = 0; t < trials; t++)
						{
							vu::ResetState(g_fz);
							for (int r = 1; r < 4; r++)
								for (int k = 0; k < 4; k++)
									g_fz.vf[r][k] = RandomBits(rng, 60);
							for (int k = 0; k < 4; k++)
								g_fz.acc[k] = RandomBits(rng, 60);
							g_fz.mac = rng() & 0xffff;
							g_fz.status = rng() & 0xf;
							g_fz.clip = rng() & 0xffffff;
							g_ref = g_fz;
							g_init = g_fz;
							vu::interp::Run(&g_ref, prog.data());
							fn();
							runs++;
							if (!SameState(g_ref, g_fz, prog_bad == 0))
							{
								if (prog_bad == 0)
								{
									printf("  op %d dest %x bc %d alias %d tuned %d differs\n", static_cast<int>(op), d, bc, alias, tuned);
									for (int r = 1; r < 4; r++)
										printf("    in  vf%d %08x %08x %08x %08x\n", r, g_init.vf[r][0], g_init.vf[r][1], g_init.vf[r][2], g_init.vf[r][3]);
									printf("    in  acc %08x %08x %08x %08x\n", g_init.acc[0], g_init.acc[1], g_init.acc[2], g_init.acc[3]);
									for (int r = 1; r < 4; r++)
										printf("    out vf%d %08x %08x %08x %08x | %08x %08x %08x %08x\n", r, g_ref.vf[r][0], g_ref.vf[r][1], g_ref.vf[r][2], g_ref.vf[r][3], g_fz.vf[r][0], g_fz.vf[r][1], g_fz.vf[r][2], g_fz.vf[r][3]);
								}
								prog_bad++;
							}
						}
						bad += prog_bad;
						jit_remove_function(idx);
						l.Release();
					}
		}
		printf("[fuzz] %d single-instruction programs x %d random states (60%% special operands): %d of %d runs differ; compile+instantiate %.3f ms per program\n",
			programs, trials, bad, runs, compile_ms / programs);
		Check(bad == 0, "every instruction, dest mask and broadcast lane is bit-exact with the interpreter");

		// Random straight-line programs over VF01-VF06 and ACC, with loads, compiled with every
		// optimisation (clean-register analysis, clamp cache, dead flag elision).
		const int nprogs = static_cast<int>(ArgInt("progs", 400));
		int pbad = 0, pruns = 0;
		const Op pool[] = {Op::MULAbc, Op::MADDAbc, Op::MADDbc, Op::MULbc, Op::ADD, Op::SUB, Op::MUL, Op::MADD, Op::MAX, Op::MINI, Op::CLIP, Op::LQI};
		for (int p = 0; p < nprogs; p++)
		{
			std::vector<vu::Insn> prog;
			const int len = 8 + static_cast<int>(rng() % 17);
			// Half of them run their body in a loop of 1-4 iterations (VI03 counts down).
			const bool looped = p & 1;
			if (looped)
			{
				vu::Insn c{Op::IADDIU};
				c.it = 3;
				c.imm = 1 + static_cast<int>(rng() % 4);
				prog.push_back(c);
			}
			const int head = static_cast<int>(prog.size());
			for (int i = 0; i < len; i++)
			{
				vu::Insn in{pool[rng() % (sizeof(pool) / sizeof(pool[0]))]};
				in.dest = static_cast<u8>((rng() % 3) ? 0xf : 1 + rng() % 15);
				in.fs = static_cast<u8>(rng() % 7);
				in.ft = static_cast<u8>(rng() % 7);
				in.fd = static_cast<u8>(rng() % 7);
				in.bc = static_cast<u8>(rng() % 4);
				in.is = 5;
				prog.push_back(in);
			}
			if (looped)
			{
				vu::Insn dec{Op::ISUBIU};
				dec.it = 3;
				dec.is = 3;
				dec.imm = 1;
				prog.push_back(dec);
				vu::Insn br{Op::IBNE};
				br.is = 3;
				br.it = 0;
				br.imm = head;
				prog.push_back(br);
			}
			prog.push_back(vu::Insn{Op::END});
			for (int variant = 0; variant < 2; variant++)
			{
				vu::Compiler c(prog, state, {true, true, variant == 1, false, variant == 1, variant == 1});
				Loaded l = Load(c.Build());
				const int idx = jit_add_function(l.inst, "run");
				VoidFn fn = TableFn<VoidFn>(idx);
				for (int t = 0; t < 20; t++)
				{
					vu::ResetState(g_fz);
					for (int r = 1; r < 7; r++)
						for (int k = 0; k < 4; k++)
							g_fz.vf[r][k] = RandomBits(rng, 40);
					for (int k = 0; k < 4; k++)
						g_fz.acc[k] = RandomBits(rng, 40);
					for (int q = 0; q < 64; q++)
						reinterpret_cast<u32*>(g_fz.mem)[q] = RandomBits(rng, 40);
					g_fz.vi[5] = rng() % 8;
					g_ref = g_fz;
					vu::interp::Run(&g_ref, prog.data());
					fn();
					pruns++;
					if (!SameState(g_ref, g_fz, pbad == 0))
					{
						if (pbad == 0)
							printf("  random program %d (variant %d) differs\n", p, variant);
						pbad++;
					}
				}
				jit_remove_function(idx);
				l.Release();
			}
		}
		printf("[fuzz] %d random programs of 8-24 instructions (half in loops), plain and fully optimised, x 20 states: %d of %d runs differ\n", nprogs, pbad, pruns);
		Check(pbad == 0, "random programs are bit-exact with the interpreter, with and without the optimisations");
	});
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// nan: what this engine computes for NaN operands, scalar vs SIMD, clang-compiled vs generated.

alignas(16) static u32 g_nan_in[64][2];
alignas(16) static u32 g_nan_gen[64][4];

__attribute__((noinline)) static u32 ScalarAdd(u32 a, u32 b)
{
	float x, y;
	memcpy(&x, &a, 4);
	memcpy(&y, &b, 4);
	return F2U(x + y);
}

__attribute__((noinline)) static u32 ScalarMul(u32 a, u32 b)
{
	float x, y;
	memcpy(&x, &a, 4);
	memcpy(&y, &b, 4);
	return F2U(x * y);
}

static void TestNan()
{
	const u32 cases[][2] = {
		{0x7fc00001, 0x7fc00002}, // qNaN + qNaN
		{0xffc00003, 0x7fc00004}, // -qNaN + qNaN
		{0x7f800005, 0x3f800000}, // sNaN + 1
		{0x3f800000, 0x7f800006}, // 1 + sNaN
		{0x3f800000, 0xffc00007}, // 1 + -qNaN
		{0x7f800000, 0xff800000}, // inf + -inf
		{0x00000000, 0x7f800000}, // 0 * inf (mul)
		{0x00000001, 0x00000001}, // denormal + denormal
		{0x00400000, 0x40000000}, // denormal * 2 (mul)
	};
	const int n = sizeof(cases) / sizeof(cases[0]);
	for (int i = 0; i < n; i++)
	{
		g_nan_in[i][0] = cases[i][0];
		g_nan_in[i][1] = cases[i][1];
	}
	// Generated: for each case, f32.add, f32.mul, f32x4.add lane 0, f32x4.mul lane 0.
	Module m;
	const u32 t = m.type({}, {});
	m.import_memory("env", "mem", 0, 65536, true);
	Function f(0);
	const u32 in = static_cast<u32>(reinterpret_cast<uintptr_t>(g_nan_in));
	const u32 out = static_cast<u32>(reinterpret_cast<uintptr_t>(g_nan_gen));
	for (int i = 0; i < n; i++)
	{
		for (int k = 0; k < 2; k++)
		{
			f.i32_const(0);
			f.i32_const(0);
			f.f32_load(in + i * 8);
			f.i32_const(0);
			f.f32_load(in + i * 8 + 4);
			if (k == 0)
				f.f32_add();
			else
				f.f32_mul();
			f.f32_store(out + i * 16 + k * 4);
		}
		for (int k = 0; k < 2; k++)
		{
			f.i32_const(0);
			f.i32_const(0);
			f.simd_mem(Simd::V128Load32Splat, in + i * 8, 2);
			f.i32_const(0);
			f.simd_mem(Simd::V128Load32Splat, in + i * 8 + 4, 2);
			f.simd(k == 0 ? Simd::F32x4Add : Simd::F32x4Mul);
			f.simd_lane(Simd::F32x4ExtractLane, 0);
			f.f32_store(out + i * 16 + 8 + k * 4);
		}
	}
	m.export_function("run", m.add_function(t, f));
	OnPthread([&] {
		Loaded l = Load(m.finish());
		const int idx = jit_add_function(l.inst, "run");
		TableFn<VoidFn>(idx)();
		int differ = 0;
		printf("[nan] %-10s %-10s | %-10s %-10s %-10s | %-10s %-10s %-10s | %-10s %-10s\n", "a", "b", "C++ a+b", "gen f32", "gen f32x4", "C++ a*b", "gen f32",
			"gen f32x4", "C++ simd+", "C++ simd*");
		for (int i = 0; i < n; i++)
		{
			const u32 a = g_nan_in[i][0], b = g_nan_in[i][1];
			const v128_t va = wasm_i32x4_splat(a), vb = wasm_i32x4_splat(b);
			const u32 sa = wasm_i32x4_extract_lane(wasm_f32x4_add(va, vb), 0);
			const u32 sm = wasm_i32x4_extract_lane(wasm_f32x4_mul(va, vb), 0);
			const u32 ca = ScalarAdd(a, b), cm = ScalarMul(a, b);
			printf("[nan] %08x   %08x   | %08x   %08x   %08x   | %08x   %08x   %08x   | %08x   %08x\n", a, b, ca, g_nan_gen[i][0], g_nan_gen[i][2], cm,
				g_nan_gen[i][1], g_nan_gen[i][3], sa, sm);
			differ += (ca != g_nan_gen[i][0]) + (ca != g_nan_gen[i][2]) + (cm != g_nan_gen[i][1]) + (cm != g_nan_gen[i][3]) + (ca != sa) + (cm != sm);
		}
		printf("[nan] %d differences between clang-compiled scalar code and generated scalar/SIMD code on this engine and CPU\n", differ);
		jit_remove_function(idx);
		l.Release();
	});
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// simdops: throughput of the SIMD operations a VU JIT needs, as V8 compiles them.

struct SimdOpDef
{
	const char* name;
	int ops; // wasm operations per step
	std::function<void(Function&, u32 acc, u32 k, u32 mask, u32 tmp, u32 isum)> emit;
};

static void TestSimdOps()
{
	const u32 base = static_cast<u32>(reinterpret_cast<uintptr_t>(g_scratch_state));
	for (int i = 0; i < 64; i++)
		g_scratch_state[i] = 0x3f800000 + (i * 0x1234);
	auto bin = [](Simd op) {
		return [op](Function& f, u32 acc, u32 k, u32, u32, u32) {
			f.local_get(acc);
			f.local_get(k);
			f.simd(op);
			f.local_set(acc);
		};
	};
	std::vector<SimdOpDef> defs = {
		{"f32x4.add", 1, bin(Simd::F32x4Add)},
		{"f32x4.mul", 1, bin(Simd::F32x4Mul)},
		{"f32x4.mul+add (madd)", 2, [](Function& f, u32 acc, u32 k, u32, u32, u32) { f.local_get(acc); f.local_get(k); f.simd(Simd::F32x4Mul); f.local_get(k); f.simd(Simd::F32x4Add); f.local_set(acc); }},
		{"f32x4.relaxed_madd", 1, [](Function& f, u32 acc, u32 k, u32, u32, u32) { f.local_get(acc); f.local_get(k); f.local_get(k); f.simd(Simd::F32x4RelaxedMadd); f.local_set(acc); }},
		{"f32x4.min (IEEE, NaN-propagating)", 1, bin(Simd::F32x4Min)},
		{"f32x4.max (IEEE, NaN-propagating)", 1, bin(Simd::F32x4Max)},
		{"f32x4.pmin", 1, bin(Simd::F32x4PMin)},
		{"f32x4.pmax", 1, bin(Simd::F32x4PMax)},
		{"f32x4.lt", 1, bin(Simd::F32x4Lt)},
		{"f32x4.eq", 1, bin(Simd::F32x4Eq)},
		{"i32x4.eq", 1, bin(Simd::I32x4Eq)},
		{"i32x4.gt_s", 1, bin(Simd::I32x4GtS)},
		{"i32x4.min_s", 1, bin(Simd::I32x4MinS)},
		{"i32x4.max_s", 1, bin(Simd::I32x4MaxS)},
		{"i32x4.min_u", 1, bin(Simd::I32x4MinU)},
		{"v128.and", 1, bin(Simd::V128And)},
		{"v128.and with a v128.const operand", 1, [](Function& f, u32 acc, u32, u32, u32, u32) { f.local_get(acc); f.v128_const_splat(0x7fffffff); f.simd(Simd::V128And); f.local_set(acc); }},
		{"i32x4.min_u with a v128.const operand", 1, [](Function& f, u32 acc, u32, u32, u32, u32) { f.local_get(acc); f.v128_const_splat(0x7f7fffff); f.simd(Simd::I32x4MinU); f.local_set(acc); }},
		{"v128.andnot", 1, bin(Simd::V128AndNot)},
		{"i16x8.narrow_i32x4_s", 1, bin(Simd::I16x8NarrowI32x4S)},
		{"v128.bitselect", 1, [](Function& f, u32 acc, u32 k, u32 mask, u32, u32) { f.local_get(acc); f.local_get(k); f.local_get(mask); f.simd(Simd::V128Bitselect); f.local_set(acc); }},
		{"i32x4.shr_s 31", 1, [](Function& f, u32 acc, u32, u32, u32, u32) { f.local_get(acc); f.i32_const(31); f.simd(Simd::I32x4ShrS); f.local_set(acc); }},
		{"broadcast lane (i8x16.shuffle)", 1, [](Function& f, u32 acc, u32, u32, u32 tmp, u32) { f.local_get(acc); f.broadcast32(1, tmp); f.local_set(acc); }},
		{"broadcast (extract_lane+splat)", 2, [](Function& f, u32 acc, u32, u32, u32, u32) { f.local_get(acc); f.simd_lane(Simd::F32x4ExtractLane, 1); f.simd(Simd::F32x4Splat); f.local_set(acc); }},
		{"reverse (i8x16.shuffle)", 1, [](Function& f, u32 acc, u32, u32, u32 tmp, u32) { static constexpr u8 rev[16] = {3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12}; f.local_get(acc); f.shuffle1(rev, tmp); f.local_set(acc); }},
		{"i8x16.swizzle (variable)", 1, [](Function& f, u32 acc, u32, u32 mask, u32, u32) { f.local_get(acc); f.local_get(mask); f.simd(Simd::I8x16Swizzle); f.local_set(acc); }},
		{"i8x16.bitmask (+ xor, i32 add)", 3, [](Function& f, u32 acc, u32 k, u32, u32, u32 isum) { f.local_get(acc); f.local_get(k); f.simd(Simd::V128Xor); f.local_tee(acc); f.simd(Simd::I8x16Bitmask); f.local_get(isum); f.i32_add(); f.local_set(isum); }},
		{"i32x4.bitmask (+ xor, i32 add)", 3, [](Function& f, u32 acc, u32 k, u32, u32, u32 isum) { f.local_get(acc); f.local_get(k); f.simd(Simd::V128Xor); f.local_tee(acc); f.simd(Simd::I32x4Bitmask); f.local_get(isum); f.i32_add(); f.local_set(isum); }},
	};
	OnPthread([&] {
		const int iters = static_cast<int>(ArgInt("iters", 2000000));
		Module m;
		const u32 t = m.type({ValType::I32}, {});
		m.import_memory("env", "mem", 0, 65536, true);
		for (size_t d = 0; d < defs.size() + 1; d++)
		{
			// (iters): 8 independent chains acc_i = op(acc_i, ...) per iteration.
			Function f(1);
			u32 acc[8];
			for (int i = 0; i < 8; i++)
				acc[i] = f.local(ValType::V128);
			const u32 k = f.local(ValType::V128), mask = f.local(ValType::V128), tmp = f.local(ValType::V128);
			const u32 isum = f.local(ValType::I32);
			for (int i = 0; i < 8; i++)
			{
				f.i32_const(0);
				f.v128_load(base + i * 16);
				f.local_set(acc[i]);
			}
			f.i32_const(0);
			f.v128_load(base + 128);
			f.local_set(k);
			f.v128_const_i32x4(0x0c0d0e0f, 0x08090a0b, 0x04050607, 0x00010203);
			f.local_set(mask);
			f.loop();
			if (d < defs.size())
				for (int i = 0; i < 8; i++)
					defs[d].emit(f, acc[i], k, mask, tmp, isum);
			f.local_get(0);
			f.i32_const(1);
			f.i32_sub();
			f.local_tee(0);
			f.br_if(0);
			f.end();
			for (int i = 0; i < 8; i++)
			{
				f.i32_const(0);
				f.local_get(acc[i]);
				f.v128_store(base + 256 + i * 16);
			}
			f.i32_const(0);
			f.local_get(isum);
			f.i32_store(base + 240);
			char name[16];
			snprintf(name, sizeof(name), "op%zu", d);
			m.export_function(name, m.add_function(t, f));
		}
		const std::vector<u8> bytes = m.finish();
		Dump("simdops.wasm", bytes);
		{
			// One function per operation, no loop, for reading V8's code (lowering.mjs):
			// loads acc, k, mask; one step; stores acc and the i32 sum.
			Module one;
			const u32 t1 = one.type({ValType::I32}, {});
			one.import_memory("env", "mem", 0, 65536, true);
			for (size_t d = 0; d < defs.size(); d++)
			{
				Function f(1);
				const u32 acc = f.local(ValType::V128), k = f.local(ValType::V128), mask = f.local(ValType::V128), tmp = f.local(ValType::V128);
				const u32 isum = f.local(ValType::I32);
				const u32 regs[3] = {acc, k, mask};
				for (int i = 0; i < 3; i++)
				{
					f.i32_const(0);
					f.v128_load(base + i * 16);
					f.local_set(regs[i]);
				}
				defs[d].emit(f, acc, k, mask, tmp, isum);
				f.i32_const(0);
				f.local_get(acc);
				f.v128_store(base + 256);
				f.i32_const(0);
				f.local_get(isum);
				f.i32_store(base + 240);
				one.export_function(defs[d].name, one.add_function(t1, f));
			}
			Dump("lowering.wasm", one.finish());
		}
		Loaded l = Load(bytes);
		if (!l)
		{
			g_failures++;
			return;
		}
		char name[16];
		snprintf(name, sizeof(name), "op%zu", defs.size());
		using IterFn = void (*)(int);
		IterFn empty = TableFn<IterFn>(jit_add_function(l.inst, name));
		auto time = [&](IterFn fn) {
			fn(iters / 10); // warm up and tier up (a second call runs the optimized code)
			fn(iters / 10);
			std::vector<double> v;
			for (int r = 0; r < 5; r++)
			{
				const double t0 = Now();
				fn(iters);
				v.push_back((Now() - t0) * 1e6);
			}
			return Median(v);
		};
		const double loop_ns = time(empty);
		double add_ns = 0;
		printf("[simdops] %d iterations of 8 independent chains; loop overhead %.3f ns/iteration (subtracted)\n", iters, loop_ns / iters);
		for (size_t d = 0; d < defs.size(); d++)
		{
			snprintf(name, sizeof(name), "op%zu", d);
			IterFn fn = TableFn<IterFn>(jit_add_function(l.inst, name));
			const double ns = (time(fn) - loop_ns) / (double(iters) * 8);
			if (d == 0)
				add_ns = ns;
			printf("[simdops] %-36s %6.3f ns per step (%d wasm op%s)  %5.2fx f32x4.add\n", defs[d].name, ns, defs[d].ops, defs[d].ops > 1 ? "s" : "", ns / add_ns);
		}
		l.Release();
	});
}

//////////////////////////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv)
{
	std::string tests = argc > 1 ? argv[1] : "all";
	for (int i = 2; i < argc; i++)
	{
		const char* eq = strchr(argv[i], '=');
		if (eq)
			g_args[std::string(argv[i], eq - argv[i])] = eq + 1;
	}
	printf("engine: %s\n", jit_engine());
	printf("heap: %zu MB, max %zu MB\n", emscripten_get_heap_size() >> 20, emscripten_get_heap_max() >> 20);
	const std::pair<const char*, void (*)()> all[] = {
		{"basic", TestBasic}, {"grow", TestGrow}, {"mainsize", TestMainSize}, {"latency", TestLatency}, {"calls", TestCalls},
		{"many", TestMany}, {"kernel", TestKernel}, {"progsize", TestProgSize}, {"fuzz", TestFuzz}, {"nan", TestNan}, {"simdops", TestSimdOps},
	};
	const std::string list = "," + tests + ",";
	for (const auto& [name, fn] : all)
	{
		if (tests == "all" || list.find(std::string(",") + name + ",") != std::string::npos)
		{
			const double t0 = Now();
			fn();
			printf("(%s: %.1f s)\n", name, (Now() - t0) / 1000);
		}
	}
	if (g_failures)
		printf("FAIL %d check(s) failed\n", g_failures);
	else
		printf("DONE\n");
	fflush(stdout);
	return g_failures ? 1 : 0;
}
