// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// A small WebAssembly binary emitter, enough to build one module in memory: function types,
// imported memory, table and functions, defined functions with locals, and exports. The engine
// validates the module when it compiles it; this file does not.

#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace wasmjit {

using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
using s64 = int64_t;

enum class ValType : u8
{
	I32 = 0x7f,
	I64 = 0x7e,
	F32 = 0x7d,
	F64 = 0x7c,
	V128 = 0x7b,
};

// Opcodes after the 0xFD prefix (SIMD128).
enum class Simd : u32
{
	V128Load = 0x00,
	V128Load32Splat = 0x09,
	V128Store = 0x0b,
	V128Const = 0x0c,
	I8x16Shuffle = 0x0d,
	I8x16Swizzle = 0x0e,
	I32x4Splat = 0x11,
	F32x4Splat = 0x13,
	I32x4ExtractLane = 0x1b,
	I32x4ReplaceLane = 0x1c,
	F32x4ExtractLane = 0x1f,
	I32x4Eq = 0x37,
	I32x4Ne = 0x38,
	I32x4LtS = 0x39,
	I32x4LtU = 0x3a,
	I32x4GtS = 0x3b,
	I32x4GtU = 0x3c,
	F32x4Eq = 0x41,
	F32x4Ne = 0x42,
	F32x4Lt = 0x43,
	F32x4Gt = 0x44,
	F32x4Le = 0x45,
	F32x4Ge = 0x46,
	V128Not = 0x4d,
	V128And = 0x4e,
	V128AndNot = 0x4f,
	V128Or = 0x50,
	V128Xor = 0x51,
	V128Bitselect = 0x52,
	V128AnyTrue = 0x53,
	I8x16Bitmask = 0x64,
	I8x16NarrowI16x8S = 0x65,
	I16x8NarrowI32x4S = 0x85,
	I32x4AllTrue = 0xa3,
	I32x4Bitmask = 0xa4,
	I32x4Shl = 0xab,
	I32x4ShrS = 0xac,
	I32x4ShrU = 0xad,
	I32x4Add = 0xae,
	I32x4Sub = 0xb1,
	I32x4MinS = 0xb6,
	I32x4MinU = 0xb7,
	I32x4MaxS = 0xb8,
	I32x4MaxU = 0xb9,
	F32x4Abs = 0xe0,
	F32x4Neg = 0xe1,
	F32x4Sqrt = 0xe3,
	F32x4Add = 0xe4,
	F32x4Sub = 0xe5,
	F32x4Mul = 0xe6,
	F32x4Div = 0xe7,
	F32x4Min = 0xe8,
	F32x4Max = 0xe9,
	F32x4PMin = 0xea,
	F32x4PMax = 0xeb,
	I32x4TruncSatF32x4S = 0xf8,
	F32x4ConvertI32x4S = 0xfa,
	// Relaxed SIMD (not used for exact code: results may differ between engines and CPUs).
	F32x4RelaxedMadd = 0x105,
};

class ByteBuffer
{
public:
	std::vector<u8> data;

	void byte(u8 v) { data.push_back(v); }
	void bytes(const void* p, size_t n)
	{
		const u8* b = static_cast<const u8*>(p);
		data.insert(data.end(), b, b + n);
	}
	void uleb(u64 v)
	{
		do
		{
			u8 b = v & 0x7f;
			v >>= 7;
			if (v)
				b |= 0x80;
			data.push_back(b);
		} while (v);
	}
	void sleb(s64 v)
	{
		for (;;)
		{
			const u8 b = v & 0x7f;
			v >>= 7; // arithmetic shift (C++20)
			if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)))
			{
				data.push_back(b);
				return;
			}
			data.push_back(b | 0x80);
		}
	}
	void name(std::string_view s)
	{
		uleb(s.size());
		bytes(s.data(), s.size());
	}
	void sized(const ByteBuffer& b)
	{
		uleb(b.data.size());
		bytes(b.data.data(), b.data.size());
	}
};

// Body of one defined function. Parameters take the first local indices.
class Function
{
public:
	explicit Function(u32 num_params = 0)
		: m_next_local(num_params)
	{
	}

	u32 local(ValType t)
	{
		if (!m_locals.empty() && m_locals.back().second == t)
			m_locals.back().first++;
		else
			m_locals.emplace_back(1, t);
		return m_next_local++;
	}

	// Number of instructions emitted so far (each opcode counts once).
	u32 instructions() const { return m_count; }
	size_t body_size() const { return m_body.data.size(); }

	// Control.
	static constexpr u8 kVoid = 0x40;
	void block(u8 type = kVoid) { op(0x02); m_body.byte(type); }
	void loop(u8 type = kVoid) { op(0x03); m_body.byte(type); }
	void if_(u8 type = kVoid) { op(0x04); m_body.byte(type); }
	void else_() { op(0x05); }
	void end() { op(0x0b); }
	void br(u32 depth) { op(0x0c); m_body.uleb(depth); }
	void br_if(u32 depth) { op(0x0d); m_body.uleb(depth); }
	void return_() { op(0x0f); }
	void call(u32 func) { op(0x10); m_body.uleb(func); }
	void call_indirect(u32 type, u32 table = 0) { op(0x11); m_body.uleb(type); m_body.uleb(table); }
	void drop() { op(0x1a); }
	void unreachable() { op(0x00); }

	// Variables.
	void local_get(u32 i) { op(0x20); m_body.uleb(i); }
	void local_set(u32 i) { op(0x21); m_body.uleb(i); }
	void local_tee(u32 i) { op(0x22); m_body.uleb(i); }

	// Memory (memory 0). align is log2 of the alignment hint.
	void i32_load(u32 offset, u32 align = 2) { op(0x28); memarg(align, offset); }
	void f32_load(u32 offset, u32 align = 2) { op(0x2a); memarg(align, offset); }
	void i32_store(u32 offset, u32 align = 2) { op(0x36); memarg(align, offset); }
	void i32_load8_u(u32 offset) { op(0x2d); memarg(0, offset); }
	void f32_store(u32 offset, u32 align = 2) { op(0x38); memarg(align, offset); }

	// Constants and scalar arithmetic.
	void i32_const(s32 v) { op(0x41); m_body.sleb(v); }
	void f32_const(float v) { op(0x43); m_body.bytes(&v, 4); }
	void i32_eqz() { op(0x45); }
	void i32_eq() { op(0x46); }
	void i32_ne() { op(0x47); }
	void i32_lt_u() { op(0x49); }
	void i32_add() { op(0x6a); }
	void i32_sub() { op(0x6b); }
	void i32_mul() { op(0x6c); }
	void i32_and() { op(0x71); }
	void i32_or() { op(0x72); }
	void i32_xor() { op(0x73); }
	void i32_shl() { op(0x74); }
	void i32_shr_s() { op(0x75); }
	void i32_shr_u() { op(0x76); }
	void f32_add() { op(0x92); }
	void f32_sub() { op(0x93); }
	void f32_mul() { op(0x94); }
	void i32_reinterpret_f32() { op(0xbc); }
	void f32_reinterpret_i32() { op(0xbe); }

	// SIMD.
	void simd(Simd o)
	{
		op(0xfd);
		m_body.uleb(static_cast<u32>(o));
	}
	void simd_mem(Simd o, u32 offset, u32 align = 4)
	{
		simd(o);
		memarg(align, offset);
	}
	void simd_lane(Simd o, u8 lane)
	{
		simd(o);
		m_body.byte(lane);
	}
	void v128_load(u32 offset) { simd_mem(Simd::V128Load, offset); }
	void v128_store(u32 offset) { simd_mem(Simd::V128Store, offset); }
	void v128_const(const void* bytes16)
	{
		simd(Simd::V128Const);
		m_body.bytes(bytes16, 16);
	}
	void v128_const_i32x4(u32 a, u32 b, u32 c, u32 d)
	{
		const u32 v[4] = {a, b, c, d};
		v128_const(v);
	}
	void v128_const_splat(u32 a) { v128_const_i32x4(a, a, a, a); }
	void shuffle(const u8 lanes[16])
	{
		simd(Simd::I8x16Shuffle);
		m_body.bytes(lanes, 16);
	}
	// Shuffles the vector on the stack with itself; tmp is a v128 local the caller lends.
	void shuffle1(const u8 lanes[16], u32 tmp)
	{
		local_tee(tmp);
		local_get(tmp);
		shuffle(lanes);
	}
	// Broadcasts 32-bit lane `lane` of the vector on the stack.
	void broadcast32(u8 lane, u32 tmp)
	{
		u8 l[16];
		for (int i = 0; i < 16; i++)
			l[i] = static_cast<u8>(lane * 4 + (i & 3));
		shuffle1(l, tmp);
	}

	ByteBuffer finish() const
	{
		ByteBuffer out;
		out.uleb(m_locals.size());
		for (const auto& [count, type] : m_locals)
		{
			out.uleb(count);
			out.byte(static_cast<u8>(type));
		}
		out.bytes(m_body.data.data(), m_body.data.size());
		out.byte(0x0b); // end of the function body
		return out;
	}

private:
	void op(u8 o)
	{
		m_body.byte(o);
		m_count++;
	}
	void memarg(u32 align, u32 offset)
	{
		m_body.uleb(align);
		m_body.uleb(offset);
	}

	ByteBuffer m_body;
	std::vector<std::pair<u32, ValType>> m_locals;
	u32 m_next_local;
	u32 m_count = 0;
};

class Module
{
public:
	u32 type(std::initializer_list<ValType> params, std::initializer_list<ValType> results)
	{
		ByteBuffer t;
		t.byte(0x60);
		t.uleb(params.size());
		for (ValType v : params)
			t.byte(static_cast<u8>(v));
		t.uleb(results.size());
		for (ValType v : results)
			t.byte(static_cast<u8>(v));
		for (u32 i = 0; i < m_types.size(); i++)
		{
			if (m_types[i].data == t.data)
				return i;
		}
		m_types.push_back(std::move(t));
		return static_cast<u32>(m_types.size() - 1);
	}

	// Imports must all come before the first add_function().
	void import_memory(std::string_view mod, std::string_view field, u32 min_pages, u32 max_pages, bool shared)
	{
		ByteBuffer& b = import_header(mod, field);
		b.byte(0x02);
		b.byte(shared ? 0x03 : 0x01);
		b.uleb(min_pages);
		b.uleb(max_pages);
	}
	void import_table(std::string_view mod, std::string_view field, u32 min)
	{
		ByteBuffer& b = import_header(mod, field);
		b.byte(0x01);
		b.byte(0x70); // funcref
		b.byte(0x00); // no maximum: matches a growable table
		b.uleb(min);
	}
	u32 import_function(std::string_view mod, std::string_view field, u32 type)
	{
		ByteBuffer& b = import_header(mod, field);
		b.byte(0x00);
		b.uleb(type);
		return m_num_imported_funcs++;
	}

	u32 add_function(u32 type, const Function& f)
	{
		m_func_types.push_back(type);
		m_bodies.push_back(f.finish());
		return m_num_imported_funcs + static_cast<u32>(m_func_types.size() - 1);
	}
	void export_function(std::string_view name, u32 index)
	{
		m_exports.name(name);
		m_exports.byte(0x00);
		m_exports.uleb(index);
		m_num_exports++;
	}

	std::vector<u8> finish() const
	{
		ByteBuffer m;
		static constexpr u8 header[8] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
		m.bytes(header, sizeof(header));

		ByteBuffer s;
		s.uleb(m_types.size());
		for (const ByteBuffer& t : m_types)
			s.bytes(t.data.data(), t.data.size());
		section(m, 1, s);

		if (m_num_imports)
		{
			s.data.clear();
			s.uleb(m_num_imports);
			s.bytes(m_imports.data.data(), m_imports.data.size());
			section(m, 2, s);
		}

		s.data.clear();
		s.uleb(m_func_types.size());
		for (u32 t : m_func_types)
			s.uleb(t);
		section(m, 3, s);

		if (m_num_exports)
		{
			s.data.clear();
			s.uleb(m_num_exports);
			s.bytes(m_exports.data.data(), m_exports.data.size());
			section(m, 7, s);
		}

		s.data.clear();
		s.uleb(m_bodies.size());
		for (const ByteBuffer& b : m_bodies)
			s.sized(b);
		section(m, 10, s);
		return std::move(m.data);
	}

private:
	static void section(ByteBuffer& m, u8 id, const ByteBuffer& contents)
	{
		m.byte(id);
		m.sized(contents);
	}
	ByteBuffer& import_header(std::string_view mod, std::string_view field)
	{
		m_imports.name(mod);
		m_imports.name(field);
		m_num_imports++;
		return m_imports;
	}

	std::vector<ByteBuffer> m_types;
	ByteBuffer m_imports;
	u32 m_num_imports = 0;
	u32 m_num_imported_funcs = 0;
	std::vector<u32> m_func_types;
	std::vector<ByteBuffer> m_bodies;
	ByteBuffer m_exports;
	u32 m_num_exports = 0;
};

} // namespace wasmjit
