// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "VU1JitAnalysis.h"

#include "VUmicro.h"

#include "fmt/format.h"

#include <array>
#include <cstring>

// Interpreter primitives the timing model runs on its scratch VURegs. VUops.h declares most of them; these two are
// defined in VUops.cpp without a declaration in a header.
extern void _vuFlushAll(VURegs* VU);
extern void _vuBackupVI(VURegs* VU, u32 reg);

namespace VU1Jit
{
	//////////////////////////////////////////////////////////////////////////
	// Decoding

	using U = Upper;
	using L = Lower;

	// VU1_UPPER_OPCODE[0x00..0x2f] (VUops.cpp:3758-3771).
	static constexpr Upper s_upper_main[48] = {
		U::ADDx, U::ADDy, U::ADDz, U::ADDw, U::SUBx, U::SUBy, U::SUBz, U::SUBw,
		U::MADDx, U::MADDy, U::MADDz, U::MADDw, U::MSUBx, U::MSUBy, U::MSUBz, U::MSUBw,
		U::MAXx, U::MAXy, U::MAXz, U::MAXw, U::MINIx, U::MINIy, U::MINIz, U::MINIw,
		U::MULx, U::MULy, U::MULz, U::MULw, U::MULq, U::MAXi, U::MULi, U::MINIi,
		U::ADDq, U::MADDq, U::ADDi, U::MADDi, U::SUBq, U::MSUBq, U::SUBi, U::MSUBi,
		U::ADD, U::MADD, U::MUL, U::MAX, U::SUB, U::MSUB, U::OPMSUB, U::MINI,
	};

	// _UPPER_FD_00.._11_TABLE[0..11] (VUops.cpp:3714-3756), selected by code & 3 of 0x3c-0x3f and indexed by
	// (code >> 6) & 0x1f. Entries 12-31 are unknown.
	static constexpr Upper s_upper_fd[4][12] = {
		{U::ADDAx, U::SUBAx, U::MADDAx, U::MSUBAx, U::ITOF0, U::FTOI0, U::MULAx, U::MULAq, U::ADDAq, U::SUBAq, U::ADDA, U::SUBA},
		{U::ADDAy, U::SUBAy, U::MADDAy, U::MSUBAy, U::ITOF4, U::FTOI4, U::MULAy, U::ABS, U::MADDAq, U::MSUBAq, U::MADDA, U::MSUBA},
		{U::ADDAz, U::SUBAz, U::MADDAz, U::MSUBAz, U::ITOF12, U::FTOI12, U::MULAz, U::MULAi, U::ADDAi, U::SUBAi, U::MULA, U::OPMULA},
		{U::ADDAw, U::SUBAw, U::MADDAw, U::MSUBAw, U::ITOF15, U::FTOI15, U::MULAw, U::CLIP, U::MADDAi, U::MSUBAi, U::Unknown, U::NOP},
	};

	static Upper DecodeUpper(u32 code)
	{
		const u32 op = code & 0x3f;
		if (op < 0x30)
			return s_upper_main[op];
		if (op < 0x3c)
			return U::Unknown;
		const u32 sub = (code >> 6) & 0x1f;
		return (sub < 12) ? s_upper_fd[op & 3][sub] : U::Unknown;
	}

	// LowerOP_T3_00.._11_OPCODE (VUops.cpp:3615-3663), indexed by (code >> 6) & 0x1f.
	static Lower DecodeLowerT3(u32 group, u32 sub)
	{
		switch ((group << 8) | sub)
		{
			case 0x00c: return L::MOVE;
			case 0x00d: return L::LQI;
			case 0x00e: return L::DIV;
			case 0x00f: return L::MTIR;
			case 0x010: return L::RNEXT;
			case 0x019: return L::MFP;
			case 0x01a: return L::XTOP;
			case 0x01b: return L::XGKICK;
			case 0x01c: return L::ESADD;
			case 0x01d: return L::EATANxy;
			case 0x01e: return L::ESQRT;
			case 0x01f: return L::ESIN;
			case 0x10c: return L::MR32;
			case 0x10d: return L::SQI;
			case 0x10e: return L::SQRT;
			case 0x10f: return L::MFIR;
			case 0x110: return L::RGET;
			case 0x11a: return L::XITOP;
			case 0x11c: return L::ERSADD;
			case 0x11d: return L::EATANxz;
			case 0x11e: return L::ERSQRT;
			case 0x11f: return L::EATAN;
			case 0x20d: return L::LQD;
			case 0x20e: return L::RSQRT;
			case 0x20f: return L::ILWR;
			case 0x210: return L::RINIT;
			case 0x21c: return L::ELENG;
			case 0x21d: return L::ESUM;
			case 0x21e: return L::ERCPR;
			case 0x21f: return L::EEXP;
			case 0x30d: return L::SQD;
			case 0x30e: return L::WAITQ;
			case 0x30f: return L::ISWR;
			case 0x310: return L::RXOR;
			case 0x31c: return L::ERLENG;
			case 0x31e: return L::WAITP;
			default: return L::Unknown;
		}
	}

	// VU1_LOWER_OPCODE[code >> 25] (VUops.cpp:3683-3712) and LowerOP_OPCODE[code & 0x3f] (:3665-3681).
	static Lower DecodeLower(u32 code)
	{
		switch (code >> 25)
		{
			case 0x00: return L::LQ;
			case 0x01: return L::SQ;
			case 0x04: return L::ILW;
			case 0x05: return L::ISW;
			case 0x08: return L::IADDIU;
			case 0x09: return L::ISUBIU;
			case 0x10: return L::FCEQ;
			case 0x11: return L::FCSET;
			case 0x12: return L::FCAND;
			case 0x13: return L::FCOR;
			case 0x14: return L::FSEQ;
			case 0x15: return L::FSSET;
			case 0x16: return L::FSAND;
			case 0x17: return L::FSOR;
			case 0x18: return L::FMEQ;
			case 0x1a: return L::FMAND;
			case 0x1b: return L::FMOR;
			case 0x1c: return L::FCGET;
			case 0x20: return L::B;
			case 0x21: return L::BAL;
			case 0x24: return L::JR;
			case 0x25: return L::JALR;
			case 0x28: return L::IBEQ;
			case 0x29: return L::IBNE;
			case 0x2c: return L::IBLTZ;
			case 0x2d: return L::IBGTZ;
			case 0x2e: return L::IBLEZ;
			case 0x2f: return L::IBGEZ;
			case 0x40:
				switch (code & 0x3f)
				{
					case 0x30: return L::IADD;
					case 0x31: return L::ISUB;
					case 0x32: return L::IADDI;
					case 0x34: return L::IAND;
					case 0x35: return L::IOR;
					case 0x3c:
					case 0x3d:
					case 0x3e:
					case 0x3f: return DecodeLowerT3(code & 3, (code >> 6) & 0x1f);
					default: return L::Unknown;
				}
			default: return L::Unknown;
		}
	}

	Instruction Decode(u32 lower, u32 upper)
	{
		Instruction ins;
		ins.lower = lower;
		ins.upper = upper;
		ins.up = DecodeUpper(upper);
		ins.lo = (upper & 0x80000000u) ? L::Immediate : DecodeLower(lower);
		return ins;
	}

#define VU1JIT_NAME_ENTRY(name) #name,
	static constexpr const char* s_upper_names[] = {"???", VU1JIT_UPPER_OPS(VU1JIT_NAME_ENTRY)};
	static constexpr const char* s_lower_names[] = {"???", "LOI", VU1JIT_LOWER_OPS(VU1JIT_NAME_ENTRY)};
#undef VU1JIT_NAME_ENTRY
	static_assert(std::size(s_upper_names) == static_cast<size_t>(Upper::Count));
	static_assert(std::size(s_lower_names) == static_cast<size_t>(Lower::Count));

	const char* UpperName(Upper op) { return s_upper_names[static_cast<u32>(op)]; }
	const char* LowerName(Lower op) { return s_lower_names[static_cast<u32>(op)]; }

	u16 GetLowerFlags(Lower op)
	{
		switch (op)
		{
			case L::B: return LOWER_BRANCH;
			case L::BAL: return LOWER_BRANCH | LOWER_LINK;
			case L::JR: return LOWER_BRANCH | LOWER_DYNAMIC;
			case L::JALR: return LOWER_BRANCH | LOWER_DYNAMIC | LOWER_LINK;
			case L::IBEQ:
			case L::IBNE:
			case L::IBLTZ:
			case L::IBGTZ:
			case L::IBLEZ:
			case L::IBGEZ: return LOWER_BRANCH | LOWER_COND | LOWER_VI_OLD;
			case L::SQ:
			case L::SQD:
			case L::SQI:
			case L::ISW:
			case L::ISWR: return LOWER_STORE;
			case L::XGKICK: return LOWER_XGKICK;
			case L::DIV:
			case L::SQRT:
			case L::RSQRT:
			case L::WAITQ: return LOWER_FDIV;
			case L::ESADD:
			case L::ERSADD:
			case L::ELENG:
			case L::ERLENG:
			case L::EATANxy:
			case L::EATANxz:
			case L::ESUM:
			case L::ERCPR:
			case L::ESQRT:
			case L::ERSQRT:
			case L::ESIN:
			case L::EATAN:
			case L::EEXP:
			case L::WAITP: return LOWER_EFU;
			default: return 0;
		}
	}

	namespace
	{
		struct OpcodeBits
		{
			std::array<u32, static_cast<size_t>(Upper::Count)> upper{};
			std::array<u32, static_cast<size_t>(Lower::Count)> lower{};

			OpcodeBits()
			{
				for (u32 op = 0; op < 0x40; op++)
				{
					for (u32 sub = 0; sub < ((op >= 0x3c) ? 32u : 1u); sub++)
					{
						const u32 code = op | (sub << 6);
						const Upper u = DecodeUpper(code);
						if (u != U::Unknown && !upper[static_cast<u32>(u)])
							upper[static_cast<u32>(u)] = code | 0x80000000u; // marks "found"; stripped below
					}
				}
				for (u32 main = 0; main < 0x80; main++)
				{
					for (u32 low = 0; low < ((main == 0x40) ? 64u : 1u); low++)
					{
						for (u32 sub = 0; sub < ((main == 0x40 && low >= 0x3c) ? 32u : 1u); sub++)
						{
							const u32 code = (main << 25) | low | (sub << 6);
							const Lower l = DecodeLower(code);
							if (l != L::Unknown && !lower[static_cast<u32>(l)])
								lower[static_cast<u32>(l)] = code;
						}
					}
				}
				for (u32& u : upper)
					u &= 0x7fffffffu;
			}
		};
	} // namespace

	static const OpcodeBits& GetOpcodeBits()
	{
		static const OpcodeBits bits;
		return bits;
	}

	u32 UpperOpcodeBits(Upper op) { return GetOpcodeBits().upper[static_cast<u32>(op)]; }
	u32 LowerOpcodeBits(Lower op) { return GetOpcodeBits().lower[static_cast<u32>(op)]; }

	int BackupVIRegister(const Instruction& ins)
	{
		const u32 code = ins.lower;
		const int it = (code >> 16) & 0xf; // _It_
		const int is = (code >> 11) & 0xf; // _Is_
		const int id = (code >> 6) & 0xf; // _Id_
		switch (ins.lo)
		{
			// "if (_Id_ == 0) return; _vuBackupVI(VU, _Id_);" (VUops.cpp:1036-1076)
			case L::IADD:
			case L::ISUB:
			case L::IAND:
			case L::IOR:
				return id ? id : -1;
			// "if (_It_ == 0) return; _vuBackupVI(VU, _It_);" (VUops.cpp:1014-1034, 1078-1086, 1108-1116)
			case L::IADDI:
			case L::IADDIU:
			case L::ISUBIU:
			case L::MTIR:
				return it ? it : -1;
			// Unconditional (VUops.cpp:1168-1234).
			case L::LQD:
			case L::LQI:
				return is;
			case L::SQD:
			case L::SQI:
				return it;
			default:
				return -1;
		}
	}

	u32 StaticBranchTarget(u32 pc, u32 lower)
	{
		// _branchAddr: TPC (already advanced past the instruction, unmasked) + _Imm11_ * 8, masked for VU1.
		const s32 imm11 = (lower & 0x400) ? static_cast<s32>(0xfffffc00u | (lower & 0x3ff)) : static_cast<s32>(lower & 0x3ff);
		return static_cast<u32>(static_cast<s32>(pc + 8) + imm11 * 8) & 0x3fff;
	}

	std::string Disassemble(u32 lower, u32 upper)
	{
		const Instruction ins = Decode(lower, upper);
		static constexpr const char* dests[16] = {"", ".w", ".z", ".zw", ".y", ".yw", ".yz", ".yzw", ".x", ".xw",
			".xz", ".xzw", ".xy", ".xyw", ".xyz", ".xyzw"};
		std::string s = fmt::format("{}{} fd{} fs{} ft{}", UpperName(ins.up), dests[(upper >> 21) & 0xf],
			(upper >> 6) & 0x1f, (upper >> 11) & 0x1f, (upper >> 16) & 0x1f);
		if (ins.IBit())
			s += fmt::format(" / LOI {:08x}", lower);
		else
			s += fmt::format(" / {}{} t{} s{} d{} imm{:03x}", LowerName(ins.lo), dests[(lower >> 21) & 0xf],
				(lower >> 16) & 0x1f, (lower >> 11) & 0x1f, (lower >> 6) & 0x1f, lower & 0x7ff);
		if (ins.EBit())
			s += " [E]";
		if (ins.MBit())
			s += " [M]";
		if (ins.DBit())
			s += " [D]";
		if (ins.TBit())
			s += " [T]";
		return s;
	}

	//////////////////////////////////////////////////////////////////////////
	// Signatures

	static constexpr u32 FMAC_LATENCY = 4; // _vuAddFMACStalls (VUops.cpp:331)
	static constexpr u32 IALU_LATENCY = 4; // ILW and ILWR, the only IALU ops with cycles (VUops.cpp:2501-2532)
	static constexpr u32 FDIV_LATENCY = 13; // RSQRT; DIV and SQRT are 7 (VUops.cpp:2341-2386)
	static constexpr u32 EFU_LATENCY = 54; // EATAN, EATANxy, EATANxz (VUops.cpp:2838-2851)

	bool Signature::IsClean() const
	{
		static constexpr Signature clean = {};
		return *this == clean;
	}

	u32 Signature::Hash() const
	{
		u32 h = 2166136261u;
		const u8* p = reinterpret_cast<const u8*>(this);
		for (size_t i = 0; i < sizeof(*this); i++)
			h = (h ^ p[i]) * 16777619u;
		return h;
	}

	std::string Signature::ToString() const
	{
		if (IsClean())
			return "clean";
		std::string s;
		for (u32 i = 0; i < fmac_count; i++)
		{
			const Fmac& f = fmac[i];
			s += fmt::format("fmac[{}r up {}.{:x} lo {}.{:x}{}{}] ", f.remaining, f.regupper, f.xyzwupper, f.reglower,
				f.xyzwlower, (f.flags & SIG_FMAC_CLIP) ? " clip" : "", (f.flags & SIG_FMAC_STATUS) ? " status" : "");
		}
		if (fdiv)
			s += fmt::format("fdiv[{}r] ", fdiv);
		if (efu)
			s += fmt::format("efu[{}r] ", efu);
		for (u32 i = 0; i < ialu_count; i++)
			s += fmt::format("ialu[{}r {:04x}] ", ialu[i].remaining, ialu[i].reg);
		if (vi_backup_cycles)
			s += fmt::format("vibackup[{} vi{}]", vi_backup_cycles, vi_backup_reg);
		return s;
	}

	bool operator==(const Signature& a, const Signature& b)
	{
		return std::memcmp(&a, &b, sizeof(Signature)) == 0;
	}

	const char* RefusalName(Refusal r)
	{
		static constexpr const char* names[] = {"none", "fmac ring", "fmac stale", "fmac range", "ialu ring",
			"ialu stale", "ialu range", "fdiv stale", "fdiv range", "efu stale", "efu range", "vi backup"};
		static_assert(std::size(names) == static_cast<size_t>(Refusal::Count));
		return names[static_cast<u32>(r)];
	}

	// Cycles until an entry completes, or 0 if it is out of the range the interpreter produces. Only the completion
	// time sCycle + Cycle matters: every flush test, stall test and _vuFlushAll compares against it
	// (VUops.cpp:45, 97, 116, 149, 171, 181, 217, 251, 271, 284), and an entry that started at or before the current
	// cycle makes all of them exact.
	static u32 Remaining(u64 sCycle, u32 Cycle, u64 cycle, u32 latency)
	{
		const u64 end = sCycle + Cycle;
		if (end <= cycle || (end - cycle) > latency)
			return 0;
		return static_cast<u32>(end - cycle);
	}

	Refusal DeriveSignature(const VURegs& vu, Signature* out)
	{
		Signature s = {};

		// FMAC: fmac[], fmacreadpos, fmacwritepos, fmaccount (VUops.cpp:37-72, 210-243, 317-349).
		if (vu.fmaccount > 4 || vu.fmacreadpos > 3 || vu.fmacwritepos > 3 ||
			((vu.fmacreadpos + vu.fmaccount) & 3) != vu.fmacwritepos)
		{
			return Refusal::FmacRing;
		}
		s.fmac_count = static_cast<u8>(vu.fmaccount);
		for (u32 i = 0; i < vu.fmaccount; i++)
		{
			const fmacPipe& p = vu.fmac[(vu.fmacreadpos + i) & 3];
			if (p.sCycle > vu.cycle)
				return Refusal::FmacStale;
			const u32 remaining = Remaining(p.sCycle, p.Cycle, vu.cycle, FMAC_LATENCY);
			if (remaining == 0 || p.regupper > 31 || p.reglower > 31)
				return Refusal::FmacRange;
			// Each instruction adds at most one entry, at its own cycle, so completions rise along the ring. Entries
			// out of that order were left from before VU1.cycle = 0 (MTVU.cpp:145) at a time the reset did not make
			// them look stale. The flush stops at the first incomplete entry (VUops.cpp:45-49), so they block the
			// ring, which then overflows past 4 entries.
			if (i && remaining <= s.fmac[i - 1].remaining)
				return Refusal::FmacStale;
			Signature::Fmac& f = s.fmac[i];
			f.remaining = static_cast<u8>(remaining);
			f.regupper = static_cast<u8>(p.regupper);
			f.xyzwupper = p.regupper ? static_cast<u8>(p.xyzwupper & 0xf) : 0;
			f.reglower = static_cast<u8>(p.reglower);
			f.xyzwlower = p.reglower ? static_cast<u8>(p.xyzwlower & 0xf) : 0;
			f.flags = ((p.flagreg & (1 << REG_CLIP_FLAG)) ? SIG_FMAC_CLIP : 0) |
					  ((p.flagreg & (1 << REG_STATUS_FLAG)) ? SIG_FMAC_STATUS : 0);
		}

		// IALU: ialu[], ialureadpos, ialuwritepos, ialucount (VUops.cpp:74-90, 278-296, 373-388).
		if (vu.ialucount > 4 || vu.ialureadpos > 3 || vu.ialuwritepos > 3 ||
			((vu.ialureadpos + vu.ialucount) & 3) != vu.ialuwritepos)
		{
			return Refusal::IaluRing;
		}
		s.ialu_count = static_cast<u8>(vu.ialucount);
		for (u32 i = 0; i < vu.ialucount; i++)
		{
			const ialuPipe& p = vu.ialu[(vu.ialureadpos + i) & 3];
			if (p.sCycle > vu.cycle)
				return Refusal::IaluStale;
			const u32 remaining = Remaining(p.sCycle, p.Cycle, vu.cycle, IALU_LATENCY);
			if (remaining == 0 || (static_cast<u32>(p.reg) >> 16) != 0)
				return Refusal::IaluRange;
			if (i && remaining <= s.ialu[i - 1].remaining) // as for FMAC entries, above
				return Refusal::IaluStale;
			s.ialu[i].remaining = static_cast<u8>(remaining);
			s.ialu[i].reg = static_cast<u16>(p.reg);
		}

		// FDIV and EFU (VUops.cpp:92-127, 245-276, 352-371).
		if (vu.fdiv.enable)
		{
			if (vu.fdiv.sCycle > vu.cycle)
				return Refusal::FdivStale;
			s.fdiv = static_cast<u8>(Remaining(vu.fdiv.sCycle, vu.fdiv.Cycle, vu.cycle, FDIV_LATENCY));
			if (s.fdiv == 0)
				return Refusal::FdivRange;
		}
		if (vu.efu.enable)
		{
			if (vu.efu.sCycle > vu.cycle)
				return Refusal::EfuStale;
			s.efu = static_cast<u8>(Remaining(vu.efu.sCycle, vu.efu.Cycle, vu.cycle, EFU_LATENCY));
			if (s.efu == 0)
				return Refusal::EfuRange;
		}

		// The VI branch delay window (_vuBackupVI, VUops.cpp:420-434; the decrement, VU1microInterp.cpp:100-101).
		if (vu.VIBackupCycles > 2 || (vu.VIBackupCycles && vu.VIRegNumber > 15))
			return Refusal::ViBackup;
		s.vi_backup_cycles = vu.VIBackupCycles;
		s.vi_backup_reg = vu.VIBackupCycles ? static_cast<u8>(vu.VIRegNumber) : 0;

		*out = s;
		return Refusal::None;
	}

	Control GetControl(const VURegs& vu)
	{
		Control c;
		c.branch = vu.branch;
		c.branchpc = vu.branchpc;
		c.takedelaybranch = vu.takedelaybranch;
		c.delaybranchpc = vu.delaybranchpc;
		c.ebit = vu.ebit;
		return c;
	}

	//////////////////////////////////////////////////////////////////////////
	// The timing model

	// The scratch keeps absolute times around an arbitrary base. Every entry is loaded with sCycle = base - SLACK and
	// Cycle = remaining + SLACK: the same completion time, an entry that started before the current cycle, and room
	// for efu.Cycle -= 1 (VUops.cpp:270) without wrapping.
	static constexpr u64 MODEL_BASE = u64(1) << 24;
	static constexpr u32 MODEL_SLACK = 64;

	TimingModel::TimingModel()
	{
		Load(Signature{}, Control{});
	}

	void TimingModel::Load(const Signature& sig, const Control& ctl)
	{
		std::memset(static_cast<void*>(&m_vu), 0, sizeof(m_vu));
		m_vu.cycle = MODEL_BASE;

		for (u32 i = 0; i < sig.fmac_count; i++)
		{
			const Signature::Fmac& f = sig.fmac[i];
			fmacPipe& p = m_vu.fmac[i];
			p.regupper = f.regupper;
			p.xyzwupper = f.xyzwupper;
			p.reglower = f.reglower;
			p.xyzwlower = f.xyzwlower;
			p.flagreg = ((f.flags & SIG_FMAC_CLIP) ? (1 << REG_CLIP_FLAG) : 0) | ((f.flags & SIG_FMAC_STATUS) ? (1 << REG_STATUS_FLAG) : 0);
			p.sCycle = MODEL_BASE - MODEL_SLACK;
			p.Cycle = f.remaining + MODEL_SLACK;
		}
		m_vu.fmacreadpos = 0;
		m_vu.fmaccount = sig.fmac_count;
		m_vu.fmacwritepos = sig.fmac_count & 3;

		for (u32 i = 0; i < sig.ialu_count; i++)
		{
			ialuPipe& p = m_vu.ialu[i];
			p.reg = sig.ialu[i].reg;
			p.sCycle = MODEL_BASE - MODEL_SLACK;
			p.Cycle = sig.ialu[i].remaining + MODEL_SLACK;
		}
		m_vu.ialureadpos = 0;
		m_vu.ialucount = sig.ialu_count;
		m_vu.ialuwritepos = sig.ialu_count & 3;

		m_vu.fdiv.enable = sig.fdiv != 0;
		m_vu.fdiv.sCycle = MODEL_BASE - MODEL_SLACK;
		m_vu.fdiv.Cycle = sig.fdiv + MODEL_SLACK;
		m_vu.efu.enable = sig.efu != 0;
		m_vu.efu.sCycle = MODEL_BASE - MODEL_SLACK;
		m_vu.efu.Cycle = sig.efu + MODEL_SLACK;

		m_vu.VIBackupCycles = sig.vi_backup_cycles;
		m_vu.VIRegNumber = sig.vi_backup_reg;

		m_vu.branch = ctl.branch;
		m_vu.branchpc = ctl.branchpc;
		m_vu.takedelaybranch = ctl.takedelaybranch;
		m_vu.delaybranchpc = ctl.delaybranchpc;
		m_vu.ebit = ctl.ebit;
	}

	bool TimingModel::GetSignature(Signature* out) const
	{
		return DeriveSignature(m_vu, out) == Refusal::None;
	}

	Control TimingModel::GetControl() const
	{
		return VU1Jit::GetControl(m_vu);
	}

	u32 TimingModel::GetTPC() const
	{
		return m_vu.VI[REG_TPC].UL;
	}

	u64 TimingModel::GetCycles() const
	{
		return m_vu.cycle - MODEL_BASE;
	}

	// VU1regs_*_OPCODE read the instruction from the global VU1.code. The model runs on the thread that runs VU1, before
	// the code it models runs, and every interpreter step sets VU1.code before using it; it is restored anyway.
	static void GetRegisterInfo(u32 code, bool upper, _VURegsNum* regs)
	{
		std::memset(regs, 0, sizeof(*regs)); // the interpreter leaves fields unset (section 4.2)
		const u32 saved = VU1.code;
		VU1.code = code;
		if (upper)
			VU1regs_UPPER_OPCODE[code & 0x3f](regs);
		else
			VU1regs_LOWER_OPCODE[code >> 25](regs);
		VU1.code = saved;
	}

	namespace
	{
		struct Flushes
		{
			u8 fmac = 0;
			u8 ialu = 0;
			bool fdiv = false;
			bool efu = false;
		};
	} // namespace

	// _vuTestPipes on the scratch (not &VU1, so it feeds no XGKICK), counting what it wrote back.
	static Flushes TestPipes(VURegs* VU)
	{
		const u32 fmac = VU->fmaccount;
		const u32 ialu = VU->ialucount;
		const bool fdiv = VU->fdiv.enable != 0;
		const bool efu = VU->efu.enable != 0;
		_vuTestPipes(VU);
		Flushes f;
		f.fmac = static_cast<u8>(fmac - VU->fmaccount);
		f.ialu = static_cast<u8>(ialu - VU->ialucount);
		f.fdiv = fdiv && !VU->fdiv.enable;
		f.efu = efu && !VU->efu.enable;
		return f;
	}

	// _setBranch (VUops.cpp:1470-1484).
	static void SetBranch(VURegs* VU, u32 bpc)
	{
		if (VU->branch == 1)
		{
			VU->delaybranchpc = bpc;
			VU->takedelaybranch = true;
		}
		else
		{
			VU->branch = 2;
			VU->branchpc = bpc;
		}
	}

	// A copy of _vu1Exec's orchestration (VU1microInterp.cpp:38-247, with vu1Exec's cycle++ at :251) on the
	// scratch, for the MTVU interpreter (THREAD_VU1_INTERP). Timing decisions are made by the interpreter's own
	// functions; data work is left out. Line references are to VU1microInterp.cpp unless noted.
	bool TimingModel::Step(const StepInput& in, StepInfo* out)
	{
		*out = {};
		out->backup_vi = -1;
		const Instruction ins = Decode(in.lower, in.upper);
		if (!ins.IsValid())
			return false;

		VURegs* const VU = &m_vu;
		const u64 cycle_start = VU->cycle;

		// :44-45 fetch.
		VU->VI[REG_TPC].UL = in.pc + 8;

		// :47-84 E, D and T bits. On MTVU, D/T stops are reported through s_mtvu_dt_stop.
		out->ebit_fetched = ins.EBit();
		if (ins.EBit())
		{
			out->ebit_rearmed = VU->ebit != 0;
			VU->ebit = 2;
		}
		if (ins.DBit() && (in.fbrst & 0x400))
		{
			out->dt_stop = true;
			VU->ebit = 1;
		}
		if (ins.TBit() && (in.fbrst & 0x800))
		{
			out->dt_stop = true;
			VU->ebit = 1;
		}

		// :88-89 upper register info.
		_VURegsNum uregs, lregs;
		GetRegisterInfo(in.upper, true, &uregs);
		std::memset(&lregs, 0, sizeof(lregs));

		// vu1Exec (:251) increments the cycle; :91 reads it from the global VU1, which is VU in the interpreter.
		VU->cycle++;
		const u32 cyclesBeforeOp = VU->cycle - 1;

		// :93
		const u64 before_upper = VU->cycle;
		_vuTestUpperStalls(VU, &uregs);
		out->upper_stall = static_cast<u32>(VU->cycle - before_upper);

		Flushes flushes;
		out->ibit = ins.IBit();
		if (ins.IBit())
		{
			// :96-109 I bit: no lower op; lregs stays zeroed (:108).
			flushes = TestPipes(VU);
			if (VU->VIBackupCycles > 0)
				VU->VIBackupCycles -= std::min((u8)(VU->cycle - cyclesBeforeOp), VU->VIBackupCycles);
			out->vi_backup_cycles = VU->VIBackupCycles;
			out->vi_backup_reg = VU->VIBackupCycles ? static_cast<u8>(VU->VIRegNumber) : 0;
			// The upper op, then VI[REG_I] = the lower word: data only.
		}
		else
		{
			// :120-128
			GetRegisterInfo(in.lower, false, &lregs);
			const u64 before_lower = VU->cycle;
			out->fdiv_wait = (lregs.pipe == VUPIPE_FDIV) && VU->fdiv.enable;
			out->efu_wait = (lregs.pipe == VUPIPE_EFU) && VU->efu.enable;
			_vuTestLowerStalls(VU, &lregs);
			out->lower_stall = static_cast<u32>(VU->cycle - before_lower);
			out->ialu_wait = (lregs.pipe == VUPIPE_BRANCH) && out->lower_stall != 0;
			flushes = TestPipes(VU);
			if (VU->VIBackupCycles > 0)
				VU->VIBackupCycles -= std::min((u8)(VU->cycle - cyclesBeforeOp), VU->VIBackupCycles);
			out->vi_backup_cycles = VU->VIBackupCycles;
			out->vi_backup_reg = VU->VIBackupCycles ? static_cast<u8>(VU->VIRegNumber) : 0;

			// :130-158 upper/lower hazards (register indices only).
			bool discard = false;
			if (uregs.VFwrite)
			{
				if (lregs.VFwrite == uregs.VFwrite)
					discard = true;
				if (lregs.VFread0 == uregs.VFwrite || lregs.VFread1 == uregs.VFwrite)
					out->old_vf = uregs.VFwrite;
			}
			if (uregs.VIwrite & (1 << REG_CLIP_FLAG))
			{
				if (lregs.VIwrite & (1 << REG_CLIP_FLAG))
					discard = true;
				if (lregs.VIread & (1 << REG_CLIP_FLAG))
					out->old_clip = true;
			}
			out->discarded = discard;
			out->lower_runs = !discard;

			// :160-185 the upper op, then the lower op unless discarded. Of the op bodies, only these affect timing
			// or control flow: _vuBackupVI, _setBranch and a busy XGKICK.
			if (!discard)
			{
				const u16 flags = GetLowerFlags(ins.lo);
				out->backup_vi = static_cast<s8>(BackupVIRegister(ins));
				if (out->backup_vi >= 0)
				{
					out->backup_chained = VU->VIBackupCycles && static_cast<u32>(out->backup_vi) == VU->VIRegNumber;
					_vuBackupVI(VU, static_cast<u32>(out->backup_vi));
				}
				if ((flags & LOWER_BRANCH) && (!(flags & LOWER_COND) || in.branch_taken))
				{
					out->branch_set = true;
					out->delay_branch = VU->branch == 1;
					SetBranch(VU, (flags & LOWER_DYNAMIC) ? in.jump_target : StaticBranchTarget(in.pc, in.lower));
				}
				if ((flags & LOWER_XGKICK) && in.kick_bump)
				{
					// _vuXGKICK with the previous kick still running (VUops.cpp:1925-1926, 1887-1888, 1915-1919):
					// _vuXGKICKTransfer(0, true) adds 2 cycles per qword it sends, then calls _vuTestPipes.
					VU->cycle += in.kick_bump;
					const Flushes k = TestPipes(VU);
					out->kick_fmac_flushed = k.fmac;
					out->kick_ialu_flushed = k.ialu;
					out->kick_fdiv_flushed = k.fdiv;
					out->kick_efu_flushed = k.efu;
				}
			}
		}
		out->fmac_flushed = flushes.fmac;
		out->ialu_flushed = flushes.ialu;
		out->fdiv_flushed = flushes.fdiv;
		out->efu_flushed = flushes.efu;
		out->op_cycle = static_cast<u32>(before_upper - cycle_start) + out->upper_stall + out->lower_stall;

		// :188-192 pipe entries.
		const bool fmac_op = uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC;
		if (fmac_op)
			_vuClearFMAC(VU);
		[[maybe_unused]] const bool fdiv_enabled = VU->fdiv.enable != 0;
		[[maybe_unused]] const bool efu_enabled = VU->efu.enable != 0;
		const u32 ialu_count = VU->ialucount;
		_vuAddUpperStalls(VU, &uregs);
		_vuAddLowerStalls(VU, &lregs);
		out->fmac_added = fmac_op;
		if (fmac_op)
		{
			const int flagreg = VU->fmac[VU->fmacwritepos].flagreg;
			out->fmac_added_flags = ((flagreg & (1 << REG_CLIP_FLAG)) ? SIG_FMAC_CLIP : 0) |
									((flagreg & (1 << REG_STATUS_FLAG)) ? SIG_FMAC_STATUS : 0);
		}
		out->fdiv_added = (lregs.pipe == VUPIPE_FDIV) && (lregs.VIwrite & (1 << REG_Q));
		out->efu_added = (lregs.pipe == VUPIPE_EFU) && (lregs.VIwrite & (1 << REG_P));
		out->ialu_added = VU->ialucount != ialu_count;
		pxAssert(!out->fdiv_added || !fdiv_enabled); // the stall test waited for the previous FDIV entry
		pxAssert(!out->efu_added || !efu_enabled);

		// :194-208 branch countdown.
		if (VU->branch > 0)
		{
			if (VU->branch-- == 1)
			{
				VU->VI[REG_TPC].UL = VU->branchpc;
				if (VU->takedelaybranch)
				{
					VU->branch = 1;
					VU->branchpc = VU->delaybranchpc;
					VU->takedelaybranch = false;
				}
			}
		}

		// :210-215 E-bit countdown and the program end. The XGKICK flush that follows (:226-227) adds no cycles:
		// vu1InterpMTVURunning is already false (VUops.cpp:1887).
		if (VU->ebit > 0)
		{
			if (VU->ebit-- == 1)
			{
				VU->VIBackupCycles = 0;
				_vuFlushAll(VU);
				out->end = true;
			}
		}

		// :245-246
		if (fmac_op)
			VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;

		out->cycles = static_cast<u32>(VU->cycle - cycle_start);
		return true;
	}
} // namespace VU1Jit
