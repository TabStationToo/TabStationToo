// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// A miniature VU1 for the JIT spike: register state, a decoded instruction form for the handful of
// instructions a vertex-transform loop uses, an interpreter that reproduces the arithmetic of
// PCSX2's VU interpreter (pcsx2/VUops.cpp, pcsx2/VUflags.cpp: vuDouble operand clamping,
// VU_MAC_UPDATE result clamping and MAC/status flags, CLIP, integer MAX/MINI) with the same
// per-instruction dispatch style, and a compiler from that form to WebAssembly SIMD.
//
// It omits what the real interpreter also does per instruction (decoding the 64-bit word, the
// FMAC/FDIV/EFU pipeline and stall model, branch delay slots, flag pipelines), so it is cheaper
// than the real one: speedups measured against it are a lower bound.

#pragma once

#include "wasm_emitter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace vu {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using s16 = int16_t;
using s32 = int32_t;

struct alignas(16) State
{
	u32 vf[32][4];
	u32 acc[4];
	u32 vi[16];
	u32 mac, status, clip, pad;
	alignas(16) u8 mem[16384]; // VU1 data memory
};

enum class Op : u8
{
	MULAbc, // ACC = fs * ft.bc
	MADDAbc, // ACC = ACC + fs * ft.bc
	MADDbc, // fd = ACC + fs * ft.bc
	MULbc, // fd = fs * ft.bc
	ADD, // fd = fs + ft
	SUB, // fd = fs - ft
	MUL, // fd = fs * ft
	MADD, // fd = ACC + fs * ft
	MAX, // fd = max(fs, ft) (integer compare, like fp_max)
	MINI, // fd = min(fs, ft)
	CLIP, // clip flags of fs.xyz against |ft.w|
	LQI, // ft = mem[vi[is]++]
	SQI, // mem[vi[it]++] = fs
	FCAND, // vi[1] = (clip & imm) != 0
	IADD, // vi[id] = vi[is] + vi[it]
	IADDIU, // vi[it] = vi[is] + imm
	ISUBIU, // vi[it] = vi[is] - imm
	IBNE, // if (vi[is] != vi[it]) goto imm
	END,
	Count
};

// dest uses the VU's field order: x = 8, y = 4, z = 2, w = 1.
struct Insn
{
	Op op;
	u8 dest = 0xf;
	u8 fd = 0, fs = 0, ft = 0, bc = 0;
	u8 id = 0, is = 0, it = 0;
	s32 imm = 0;
};

inline bool IsFmac(Op op)
{
	return op <= Op::MADD;
}

inline bool WritesAcc(Op op)
{
	return op == Op::MULAbc || op == Op::MADDAbc;
}

inline bool ReadsAcc(Op op)
{
	return op == Op::MADDAbc || op == Op::MADDbc || op == Op::MADD;
}

inline void ResetState(State& s)
{
	std::memset(&s, 0, sizeof(s));
	s.vf[0][3] = 0x3f800000; // VF00 = (0, 0, 0, 1)
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// Interpreter: the arithmetic of pcsx2/VUops.cpp and pcsx2/VUflags.cpp with the overflow clamp on
// (CHECK_VU_OVERFLOW(1), PCSX2's default).

namespace interp {

static inline float AsFloat(u32 v)
{
	float f;
	std::memcpy(&f, &v, 4);
	return f;
}

static inline u32 AsU32(float f)
{
	u32 v;
	std::memcpy(&v, &f, 4);
	return v;
}

static inline float vuDouble(u32 f)
{
	switch (f & 0x7f800000)
	{
		case 0x0:
			f &= 0x80000000;
			return AsFloat(f);
		case 0x7f800000:
			return AsFloat((f & 0x80000000) | 0x7f7fffff);
	}
	return AsFloat(f);
}

__attribute__((noinline)) static u32 MacUpdate(int shift, State* vu, float f)
{
	const u32 v = AsU32(f);
	const int exp = (v >> 23) & 0xff;
	const u32 s = v & 0x80000000;
	if (s)
		vu->mac |= 0x0010 << shift;
	else
		vu->mac &= ~(0x0010 << shift);
	if (f == 0)
	{
		vu->mac = (vu->mac & ~(0x1100 << shift)) | (0x0001 << shift);
		return v;
	}
	switch (exp)
	{
		case 0:
			vu->mac = (vu->mac & ~(0x1000 << shift)) | (0x0101 << shift);
			return s;
		case 255:
			vu->mac = (vu->mac & ~(0x0101 << shift)) | (0x1000 << shift);
			return s | 0x7f7fffff;
		default:
			vu->mac = (vu->mac & ~(0x1101 << shift));
			return v;
	}
}

static inline void MacClear(int shift, State* vu)
{
	vu->mac &= ~(0x1111 << shift);
}

static inline void StatUpdate(State* vu)
{
	u32 n = 0;
	if (vu->mac & 0x000f)
		n = 0x1;
	if (vu->mac & 0x00f0)
		n |= 0x2;
	if (vu->mac & 0x0f00)
		n |= 0x4;
	if (vu->mac & 0xf000)
		n |= 0x8;
	vu->status = n;
}

static u32 s_zero_sink[4];

static inline u32* Dst(State* vu, const Insn& in, bool acc)
{
	return acc ? vu->acc : (in.fd ? vu->vf[in.fd] : s_zero_sink);
}

// Lane l of the VU is x, y, z, w = 0..3; its dest bit is 8 >> l and its flag shift 3 - l.
template <typename Fn>
static inline void Fmac(State* vu, const Insn& in, u32* dst, Fn fn)
{
	for (int l = 0; l < 4; l++)
	{
		if (in.dest & (8 >> l))
			dst[l] = MacUpdate(3 - l, vu, fn(l));
		else
			MacClear(3 - l, vu);
	}
	StatUpdate(vu);
}

static inline u32 fp_max(u32 a, u32 b)
{
	return (static_cast<s32>(a) < 0 && static_cast<s32>(b) < 0) ? std::min<s32>(a, b) : std::max<s32>(a, b);
}

static inline u32 fp_min(u32 a, u32 b)
{
	return (static_cast<s32>(a) < 0 && static_cast<s32>(b) < 0) ? std::max<s32>(a, b) : std::min<s32>(a, b);
}

// Each handler returns the index of the next instruction, or -1 to stop.
using Handler = int (*)(State*, const Insn&, int pc);

static int opMULAbc(State* vu, const Insn& in, int pc)
{
	const u32 bc = vu->vf[in.ft][in.bc];
	Fmac(vu, in, vu->acc, [&](int l) { return vuDouble(vu->vf[in.fs][l]) * vuDouble(bc); });
	return pc + 1;
}
static int opMADDAbc(State* vu, const Insn& in, int pc)
{
	const u32 bc = vu->vf[in.ft][in.bc];
	Fmac(vu, in, vu->acc, [&](int l) { return vuDouble(vu->acc[l]) + vuDouble(vu->vf[in.fs][l]) * vuDouble(bc); });
	return pc + 1;
}
static int opMADDbc(State* vu, const Insn& in, int pc)
{
	const u32 bc = vu->vf[in.ft][in.bc];
	Fmac(vu, in, Dst(vu, in, false), [&](int l) { return vuDouble(vu->acc[l]) + vuDouble(vu->vf[in.fs][l]) * vuDouble(bc); });
	return pc + 1;
}
static int opMULbc(State* vu, const Insn& in, int pc)
{
	const u32 bc = vu->vf[in.ft][in.bc];
	Fmac(vu, in, Dst(vu, in, false), [&](int l) { return vuDouble(vu->vf[in.fs][l]) * vuDouble(bc); });
	return pc + 1;
}
static int opADD(State* vu, const Insn& in, int pc)
{
	Fmac(vu, in, Dst(vu, in, false), [&](int l) { return vuDouble(vu->vf[in.fs][l]) + vuDouble(vu->vf[in.ft][l]); });
	return pc + 1;
}
static int opSUB(State* vu, const Insn& in, int pc)
{
	Fmac(vu, in, Dst(vu, in, false), [&](int l) { return vuDouble(vu->vf[in.fs][l]) - vuDouble(vu->vf[in.ft][l]); });
	return pc + 1;
}
static int opMUL(State* vu, const Insn& in, int pc)
{
	Fmac(vu, in, Dst(vu, in, false), [&](int l) { return vuDouble(vu->vf[in.fs][l]) * vuDouble(vu->vf[in.ft][l]); });
	return pc + 1;
}
static int opMADD(State* vu, const Insn& in, int pc)
{
	Fmac(vu, in, Dst(vu, in, false), [&](int l) { return vuDouble(vu->acc[l]) + vuDouble(vu->vf[in.fs][l]) * vuDouble(vu->vf[in.ft][l]); });
	return pc + 1;
}
static int opMAX(State* vu, const Insn& in, int pc)
{
	if (in.fd)
	{
		const u32 a[4] = {vu->vf[in.fs][0], vu->vf[in.fs][1], vu->vf[in.fs][2], vu->vf[in.fs][3]};
		for (int l = 0; l < 4; l++)
			if (in.dest & (8 >> l))
				vu->vf[in.fd][l] = fp_max(a[l], vu->vf[in.ft][l]);
	}
	return pc + 1;
}
static int opMINI(State* vu, const Insn& in, int pc)
{
	if (in.fd)
	{
		const u32 a[4] = {vu->vf[in.fs][0], vu->vf[in.fs][1], vu->vf[in.fs][2], vu->vf[in.fs][3]};
		for (int l = 0; l < 4; l++)
			if (in.dest & (8 >> l))
				vu->vf[in.fd][l] = fp_min(a[l], vu->vf[in.ft][l]);
	}
	return pc + 1;
}
static int opCLIP(State* vu, const Insn& in, int pc)
{
	s32 value = vu->vf[in.ft][3];
	value = (value & 0x7f800000) ? value & 0x7fffffff : 0x007fffff;
	vu->clip <<= 6;
	for (int l = 0; l < 3; l++)
	{
		if (static_cast<s32>(vu->vf[in.fs][l]) > value)
			vu->clip |= 0x01 << (l * 2);
		if (static_cast<s32>(vu->vf[in.fs][l] ^ 0x80000000) > value)
			vu->clip |= 0x02 << (l * 2);
	}
	vu->clip &= 0xffffff;
	return pc + 1;
}
static int opLQI(State* vu, const Insn& in, int pc)
{
	if (in.ft)
	{
		const u32* p = reinterpret_cast<const u32*>(vu->mem + ((vu->vi[in.is] * 16) & 0x3fff));
		for (int l = 0; l < 4; l++)
			if (in.dest & (8 >> l))
				vu->vf[in.ft][l] = p[l];
	}
	if (in.is)
		vu->vi[in.is] = (vu->vi[in.is] + 1) & 0xffff;
	return pc + 1;
}
static int opSQI(State* vu, const Insn& in, int pc)
{
	u32* p = reinterpret_cast<u32*>(vu->mem + ((vu->vi[in.it] * 16) & 0x3fff));
	for (int l = 0; l < 4; l++)
		if (in.dest & (8 >> l))
			p[l] = vu->vf[in.fs][l];
	if (in.it)
		vu->vi[in.it] = (vu->vi[in.it] + 1) & 0xffff;
	return pc + 1;
}
static int opFCAND(State* vu, const Insn& in, int pc)
{
	vu->vi[1] = (vu->clip & 0xffffff & static_cast<u32>(in.imm)) ? 1 : 0;
	return pc + 1;
}
static int opIADD(State* vu, const Insn& in, int pc)
{
	if (in.id)
		vu->vi[in.id] = (vu->vi[in.is] + vu->vi[in.it]) & 0xffff;
	return pc + 1;
}
static int opIADDIU(State* vu, const Insn& in, int pc)
{
	if (in.it)
		vu->vi[in.it] = (vu->vi[in.is] + in.imm) & 0xffff;
	return pc + 1;
}
static int opISUBIU(State* vu, const Insn& in, int pc)
{
	if (in.it)
		vu->vi[in.it] = (vu->vi[in.is] - in.imm) & 0xffff;
	return pc + 1;
}
static int opIBNE(State* vu, const Insn& in, int pc)
{
	return vu->vi[in.is] != vu->vi[in.it] ? in.imm : pc + 1;
}
static int opEND(State*, const Insn&, int)
{
	return -1;
}

static constexpr Handler kHandlers[static_cast<int>(Op::Count)] = {
	opMULAbc, opMADDAbc, opMADDbc, opMULbc, opADD, opSUB, opMUL, opMADD, opMAX, opMINI, opCLIP,
	opLQI, opSQI, opFCAND, opIADD, opIADDIU, opISUBIU, opIBNE, opEND};

// Runs from instruction 0 until END. Returns the number of instructions executed.
__attribute__((noinline)) inline u32 Run(State* vu, const Insn* prog)
{
	u32 n = 0;
	for (int pc = 0; pc >= 0; n++)
		pc = kHandlers[static_cast<int>(prog[pc].op)](vu, prog[pc], pc);
	return n;
}

} // namespace interp

//////////////////////////////////////////////////////////////////////////////////////////////////
// Compiler: the instruction list to one wasm function `run` with no parameters. The state's
// address is a constant in the code. VF registers, ACC, VI registers and the flags live in wasm
// locals for the whole function and are stored back on exit.

struct CompileOptions
{
	bool exact = true; // clamp operands like vuDouble and results like VU_MAC_UPDATE
	bool flags = true; // compute MAC and status flags
	bool elide_dead_flags = false; // skip MAC/status flags no later instruction can observe
	bool relaxed_madd = false; // use f32x4.relaxed_madd (fused on FMA hardware: not exact)
	bool cache_clamps = false; // keep clamped copies of registers until they are written; hoist loop-invariant ones
	// Shape the code for V8's lowering: clamp/flag constants in locals (TurboFan rematerialises a
	// v128.const at every use, 3 instructions), lane reversal with a 32-bit shuffle (pshufd)
	// rather than a byte shuffle (6 instructions), i32x4.eq rather than i32x4.ne (3 instructions),
	// and a table lookup for CLIP's bit interleave.
	bool tuned = false;
	u32 salt = 0; // when non-zero, an unused constant that makes the module's bytes unique
};

// CLIP: i8x16.bitmask of the narrowed compare masks gives +x +y +z in bits 0-2 and -x -y -z in
// bits 4-6; this table interleaves them into the clip flag order (+x -x +y -y +z -z).
struct ClipTable
{
	u8 v[128];
	constexpr ClipTable()
		: v()
	{
		for (int i = 0; i < 128; i++)
		{
			int r = 0;
			for (int l = 0; l < 3; l++)
				r |= ((i >> l) & 1) << (2 * l) | ((i >> (4 + l)) & 1) << (2 * l + 1);
			v[i] = static_cast<u8>(r);
		}
	}
};
alignas(16) inline constexpr ClipTable kClipTable{};

struct CompileStats
{
	u32 wasm_instructions = 0;
	u32 fmac_with_flags = 0;
	u32 fmac_total = 0;
	u32 clamps = 0;
	u32 clamps_elided = 0;
};

class Compiler
{
public:
	using Function = wasmjit::Function;
	using Simd = wasmjit::Simd;
	using ValType = wasmjit::ValType;

	Compiler(const std::vector<Insn>& prog, u32 state_addr, const CompileOptions& opt)
		: m_prog(prog)
		, m_base(state_addr)
		, m_opt(opt)
	{
	}

	// One module exporting `run: [] -> []`, importing env.mem (the shared memory).
	std::vector<u8> Build(CompileStats* stats = nullptr)
	{
		wasmjit::Module m;
		const u32 t_void = m.type({}, {});
		m.import_memory("env", "mem", 0, 65536, true);
		Function f(0);
		Emit(f);
		m.export_function("run", m.add_function(t_void, f));
		m_stats.wasm_instructions = f.instructions();
		if (stats)
			*stats = m_stats;
		return m.finish();
	}

private:
	static constexpr int kAcc = 32; // index of ACC in the per-register sets
	static constexpr u32 kConsts[5] = {0x7fffffff, 0x7f7fffff, 0x007fffff, 0x80000000, 0x7f800000};
	using Clean = std::vector<bool>; // 33 entries: VF00..VF31, ACC

	u32 VfAddr(int r) const { return m_base + offsetof(State, vf) + r * 16; }
	u32 AccAddr() const { return m_base + offsetof(State, acc); }
	u32 ViAddr(int r) const { return m_base + offsetof(State, vi) + r * 4; }
	u32 MemAddr() const { return m_base + offsetof(State, mem); }
	u32 FlagAddr(int which) const { return m_base + offsetof(State, mac) + which * 4; }
	bool IsLoopHead(int pc) const { return std::find(m_loop_heads.begin(), m_loop_heads.end(), pc) != m_loop_heads.end(); }

	//////////////////////////////////////////////////////////////////////////////////////////////
	// Analysis

	void Transfer(const Insn& in, Clean& c) const
	{
		if (IsFmac(in.op))
		{
			const int dst = WritesAcc(in.op) ? kAcc : in.fd;
			// A full write leaves a clamped value; a partial one keeps the old lanes' status.
			if (dst && in.dest == 0xf)
				c[dst] = true;
		}
		else if ((in.op == Op::MAX || in.op == Op::MINI) && in.fd)
			c[in.fd] = c[in.fs] && c[in.ft] && (in.dest == 0xf || c[in.fd]);
		else if (in.op == Op::LQI && in.ft)
			c[in.ft] = false;
	}

	void Analyse()
	{
		const int n = static_cast<int>(m_prog.size());
		for (const Insn& in : m_prog)
		{
			auto vf = [&](u8 r) { if (r) m_vf_used[r] = true; };
			auto vi = [&](u8 r) { if (r) m_vi_used[r] = true; };
			switch (in.op)
			{
				case Op::LQI: vf(in.ft); vi(in.is); break;
				case Op::SQI: vf(in.fs); vi(in.it); break;
				case Op::CLIP: vf(in.fs); vf(in.ft); break;
				case Op::FCAND: m_vi_used[1] = true; break;
				case Op::IADD: vi(in.id); vi(in.is); vi(in.it); break;
				case Op::IADDIU: case Op::ISUBIU: vi(in.it); vi(in.is); break;
				case Op::IBNE: vi(in.is); vi(in.it); m_loop_heads.push_back(in.imm); break;
				case Op::END: case Op::Count: break;
				default:
					vf(in.fd); vf(in.fs); vf(in.ft);
					if (WritesAcc(in.op) || ReadsAcc(in.op))
						m_acc_used = true;
					break;
			}
		}

		// MAC/status liveness (may-analysis, backwards, from "not live"). Every FMAC op rewrites
		// both words entirely; nothing in this instruction set reads them; they are stored on
		// exit, so they are live at END.
		m_flag_live.assign(n, true);
		if (m_opt.elide_dead_flags)
		{
			std::vector<bool> live_in(n + 1, false);
			for (bool changed = true; changed;)
			{
				changed = false;
				for (int pc = n - 1; pc >= 0; pc--)
				{
					const Insn& in = m_prog[pc];
					const bool out = in.op == Op::END ? true
					               : in.op == Op::IBNE ? (live_in[pc + 1] || live_in[in.imm])
					                                   : live_in[pc + 1];
					m_flag_live[pc] = out;
					const bool in_live = IsFmac(in.op) ? false : out;
					if (live_in[pc] != in_live)
					{
						live_in[pc] = in_live;
						changed = true;
					}
				}
			}
		}

		// Registers known to hold clamped values (must-analysis, forwards). FMAC results are
		// clamped; loads are not; VF00 always is. Back edges start optimistic and are met (AND)
		// at loop heads until nothing changes.
		Clean entry(33, false);
		entry[0] = true;
		std::vector<Clean> back(n + 1, Clean(33, true));
		m_clean_at.assign(n + 1, Clean(33, false));
		for (bool changed = true; changed;)
		{
			changed = false;
			Clean cur = entry;
			for (int pc = 0; pc < n; pc++)
			{
				if (IsLoopHead(pc))
					for (int r = 0; r < 33; r++)
						cur[r] = cur[r] && back[pc][r];
				m_clean_at[pc] = cur;
				const Insn& in = m_prog[pc];
				Transfer(in, cur);
				if (in.op == Op::IBNE && back[in.imm] != cur)
				{
					back[in.imm] = cur;
					changed = true;
				}
				if (in.op == Op::END)
					break;
			}
		}
	}

	//////////////////////////////////////////////////////////////////////////////////////////////
	// Emission

	void Emit(Function& f)
	{
		Analyse();
		m_t0 = f.local(ValType::V128);
		m_t1 = f.local(ValType::V128);
		m_t2 = f.local(ValType::V128);
		m_ts = f.local(ValType::V128);
		m_res = f.local(ValType::V128);
		for (int r = 1; r < 32; r++)
			if (m_vf_used[r])
				m_vf[r] = f.local(ValType::V128);
		if (m_acc_used)
			m_acc = f.local(ValType::V128);
		for (int r = 1; r < 16; r++)
			if (m_vi_used[r])
				m_vi[r] = f.local(ValType::I32);
		m_mac = f.local(ValType::I32);
		m_status = f.local(ValType::I32);
		m_clip = f.local(ValType::I32);

		if (m_opt.salt)
		{
			f.i32_const(static_cast<s32>(m_opt.salt));
			f.drop();
		}
		// Prologue: guest registers into locals, constants into locals (tuned).
		if (m_opt.tuned)
			for (int i = 0; i < 5; i++)
			{
				m_const[i] = f.local(ValType::V128);
				f.v128_const_splat(kConsts[i]);
				f.local_set(m_const[i]);
			}
		for (int r = 1; r < 32; r++)
			if (m_vf_used[r])
			{
				f.i32_const(0);
				f.v128_load(VfAddr(r));
				f.local_set(m_vf[r]);
			}
		if (m_acc_used)
		{
			f.i32_const(0);
			f.v128_load(AccAddr());
			f.local_set(m_acc);
		}
		for (int r = 1; r < 16; r++)
			if (m_vi_used[r])
			{
				f.i32_const(0);
				f.i32_load(ViAddr(r));
				f.local_set(m_vi[r]);
			}
		const int flag_locals[3] = {m_mac, m_status, m_clip};
		for (int i = 0; i < 3; i++)
		{
			f.i32_const(0);
			f.i32_load(FlagAddr(i));
			f.local_set(flag_locals[i]);
		}

		const int n = static_cast<int>(m_prog.size());
		for (int pc = 0; pc < n; pc++)
		{
			if (IsLoopHead(pc))
			{
				if (m_opt.cache_clamps && m_opt.exact)
					HoistClamps(f, pc);
				f.loop();
			}
			EmitInsn(f, pc);
			if (m_prog[pc].op == Op::END)
				break;
		}

		// Epilogue: locals back into the guest registers.
		for (int r = 1; r < 32; r++)
			if (m_vf_used[r])
			{
				f.i32_const(0);
				f.local_get(m_vf[r]);
				f.v128_store(VfAddr(r));
			}
		if (m_acc_used)
		{
			f.i32_const(0);
			f.local_get(m_acc);
			f.v128_store(AccAddr());
		}
		for (int r = 1; r < 16; r++)
			if (m_vi_used[r])
			{
				f.i32_const(0);
				f.local_get(m_vi[r]);
				f.i32_store(ViAddr(r));
			}
		for (int i = 0; i < 3; i++)
		{
			f.i32_const(0);
			f.local_get(flag_locals[i]);
			f.i32_store(FlagAddr(i));
		}
	}

	// A constant in every lane: a local holding it (tuned), or v128.const.
	void Splat(Function& f, u32 v)
	{
		if (m_opt.tuned)
			for (int i = 0; i < 5; i++)
				if (kConsts[i] == v)
				{
					f.local_get(m_const[i]);
					return;
				}
		f.v128_const_splat(v);
	}

	void GetVf(Function& f, int r)
	{
		if (r == 0)
			f.v128_const_i32x4(0, 0, 0, 0x3f800000);
		else
			f.local_get(m_vf[r]);
	}

	void PushReg(Function& f, int slot)
	{
		if (slot == kAcc)
			f.local_get(m_acc);
		else
			GetVf(f, slot);
	}

	// Clamps the vector on the stack like vuDouble, which is also what VU_MAC_UPDATE does to a
	// result: exponent 0 becomes a signed zero, exponent 255 becomes +-FLT_MAX.
	//   a = min_u(x & 0x7fffffff, 0x7f7fffff); a &= (a > 0x007fffff); x = a | (x & 0x80000000)
	void Clamp(Function& f)
	{
		f.local_tee(m_t0);
		Splat(f, 0x7fffffff);
		f.simd(Simd::V128And);
		Splat(f, 0x7f7fffff);
		f.simd(Simd::I32x4MinU);
		f.local_tee(m_t1);
		f.local_get(m_t1);
		Splat(f, 0x007fffff);
		f.simd(Simd::I32x4GtS);
		f.simd(Simd::V128And);
		f.local_get(m_t0);
		Splat(f, 0x80000000);
		f.simd(Simd::V128And);
		f.simd(Simd::V128Or);
		m_stats.clamps++;
	}

	u32 Shadow(Function& f, int slot)
	{
		if (m_shadow[slot] < 0)
			m_shadow[slot] = static_cast<int>(f.local(ValType::V128));
		return static_cast<u32>(m_shadow[slot]);
	}

	// Pushes an FMAC operand: a register, a broadcast lane of one, or ACC; clamped unless the
	// analysis knows it already is. With cache_clamps the clamped value is kept in a shadow local
	// until the register is written.
	void Operand(Function& f, int pc, int r, int bc = -1, bool acc = false)
	{
		const int slot = acc ? kAcc : r;
		const bool clean = !m_opt.exact || m_clean_at[pc][slot];
		if (clean && m_opt.exact)
			m_stats.clamps_elided++;
		if (!clean && m_opt.cache_clamps && m_shadow_valid[slot])
		{
			f.local_get(m_shadow[slot]);
			m_stats.clamps_elided++;
		}
		else
		{
			PushReg(f, slot);
			if (!clean)
			{
				Clamp(f);
				if (m_opt.cache_clamps)
				{
					f.local_tee(Shadow(f, slot));
					m_shadow_valid[slot] = true;
				}
			}
		}
		if (bc >= 0)
			f.broadcast32(static_cast<u8>(bc), m_ts);
	}

	// Before a loop: clamped copies of the registers the loop reads as FMAC operands and never
	// writes, so the loop body does not clamp them on every iteration. Other shadows are dropped:
	// the back edge may arrive with the register rewritten.
	void HoistClamps(Function& f, int head)
	{
		const int n = static_cast<int>(m_prog.size());
		int end = head;
		while (end < n && !(m_prog[end].op == Op::IBNE && m_prog[end].imm == head))
			end++;
		bool read[33] = {}, written[33] = {};
		for (int pc = head; pc <= end && pc < n; pc++)
		{
			const Insn& in = m_prog[pc];
			if (IsFmac(in.op))
			{
				read[in.fs] = read[in.ft] = true;
				if (ReadsAcc(in.op))
					read[kAcc] = true;
				written[WritesAcc(in.op) ? kAcc : in.fd] = true;
			}
			else if ((in.op == Op::MAX || in.op == Op::MINI) && in.fd)
				written[in.fd] = true;
			else if (in.op == Op::LQI && in.ft)
				written[in.ft] = true;
		}
		for (int r = 1; r < 33; r++)
		{
			if (!read[r] || written[r])
			{
				m_shadow_valid[r] = false;
				continue;
			}
			if (m_clean_at[head][r] || m_shadow_valid[r])
				continue;
			PushReg(f, r);
			Clamp(f);
			f.local_set(Shadow(f, r));
			m_shadow_valid[r] = true;
		}
	}

	// The raw result is on the stack: MAC and status flags (when live), clamp, dest write.
	void FmacResult(Function& f, int pc, const Insn& in)
	{
		m_stats.fmac_total++;
		f.local_set(m_res);
		if (m_opt.flags && m_flag_live[pc])
		{
			m_stats.fmac_with_flags++;
			// As VU_MAC_UPDATE, in 0/-1 lanes: Z = exp == 0 (zero or denormal: the interpreter sets
			// Z for both), S = sign bit, U = exp == 0 && r != +-0, O = exp == 255. The flag word has
			// w in bit 0 of each nibble: tuned code reverses the lanes first (one pshufd).
			static constexpr u8 rev32[16] = {12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3};
			f.local_get(m_res);
			if (m_opt.tuned)
				f.shuffle1(rev32, m_ts);
			f.local_tee(m_t2); // r, lanes reversed when tuned
			Splat(f, 0x7f800000);
			f.simd(Simd::V128And);
			f.local_tee(m_t1); // exponent bits
			f.v128_const_splat(0);
			f.simd(Simd::I32x4Eq);
			f.local_tee(m_t0); // Z
			f.local_get(m_t2);
			f.v128_const_splat(0);
			f.simd(Simd::I32x4LtS); // S
			f.simd(Simd::I16x8NarrowI32x4S);
			f.local_get(m_t0);
			f.local_get(m_t2);
			Splat(f, 0x7fffffff);
			f.simd(Simd::V128And);
			f.v128_const_splat(0);
			f.simd(Simd::I32x4Eq);
			f.simd(Simd::V128AndNot); // U = Z & ~(r == +-0)
			f.local_get(m_t1);
			Splat(f, 0x7f800000);
			f.simd(Simd::I32x4Eq); // O
			f.simd(Simd::I16x8NarrowI32x4S);
			f.simd(Simd::I8x16NarrowI16x8S); // bytes Zx Zy Zz Zw Sx .. Ow (Zw Zz Zy Zx .. tuned)
			if (!m_opt.tuned)
			{
				static constexpr u8 rev[16] = {3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12};
				f.shuffle1(rev, m_ts);
			}
			if (in.dest != 0xf)
			{
				// Lanes outside dest have their flags cleared (VU_MACx_CLEAR).
				u8 keep[16];
				for (int i = 0; i < 16; i++)
					keep[i] = (in.dest & (1 << (i & 3))) ? 0xff : 0;
				f.v128_const(keep);
				f.simd(Simd::V128And);
			}
			f.local_tee(m_t0);
			f.simd(Simd::I8x16Bitmask);
			f.local_set(m_mac);
			// Status: bit k set when group k (Z, S, U, O) has a flag, one i32 lane per group.
			f.local_get(m_t0);
			f.v128_const_splat(0);
			if (m_opt.tuned)
			{
				f.simd(Simd::I32x4Eq);
				f.simd(Simd::I32x4Bitmask);
				f.i32_const(0xf);
				f.i32_xor();
			}
			else
			{
				f.simd(Simd::I32x4Ne);
				f.simd(Simd::I32x4Bitmask);
			}
			f.local_set(m_status);
		}
		const int dst = WritesAcc(in.op) ? m_acc : (in.fd ? m_vf[in.fd] : -1);
		if (dst < 0)
			return;
		f.local_get(m_res);
		if (m_opt.exact)
			Clamp(f);
		WriteDest(f, in.dest, dst);
	}

	// Stack: the new value. Writes its dest lanes into a local.
	void WriteDest(Function& f, u8 dest, int local)
	{
		if (dest != 0xf)
		{
			f.local_get(local);
			PushDestMask(f, dest);
			f.simd(Simd::V128Bitselect);
		}
		f.local_set(local);
	}

	static void PushDestMask(Function& f, u8 dest)
	{
		f.v128_const_i32x4((dest & 8) ? ~0u : 0, (dest & 4) ? ~0u : 0, (dest & 2) ? ~0u : 0, (dest & 1) ? ~0u : 0);
	}

	void EmitInsn(Function& f, int pc)
	{
		const Insn& in = m_prog[pc];
		EmitInsnBody(f, pc);
		// A written register's clamped copy is stale.
		if (IsFmac(in.op))
			m_shadow_valid[WritesAcc(in.op) ? kAcc : in.fd] = false;
		else if (in.op == Op::MAX || in.op == Op::MINI)
			m_shadow_valid[in.fd] = false;
		else if (in.op == Op::LQI)
			m_shadow_valid[in.ft] = false;
	}

	void EmitInsnBody(Function& f, int pc)
	{
		const Insn& in = m_prog[pc];
		switch (in.op)
		{
			case Op::MULAbc:
			case Op::MULbc:
				Operand(f, pc, in.fs);
				Operand(f, pc, in.ft, in.bc);
				f.simd(Simd::F32x4Mul);
				FmacResult(f, pc, in);
				break;
			case Op::MADDAbc:
			case Op::MADDbc:
			case Op::MADD:
			{
				const int bc = in.op == Op::MADD ? -1 : in.bc;
				if (m_opt.relaxed_madd)
				{
					Operand(f, pc, in.fs);
					Operand(f, pc, in.ft, bc);
					Operand(f, pc, 0, -1, true);
					f.simd(Simd::F32x4RelaxedMadd); // fs * ft + acc, maybe fused
				}
				else
				{
					Operand(f, pc, 0, -1, true);
					Operand(f, pc, in.fs);
					Operand(f, pc, in.ft, bc);
					f.simd(Simd::F32x4Mul);
					f.simd(Simd::F32x4Add); // acc + (fs * ft), two roundings like the interpreter
				}
				FmacResult(f, pc, in);
				break;
			}
			case Op::ADD:
			case Op::SUB:
			case Op::MUL:
				Operand(f, pc, in.fs);
				Operand(f, pc, in.ft);
				f.simd(in.op == Op::ADD ? Simd::F32x4Add : in.op == Op::SUB ? Simd::F32x4Sub : Simd::F32x4Mul);
				FmacResult(f, pc, in);
				break;
			case Op::MAX:
			case Op::MINI:
			{
				if (!in.fd)
					break;
				// Signed integer order, reversed when both are negative (fp_max/fp_min).
				const bool mx = in.op == Op::MAX;
				GetVf(f, in.fs);
				GetVf(f, in.ft);
				f.simd(mx ? Simd::I32x4MinS : Simd::I32x4MaxS); // both negative
				GetVf(f, in.fs);
				GetVf(f, in.ft);
				f.simd(mx ? Simd::I32x4MaxS : Simd::I32x4MinS); // otherwise
				GetVf(f, in.fs);
				GetVf(f, in.ft);
				f.simd(Simd::V128And);
				f.i32_const(31);
				f.simd(Simd::I32x4ShrS);
				f.simd(Simd::V128Bitselect);
				WriteDest(f, in.dest, m_vf[in.fd]);
				break;
			}
			case Op::CLIP:
			{
				// value = exp(ft.w) ? |ft.w| : 0x007fffff, in every lane.
				GetVf(f, in.ft);
				f.broadcast32(3, m_ts);
				f.local_set(m_t0);
				if (m_opt.tuned)
				{
					Splat(f, 0x007fffff);
					f.local_get(m_t0);
					Splat(f, 0x7fffffff);
					f.simd(Simd::V128And);
					f.local_get(m_t0);
					Splat(f, 0x7f800000);
					f.simd(Simd::V128And);
					f.v128_const_splat(0);
					f.simd(Simd::I32x4Eq);
					f.simd(Simd::V128Bitselect); // exponent 0 ? 0x007fffff : |w|
				}
				else
				{
					f.local_get(m_t0);
					Splat(f, 0x7fffffff);
					f.simd(Simd::V128And);
					Splat(f, 0x007fffff);
					f.local_get(m_t0);
					Splat(f, 0x7f800000);
					f.simd(Simd::V128And);
					f.v128_const_splat(0);
					f.simd(Simd::I32x4Ne);
					f.simd(Simd::V128Bitselect); // exponent != 0 ? |w| : 0x007fffff
				}
				f.local_set(m_t1);
				// +x > value and -x > value as signed integers.
				GetVf(f, in.fs);
				f.local_get(m_t1);
				f.simd(Simd::I32x4GtS);
				GetVf(f, in.fs);
				Splat(f, 0x80000000);
				f.simd(Simd::V128Xor);
				f.local_get(m_t1);
				f.simd(Simd::I32x4GtS);
				f.simd(Simd::I16x8NarrowI32x4S); // i16 lanes p.x p.y p.z p.w n.x n.y n.z n.w
				if (m_opt.tuned)
				{
					// Byte bitmask: p in bits 0-3, n in bits 4-7; a table interleaves them.
					f.local_tee(m_t2);
					f.local_get(m_t2);
					f.simd(Simd::I8x16NarrowI16x8S);
					f.simd(Simd::I8x16Bitmask);
					f.i32_const(0x77);
					f.i32_and();
					f.i32_load8_u(static_cast<u32>(reinterpret_cast<uintptr_t>(kClipTable.v)));
				}
				else
				{
					static constexpr u8 inter[16] = {0, 8, 2, 10, 4, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
					f.shuffle1(inter, m_ts);
					f.simd(Simd::I8x16Bitmask);
					f.i32_const(0x3f);
					f.i32_and();
				}
				f.local_get(m_clip);
				f.i32_const(6);
				f.i32_shl();
				f.i32_or();
				f.i32_const(0xffffff);
				f.i32_and();
				f.local_set(m_clip);
				break;
			}
			case Op::LQI:
				if (in.ft)
				{
					ViAddress(f, in.is);
					f.v128_load(MemAddr());
					WriteDest(f, in.dest, m_vf[in.ft]);
				}
				ViIncrement(f, in.is);
				break;
			case Op::SQI:
				ViAddress(f, in.it);
				GetVf(f, in.fs);
				if (in.dest != 0xf)
				{
					ViAddress(f, in.it);
					f.v128_load(MemAddr());
					PushDestMask(f, in.dest);
					f.simd(Simd::V128Bitselect);
				}
				f.v128_store(MemAddr());
				ViIncrement(f, in.it);
				break;
			case Op::FCAND:
				f.local_get(m_clip);
				f.i32_const(in.imm & 0xffffff);
				f.i32_and();
				f.i32_const(0);
				f.i32_ne();
				f.local_set(m_vi[1]);
				break;
			case Op::IADD:
				if (in.id)
				{
					GetVi(f, in.is);
					GetVi(f, in.it);
					f.i32_add();
					f.i32_const(0xffff);
					f.i32_and();
					f.local_set(m_vi[in.id]);
				}
				break;
			case Op::IADDIU:
			case Op::ISUBIU:
				if (in.it)
				{
					GetVi(f, in.is);
					f.i32_const(in.imm);
					if (in.op == Op::IADDIU)
						f.i32_add();
					else
						f.i32_sub();
					f.i32_const(0xffff);
					f.i32_and();
					f.local_set(m_vi[in.it]);
				}
				break;
			case Op::IBNE:
				// Only backward branches to a loop head (enough for the spike): continue the loop.
				GetVi(f, in.is);
				GetVi(f, in.it);
				f.i32_ne();
				f.br_if(0);
				f.end();
				break;
			case Op::END:
			case Op::Count:
				break;
		}
	}

	void GetVi(Function& f, int r)
	{
		if (r == 0)
			f.i32_const(0);
		else
			f.local_get(m_vi[r]);
	}
	void ViAddress(Function& f, int r)
	{
		GetVi(f, r);
		f.i32_const(4);
		f.i32_shl();
		f.i32_const(0x3fff);
		f.i32_and();
	}
	void ViIncrement(Function& f, int r)
	{
		if (!r)
			return;
		f.local_get(m_vi[r]);
		f.i32_const(1);
		f.i32_add();
		f.i32_const(0xffff);
		f.i32_and();
		f.local_set(m_vi[r]);
	}

	const std::vector<Insn>& m_prog;
	u32 m_base;
	CompileOptions m_opt;
	CompileStats m_stats;
	bool m_vf_used[32] = {};
	bool m_vi_used[16] = {};
	bool m_acc_used = false;
	std::vector<int> m_loop_heads;
	std::vector<bool> m_flag_live;
	std::vector<Clean> m_clean_at;
	int m_vf[32] = {};
	int m_acc = -1;
	int m_vi[16] = {};
	int m_mac = 0, m_status = 0, m_clip = 0;
	int m_t0 = 0, m_t1 = 0, m_t2 = 0, m_ts = 0, m_res = 0;
	int m_const[5] = {};
	int m_shadow[33] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
	bool m_shadow_valid[33] = {};
};

} // namespace vu
