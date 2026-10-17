// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "VU1JitLockstep.h"

#include "VU1JitHooks.h"

#include "MTVU.h"

#include "fmt/format.h"

#include <cstring>

namespace VU1Jit
{
	//////////////////////////////////////////////////////////////////////////
	// PATH1 recorder and interrupt sink

	void Path1Record::Reset()
	{
		bytes.clear();
		tag_ends.clear();
		hash = 0xcbf29ce484222325ULL;
		size = 0;
	}

	void Path1Record::Append(const u8* data, u32 n, bool complete)
	{
		if (keep_bytes)
			bytes.insert(bytes.end(), data, data + n);
		for (u32 i = 0; i < n; i++)
			hash = (hash ^ data[i]) * 0x100000001b3ULL;
		size += n;
		if (complete)
			tag_ends.push_back(static_cast<u32>(size));
	}

	static thread_local RecordingScope* t_innermost;

	void RecordPath1(u8* data, u32 size, bool complete)
	{
		for (RecordingScope* s = t_innermost; s; s = s->m_outer)
		{
			if (s->m_path1)
				s->m_path1->Append(data, size, complete);
			if (!s->m_pass_through)
				return;
		}
		VU1JitHooks::Path1ToGif(data, size, complete);
	}

	void RecordInterrupt(u32 flag)
	{
		for (RecordingScope* s = t_innermost; s; s = s->m_outer)
		{
			if (s->m_interrupts)
			{
				*s->m_interrupts |= flag;
				return;
			}
		}
		vu1Thread.mtvuInterrupts.fetch_or(flag, std::memory_order_release);
	}

	RecordingScope::RecordingScope(Path1Record* path1, u32* interrupts, bool pass_through)
		: m_path1(path1)
		, m_interrupts(interrupts)
		, m_pass_through(pass_through || !path1)
		, m_outer(t_innermost)
	{
		t_innermost = this;
		VU1JitHooks::path1 = &RecordPath1;
		VU1JitHooks::interrupt = &RecordInterrupt;
	}

	RecordingScope::~RecordingScope()
	{
		t_innermost = m_outer;
		if (!m_outer)
		{
			VU1JitHooks::path1 = nullptr;
			VU1JitHooks::interrupt = nullptr;
		}
	}

	void PublishInterrupts(u32 flags)
	{
		if (flags)
			RecordInterrupt(flags);
	}

	//////////////////////////////////////////////////////////////////////////
	// Run states

	void CaptureRunState(RunState* s)
	{
		std::memcpy(static_cast<void*>(&s->regs), &VU1, sizeof(VURegs));
		std::memcpy(s->mem, VU1.Mem, VU1_MEMSIZE);
		s->running = vu1InterpMTVURunning;
	}

	void RestoreRunState(const RunState& s)
	{
		std::memcpy(static_cast<void*>(&VU1), &s.regs, sizeof(VURegs));
		std::memcpy(VU1.Mem, s.mem, VU1_MEMSIZE);
		vu1InterpMTVURunning = s.running;
	}

	//////////////////////////////////////////////////////////////////////////
	// The contract (section 9.1)
	//
	// Compared: VF[32] (VF0 included), ACC, VI[32] (VI0, the flags, R, I, Q, P, TPC included), the pending q, p,
	// macflag, statusflag, clipflag, cycle; the live pipe entries in ring order; the VI branch delay window; the
	// branch and E-bit state; the XGKICK state while a kick runs; the fields a run never changes; the data memory;
	// whether the program still runs; the PATH1 output and the interrupts published.
	//
	// Normalised, with the reason:
	// - ring positions (fmac/ialu read and write positions) and dead slots: entries are compared from the read
	//   position, and only live ones (DeriveSignature refuses inconsistent rings);
	// - flagreg bits other than CLIP and STATUS: only those two are ever tested (VUops.cpp:54-62);
	// - the xyzw fields of pipe entries whose register is 0: stack garbage for ops that write no VF, never tested
	//   (VUops.cpp:221-222; VU1microInterp.cpp:40-41);
	// - VIOldValue and VIRegNumber while VIBackupCycles is 0: only read while it is non-zero (VUops.cpp:1486-1595);
	// - branchpc without a pending branch, delaybranchpc without takedelaybranch: only read when set;
	// - the xgkick counters once xgkickenable is 0: the next XGKICK resets them and nothing else reads them, and a
	//   batched catch-up feeds a finished kick more cycles than the interpreter (section 7.7);
	// - code: the last instruction word decoded, rewritten before every use.

	static const char* VIName(u32 i, char* buf, size_t size)
	{
		switch (i)
		{
			case REG_STATUS_FLAG: return "VI[STATUS]";
			case REG_MAC_FLAG: return "VI[MAC]";
			case REG_CLIP_FLAG: return "VI[CLIP]";
			case REG_R: return "VI[R]";
			case REG_I: return "VI[I]";
			case REG_Q: return "VI[Q]";
			case REG_P: return "VI[P]";
			case REG_TPC: return "VI[TPC]";
			default:
				std::snprintf(buf, size, "VI[%u]", i);
				return buf;
		}
	}

	std::string CompareRunStates(const RunState& a, const RunState& b)
	{
		const VURegs& x = a.regs;
		const VURegs& y = b.regs;
		char name[64];

#define VU1JIT_CMP(what, jit, ref) \
	do \
	{ \
		const u64 jit_value_ = static_cast<u64>(jit); \
		const u64 ref_value_ = static_cast<u64>(ref); \
		if (jit_value_ != ref_value_) \
			return fmt::format("{}: jit {:#x}, interpreter {:#x}", what, jit_value_, ref_value_); \
	} while (0)

		for (u32 i = 0; i < 32; i++)
		{
			for (u32 l = 0; l < 4; l++)
			{
				std::snprintf(name, sizeof(name), "VF%u.%c", i, "xyzw"[l]);
				VU1JIT_CMP(name, x.VF[i].UL[l], y.VF[i].UL[l]);
			}
		}
		for (u32 l = 0; l < 4; l++)
		{
			std::snprintf(name, sizeof(name), "ACC.%c", "xyzw"[l]);
			VU1JIT_CMP(name, x.ACC.UL[l], y.ACC.UL[l]);
		}
		for (u32 i = 0; i < 32; i++)
			VU1JIT_CMP(VIName(i, name, sizeof(name)), x.VI[i].UL, y.VI[i].UL);
		VU1JIT_CMP("pending q", x.q.UL, y.q.UL);
		VU1JIT_CMP("pending p", x.p.UL, y.p.UL);
		VU1JIT_CMP("pending macflag", x.macflag, y.macflag);
		VU1JIT_CMP("pending statusflag", x.statusflag, y.statusflag);
		VU1JIT_CMP("pending clipflag", x.clipflag, y.clipflag);
		VU1JIT_CMP("cycle", x.cycle, y.cycle);

		// Pipes, live entries from the read position, compared by completion time and their fields.
		VU1JIT_CMP("fmaccount", x.fmaccount, y.fmaccount);
		for (u32 i = 0; i < std::min<u32>(x.fmaccount, 4); i++)
		{
			const fmacPipe& p = x.fmac[(x.fmacreadpos + i) & 3];
			const fmacPipe& q = y.fmac[(y.fmacreadpos + i) & 3];
			const int flag_bits = (1 << REG_CLIP_FLAG) | (1 << REG_STATUS_FLAG);
			std::snprintf(name, sizeof(name), "fmac entry %u", i);
			VU1JIT_CMP(fmt::format("{} completion", name), p.sCycle + p.Cycle, q.sCycle + q.Cycle);
			VU1JIT_CMP(fmt::format("{} regupper", name), p.regupper, q.regupper);
			VU1JIT_CMP(fmt::format("{} xyzwupper", name), p.regupper ? (p.xyzwupper & 0xf) : 0, q.regupper ? (q.xyzwupper & 0xf) : 0);
			VU1JIT_CMP(fmt::format("{} reglower", name), p.reglower, q.reglower);
			VU1JIT_CMP(fmt::format("{} xyzwlower", name), p.reglower ? (p.xyzwlower & 0xf) : 0, q.reglower ? (q.xyzwlower & 0xf) : 0);
			VU1JIT_CMP(fmt::format("{} flagreg", name), p.flagreg & flag_bits, q.flagreg & flag_bits);
			VU1JIT_CMP(fmt::format("{} macflag", name), p.macflag, q.macflag);
			VU1JIT_CMP(fmt::format("{} statusflag", name), p.statusflag, q.statusflag);
			VU1JIT_CMP(fmt::format("{} clipflag", name), p.clipflag, q.clipflag);
		}
		VU1JIT_CMP("fdiv.enable", x.fdiv.enable != 0, y.fdiv.enable != 0);
		if (x.fdiv.enable)
		{
			VU1JIT_CMP("fdiv completion", x.fdiv.sCycle + x.fdiv.Cycle, y.fdiv.sCycle + y.fdiv.Cycle);
			VU1JIT_CMP("fdiv.reg", x.fdiv.reg.UL, y.fdiv.reg.UL);
			VU1JIT_CMP("fdiv.statusflag", x.fdiv.statusflag, y.fdiv.statusflag);
		}
		VU1JIT_CMP("efu.enable", x.efu.enable != 0, y.efu.enable != 0);
		if (x.efu.enable)
		{
			VU1JIT_CMP("efu completion", x.efu.sCycle + x.efu.Cycle, y.efu.sCycle + y.efu.Cycle);
			VU1JIT_CMP("efu.reg", x.efu.reg.UL, y.efu.reg.UL);
		}
		VU1JIT_CMP("ialucount", x.ialucount, y.ialucount);
		for (u32 i = 0; i < std::min<u32>(x.ialucount, 4); i++)
		{
			const ialuPipe& p = x.ialu[(x.ialureadpos + i) & 3];
			const ialuPipe& q = y.ialu[(y.ialureadpos + i) & 3];
			VU1JIT_CMP(fmt::format("ialu entry {} completion", i), p.sCycle + p.Cycle, q.sCycle + q.Cycle);
			VU1JIT_CMP(fmt::format("ialu entry {} reg", i), p.reg, q.reg);
		}
		VU1JIT_CMP("VIBackupCycles", x.VIBackupCycles, y.VIBackupCycles);
		if (x.VIBackupCycles)
		{
			VU1JIT_CMP("VIOldValue", x.VIOldValue, y.VIOldValue);
			VU1JIT_CMP("VIRegNumber", x.VIRegNumber, y.VIRegNumber);
		}

		// Control state.
		VU1JIT_CMP("branch", x.branch, y.branch);
		if (x.branch)
			VU1JIT_CMP("branchpc", x.branchpc, y.branchpc);
		VU1JIT_CMP("takedelaybranch", x.takedelaybranch, y.takedelaybranch);
		if (x.takedelaybranch)
			VU1JIT_CMP("delaybranchpc", x.delaybranchpc, y.delaybranchpc);
		VU1JIT_CMP("ebit", x.ebit, y.ebit);

		// XGKICK.
		VU1JIT_CMP("xgkickenable", x.xgkickenable, y.xgkickenable);
		if (x.xgkickenable)
		{
			VU1JIT_CMP("xgkickaddr", x.xgkickaddr, y.xgkickaddr);
			VU1JIT_CMP("xgkickdiff", x.xgkickdiff, y.xgkickdiff);
			VU1JIT_CMP("xgkicksizeremaining", x.xgkicksizeremaining, y.xgkicksizeremaining);
			VU1JIT_CMP("xgkicklastcycle", x.xgkicklastcycle, y.xgkicklastcycle);
			VU1JIT_CMP("xgkickcyclecount", x.xgkickcyclecount, y.xgkickcyclecount);
			VU1JIT_CMP("xgkickendpacket", x.xgkickendpacket, y.xgkickendpacket);
		}

		// Fields a VU1 run never changes.
		VU1JIT_CMP("start_pc", x.start_pc, y.start_pc);
		VU1JIT_CMP("flags", x.flags, y.flags);
		VU1JIT_CMP("pending_q", x.pending_q, y.pending_q);
		VU1JIT_CMP("pending_p", x.pending_p, y.pending_p);
		VU1JIT_CMP("nextBlockCycles", x.nextBlockCycles, y.nextBlockCycles);
		VU1JIT_CMP("micro flags", std::memcmp(x.micro_macflags, y.micro_macflags, sizeof(x.micro_macflags) * 3), 0);
		VU1JIT_CMP("Mem", reinterpret_cast<uptr>(x.Mem), reinterpret_cast<uptr>(y.Mem));
		VU1JIT_CMP("Micro", reinterpret_cast<uptr>(x.Micro), reinterpret_cast<uptr>(y.Micro));

		// Data memory, by qword.
		if (std::memcmp(a.mem, b.mem, VU1_MEMSIZE) != 0)
		{
			for (u32 off = 0; off < VU1_MEMSIZE; off += 4)
			{
				u32 p, q;
				std::memcpy(&p, &a.mem[off], 4);
				std::memcpy(&q, &b.mem[off], 4);
				std::snprintf(name, sizeof(name), "VU1 memory at %#x", off);
				VU1JIT_CMP(name, p, q);
			}
		}

		VU1JIT_CMP("running", a.running, b.running);

		VU1JIT_CMP("PATH1 bytes", a.path1.size, b.path1.size);
		VU1JIT_CMP("PATH1 hash", a.path1.hash, b.path1.hash);
		VU1JIT_CMP("PATH1 complete tags", a.path1.tag_ends.size(), b.path1.tag_ends.size());
		for (size_t i = 0; i < a.path1.tag_ends.size(); i++)
			VU1JIT_CMP(fmt::format("PATH1 end of tag {}", i), a.path1.tag_ends[i], b.path1.tag_ends[i]);

		VU1JIT_CMP("interrupts", a.interrupts, b.interrupts);

#undef VU1JIT_CMP
		return {};
	}

	//////////////////////////////////////////////////////////////////////////
	// A check of the comparison itself: every contract field must be seen, every normalised field ignored.

	u32 SelfTestCompare(RunState& base, RunState& other)
	{
		u32 failures = 0;
		const auto expect = [&](bool differs, const char* what) {
			const std::string diff = CompareRunStates(other, base);
			if (diff.empty() == differs)
			{
				Console.Error(fmt::format("VU1 JIT lockstep self-check: {} {}", what, differs ? "not detected" : "reported"));
				failures++;
			}
			std::memcpy(static_cast<void*>(&other.regs), &base.regs, sizeof(VURegs));
			std::memcpy(other.mem, base.mem, VU1_MEMSIZE);
			other.running = base.running;
			other.path1 = base.path1;
			other.interrupts = base.interrupts;
		};
		VURegs& r = other.regs;

		// A live state: two FMAC entries, FDIV, EFU and IALU busy, a VI backup window, a pending branch, a kick.
		VURegs& b = base.regs;
		b.fmacreadpos = 1;
		b.fmaccount = 2;
		b.fmacwritepos = 3;
		for (u32 i = 1; i < 3; i++)
		{
			b.fmac[i].sCycle = 100 + i;
			b.fmac[i].Cycle = 4;
			b.fmac[i].regupper = 3;
			b.fmac[i].xyzwupper = 0xf;
			b.fmac[i].reglower = 0;
			b.fmac[i].flagreg = 1 << REG_STATUS_FLAG;
		}
		b.fdiv.enable = 1;
		b.efu.enable = 1;
		b.ialucount = 1;
		b.ialureadpos = 2;
		b.ialuwritepos = 3;
		b.VIBackupCycles = 2;
		b.branch = 1;
		b.takedelaybranch = true;
		b.xgkickenable = 1;
		base.path1.Reset();
		const u8 bytes[32] = {1, 2, 3};
		base.path1.Append(bytes, 32, true);
		std::memcpy(static_cast<void*>(&other.regs), &b, sizeof(VURegs));
		std::memcpy(other.mem, base.mem, VU1_MEMSIZE);
		other.running = base.running;
		other.path1 = base.path1;
		other.interrupts = base.interrupts;
		expect(false, "an identical state");

		r.VF[0].UL[3] ^= 1; expect(true, "VF0.w");
		r.VF[31].UL[0] ^= 1; expect(true, "VF31.x");
		r.ACC.UL[2] ^= 1; expect(true, "ACC.z");
		r.VI[0].UL ^= 1; expect(true, "VI0");
		r.VI[REG_STATUS_FLAG].UL ^= 1; expect(true, "VI[STATUS]");
		r.VI[REG_TPC].UL ^= 8; expect(true, "TPC");
		r.q.UL ^= 1; expect(true, "pending q");
		r.p.UL ^= 1; expect(true, "pending p");
		r.macflag ^= 1; expect(true, "pending MAC");
		r.statusflag ^= 1; expect(true, "pending status");
		r.clipflag ^= 1; expect(true, "pending clip");
		r.cycle++; expect(true, "cycle");
		r.fmaccount = 1; expect(true, "FMAC count");
		r.fmac[2].Cycle++; expect(true, "FMAC completion");
		r.fmac[1].regupper = 4; expect(true, "FMAC register");
		r.fmac[1].xyzwupper = 7; expect(true, "FMAC xyzw");
		r.fmac[2].flagreg = 0; expect(true, "FMAC flag kind");
		r.fmac[1].macflag ^= 1; expect(true, "FMAC MAC snapshot");
		r.fmac[2].clipflag ^= 1; expect(true, "FMAC clip snapshot");
		r.fdiv.sCycle++; expect(true, "FDIV completion");
		r.fdiv.reg.UL ^= 1; expect(true, "FDIV value");
		r.efu.enable = 0; expect(true, "EFU busy");
		r.efu.reg.UL ^= 1; expect(true, "EFU value");
		r.ialu[2].reg ^= 1; expect(true, "IALU register");
		r.VIBackupCycles = 1; expect(true, "VIBackupCycles");
		r.VIOldValue ^= 1; expect(true, "VIOldValue in the window");
		r.branchpc ^= 8; expect(true, "branchpc with a branch pending");
		r.delaybranchpc ^= 8; expect(true, "delaybranchpc with takedelaybranch");
		r.ebit = 1; expect(true, "ebit");
		r.xgkicksizeremaining++; expect(true, "xgkicksizeremaining during a kick");
		r.xgkicklastcycle++; expect(true, "xgkicklastcycle during a kick");
		r.start_pc ^= 8; expect(true, "start_pc");
		other.mem[0x3fff] ^= 1; expect(true, "the last byte of VU memory");
		other.running = !other.running; expect(true, "running");
		other.interrupts ^= 8; expect(true, "interrupts");
		other.path1.Append(bytes, 16, false); expect(true, "PATH1 bytes");
		other.path1.tag_ends.back()--; expect(true, "PATH1 tag end");

		// Normalised: ring positions, dead slots, flagreg bits other than CLIP and STATUS, the xyzw of register 0,
		// VIOldValue outside the window, branch targets without a branch, xgkick counters once the kick ended, code.
		r.fmacreadpos = 2; r.fmacwritepos = 0; r.fmac[2] = b.fmac[1]; r.fmac[3] = b.fmac[2]; expect(false, "rotated FMAC ring");
		r.fmac[0].macflag ^= 1; r.fmac[3].Cycle = 99; expect(false, "a dead FMAC slot");
		r.fmac[1].flagreg |= 1 << REG_MAC_FLAG; expect(false, "a flagreg bit other than CLIP and STATUS");
		r.fmac[1].xyzwlower = 5; expect(false, "the xyzw of register 0");
		r.code ^= 1; expect(false, "code");
		b.VIBackupCycles = 0; b.branch = 0; b.takedelaybranch = false; b.xgkickenable = 0;
		std::memcpy(static_cast<void*>(&r), &b, sizeof(VURegs));
		r.VIOldValue ^= 1; expect(false, "VIOldValue outside the window");
		r.branchpc ^= 8; expect(false, "branchpc without a branch");
		r.delaybranchpc ^= 8; expect(false, "delaybranchpc without takedelaybranch");
		r.xgkickcyclecount++; r.xgkicklastcycle++; expect(false, "xgkick counters after the kick");
		other.path1.Reset(); other.path1.Append(bytes, 16, false); other.path1.Append(bytes + 16, 16, true);
		expect(false, "PATH1 split into a partial and a complete transfer");
		return failures;
	}
} // namespace VU1Jit
