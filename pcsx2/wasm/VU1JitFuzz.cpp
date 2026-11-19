// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "VU1JitFuzz.h"
#include "VU1JitEmit.h"

#include "VU1Jit.h"
#include "VU0Jit.h"
#include "VU1JitLockstep.h"
#include "MTVU.h"
#include "VUmicro.h"

#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <thread>

namespace VU1Jit
{
	void OracleCounters::Add(const OracleCounters& o)
	{
		const u64* src = reinterpret_cast<const u64*>(&o);
		u64* dst = reinterpret_cast<u64*>(this);
		static_assert(sizeof(OracleCounters) % sizeof(u64) == 0);
		for (size_t i = 0; i < sizeof(OracleCounters) / sizeof(u64); i++)
			dst[i] += src[i];
	}

	//////////////////////////////////////////////////////////////////////////
	// The timing oracle (section 9.2)

	namespace
	{
		// The registers pipe write-backs set.
		struct FlagRegs
		{
			u32 status, mac, clip, q, p;
		};

		// _vuFMACflush and _vuFlushAll, one entry (VUops.cpp:53-63, 157-167).
		void ApplyFmac(FlagRegs& r, int flagreg, u32 macflag, u32 statusflag, u32 clipflag)
		{
			if (flagreg & (1 << REG_CLIP_FLAG))
				r.clip = clipflag;
			if (flagreg & (1 << REG_STATUS_FLAG))
				r.status = (r.status & 0x30) | (statusflag & 0xFC0) | (statusflag & 0xF);
			else
				r.status = (r.status & 0xFF0) | (statusflag & 0xF) | ((statusflag & 0xF) << 6);
			r.mac = macflag;
		}

		// _vuFDIVflush and _vuFlushAll (VUops.cpp:101-104, 136-138).
		void ApplyFdiv(FlagRegs& r, u32 reg, u32 statusflag)
		{
			r.q = reg;
			r.status = (r.status & 0xFCF) | (statusflag & 0xC30);
		}

		int FlagregOf(u8 sig_flags)
		{
			return ((sig_flags & SIG_FMAC_CLIP) ? (1 << REG_CLIP_FLAG) : 0) | ((sig_flags & SIG_FMAC_STATUS) ? (1 << REG_STATUS_FLAG) : 0);
		}
	} // namespace

	// What the write-backs the model reports must leave in the flag registers: the pre-existing entries' snapshots,
	// and at a program end the new entries' snapshots, which are the pending values after the ops. This is the rule
	// generated code follows (section 7.3).
	static FlagRegs ExpectedFlags(const VURegs& pre, const VURegs& post, const StepInfo& info)
	{
		FlagRegs r = {pre.VI[REG_STATUS_FLAG].UL, pre.VI[REG_MAC_FLAG].UL, pre.VI[REG_CLIP_FLAG].UL, pre.VI[REG_Q].UL, pre.VI[REG_P].UL};
		u32 next = 0; // pre-existing FMAC entries written back so far, in ring order
		const auto fmac_at = [&pre](u32 i) -> const fmacPipe& { return pre.fmac[(pre.fmacreadpos + i) & 3]; };

		// _vuTestPipes order: FMAC, FDIV, EFU, IALU.
		const auto test_pipes = [&](u8 fmac, bool fdiv, bool efu) {
			for (u32 i = 0; i < fmac; i++, next++)
				ApplyFmac(r, fmac_at(next).flagreg, fmac_at(next).macflag, fmac_at(next).statusflag, fmac_at(next).clipflag);
			if (fdiv)
				ApplyFdiv(r, pre.fdiv.reg.UL, pre.fdiv.statusflag);
			if (efu)
				r.p = pre.efu.reg.UL;
		};
		test_pipes(info.fmac_flushed, info.fdiv_flushed, info.efu_flushed);
		test_pipes(info.kick_fmac_flushed, info.kick_fdiv_flushed, info.kick_efu_flushed);

		if (info.end)
		{
			// _vuFlushAll order: FDIV, EFU, FMAC, IALU.
			const bool pre_fdiv_left = pre.fdiv.enable && !info.fdiv_flushed && !info.kick_fdiv_flushed;
			if (info.fdiv_added)
				ApplyFdiv(r, post.q.UL, post.statusflag);
			else if (pre_fdiv_left)
				ApplyFdiv(r, pre.fdiv.reg.UL, pre.fdiv.statusflag);
			const bool pre_efu_left = pre.efu.enable && !info.efu_flushed && !info.kick_efu_flushed;
			if (info.efu_added)
				r.p = post.p.UL;
			else if (pre_efu_left)
				r.p = pre.efu.reg.UL;
			for (; next < pre.fmaccount; next++)
				ApplyFmac(r, fmac_at(next).flagreg, fmac_at(next).macflag, fmac_at(next).statusflag, fmac_at(next).clipflag);
			if (info.fmac_added)
				ApplyFmac(r, FlagregOf(info.fmac_added_flags), post.macflag, post.statusflag, post.clipflag);
		}
		return r;
	}

	// IBxx's condition with the VI branch delay as the model reports it (VUops.cpp:1486-1595).
	static bool PredictCondition(Lower op, const VURegs& pre, u32 lower, const StepInfo& info)
	{
		const u32 it = (lower >> 16) & 0xf;
		const u32 is = (lower >> 11) & 0xf;
		s16 dest = static_cast<s16>(pre.VI[it].US[0]);
		s16 src = static_cast<s16>(pre.VI[is].US[0]);
		if (info.vi_backup_cycles > 0)
		{
			if (info.vi_backup_reg == it)
				dest = static_cast<s16>(pre.VIOldValue);
			if (info.vi_backup_reg == is)
				src = static_cast<s16>(pre.VIOldValue);
		}
		switch (op)
		{
			case Lower::IBEQ: return dest == src;
			case Lower::IBNE: return dest != src;
			case Lower::IBLTZ: return src < 0;
			case Lower::IBGTZ: return src > 0;
			case Lower::IBLEZ: return src <= 0;
			case Lower::IBGEZ: return src >= 0;
			default: return true;
		}
	}

	void TimingOracle::BeginRun(u32 fbrst)
	{
		m_fbrst = fbrst;
		m_chain_valid = false;
	}

	void TimingOracle::Invalidate()
	{
		m_chain_valid = false;
	}

	void TimingOracle::Before()
	{
		std::memcpy(static_cast<void*>(&m_pre), &VU1, sizeof(VURegs));
		m_pre_running = vu1InterpMTVURunning;
		m_pc = VU1.VI[REG_TPC].UL & VU1_PROGMASK; // InterpVU1::Step masks it before the fetch
		std::memcpy(&m_lower, &VU1.Micro[m_pc], 4);
		std::memcpy(&m_upper, &VU1.Micro[m_pc + 4], 4);
		m_pre_refusal = DeriveSignature(m_pre, &m_pre_sig);
		m_pre_ctl = GetControl(m_pre);
	}

	void TimingOracle::Report(const std::string& what)
	{
		if (m_reports++ >= m_max_reports)
			return;
		Console.Error(fmt::format("VU1 JIT oracle{}{}: {} at pc {:#06x}: {}; state before: {}, branch {} ebit {}, cycle {}",
			*m_label ? " " : "", m_label, what, m_pc, Disassemble(m_lower, m_upper), m_pre_sig.ToString(),
			m_pre_ctl.branch, m_pre_ctl.ebit, m_pre.cycle));
	}

	void TimingOracle::After()
	{
		OracleCounters& c = m_counters;
		c.steps++;
		if (m_pre_refusal != Refusal::None)
		{
			c.refused[static_cast<u32>(m_pre_refusal)]++;
			m_chain_valid = false;
			return;
		}
		const Instruction ins = Decode(m_lower, m_upper);
		if (!ins.IsValid())
		{
			c.unknown++;
			m_chain_valid = false;
			return;
		}

		const VURegs& post = VU1;
		const u16 lflags = ins.IBit() ? 0 : GetLowerFlags(ins.lo);

		// Data the model does not decide: a conditional branch's outcome, a JR target, a busy kick's length.
		StepInput in;
		in.pc = m_pc;
		in.lower = m_lower;
		in.upper = m_upper;
		in.fbrst = m_fbrst;
		in.branch_taken = post.branch == 1; // only a branch set in this step leaves branch at 1
		in.jump_target = post.branchpc;

		StepInfo info;
		m_model.Load(m_pre_sig, m_pre_ctl);
		if (!m_model.Step(in, &info))
		{
			Report("model refused a known instruction");
			c.differences++;
			return;
		}
		if ((lflags & LOWER_XGKICK) && info.lower_runs)
		{
			c.kicks++;
			// _vuXGKICK sets xgkicklastcycle to the cycle after the busy-kick flush (VUops.cpp:1936).
			const s64 bump = static_cast<s64>(post.xgkicklastcycle - (m_pre.cycle + info.op_cycle));
			if (bump != 0)
			{
				if (bump < 0 || !m_pre.xgkickenable)
				{
					Report(fmt::format("XGKICK started at cycle {:#x}, model {:#x}", post.xgkicklastcycle, m_pre.cycle + info.op_cycle));
					c.differences++;
					m_chain_valid = false;
					return;
				}
				c.busy_kicks++;
				in.kick_bump = static_cast<u32>(bump);
				m_model.Load(m_pre_sig, m_pre_ctl);
				m_model.Step(in, &info);
			}
		}

		c.checked++;
		std::string diff;
		const u64 actual_cycles = post.cycle - m_pre.cycle;
		Signature model_sig, post_sig;
		const Refusal post_refusal = DeriveSignature(post, &post_sig);
		const Control model_ctl = m_model.GetControl();
		const Control post_ctl = GetControl(post);
		if (actual_cycles != info.cycles)
			diff = fmt::format("cycles: model {}, interpreter {}", info.cycles, actual_cycles);
		else if (!m_model.GetSignature(&model_sig))
			diff = "the model's own state has no signature";
		else if (post_refusal != Refusal::None)
			diff = fmt::format("the interpreter's state after it has no signature ({})", RefusalName(post_refusal));
		else if (model_sig != post_sig)
			diff = fmt::format("signature: model {}, interpreter {}", model_sig.ToString(), post_sig.ToString());
		else if (model_ctl.branch != post_ctl.branch || (post_ctl.branch && model_ctl.branchpc != post_ctl.branchpc) ||
				 model_ctl.takedelaybranch != post_ctl.takedelaybranch || model_ctl.ebit != post_ctl.ebit)
		{
			diff = fmt::format("control: model branch {} pc {:#x} delay {} ebit {}, interpreter branch {} pc {:#x} delay {} ebit {}",
				model_ctl.branch, model_ctl.branchpc, model_ctl.takedelaybranch, model_ctl.ebit, post_ctl.branch,
				post_ctl.branchpc, post_ctl.takedelaybranch, post_ctl.ebit);
		}
		else if (m_model.GetTPC() != post.VI[REG_TPC].UL)
			diff = fmt::format("TPC: model {:#x}, interpreter {:#x}", m_model.GetTPC(), post.VI[REG_TPC].UL);
		else if (info.end != (m_pre_running && !vu1InterpMTVURunning))
			diff = fmt::format("program end: model {}, interpreter {}", info.end, !vu1InterpMTVURunning);
		if (diff.empty())
		{
			// The data side of the model's events.
			const FlagRegs want = ExpectedFlags(m_pre, post, info);
			if (want.status != post.VI[REG_STATUS_FLAG].UL || want.mac != post.VI[REG_MAC_FLAG].UL ||
				want.clip != post.VI[REG_CLIP_FLAG].UL || want.q != post.VI[REG_Q].UL || want.p != post.VI[REG_P].UL)
			{
				diff = fmt::format("write-backs: expected status {:x} mac {:x} clip {:x} q {:x} p {:x}, interpreter {:x} {:x} {:x} {:x} {:x}",
					want.status, want.mac, want.clip, want.q, want.p, post.VI[REG_STATUS_FLAG].UL, post.VI[REG_MAC_FLAG].UL,
					post.VI[REG_CLIP_FLAG].UL, post.VI[REG_Q].UL, post.VI[REG_P].UL);
			}
			else if (info.backup_vi >= 0 &&
					 post.VIOldValue != (info.backup_chained ? m_pre.VIOldValue : m_pre.VI[info.backup_vi].US[0]))
			{
				diff = fmt::format("VIOldValue: expected {:x}, interpreter {:x}",
					info.backup_chained ? m_pre.VIOldValue : m_pre.VI[info.backup_vi].US[0], post.VIOldValue);
			}
			else if ((lflags & LOWER_COND) && info.lower_runs &&
					 PredictCondition(ins.lo, m_pre, m_lower, info) != in.branch_taken)
			{
				diff = fmt::format("branch condition with the VI delay window {}/vi{}: predicted {}, interpreter {}",
					info.vi_backup_cycles, info.vi_backup_reg, !in.branch_taken, in.branch_taken);
			}
			else if ((lflags & LOWER_LINK) && info.lower_runs && ((m_lower >> 16) & 0xf) != 0)
			{
				// BAL/JALR link: the address after the delay slot, from the unmasked TPC (VUops.cpp:1603-1641).
				const u32 want_link = ((m_pre.branch == 1) ? (m_pre.branchpc + 8) / 8 : (m_pc + 16) / 8) & 0xffff;
				if (post.VI[(m_lower >> 16) & 0xf].US[0] != want_link)
					diff = fmt::format("link: expected {:x}, interpreter {:x}", want_link, post.VI[(m_lower >> 16) & 0xf].US[0]);
				c.links++;
			}
		}
		if (!diff.empty())
		{
			c.differences++;
			Report(diff);
		}

		// The model carried along the run, as the compiler uses it.
		if (m_chain_valid)
		{
			StepInfo chain_info;
			Signature chain_sig;
			c.chained++;
			if (!m_chain.Step(in, &chain_info) || chain_info.cycles != actual_cycles || !m_chain.GetSignature(&chain_sig) ||
				(post_refusal == Refusal::None && chain_sig != post_sig) || m_chain.GetTPC() != post.VI[REG_TPC].UL)
			{
				c.chain_differences++;
				Report("the model carried along the run differs");
				m_chain_valid = false;
			}
		}
		else if (diff.empty())
		{
			m_chain.Load(post_sig, post_ctl);
			m_chain_valid = true;
		}

		// Coverage.
		c.upper_stalls += info.upper_stall != 0;
		c.lower_stalls += info.lower_stall != 0;
		c.fdiv_waits += info.fdiv_wait;
		c.efu_waits += info.efu_wait;
		c.ialu_waits += info.ialu_wait;
		c.fmac_writebacks += info.fmac_flushed + info.kick_fmac_flushed;
		c.fdiv_writebacks += info.fdiv_flushed || info.kick_fdiv_flushed;
		c.efu_writebacks += info.efu_flushed || info.kick_efu_flushed;
		c.ibits += info.ibit;
		c.discards += info.discarded;
		c.old_vf += info.old_vf != 0;
		c.old_clip += info.old_clip;
		c.vi_backups += info.backup_vi >= 0;
		c.vi_backups_chained += info.backup_chained;
		if ((lflags & LOWER_VI_OLD) && info.lower_runs && info.vi_backup_cycles &&
			(info.vi_backup_reg == ((m_lower >> 16) & 0xf) || info.vi_backup_reg == ((m_lower >> 11) & 0xf)))
		{
			c.vi_old_reads++;
		}
		c.branches_taken += info.branch_set;
		c.delay_branches += info.delay_branch;
		c.ebit_in_branch_delay += info.ebit_fetched && m_pre_ctl.branch == 1;
		c.branch_in_ebit_delay += info.branch_set && m_pre_ctl.ebit == 1;
		c.ebit_rearms += info.ebit_rearmed;
		c.dt_stops += info.dt_stop;
		c.ends += info.end;
	}

	void TimingOracle::LogSummary(const char* what) const
	{
		const OracleCounters& c = m_counters;
		u64 refused = 0;
		std::string refusals;
		for (u32 i = 1; i < static_cast<u32>(Refusal::Count); i++)
		{
			refused += c.refused[i];
			if (c.refused[i])
				refusals += fmt::format(" {} {}", RefusalName(static_cast<Refusal>(i)), c.refused[i]);
		}
		Console.WriteLn(fmt::format("{}: {} instructions, {} checked, {} differences; chained model {} checked, {} differences; "
									"refused {}{}{}, unknown words {}",
			what, c.steps, c.checked, c.differences, c.chained, c.chain_differences, refused, refusals.empty() ? "" : " (",
			refusals.empty() ? "" : refusals.substr(1) + ")", c.unknown));
		Console.WriteLn(fmt::format("{} coverage: upper stalls {}, lower stalls {}, FDIV waits {}, EFU waits {}, IALU waits {}, "
									"FMAC/FDIV/EFU write-backs {}/{}/{}, I bits {}, discarded lower ops {}, old VF reads {}, "
									"old clip reads {}, VI backups {} (chained {}), branches reading the old VI {}, branches taken {}, "
									"branches in delay slots {}, E bits in branch delay slots {}, branches in E delay slots {}, "
									"E-bit re-arms {}, D/T stops {}, program ends {}, XGKICKs {} (busy {}), links {}",
			what, c.upper_stalls, c.lower_stalls, c.fdiv_waits, c.efu_waits, c.ialu_waits, c.fmac_writebacks,
			c.fdiv_writebacks, c.efu_writebacks, c.ibits, c.discards, c.old_vf, c.old_clip, c.vi_backups,
			c.vi_backups_chained, c.vi_old_reads, c.branches_taken, c.delay_branches, c.ebit_in_branch_delay,
			c.branch_in_ebit_delay, c.ebit_rearms, c.dt_stops, c.ends, c.kicks, c.busy_kicks, c.links));
	}

	//////////////////////////////////////////////////////////////////////////
	// Random programs (section 9.3)

	namespace
	{
		class Rng
		{
		public:
			explicit Rng(u64 seed)
				: m_engine(seed)
			{
			}

			u32 U32() { return static_cast<u32>(m_engine()); }
			u32 Below(u32 n) { return n ? static_cast<u32>(m_engine() % n) : 0; }
			bool Chance(u32 percent) { return Below(100) < percent; }

		private:
			std::mt19937_64 m_engine;
		};

		// A float bit pattern: mostly ordinary values, special_percent of specials (the spike's generator).
		u32 RandomFloatBits(Rng& rng, u32 special_percent)
		{
			if (rng.Chance(special_percent))
			{
				const u32 sign = rng.U32() & 0x80000000u;
				switch (rng.Below(9))
				{
					case 0: return sign; // +-0
					case 1: return sign | (rng.U32() & 0x007fffff); // denormal
					case 2: return sign | 0x7f800000; // inf
					case 3: return sign | 0x7f800000 | (1 + rng.Below(0x7fffff)); // NaN
					case 4: return sign | 0x7f7fffff; // FLT_MAX
					case 5: return sign | 0x00800000; // FLT_MIN
					case 6: return sign | ((0xf0 + rng.Below(15)) << 23) | (rng.U32() & 0x7fffff); // huge
					case 7: return sign | ((1 + rng.Below(12)) << 23) | (rng.U32() & 0x7fffff); // tiny
					default: return rng.U32(); // any bits
				}
			}
			const float f = (static_cast<float>(rng.U32()) / 4294967296.0f - 0.5f) * 2000.0f;
			u32 bits;
			std::memcpy(&bits, &f, 4);
			return bits;
		}

		// Weighted toward the BT3 mix (section 2.1), every op at least once in the table.
		u32 UpperWeight(Upper op)
		{
			switch (op)
			{
				case Upper::NOP: return 60;
				case Upper::MADDAx: case Upper::MADDAy: case Upper::MADDAz: case Upper::MADDAw: return 6;
				case Upper::MADD: case Upper::MADDw: case Upper::MULA: case Upper::MULAw: case Upper::MULAx: return 4;
				case Upper::MUL: case Upper::MULq: case Upper::MULw: case Upper::CLIP: case Upper::ADD: case Upper::SUB: return 5;
				case Upper::FTOI0: case Upper::FTOI4: case Upper::ITOF0: return 3;
				default: return 1;
			}
		}

		u32 LowerWeight(Lower op)
		{
			switch (op)
			{
				case Lower::MOVE: return 40; // mostly the lower NOP (MOVE with Ft = 0)
				case Lower::LQ: return 25;
				case Lower::SQ: return 8;
				case Lower::IADDIU: case Lower::IADD: case Lower::IADDI: case Lower::ISUBIU: return 4;
				case Lower::IBNE: case Lower::IBEQ: case Lower::IBGTZ: case Lower::B: return 3;
				case Lower::DIV: case Lower::FCAND: case Lower::LQI: case Lower::SQI: case Lower::XGKICK: case Lower::ILW: return 3;
				case Lower::SQRT: case Lower::RSQRT: case Lower::WAITQ: case Lower::BAL: case Lower::JR: return 2;
				case Lower::Immediate:
				case Lower::Unknown:
				case Lower::Count: return 0;
				default: return 1;
			}
		}

		template <typename Op>
		std::vector<Op> WeightedTable(u32 (*weight)(Op))
		{
			std::vector<Op> table;
			for (u32 i = 1; i < static_cast<u32>(Op::Count); i++)
			{
				for (u32 w = weight(static_cast<Op>(i)); w > 0; w--)
					table.push_back(static_cast<Op>(i));
			}
			return table;
		}

		struct GeneratedProgram
		{
			u32 start_pc;
			u32 fbrst;
			u32 budget; // cycles, for an Execute
			u32 prefix_steps; // interpreter steps before the comparison (jit mode)
			u32 stale_at; // timing mode: VU1.cycle = 0 after this step (~0: never)
			u32 reset_at; // timing mode: the ring counts and positions zeroed (InterpVU1::Reset) after this step
		};

		class ProgramGenerator
		{
		public:
			explicit ProgramGenerator(u64 seed)
				: m_rng(seed)
				, m_upper_ops(WeightedTable<VU1Jit::Upper>(&UpperWeight))
				, m_lower_ops(WeightedTable<VU1Jit::Lower>(&LowerWeight))
			{
			}

			GeneratedProgram Generate(u8* micro, u8* mem, VURegs& vu);

		private:
			u32 RandomVF() { return m_rng.Chance(60) ? m_rng.Below(8) : m_rng.Below(32); }
			u32 RandomVIField() { return m_rng.Chance(70) ? m_rng.Below(8) : m_rng.Below(32); } // 16-31 reach VI0-15
			u32 RandomDest() { return m_rng.Chance(50) ? 0xf : m_rng.Below(16); }
			u32 MakeUpper(bool nop);
			u32 MakeLower(VU1Jit::Lower op);
			u32 MakeLower() { return MakeLower(m_lower_ops[m_rng.Below(static_cast<u32>(m_lower_ops.size()))]); }
			void Put(u8* micro, u32 pc, u32 lower, u32 upper);
			void FillData(u8* mem, VURegs& vu);

			Rng m_rng;
			std::vector<VU1Jit::Upper> m_upper_ops;
			std::vector<VU1Jit::Lower> m_lower_ops;
		};

		u32 ProgramGenerator::MakeUpper(bool nop)
		{
			const VU1Jit::Upper op = nop ? VU1Jit::Upper::NOP : m_upper_ops[m_rng.Below(static_cast<u32>(m_upper_ops.size()))];
			const u32 bits = UpperOpcodeBits(op);
			const bool fd_group = (bits & 0x3f) >= 0x3c; // bits 6-10 are the sub-op
			u32 code = bits | (RandomDest() << 21) | (RandomVF() << 16) | (RandomVF() << 11);
			if (!fd_group)
				code |= RandomVF() << 6;
			if (m_rng.Chance(2))
				code |= 0x20000000u; // M bit: ignored on VU1
			return code;
		}

		u32 ProgramGenerator::MakeLower(VU1Jit::Lower op)
		{
			const u32 bits = LowerOpcodeBits(op);
			if (op == Lower::MOVE && m_rng.Chance(75))
				return bits; // the lower NOP: MOVE with no dest, Ft = Fs = 0
			u32 fixed = 0xfe000000u;
			if ((bits >> 25) == 0x40)
				fixed |= ((bits & 0x3f) >= 0x3c) ? 0x7ffu : 0x3fu;
			const bool vi_fields = (op >= Lower::ILW && op <= Lower::ISUBIU) || (op >= Lower::IBEQ && op <= Lower::IOR) ||
								   op == Lower::JR || op == Lower::JALR || op == Lower::BAL || op == Lower::LQI ||
								   op == Lower::LQD || op == Lower::SQI || op == Lower::SQD || op == Lower::ILWR ||
								   op == Lower::ISWR || op == Lower::MTIR || op == Lower::MFIR || op == Lower::XGKICK ||
								   op == Lower::XTOP || op == Lower::XITOP || op == Lower::LQ || op == Lower::SQ;
			u32 free = (RandomDest() << 21) | (vi_fields ? RandomVIField() : RandomVF()) << 16 |
					   (vi_fields ? RandomVIField() : RandomVF()) << 11 | (m_rng.U32() & 0x7ff);
			if (op == Lower::FCAND || op == Lower::FCEQ || op == Lower::FCOR || op == Lower::FCSET)
				free = m_rng.U32() & 0xffffff; // 24-bit immediate
			return bits | (free & ~fixed);
		}

		void ProgramGenerator::Put(u8* micro, u32 pc, u32 lower, u32 upper)
		{
			std::memcpy(&micro[pc & 0x3ff8], &lower, 4);
			std::memcpy(&micro[(pc & 0x3ff8) + 4], &upper, 4);
		}

		void ProgramGenerator::FillData(u8* mem, VURegs& vu)
		{
			for (u32 off = 0; off < VU1_MEMSIZE; off += 4)
			{
				const u32 v = m_rng.Chance(30) ? (m_rng.U32() & 0x3ff) : RandomFloatBits(m_rng, 60);
				std::memcpy(&mem[off], &v, 4);
			}
			// GIF tags at random qwords, and VI registers pointing at some of them (XGKICK takes (VI & 0x3ff) * 16).
			u32 tags[48];
			for (u32& t : tags)
			{
				t = m_rng.Chance(10) ? (0x3c0 + m_rng.Below(64)) : m_rng.Below(1024); // some near the end: packets that wrap
				const u32 nloop = m_rng.Chance(2) ? 0x7fff : m_rng.Below(m_rng.Chance(20) ? 64 : 6);
				const u32 eop = m_rng.Chance(55) ? 1 : 0;
				const u32 flg = m_rng.Below(4);
				const u32 nreg = m_rng.Below(16);
				const u32 tag[4] = {nloop | (eop << 15), (m_rng.U32() & 0x03ffc000u) | (flg << 26) | (nreg << 28),
					m_rng.U32(), m_rng.U32()};
				std::memcpy(&mem[t * 16], tag, 16);
			}
			for (u32 i = 1; i < 16; i++)
			{
				if (m_rng.Chance(40))
					vu.VI[i].UL = tags[m_rng.Below(48)] | (m_rng.Chance(20) ? (m_rng.U32() & 0xfc00) : 0);
			}
		}

		GeneratedProgram ProgramGenerator::Generate(u8* micro, u8* mem, VURegs& vu)
		{
			GeneratedProgram p;

			// Registers: 60% special float operands, random VF0 and VI0 now and then.
			for (u32 i = 0; i < 32; i++)
			{
				for (u32 l = 0; l < 4; l++)
					vu.VF[i].UL[l] = RandomFloatBits(m_rng, 60);
			}
			if (m_rng.Chance(70))
			{
				vu.VF[0].UL[0] = vu.VF[0].UL[1] = vu.VF[0].UL[2] = 0;
				vu.VF[0].UL[3] = 0x3f800000;
			}
			for (u32 l = 0; l < 4; l++)
				vu.ACC.UL[l] = RandomFloatBits(m_rng, 60);
			for (u32 i = 0; i < 32; i++)
				vu.VI[i].UL = 0;
			for (u32 i = 1; i < 16; i++)
				vu.VI[i].UL = m_rng.Chance(70) ? m_rng.Below(64) : (m_rng.U32() & 0xffff);
			vu.VI[0].UL = m_rng.Chance(85) ? 0 : (m_rng.U32() & 0xffff);
			vu.VI[REG_STATUS_FLAG].UL = m_rng.U32() & 0xfff;
			vu.VI[REG_MAC_FLAG].UL = m_rng.U32() & 0xffff;
			vu.VI[REG_CLIP_FLAG].UL = m_rng.U32() & 0xffffff;
			vu.VI[REG_R].UL = 0x3f800000 | (m_rng.U32() & 0x7fffff);
			vu.VI[REG_I].UL = RandomFloatBits(m_rng, 30);
			vu.VI[REG_Q].UL = RandomFloatBits(m_rng, 30);
			vu.VI[REG_P].UL = RandomFloatBits(m_rng, 30);
			vu.q.UL = RandomFloatBits(m_rng, 30);
			vu.p.UL = RandomFloatBits(m_rng, 30);
			vu.macflag = m_rng.U32() & 0xffff;
			vu.statusflag = m_rng.U32() & 0xfff;
			vu.clipflag = m_rng.U32() & 0xffffff;

			// Pipes empty, as after a program end; the prefix and the program itself fill them.
			vu.fmacreadpos = vu.fmacwritepos = vu.fmaccount = 0;
			vu.ialureadpos = vu.ialuwritepos = vu.ialucount = 0;
			vu.fdiv.enable = 0;
			vu.efu.enable = 0;
			vu.VIBackupCycles = 0;
			vu.branch = 0;
			vu.takedelaybranch = false;
			vu.ebit = 0;
			vu.cycle = m_rng.Chance(50) ? 0 : m_rng.Below(100000);

			FillData(mem, vu);

			// A kick may still be running from an earlier program.
			vu.xgkickenable = m_rng.Chance(20);
			vu.xgkickaddr = m_rng.Below(1024) * 16;
			vu.xgkickdiff = 0x4000 - vu.xgkickaddr;
			vu.xgkicksizeremaining = 0;
			vu.xgkickendpacket = 0;
			vu.xgkicklastcycle = vu.cycle;
			vu.xgkickcyclecount = m_rng.Below(4);

			// Background: the whole micro memory holds valid code, with E bits, so that stray jumps end.
			for (u32 pc = 0; pc < VU1_PROGSIZE; pc += 8)
			{
				u32 upper = MakeUpper(m_rng.Chance(50));
				const bool ibit = m_rng.Chance(3);
				u32 lower = ibit ? RandomFloatBits(m_rng, 40) : MakeLower();
				if (ibit)
					upper |= 0x80000000u;
				if (m_rng.Chance(8))
					upper |= 0x40000000u;
				Put(micro, pc, lower, upper);
			}

			// The program.
			p.start_pc = m_rng.Chance(15) ? (0x3f00 + m_rng.Below(32) * 8) : m_rng.Below(2048) * 8;
			const u32 length = m_rng.Chance(20) ? (16 + m_rng.Below(180)) : (2 + m_rng.Below(48));
			const bool dt_bits = m_rng.Chance(30);
			for (u32 i = 0; i < length; i++)
			{
				const u32 pc = (p.start_pc + i * 8) & VU1_PROGMASK;
				u32 upper = MakeUpper(m_rng.Chance(45));
				u32 lower = MakeLower();

				// Patterns for the timing cases random code reaches too rarely.
				const u32 pattern = m_rng.Below(100);
				const u32 reg = 1 + m_rng.Below(7);
				if (pattern < 6 && i + 4 < length)
				{
					// A VI write, then a branch that reads it inside or after the 2-cycle VI branch delay window.
					static constexpr VU1Jit::Lower writers[] = {Lower::IADDIU, Lower::ISUBIU, Lower::IADDI, Lower::IADD,
						Lower::ISUB, Lower::IAND, Lower::IOR, Lower::MTIR, Lower::LQI, Lower::SQI, Lower::LQD, Lower::SQD};
					static constexpr VU1Jit::Lower readers[] = {Lower::IBEQ, Lower::IBNE, Lower::IBLTZ, Lower::IBGTZ, Lower::IBLEZ, Lower::IBGEZ};
					const VU1Jit::Lower w = writers[m_rng.Below(std::size(writers))];
					u32 wl = MakeLower(w);
					if (w == Lower::LQI || w == Lower::LQD)
						wl = (wl & ~0x0000f800u) | (reg << 11); // _Is_
					else if (w == Lower::IADD || w == Lower::ISUB || w == Lower::IAND || w == Lower::IOR)
						wl = (wl & ~0x000007c0u) | (reg << 6); // _Id_
					else
						wl = (wl & ~0x001f0000u) | (reg << 16); // _It_
					Put(micro, pc, wl, MakeUpper(true));
					const u32 gap = m_rng.Below(3);
					for (u32 g = 1; g <= gap; g++)
						Put(micro, pc + g * 8, LowerOpcodeBits(Lower::MOVE), MakeUpper(true));
					// The branch reads the register (as Is, and as It half the time), and goes back to the writer or
					// before it: a loop the VI delay window changes.
					const u32 it = m_rng.Chance(50) ? reg : m_rng.Below(4);
					u32 bl = (MakeLower(readers[m_rng.Below(std::size(readers))]) & ~0x001ff800u) | (reg << 11) | (it << 16);
					bl = (bl & ~0x7ffu) | (static_cast<u32>(-2 - static_cast<s32>(gap) - static_cast<s32>(m_rng.Below(3))) & 0x7ff);
					Put(micro, pc + (gap + 1) * 8, bl, MakeUpper(true));
					i += gap + 1;
					continue;
				}
				if (pattern < 9)
				{
					// ILW, then a branch on its register: the IALU pipe stalls only branches (section 7.2).
					Put(micro, pc, (MakeLower(Lower::ILW) & ~0x000f0000u) | (reg << 16), MakeUpper(false));
					if (i + 2 < length)
					{
						const u32 gap = m_rng.Below(4);
						for (u32 g = 1; g <= gap && i + g + 1 < length; g++)
							Put(micro, pc + g * 8, MakeLower(), MakeUpper(false));
						Put(micro, pc + (gap + 1) * 8, (MakeLower(Lower::IBNE) & ~0x001ff800u) | (reg << 11) | (m_rng.Below(4) << 16), MakeUpper(true));
						i += gap + 1;
					}
					continue;
				}
				// Ops for the bodies below: no branch, no VI written (except by the patterns themselves).
				static constexpr VU1Jit::Lower body_lower[] = {Lower::MOVE, Lower::MOVE, Lower::LQ, Lower::LQ, Lower::SQ,
					Lower::DIV, Lower::SQRT, Lower::RSQRT, Lower::MR32, Lower::MFIR, Lower::ISW, Lower::WAITQ, Lower::FSSET,
					Lower::RNEXT, Lower::ESQRT, Lower::ERSQRT, Lower::EATAN, Lower::WAITP, Lower::MFP};
				const auto fmac_upper = [&] {
					return UpperOpcodeBits(m_rng.Chance(50) ? Upper::MUL : Upper::ADD) | (RandomDest() << 21) | (RandomVF() << 6) |
					       (RandomVF() << 11) | (RandomVF() << 16);
				};
				if (pattern >= 90 && pattern < 93 && i + 12 < length)
				{
					// Short blocks that branch to each other, forward and backward, conditionally or not, with FMAC,
					// FDIV and EFU results pending at the branches (VU1JitEmit.cpp, EmitEdge): 2 to 4 blocks of 3
					// instructions (an op, the branch, its delay slot), then a block that the chain ends on.
					const u32 blocks = 2 + m_rng.Below(3);
					const u32 counter = 8 + m_rng.Below(8);
					Put(micro, pc, LowerOpcodeBits(Lower::IADDIU) | (counter << 16) | (1 + m_rng.Below(5)), MakeUpper(true));
					for (u32 b = 0; b < blocks; b++)
					{
						const u32 at = i + 1 + b * 3; // instruction index of the block
						const u32 bpc = (p.start_pc + at * 8) & VU1_PROGMASK;
						Put(micro, bpc, MakeLower(body_lower[m_rng.Below(std::size(body_lower))]), m_rng.Chance(60) ? fmac_upper() : MakeUpper(true));
						// The target: the next block, the one after, the chain's end, or back to an earlier block while the
						// counter lasts (the counter's decrement is the delay slot then).
						u32 target = at + 3 + (m_rng.Chance(30) ? 3 : 0);
						const u32 end = i + 1 + blocks * 3;
						target = std::min(target, end);
						u32 lower;
						bool back = b > 0 && m_rng.Chance(35);
						if (back)
						{
							target = i + 1 + m_rng.Below(b + 1) * 3;
							lower = LowerOpcodeBits(Lower::IBNE) | (counter << 16); // IBNE vi_counter, vi0
						}
						else if (m_rng.Chance(50))
							lower = LowerOpcodeBits(Lower::B);
						else
						{
							static constexpr VU1Jit::Lower conds[] = {Lower::IBEQ, Lower::IBNE, Lower::IBLTZ, Lower::IBGTZ,
								Lower::IBLEZ, Lower::IBGEZ};
							lower = (MakeLower(conds[m_rng.Below(std::size(conds))]) & ~0x7ffu);
						}
						const s32 imm = static_cast<s32>(target) - static_cast<s32>(at + 1) - 1;
						Put(micro, bpc + 8, lower | (static_cast<u32>(imm) & 0x7ff), m_rng.Chance(50) ? fmac_upper() : MakeUpper(true));
						const u32 delay = back ? (LowerOpcodeBits(Lower::IADDI) | (counter << 16) | (counter << 11) | (0x1fu << 6)) : MakeLower(body_lower[m_rng.Below(std::size(body_lower))]);
						Put(micro, bpc + 16, delay, m_rng.Chance(70) ? fmac_upper() : MakeUpper(true));
					}
					i += blocks * 3;
					continue;
				}
				if (pattern >= 93 && pattern < 96 && i + 10 < length)
				{
					// A loop whose back branch is a JR (or a JALR) to a target set before the loop: generated code
					// learns the target and goes there without leaving (VU1Jit.cpp, Merge, with VU1JIT_MERGE_EXITS low).
					const u32 counter = 8 + m_rng.Below(8);
					const u32 target_vi = 1 + m_rng.Below(7);
					const u32 link_vi = 1 + (target_vi % 7); // another one
					const u32 body = 1 + m_rng.Below(3);
					const u32 loop_pc = (p.start_pc + (i + 2) * 8) & VU1_PROGMASK;
					Put(micro, pc, LowerOpcodeBits(Lower::IADDIU) | (counter << 16) | (1 + m_rng.Below(6)), MakeUpper(true));
					Put(micro, pc + 8, LowerOpcodeBits(Lower::IADDIU) | (target_vi << 16) | (loop_pc / 8), MakeUpper(true));
					for (u32 b = 0; b < body; b++)
						Put(micro, loop_pc + b * 8, MakeLower(body_lower[m_rng.Below(std::size(body_lower))]), m_rng.Chance(60) ? fmac_upper() : MakeUpper(true));
					const u32 e = loop_pc + body * 8; // the decrement, the exit test and its delay slot, the jump and its delay slot
					Put(micro, e, LowerOpcodeBits(Lower::IADDI) | (counter << 16) | (counter << 11) | (0x1fu << 6), MakeUpper(m_rng.Chance(50)));
					Put(micro, e + 8, LowerOpcodeBits(Lower::IBEQ) | (counter << 16) | 3, m_rng.Chance(50) ? fmac_upper() : MakeUpper(true));
					Put(micro, e + 16, MakeLower(body_lower[m_rng.Below(std::size(body_lower))]), MakeUpper(m_rng.Chance(50)));
					const u32 jump = m_rng.Chance(50) ? (LowerOpcodeBits(Lower::JR) | (target_vi << 11)) : (LowerOpcodeBits(Lower::JALR) | (link_vi << 16) | (target_vi << 11));
					Put(micro, e + 24, jump, m_rng.Chance(50) ? fmac_upper() : MakeUpper(true));
					Put(micro, e + 32, MakeLower(body_lower[m_rng.Below(std::size(body_lower))]), m_rng.Chance(60) ? fmac_upper() : MakeUpper(true));
					i += 2 + body + 5 - 1;
					continue;
				}
				if (pattern >= 96 && i + 6 < length)
				{
					// A counted loop, as a vertex loop: the counter set, a body of upper ops (some reading registers
					// the body never writes) with loads, stores, divides and kicks, the decrement and a branch back,
					// which generated code takes without leaving it (VU1JitEmit.cpp, EmitLoopBack). A kick that finds
					// the previous one busy leaves in the middle of a pass (EmitLoopState).
					const u32 counter = 8 + m_rng.Below(8);
					Put(micro, pc, LowerOpcodeBits(Lower::IADDIU) | (counter << 16) | (1 + m_rng.Below(m_rng.Chance(20) ? 40 : 6)),
						MakeUpper(true));
					static constexpr VU1Jit::Lower body_ops[] = {Lower::MOVE, Lower::MOVE, Lower::LQ, Lower::LQ, Lower::SQ,
						Lower::DIV, Lower::MR32, Lower::MFIR, Lower::ISW, Lower::WAITQ, Lower::FSSET, Lower::RNEXT, Lower::XGKICK,
						Lower::ESQRT};
					const u32 body = 1 + m_rng.Below(std::min<u32>(12, length - i - 5));
					for (u32 b = 1; b <= body + 2; b++)
					{
						u32 bl = MakeLower(body_ops[m_rng.Below(std::size(body_ops))]);
						if (b == body + 1)
							bl = LowerOpcodeBits(Lower::IADDI) | (counter << 16) | (counter << 11) | (0x1fu << 6); // -1
						else if (b == body + 2)
							bl = LowerOpcodeBits(Lower::IBNE) | (counter << 11) | (static_cast<u32>(-static_cast<s32>(body) - 2) & 0x7ff);
						Put(micro, pc + b * 8, bl, MakeUpper(m_rng.Chance(20)));
					}
					// The delay slot: an op that writes no VI register.
					Put(micro, pc + (body + 3) * 8, MakeLower(body_ops[m_rng.Below(std::size(body_ops))]), MakeUpper(m_rng.Chance(20)));
					if (body >= 2 && m_rng.Chance(40))
					{
						// A kick in the delay slot that the next pass's first instruction finds still running, and an
						// FMAC result of the delay slot that its second instruction reads: generated code leaves while
						// that entry is pending from the previous pass, and the stall after shows its timing.
						const u32 kick_vi = 1 + m_rng.Below(7);
						Put(micro, pc + (body + 3) * 8, (MakeLower(Lower::XGKICK) & ~0x0000f800u) | (kick_vi << 11),
							UpperOpcodeBits(Upper::MUL) | 0x01e00000u | (reg << 6) | (RandomVF() << 11) | (RandomVF() << 16));
						Put(micro, pc + 8, (MakeLower(Lower::XGKICK) & ~0x0000f800u) | (kick_vi << 11), MakeUpper(true));
						Put(micro, pc + 16, MakeLower(Lower::MOVE),
							UpperOpcodeBits(Upper::ADD) | 0x01e00000u | (RandomVF() << 6) | (reg << 11) | (RandomVF() << 16));
					}
					i += body + 3;
					continue;
				}
				if (pattern < 13)
				{
					// FDIV or EFU ops close together: waits, and the efu.Cycle -= 1 quirk.
					const bool efu = m_rng.Chance(50);
					static constexpr VU1Jit::Lower fdiv_ops[] = {Lower::DIV, Lower::SQRT, Lower::RSQRT, Lower::WAITQ};
					static constexpr VU1Jit::Lower efu_ops[] = {Lower::ESADD, Lower::ERSADD, Lower::ELENG, Lower::ERLENG,
						Lower::EATANxy, Lower::EATANxz, Lower::ESUM, Lower::ERCPR, Lower::ESQRT, Lower::ERSQRT, Lower::ESIN,
						Lower::EATAN, Lower::EEXP, Lower::WAITP};
					lower = efu ? MakeLower(efu_ops[m_rng.Below(std::size(efu_ops))]) : MakeLower(fdiv_ops[m_rng.Below(std::size(fdiv_ops))]);
				}
				else if (pattern < 16)
				{
					// An FMAC result read soon after: FMAC stalls.
					upper = (UpperOpcodeBits(Upper::MUL) | 0x01e00000u | (reg << 6) | (RandomVF() << 11) | (RandomVF() << 16));
					lower = m_rng.Chance(50) ? ((MakeLower(Lower::SQ) & ~0x0000f800u) | (reg << 11)) : MakeLower();
				}

				const u16 flags = GetLowerFlags(Decode(lower, upper).lo);
				if ((flags & LOWER_BRANCH) && !(flags & LOWER_DYNAMIC))
				{
					// Targets inside the program mostly: loops and forward skips.
					const s32 target = m_rng.Chance(80) ? static_cast<s32>(m_rng.Below(length)) : static_cast<s32>(m_rng.Below(4096)) - 2048;
					const s32 imm = std::clamp<s32>(target - static_cast<s32>(i) - 1, -1024, 1023);
					lower = (lower & ~0x7ffu) | (static_cast<u32>(imm) & 0x7ff);
				}
				if (m_rng.Chance(4))
				{
					upper |= 0x80000000u; // I bit: the lower word is the value of I
					lower = RandomFloatBits(m_rng, 40);
				}
				if (dt_bits && m_rng.Chance(3))
					upper |= m_rng.Chance(50) ? 0x10000000u : 0x08000000u;

				// The program end: an E bit on the last instruction, sometimes one in its delay slot too (re-arm),
				// sometimes none (the background or the step cap ends it).
				if (i == length - 1 && m_rng.Chance(85))
					upper |= 0x40000000u;
				else if (m_rng.Chance(2))
					upper |= 0x40000000u;
				Put(micro, pc, lower, upper);
			}
			if (m_rng.Chance(8))
			{
				// E bit in the E-bit delay slot.
				const u32 pc = (p.start_pc + length * 8) & VU1_PROGMASK;
				u32 upper;
				std::memcpy(&upper, &micro[pc + 4], 4);
				upper |= 0x40000000u;
				std::memcpy(&micro[pc + 4], &upper, 4);
			}

			const u32 fbrst = m_rng.Below(4);
			p.fbrst = (fbrst == 1 ? 0x400 : fbrst == 2 ? 0x800 : fbrst == 3 ? 0xc00 : 0);
			p.budget = m_rng.Chance(30) ? (8 + m_rng.Below(200)) : (200 + m_rng.Below(4000));
			p.prefix_steps = m_rng.Chance(40) ? 0 : m_rng.Below(24);
			p.stale_at = m_rng.Chance(4) ? m_rng.Below(64) : ~0u;
			p.reset_at = m_rng.Chance(2) ? m_rng.Below(64) : ~0u;
			return p;
		}
	} // namespace

	//////////////////////////////////////////////////////////////////////////
	// The fuzzer

	static FuzzStatus s_fuzz_status;

	FuzzStatus& GetFuzzStatus()
	{
		return s_fuzz_status;
	}

	namespace
	{
		struct FuzzSpec
		{
			u64 seed = 1;
			u32 count = 1000;
			bool timing = true;
			bool jit = false;
			bool vu0 = false; // mode vu0: the programs on VU0 (interpreter against WasmVU0)
			bool cache = false; // fixed-PC overlay reuse and eviction
			bool hashes = false; // log a hash of each program's interpreter result (to compare engines and tiers)
		};

		bool ParseFuzzSpec(const std::string& spec, FuzzSpec* out)
		{
			const std::vector<std::string_view> parts = StringUtil::SplitString(spec, ':', false);
			if (parts.size() < 2 || parts.size() > 4)
				return false;
			const std::optional<u64> seed = StringUtil::FromChars<u64>(parts[0]);
			const std::optional<u32> count = StringUtil::FromChars<u32>(parts[1]);
			if (!seed.has_value() || !count.has_value())
				return false;
			out->seed = seed.value();
			out->count = count.value();
			if (parts.size() >= 3)
			{
				out->timing = parts[2] == "timing" || parts[2] == "both";
				out->jit = parts[2] == "jit" || parts[2] == "both";
				out->vu0 = parts[2] == "vu0";
				out->cache = parts[2] == "cache" && out->count == 80;
				if (!out->timing && !out->jit && !out->vu0 && !out->cache)
					return false;
			}
			if (parts.size() == 4)
			{
				out->hashes = parts[3] == "hashes";
				if (!out->hashes)
					return false;
			}
			return true;
		}

		// The fuzzer owns VU1 while it runs: it saves the global state it touches and restores it at the end.
		class FuzzEnvironment
		{
		public:
			FuzzEnvironment()
				: m_saved_regs(std::make_unique<VURegs>())
				, m_micro(new (std::align_val_t(64)) u8[VU1_PROGSIZE])
				, m_mem(new (std::align_val_t(64)) u8[VU1_MEMSIZE])
			{
				std::memcpy(static_cast<void*>(m_saved_regs.get()), &VU1, sizeof(VURegs));
				m_saved_vu_thread = EmuConfig.Speedhacks.vuThread;
				m_saved_running = vu1InterpMTVURunning;
				m_saved_fbrst = vu1Thread.vuFBRST;
				m_saved_top = vu1Thread.vifRegs.top;
				m_saved_itop = vu1Thread.vifRegs.itop;
				EmuConfig.Speedhacks.vuThread = true; // MTVU: the only mode the JIT and its model cover
				VU1.Micro = m_micro;
				VU1.Mem = m_mem;
			}

			~FuzzEnvironment()
			{
				std::memcpy(static_cast<void*>(&VU1), m_saved_regs.get(), sizeof(VURegs));
				EmuConfig.Speedhacks.vuThread = m_saved_vu_thread;
				vu1InterpMTVURunning = m_saved_running;
				vu1Thread.vuFBRST = m_saved_fbrst;
				vu1Thread.vifRegs.top = m_saved_top;
				vu1Thread.vifRegs.itop = m_saved_itop;
				CpuWasmVU1.Clear(0, VU1_PROGSIZE);
				operator delete[](m_micro, std::align_val_t(64));
				operator delete[](m_mem, std::align_val_t(64));
			}

			u8* Micro() { return m_micro; }
			u8* Mem() { return m_mem; }

		private:
			std::unique_ptr<VURegs> m_saved_regs;
			u8* m_micro;
			u8* m_mem;
			bool m_saved_vu_thread;
			bool m_saved_running;
			u32 m_saved_fbrst;
			u32 m_saved_top;
			u32 m_saved_itop;
		};
	} // namespace

	// One program through the oracle, stepping the interpreter as InterpVU1::Execute does.
	static void FuzzTiming(TimingOracle& oracle, const GeneratedProgram& p)
	{
		Path1Record path1;
		path1.keep_bytes = false;
		u32 interrupts = 0;
		RecordingScope scope(&path1, &interrupts, false);

		oracle.BeginRun(p.fbrst);
		VU1.VI[REG_TPC].UL = p.start_pc;
		vu1InterpMTVURunning = true;
		static constexpr u32 MAX_STEPS = 3000;
		for (u32 step = 0; step < MAX_STEPS && vu1InterpMTVURunning; step++)
		{
			oracle.Before();
			CpuIntVU1.Step();
			oracle.After();
			if (step == p.stale_at)
			{
				// As MTVU_VU_EXECUTE after a budget exit: the pipes keep times from before VU1.cycle = 0.
				VU1.cycle = 0;
				oracle.Invalidate();
			}
			if (step == p.reset_at)
			{
				// InterpVU1::Reset (VU1microInterp.cpp:274-283), as on a savestate load, without its log line.
				VU1.fmacwritepos = VU1.fmacreadpos = VU1.fmaccount = 0;
				VU1.ialuwritepos = VU1.ialureadpos = VU1.ialucount = 0;
				oracle.Invalidate();
			}
		}
	}

	// One program on the interpreter and on WasmVU1 from the same state; returns the first difference. compared is
	// false when the prefix already ended the program.
	static std::string FuzzJit(const GeneratedProgram& p, RunState& start, RunState& ref, RunState& jit, bool* compared)
	{
		*compared = false;
		{
			// The prefix: entry states with pending pipes, VI backup windows and delay slots.
			Path1Record path1;
			path1.keep_bytes = false;
			u32 interrupts = 0;
			RecordingScope scope(&path1, &interrupts, false);
			VU1.VI[REG_TPC].UL = p.start_pc;
			vu1InterpMTVURunning = true;
			for (u32 step = 0; step < p.prefix_steps && vu1InterpMTVURunning; step++)
				CpuIntVU1.Step();
			if (!vu1InterpMTVURunning)
				return {};
		}
		VU1.VI[REG_TPC].UL >>= 3; // Execute's unit (InterpVU1::Execute, VU1microInterp.cpp:300)
		CaptureRunState(&start);
		*compared = true;

		for (int pass = 0; pass < 2; pass++)
		{
			RunState& out = pass ? jit : ref;
			RestoreRunState(start);
			out.path1.Reset();
			out.interrupts = 0;
			{
				RecordingScope scope(&out.path1, &out.interrupts, false);
				if (pass)
					CpuWasmVU1.Execute(p.budget);
				else if (g_trace)
				{
					// InterpVU1::Execute, logged
					VU1.VI[REG_TPC].UL <<= 3;
					const u64 s0 = VU1.cycle;
					vu1InterpMTVURunning = true;
					while ((VU1.cycle - s0) < p.budget && vu1InterpMTVURunning)
					{
						const u32 pc = VU1.VI[REG_TPC].UL & VU1_PROGMASK;
						CpuIntVU1.Step();
						Console.WriteLn(fmt::format("  ref {:#06x} {} -> cycle {} status {:#x} mac {:#x} fmaccount {}", pc,
							Disassemble(*(u32*)&VU1.Micro[pc], *(u32*)&VU1.Micro[pc + 4]), VU1.cycle, VU1.VI[REG_STATUS_FLAG].UL,
							VU1.VI[REG_MAC_FLAG].UL, VU1.fmaccount));
					}
					if (!vu1InterpMTVURunning && VU1.branch == 1)
					{
						VU1.VI[REG_TPC].UL = VU1.branchpc;
						VU1.branch = 0;
					}
					VU1.VI[REG_TPC].UL >>= 3;
				}
				else
					CpuIntVU1.Execute(p.budget);
			}
			CaptureRunState(&out);
		}
		return CompareRunStates(jit, ref);
	}

	// FNV-1a of what a run left in VU1's registers and memory, and of its PATH1 output.
	static u64 HashRunState(const RunState& s)
	{
		u64 h = 0xcbf29ce484222325ULL;
		const auto add = [&h](const void* p, size_t n) {
			for (size_t i = 0; i < n; i++)
				h = (h ^ static_cast<const u8*>(p)[i]) * 0x100000001b3ULL;
		};
		add(s.regs.VF, sizeof(s.regs.VF));
		for (const REG_VI& vi : s.regs.VI)
			add(&vi.UL, 4);
		add(&s.regs.ACC, sizeof(s.regs.ACC));
		add(&s.regs.cycle, sizeof(s.regs.cycle));
		add(s.mem, VU1_MEMSIZE);
		add(&s.path1.hash, sizeof(s.path1.hash));
		return h;
	}

	// One program on VU0 (micro mode): the generated code and data copied into VU0's 4 KB memories, then run on
	// InterpVU0 and on WasmVU0 from the same state. VU0 reaches VU1's registers through its 0x4000 mapping, so those are
	// restored between the passes too.
	static std::string FuzzVU0(const GeneratedProgram& p, const u8* micro16, const u8* mem16, RunState& start, RunState& ref,
		RunState& jit, bool* compared)
	{
		alignas(64) static u8 s_micro0[VU0_PROGSIZE];
		alignas(64) static u8 s_mem0[VU0_MEMSIZE];
		static VURegs s_vu1_start;
		*compared = false;
		// No VU thread here: GET_VU_MEM would wait for it on VU0's 0x4000 mapping.
		const bool saved_vu_thread = EmuConfig.Speedhacks.vuThread;
		EmuConfig.Speedhacks.vuThread = false;
		u8* const saved_mem = VU0.Mem;
		u8* const saved_micro = VU0.Micro;
		std::memcpy(static_cast<void*>(&VU0), &VU1, sizeof(VURegs));
		VU0.Mem = s_mem0;
		VU0.Micro = s_micro0;
		std::memcpy(s_micro0, micro16 + (p.start_pc & 0x3000), VU0_PROGSIZE);
		std::memcpy(s_mem0, mem16, VU0_MEMSIZE);
		CpuWasmVU0.Clear(0, VU0_PROGSIZE);
		VU0.VI[REG_FBRST].UL = 0; // no D/T stops: they raise EE interrupts
		VU0.VI[REG_VPU_STAT].UL = 1;
		VU0.flags = 0;
		VU0.VI[REG_TPC].UL = p.start_pc & 0xff8;

		std::string diff;
		for (u32 step = 0; step < p.prefix_steps && (VU0.VI[REG_VPU_STAT].UL & 1); step++)
			CpuIntVU0.Step();
		if (VU0.VI[REG_VPU_STAT].UL & 1)
		{
			VU0.VI[REG_TPC].UL >>= 3;
			const auto capture = [](RunState* s) {
				std::memcpy(static_cast<void*>(&s->regs), &VU0, sizeof(VURegs));
				std::memset(s->mem, 0, sizeof(s->mem));
				std::memcpy(s->mem, VU0.Mem, VU0_MEMSIZE);
				s->running = (VU0.VI[REG_VPU_STAT].UL & 1) != 0;
				s->path1.Reset();
				s->interrupts = 0;
			};
			const auto restore = [](const RunState& s) {
				std::memcpy(static_cast<void*>(&VU0), &s.regs, sizeof(VURegs));
				std::memcpy(VU0.Mem, s.mem, VU0_MEMSIZE);
				std::memcpy(static_cast<void*>(VU1.VF), s_vu1_start.VF, sizeof(VU1.VF) + sizeof(VU1.VI));
			};
			std::memcpy(static_cast<void*>(&s_vu1_start), &VU1, sizeof(VURegs));
			capture(&start);
			*compared = true;
			// A third of the programs run in short slices, as the EE runs VU0 (1 to 32 cycles each, the same ones on both):
			// blocks then stop where a slice ends and go on from there (VU1Jit::g_vu0_slices).
			const auto run = [&](BaseVUmicroCPU& cpu) {
				if (p.budget % 3 != 0)
				{
					cpu.Execute(p.budget);
					return;
				}
				u32 x = p.budget * 2654435761u;
				for (u32 used = 0; used < p.budget && (VU0.VI[REG_VPU_STAT].UL & 1);)
				{
					x = x * 1664525u + 1013904223u;
					const u32 slice = 1 + (x >> 27);
					cpu.Execute(slice);
					used += slice;
				}
			};
			for (int pass = 0; pass < 2; pass++)
			{
				restore(start);
				run(pass ? static_cast<BaseVUmicroCPU&>(CpuWasmVU0) : static_cast<BaseVUmicroCPU&>(CpuIntVU0));
				capture(pass ? &jit : &ref);
				// VU1's registers as VU0 left them, folded into the comparison through the memory image's spare room.
				std::memcpy(&(pass ? jit : ref).mem[VU0_MEMSIZE], VU1.VF, sizeof(VU1.VF) + sizeof(VU1.VI));
			}
			std::memcpy(static_cast<void*>(VU1.VF), s_vu1_start.VF, sizeof(VU1.VF) + sizeof(VU1.VI));
			diff = CompareRunStates(jit, ref);
		}
		VU0.Mem = saved_mem;
		VU0.Micro = saved_micro;
		EmuConfig.Speedhacks.vuThread = saved_vu_thread;
		return diff;
	}

	// A branch makes two dispatchable blocks. Vary the same PC's signature and its code
	// independently, then restore the earliest overlay both before and after cache pressure.
	static u32 FuzzCache(ProgramGenerator& gen, FuzzEnvironment& env, RunState& start, RunState& ref, RunState& jit)
	{
		gen.Generate(env.Micro(), env.Mem(), VU1);
		vu1Thread.vuFBRST = 0;
		VU1.xgkickenable = false;
		CpuWasmVU1.Reset();
		auto initial = std::make_unique<RunState>();
		CaptureRunState(initial.get());
		const GeneratedProgram program{0, 0, 64, 0, ~0u, ~0u};
		const u32 nop_upper = UpperOpcodeBits(Upper::NOP);
		const u32 nop_lower = LowerOpcodeBits(Lower::MOVE);
		const auto run = [&](u32 overlay, u32 backup) {
			RestoreRunState(*initial);
			VU1.VIBackupCycles = backup;
			VU1.VIRegNumber = 3;
			const u32 code[] = {
				LowerOpcodeBits(Lower::B) | 1,
				nop_upper,
				nop_lower,
				nop_upper,
				LowerOpcodeBits(Lower::IADDIU) | (2 << 16) | overlay,
				nop_upper | 0x40000000,
				nop_lower,
				nop_upper,
			};
			CpuWasmVU1.Clear(0, sizeof(code));
			std::memcpy(env.Micro(), code, sizeof(code));
			bool compared;
			const std::string diff = FuzzJit(program, start, ref, jit, &compared);
			if (!diff.empty())
				Console.ErrorFmt("VU1 cache fuzz: overlay {} signature {}: {}", overlay, backup, diff);
			return compared && diff.empty() && GetPublicStats().selftest.load(std::memory_order_relaxed) == 1;
		};
		const auto compiled = [] { return GetPublicStats().compiles.load(std::memory_order_relaxed); };
		u32 failures = 0;
		failures += !run(1, 0);
		const u32 first = compiled();
		failures += !run(1, 1);
		failures += compiled() <= first;
		failures += !run(2, 0);
		const u32 before_restore = compiled();
		failures += !run(1, 0);
		failures += compiled() != before_restore;
		for (u32 overlay = 3; overlay <= 80; overlay++)
			failures += !run(overlay, 0);
		const u32 before_evicted = compiled();
		failures += !run(1, 0);
		failures += compiled() <= before_evicted;
		const u32 before_reset = compiled();
		CpuWasmVU1.Reset();
		failures += !run(1, 0);
		failures += compiled() <= before_reset;
		Console.WriteLnFmt("VU1 cache fuzz: two signatures, 80 same-PC overlays, restore before/after eviction and reset, {} differences", failures);
		LogReport();
		return failures;
	}

	static int FuzzMain(const FuzzSpec& spec)
	{
		FuzzStatus& status = s_fuzz_status;
		FuzzEnvironment env;
		ProgramGenerator gen(spec.seed);
		TimingOracle oracle;
		oracle.SetLabel("(fuzz)");
		auto start = std::make_unique<RunState>();
		auto ref = std::make_unique<RunState>();
		auto jit = std::make_unique<RunState>();
		if (spec.cache)
		{
			const u32 failures = FuzzCache(gen, env, *start, *ref, *jit);
			status.done.store(spec.count, std::memory_order_relaxed);
			status.differences.store(failures, std::memory_order_relaxed);
			std::snprintf(status.summary, sizeof(status.summary), "fixed-PC cache replay: %u differences", failures);
			return failures ? 1 : 0;
		}
		u32 jit_differences = 0;
		u32 jit_compared = 0;
		u64 ref_hash = 0;
		if (spec.jit)
		{
			// The comparison itself first, from a state the generator made.
			gen.Generate(env.Micro(), env.Mem(), VU1);
			CaptureRunState(ref.get());
			CaptureRunState(jit.get());
			const u32 failures = SelfTestCompare(*ref, *jit);
			Console.WriteLn(fmt::format("VU1 JIT fuzz: contract comparison self-check: {}", failures ? "FAILED" : "passed"));
			jit_differences += failures;
		}
		const auto t0 = std::chrono::steady_clock::now();
		auto last_progress = t0;

		for (u32 i = 0; i < spec.count; i++)
		{
			GeneratedProgram p = gen.Generate(env.Micro(), env.Mem(), VU1);
			CpuWasmVU1.Clear(0, VU1_PROGSIZE);
			vu1Thread.vuFBRST = p.fbrst;
			vu1Thread.vifRegs.top = p.start_pc & 0x3ff;
			vu1Thread.vifRegs.itop = (p.start_pc >> 3) & 0x3ff;

			if (spec.timing)
			{
				auto saved = std::make_unique<RunState>();
				if (spec.jit)
					CaptureRunState(saved.get());
				FuzzTiming(oracle, p);
				if (spec.jit)
					RestoreRunState(*saved);
			}
			if (spec.vu0)
			{
				bool compared;
				const std::string diff = FuzzVU0(p, env.Micro(), env.Mem(), *start, *ref, *jit, &compared);
				jit_compared += compared;
				if (!diff.empty() && jit_differences++ < 10)
					Console.Error(fmt::format("VU1 JIT fuzz: VU0 program {} (seed {}) differs: {}", i, spec.seed, diff));
			}
			if (spec.jit)
			{
				static const int trace_prog = [] { const char* e = std::getenv("VU1JIT_TRACE_PROG"); return e ? std::atoi(e) : -1; }();
				g_trace = static_cast<int>(i) == trace_prog;
				bool compared;
				const std::string diff = FuzzJit(p, *start, *ref, *jit, &compared);
				jit_compared += compared;
				if (compared)
				{
					const u64 h = HashRunState(*ref);
					ref_hash = (ref_hash ^ h) * 0x100000001b3ULL;
					if (spec.hashes)
						Console.WriteLn(fmt::format("VU1 JIT fuzz: program {} interpreter result {:016x}", i, h));
				}
				if (!diff.empty())
				{
					if (jit_differences++ < 10)
						Console.Error(fmt::format("VU1 JIT fuzz: program {} (seed {}) differs: {}", i, spec.seed, diff));
				}
			}

			if (VU1.Mem != env.Mem() || VU1.Micro != env.Micro())
			{
				// Someone else set up VU1 while the fuzzer owned it (a VM starting): nothing after this can be trusted.
				Console.Error("VU1 JIT fuzz: VU1's memory was replaced by another thread during the run; stopping");
				jit_differences++;
				break;
			}

			status.done.store(i + 1, std::memory_order_relaxed);
			status.differences.store(static_cast<u32>(oracle.GetCounters().differences + oracle.GetCounters().chain_differences) + jit_differences,
				std::memory_order_relaxed);
			status.instructions.store(oracle.GetCounters().steps, std::memory_order_relaxed);
			const auto now = std::chrono::steady_clock::now();
			if (now - last_progress > std::chrono::seconds(30))
			{
				last_progress = now;
				Console.WriteLn(fmt::format("VU1 JIT fuzz: {} of {} programs, {} instructions checked, {} differences",
					i + 1, spec.count, oracle.GetCounters().checked, status.differences.load()));
			}
		}

		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (spec.timing)
			oracle.LogSummary(fmt::format("VU1 JIT fuzz, timing oracle, seed {}, {} programs", spec.seed, spec.count).c_str());
		if (spec.vu0)
		{
			Console.WriteLn(fmt::format("VU1 JIT fuzz, interpreter against WasmVU0, seed {}: {} programs compared, {} differences",
				spec.seed, jit_compared, jit_differences));
			VU0Jit::LogReport();
		}
		if (spec.jit)
			Console.WriteLn(fmt::format("VU1 JIT fuzz, interpreter against WasmVU1, seed {}: {} programs compared, {} differences; "
										"interpreter results hash {:016x}",
				spec.seed, jit_compared, jit_differences, ref_hash));
		const OracleCounters& c = oracle.GetCounters();
		const u32 differences = static_cast<u32>(c.differences + c.chain_differences) + jit_differences;
		std::snprintf(status.summary, sizeof(status.summary),
			"seed %llu, %u programs in %.1f s: timing oracle %llu instructions checked, %llu differences; interpreter "
			"against WasmVU1 %u compared, %u differences",
			static_cast<unsigned long long>(spec.seed), spec.count, seconds, static_cast<unsigned long long>(c.checked),
			static_cast<unsigned long long>(c.differences + c.chain_differences), jit_compared, jit_differences);
		Console.WriteLn(fmt::format("VU1 JIT fuzz: {}", status.summary));
		if (spec.jit)
			LogReport();
		return differences ? 1 : 0;
	}

	int RunFuzz(const std::string& spec_string)
	{
		FuzzSpec spec;
		if (!ParseFuzzSpec(spec_string, &spec))
		{
			Console.Error(fmt::format("VU1 JIT fuzz: bad spec '{}' (use SEED:COUNT[:timing|jit|both|cache[:hashes]])", spec_string));
			s_fuzz_status.state.store(4, std::memory_order_release);
			return 2;
		}
		s_fuzz_status.total.store(spec.count, std::memory_order_relaxed);
		s_fuzz_status.done.store(0, std::memory_order_relaxed);
		s_fuzz_status.differences.store(0, std::memory_order_relaxed);
		s_fuzz_status.state.store(1, std::memory_order_release);

		// Its own pthread, which then owns its JIT state (section 9.3); the thread's exit releases it.
		int result = 0;
		const u32 releases = VU1Jit::GetThreadExitReleases();
		std::thread thread([&spec, &result]() { result = FuzzMain(spec); });
		thread.join();
		if (spec.jit)
		{
			const bool released = VU1Jit::GetThreadExitReleases() != releases;
			Console.WriteLn(fmt::format("VU1 JIT fuzz: thread_local JIT state released on thread exit: {}", released ? "yes" : "NO"));
			if (!released)
				result = 1;
		}
		s_fuzz_status.state.store(result ? 3 : 2, std::memory_order_release);
		return result;
	}

	bool StartFuzz(const std::string& spec)
	{
		// The page's thread: claim the status without blocking, then run on a detached thread.
		int state = s_fuzz_status.state.load(std::memory_order_acquire);
		do
		{
			if (state == 1)
				return false;
		} while (!s_fuzz_status.state.compare_exchange_weak(state, 1, std::memory_order_acq_rel));
		std::thread([spec]() { RunFuzz(spec); }).detach();
		return true;
	}
} // namespace VU1Jit
