// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "VU1JitEmit.h"

#include "MTVU.h"
#include "VUops.h"
#include "WasmEmitter.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <utility>

extern bool vu1InterpMTVURunning;

namespace VU1Jit
{
	u64 g_block_base = 0;
	u64 g_block_base0 = 0;
	u64 g_budget_end = 0;
	u64 g_budget_end0 = 0;
	bool g_vu0_slices = true;
	u32 g_chain_instrs = 0;
	u32 g_chain_edges = 0;
	u32 g_chain_instrs0 = 0;
	u32 g_chain_edges0 = 0;
	bool g_trace = false;
}

// Helpers generated code imports (WasmJitRuntime.js binds them as env.h_*). delta is VU1.cycle at the op relative to
// the block entry: the op bodies that read the cycle (XGKICK) and the kick transfer see the interpreter's value.
extern "C" EMSCRIPTEN_KEEPALIVE void vu1jit_h_kick(u32 delta)
{
	// _vuTestPipes' kick catch-up (VUops.cpp:203-209).
	VU1.cycle = VU1Jit::g_block_base + delta;
	if (VU1.xgkickenable)
		_vuXGKICKTransfer((VU1.cycle - VU1.xgkicklastcycle) - 1, false);
}

extern "C" EMSCRIPTEN_KEEPALIVE void vu1jit_h_upper(u32 code, u32 delta)
{
	VU1.cycle = VU1Jit::g_block_base + delta;
	VU1.code = code;
	VU1_UPPER_OPCODE[code & 0x3f]();
}

extern "C" EMSCRIPTEN_KEEPALIVE void vu1jit_h_lower(u32 code, u32 delta)
{
	VU1.cycle = VU1Jit::g_block_base + delta;
	VU1.code = code;
	VU1_LOWER_OPCODE[code >> 25]();
}

extern "C" EMSCRIPTEN_KEEPALIVE void vu1jit_h_upper0(u32 code, u32 delta)
{
	VU0.cycle = VU1Jit::g_block_base0 + delta;
	VU0.code = code;
	VU0_UPPER_OPCODE[code & 0x3f]();
}

extern "C" EMSCRIPTEN_KEEPALIVE void vu1jit_h_lower0(u32 code, u32 delta)
{
	VU0.cycle = VU1Jit::g_block_base0 + delta;
	VU0.code = code;
	VU0_LOWER_OPCODE[code >> 25]();
}

namespace VU1Jit
{
	using namespace WasmJit;
	using U = Upper;
	using L = Lower;

	static constexpr u32 BLOCK_CAP = 512; // instructions per block
	static u32 s_bases[2][2]; // per VU: the addresses of VURegs and of the data memory, which generated code loads (Addr)
	static constexpr u32 EXP = 0x7f800000u;
	static constexpr u32 SIGN = 0x80000000u;

	template <typename T>
	static u32 A(const T& x)
	{
		return static_cast<u32>(reinterpret_cast<uptr>(&x));
	}

	static PipeImage CaptureImage(const VURegs& m)
	{
		PipeImage p;
		std::memcpy(p.fmac, m.fmac, sizeof(p.fmac));
		p.fmacreadpos = m.fmacreadpos;
		p.fmacwritepos = m.fmacwritepos;
		p.fmaccount = m.fmaccount;
		p.fdiv = m.fdiv;
		p.efu = m.efu;
		std::memcpy(p.ialu, m.ialu, sizeof(p.ialu));
		p.ialureadpos = m.ialureadpos;
		p.ialuwritepos = m.ialuwritepos;
		p.ialucount = m.ialucount;
		p.VIBackupCycles = m.VIBackupCycles;
		p.VIRegNumber = m.VIRegNumber;
		p.cycle = m.cycle;
		return p;
	}

	void NormalizeRings(VURegs& R)
	{
		if (R.fmacreadpos)
		{
			fmacPipe t[4];
			for (u32 k = 0; k < 4; k++)
				t[k] = R.fmac[(R.fmacreadpos + k) & 3];
			std::memcpy(R.fmac, t, sizeof(t));
			R.fmacreadpos = 0;
			R.fmacwritepos = R.fmaccount & 3;
		}
		if (R.ialureadpos)
		{
			ialuPipe t[4];
			for (u32 k = 0; k < 4; k++)
				t[k] = R.ialu[(R.ialureadpos + k) & 3];
			std::memcpy(R.ialu, t, sizeof(t));
			R.ialureadpos = 0;
			R.ialuwritepos = R.ialucount & 3;
		}
	}

	void ApplyExit(VURegs& R, const BlockExit& x, u64 base)
	{
		const PipeImage& m = x.pipes;
		const auto rel = [base](u64 model_time) { return base + (model_time - TIMING_MODEL_BASE); };
		R.cycle = rel(m.cycle);

		// Entries pending since the entry (model times below the base) keep their own times; new ones get the model's.
		for (u32 i = 0; i < m.fmaccount; i++)
		{
			const u32 s = (m.fmacreadpos + i) & 3;
			const fmacPipe& p = m.fmac[s];
			if (p.sCycle < TIMING_MODEL_BASE)
				continue;
			fmacPipe& q = R.fmac[s];
			q.regupper = p.regupper;
			q.reglower = p.reglower;
			q.flagreg = p.flagreg;
			q.xyzwupper = p.xyzwupper;
			q.xyzwlower = p.xyzwlower;
			q.sCycle = rel(p.sCycle);
			q.Cycle = p.Cycle;
		}
		R.fmacreadpos = m.fmacreadpos;
		R.fmacwritepos = m.fmacwritepos;
		R.fmaccount = m.fmaccount;

		for (u32 i = 0; i < m.ialucount; i++)
		{
			const u32 s = (m.ialureadpos + i) & 3;
			const ialuPipe& p = m.ialu[s];
			if (p.sCycle < TIMING_MODEL_BASE)
				continue;
			R.ialu[s].reg = p.reg;
			R.ialu[s].sCycle = rel(p.sCycle);
			R.ialu[s].Cycle = p.Cycle;
		}
		R.ialureadpos = m.ialureadpos;
		R.ialuwritepos = m.ialuwritepos;
		R.ialucount = m.ialucount;

		R.fdiv.enable = m.fdiv.enable;
		if (m.fdiv.enable && m.fdiv.sCycle >= TIMING_MODEL_BASE)
		{
			R.fdiv.sCycle = rel(m.fdiv.sCycle);
			R.fdiv.Cycle = m.fdiv.Cycle;
		}
		R.efu.enable = m.efu.enable;
		if (m.efu.enable)
		{
			if (m.efu.sCycle >= TIMING_MODEL_BASE)
			{
				R.efu.sCycle = rel(m.efu.sCycle);
				R.efu.Cycle = m.efu.Cycle;
			}
			else
			{
				R.efu.Cycle += x.efu_cycle_adjust;
			}
		}

		R.VIBackupCycles = m.VIBackupCycles;
		R.VIRegNumber = m.VIRegNumber;
		if (!x.dynamic_tpc)
			R.VI[REG_TPC].UL = x.tpc;
		R.branch = x.branch;
		if (x.branch)
			R.branchpc = x.branchpc;
		R.takedelaybranch = false;
		R.ebit = x.ebit;
		if (x.mbit)
			R.flags |= VUFLAG_MFLAGSET;
		if (x.kind == ExitKind::End)
		{
			if (&R == &VU1)
				vu1InterpMTVURunning = false;
			else
			{
				// _vu0Exec's program end (VU0microInterp.cpp:183-191).
				VU0.VI[REG_VPU_STAT].UL &= ~0x1;
				vif0Regs.stat.VEW = false;
			}
		}
	}

	namespace
	{
		struct Plan
		{
			u32 pc;
			Instruction ins;
			StepInfo info;
			Control pre_ctl;
			PipeImage pre; // the model before the instruction
			u32 cyc; // cycles from the entry to the start of the instruction
		};

		bool IsBranch(Lower op) { return (GetLowerFlags(op) & LOWER_BRANCH) != 0; }

		// Whether generated code may run this instruction in this control state (section 8). after_static_branch: this
		// is the delay slot of a static branch in the same block (VU1, PlanBlock), where a static branch or an XGKICK
		// may stand: the exits there know the pending branch.
		bool Allowed(const Plan& p, int vu, bool after_static_branch = false)
		{
			const Instruction& ins = p.ins;
			if (!ins.IsValid() || ins.DBit() || ins.TBit())
				return false;
			const bool branch = !ins.IBit() && IsBranch(ins.lo);
			if (p.pre_ctl.branch != 0) // a branch delay slot
			{
				const bool static_branch = branch && !(GetLowerFlags(ins.lo) & LOWER_DYNAMIC);
				const bool kick = !ins.IBit() && ins.lo == L::XGKICK;
				if ((branch && !(after_static_branch && static_branch)) || ins.EBit() || (kick && !after_static_branch))
					return false;
			}
			if (p.pre_ctl.ebit != 0 && branch) // an E-bit delay slot
				return false;
			if (branch && (ins.EBit() || p.pc == (vu ? VU1_PROGSIZE : VU0_PROGSIZE) - 8))
				return false;
			// The M bit (VU0) ends the interpreter's Execute loop after the instruction: never with a branch pending.
			if (vu == 0 && ins.MBit() && (branch || p.pre_ctl.branch != 0))
				return false;
			return true;
		}

		// A VU0 memory op that reaches memory: its address may be VU0's 0x4000 mapping of VU1's registers (GET_VU_MEM),
		// where the interpreter takes the instruction (an exit before it, EmitInstruction). The others (no lane in
		// dest, or a load to register 0 that returns before its access) read and write nothing.
		bool IsVu0Memory(const Plan& p)
		{
			return !p.ins.IBit() && ((GetLowerFlags(p.ins.lo) & LOWER_STORE) || p.ins.lo == L::LQ || p.ins.lo == L::LQI ||
										p.ins.lo == L::LQD || p.ins.lo == L::ILW || p.ins.lo == L::ILWR);
		}
		// In a branch's or an E bit's delay slot (where VU0 exits are not made either, SliceExitable) a VU0 memory op
		// calls its op body instead.
		bool Vu0MemoryNative(const Plan& p)
		{
			return p.pre_ctl.branch == 0 && p.pre_ctl.ebit == 0;
		}
		bool Vu0MemoryCheck(const Plan& p)
		{
			if (!IsVu0Memory(p) || !Vu0MemoryNative(p))
				return false;
			const u32 code = p.ins.lower;
			const u32 ft = (code >> 16) & 0x1f, dest = (code >> 21) & 0xf;
			switch (p.ins.lo)
			{
				case L::LQ:
				case L::LQI:
				case L::LQD:
					return ft != 0 && dest != 0;
				case L::ILW:
				case L::ILWR:
					return (ft & 0xf) != 0 && dest != 0;
				case L::SQ:
				case L::SQI:
				case L::SQD:
				case L::ISW:
				case L::ISWR:
					return dest != 0;
				default:
					return false;
			}
		}

		// The VU registers a block's native code reads or writes (Emitter::VF and Emitter::VI): they live in locals
		// while it runs (VF0 to VF31 and ACC as index 32, VI0 to VI15).
		struct RegUse
		{
			u64 vf_used = 0; // written ones included: a partial write merges with the old value
			u64 vf_written = 0;
			u64 vf_fmac_read = 0; // read as FMAC operands (clamped)
			u32 vi_used = 0;
			u32 vi_written = 0;
			bool helpers = false; // the module calls op bodies, which may write any register
			std::vector<u8> block_helpers; // ... per block of the module
		};
		static constexpr u32 ACC_INDEX = 32;

		class Emitter
		{
		public:
			// use: the registers to keep in locals, from a first emission of the same block (null for that one,
			// which only records them).
			Emitter(CompiledBlock* out, int vu, const RegUse* use)
				: m_out(out)
				, R(vu ? VU1 : VU0)
				, m_vu(vu)
				, m_f(1)
			{
				if (use)
					m_use = *use;
				m_type_run = m_m.type({ValType::I32}, {ValType::I32});
				const u32 t1 = m_m.type({ValType::I32}, {});
				const u32 t2 = m_m.type({ValType::I32, ValType::I32}, {});
				m_m.import_memory("env", "mem", 0, 65536, true);
				m_h_kick = m_m.import_function("env", "h_kick", t1);
				m_h_upper = m_m.import_function("env", vu ? "h_upper" : "h_upper0", t2);
				m_h_lower = m_m.import_function("env", vu ? "h_lower" : "h_lower0", t2);
				for (u32& v : m_v)
					v = m_f.local(ValType::V128);
				for (u32& i : m_i)
					i = m_f.local(ValType::I32);
				m_hc_old = m_f.local(ValType::I32);
				m_hc_new = m_f.local(ValType::I32);
				m_hs = m_f.local(ValType::V128);
				m_hn = m_f.local(ValType::V128);
				m_dx = m_f.local(ValType::I32);
				m_de = m_f.local(ValType::I32);
				m_splat = m_f.local(ValType::V128);
				for (u32* l : {&m_pmac, &m_pstat, &m_pclip, &m_vmac, &m_vstat, &m_vclip, &m_lxstat, &m_axs})
					*l = m_f.local(ValType::I32);
				for (u32* l : {&m_pr, &m_lr, &m_amin, &m_amax, &m_aor, &m_aun})
					*l = m_f.local(ValType::V128);
				for (u32 i = 0; i < 4; i++)
				{
					m_snapr[i] = m_f.local(ValType::V128);
					m_snapr_tmp[i] = m_f.local(ValType::V128);
				}
				for (auto& slot : m_snap)
				{
					for (u32& l : slot)
						l = m_f.local(ValType::I32);
				}
				for (u32& t : m_snap_tmp)
					t = m_f.local(ValType::I32);
				m_looped = m_f.local(ValType::I32);
				m_next = m_f.local(ValType::I32);
				m_exit = m_f.local(ValType::I32);
				m_cond1 = m_f.local(ValType::I32);
				m_acc = m_f.local(ValType::I32);
				m_edges = m_f.local(ValType::I32);
				m_base = m_f.local(ValType::I64);
				m_budget = m_f.local(ValType::I64);
				m_rb = m_f.local(ValType::I32);
				m_mb = m_f.local(ValType::I32);
				m_kick = m_f.local(ValType::I32);
				m_mem = A(*R.Mem);
				s_bases[vu][0] = A(R);
				s_bases[vu][1] = m_mem;
			}

			const RegUse& Use() const { return m_use; }

			// The registers and the flags in locals (LoadRegs, LoadFlags), and the snapshots of the FMAC entries
			// pending at the entry, which NormalizeRings put in ring slots 0 to the signature's count - 1 (all four
			// slots: the function may start at any of its blocks). On VU1, VU1.cycle at the entry and the budget.
			void EmitPrologue()
			{
				m_f.i32_const(0);
				m_f.i32_load(A(s_bases[m_vu][0]));
				m_f.local_set(m_rb);
				m_f.i32_const(0);
				m_f.i32_load(A(s_bases[m_vu][1]));
				m_f.local_set(m_mb);
				LoadKick();
				LoadRegs();
				LoadFlags();
				for (u32 i = 0; i < 4; i++)
				{
					Ld32(A(R.fmac[i].macflag));
					m_f.local_set(m_snap[i][0]);
					Ld32(A(R.fmac[i].statusflag));
					m_f.local_set(m_snap[i][1]);
					Ld32(A(R.fmac[i].clipflag));
					m_f.local_set(m_snap[i][2]);
					NeutralRaw();
					m_f.local_set(m_snapr[i]);
				}
				if (m_vu)
				{
					m_f.i32_const(0);
					m_f.i64_load(A(g_block_base));
					m_f.local_set(m_base);
					m_f.i32_const(0);
					m_f.i64_load(A(g_budget_end));
					m_f.local_set(m_budget);
				}
				else
				{
					// VU0: VU0.cycle at the entry and the end of the dispatcher's slice (EmitBudgetExit, EmitEdge).
					m_f.i32_const(0);
					m_f.i64_load(A(g_block_base0));
					m_f.local_set(m_base);
					m_f.i32_const(0);
					m_f.i64_load(A(g_budget_end0));
					m_f.local_set(m_budget);
				}
			}

			// The blocks of a module in layout order (CompileModule): a dispatch loop around one wasm block per module
			// block, whose br_table starts at the block the function's argument names. Block k's code follows the end
			// of wasm block k, so going to a later block is a br to the end of its wasm block, and going to the same
			// or an earlier one goes through the dispatch loop. Around it all, the wasm block every exit leaves by
			// (EmitExit, EndModule).
			void BeginModule(const std::vector<u32>& last_starts)
			{
				m_last_starts = last_starts;
				const u32 n = static_cast<u32>(last_starts.size());
				m_f.local_get(0);
				m_f.local_set(m_next);
				m_f.block();
				m_stub_level = m_f.depth();
				m_f.loop();
				m_dispatch_level = m_f.depth();
				for (u32 k = 0; k < n; k++)
					m_f.block();
				std::vector<u32> depths(n);
				for (u32 k = 0; k < n; k++)
					depths[k] = k;
				m_f.local_get(m_next);
				m_f.br_table(depths, 0);
			}
			// The level of the wasm block whose end is block k's code.
			u32 BlockLevel(u32 k) const { return m_dispatch_level + static_cast<u32>(m_last_starts.size()) - k; }
			// Starts block k's code; n: its instructions.
			void BeginBlock(u32 k, u32 n)
			{
				m_f.end(); // wasm block k
				m_k = k;
				m_block_n = n;
				m_loop = nullptr;
				m_exit_kick = -1;
				ForgetClean();
				m_cc_next = 0; // the clamped copies of earlier blocks are dead here
			}
			void EndModule()
			{
				m_f.end(); // the dispatch loop
				m_f.end(); // the exits' block
				// The code every exit shares (section 5.3): the registers and the flags, and the snapshots of the FMAC
				// entries (of every ring slot: those of dead slots are never read), to memory; ApplyExit writes the
				// rest of the timing state.
				StoreRegs();
				StoreFlags();
				for (u32 s = 0; s < 4; s++)
				{
					St32(A(R.fmac[s].macflag), [&] { PushMac(m_snapr[s], m_snap[s][0]); });
					St32(A(R.fmac[s].statusflag), [&] { PushStat(m_snapr[s], m_snap[s][1]); });
					St32(A(R.fmac[s].clipflag), [&] { m_f.local_get(m_snap[s][2]); });
				}
				St32(A(m_vu ? g_chain_instrs : g_chain_instrs0), [&] { m_f.local_get(m_acc); });
				St32(A(m_vu ? g_chain_edges : g_chain_edges0), [&] { m_f.local_get(m_edges); });
				m_f.local_get(m_exit);
				m_f.return_();
			}

			// A block that loops (CompiledBlock::loops): its instructions run inside a wasm loop, with VU1.cycle at
			// the entry of the current pass in m_base. loop is the exit that comes back to the entry.
			void BeginLoop(const BlockExit& loop)
			{
				m_loop = &loop;
				m_f.i32_const(0);
				m_f.local_set(m_looped);
				// Loop-invariant operand clamps: registers the block never writes, with no op body that could.
				ForgetClean();
				if (m_k >= m_use.block_helpers.size() || !m_use.block_helpers[m_k])
				{
					for (u32 r = 0; r <= ACC_INDEX; r++)
					{
						if ((m_use.vf_fmac_read & ~m_use.vf_written) & (u64(1) << r))
							MakeClampedCopy(r);
					}
				}
				m_f.loop();
				m_loop_level = m_f.depth();
			}
			void EndLoop() { m_f.end(); }

			// busy_kick_exit, busy_kick_untaken: exits before the instruction (VU1: an XGKICK while a kick runs; VU0: a
			// memory op at VU0's 0x4000 mapping, Vu0MemoryCheck), the second for the untaken branch of a delay slot.
			void EmitInstruction(const Plan& p, s32 busy_kick_exit, s32 busy_kick_untaken, bool last);
			void EmitVu0MappingTest(const Plan& p); // pushes whether the memory op's address is VU0's 0x4000 mapping
			// VU0: leaves by exit_id when the slice has threshold cycles or fewer left at the block's entry (m_base).
			void EmitBudgetExit(u32 threshold, u32 exit_id)
			{
				m_f.local_get(m_base);
				m_f.i64_const(static_cast<s64>(threshold));
				m_f.i64_add();
				m_f.local_get(m_budget);
				m_f.i64_ge_u();
				m_f.if_unlikely();
				EmitExit(exit_id);
				m_f.end();
			}
			void FlushAllAtEnd(const Plan& p, const int end_flagreg[4]);
			// loop: whether the taken exit is the loop exit of BeginLoop. next_taken, next_fall: the blocks of the
			// module the exits go to, or -1; jumps: a JR/JALR's targets (TPC) that are blocks of the module.
			void EmitBranchEnd(const Plan& branch, u32 exit_taken, u32 exit_fall, u32 exit_dyn, bool loop, u32 last_start,
				s32 next_taken, s32 next_fall, const std::vector<std::pair<u32, s32>>& jumps);
			// A branch whose delay slot holds a branch: its condition, before the second one's (m_i[4]) replaces it.
			void SaveBranchCondition()
			{
				m_f.local_get(m_i[4]);
				m_f.local_set(m_cond1);
			}
			// The end of a block whose last instruction is a branch in the delay slot of the one before (the interpreter's
			// takedelaybranch): the exits for both taken, the first only, the second only and neither, or -1 (cond1,
			// cond2: whether each branch is conditional), and the blocks of the module they go to (-1: exit).
			void EmitBranchInDelayEnd(bool cond1, bool cond2, const s32 exits[4], const s32 next[4])
			{
				const auto second = [&](u32 k) { // exits k (second taken) and k + 1 (not)
					if (!cond2)
					{
						EmitEdgeOrExit(static_cast<u32>(exits[k]), next[k]);
						return;
					}
					m_f.local_get(m_i[4]);
					m_f.if_();
					EmitEdgeOrExit(static_cast<u32>(exits[k]), next[k]);
					m_f.end();
					EmitEdgeOrExit(static_cast<u32>(exits[k + 1]), next[k + 1]);
				};
				if (!cond1)
				{
					second(0);
					return;
				}
				m_f.local_get(m_cond1);
				m_f.if_();
				second(0);
				m_f.end();
				second(2);
			}
			// Exit exit_id, or go to block next of the module instead (-1: exit).
			void EmitEdgeOrExit(u32 exit_id, s32 next)
			{
				if (next >= 0)
					EmitEdge(exit_id, static_cast<u32>(next));
				else
					EmitExit(exit_id);
			}
			// Leaves by exit_id: the shared exit code (EndModule) stores the registers, the flags and the snapshots,
			// and returns it.
			void EmitExit(u32 exit_id);
			void Finish()
			{
				m_f.i32_const(0); // unreachable: every path returns
				m_m.export_function("run", m_m.add_function(m_type_run, m_f));
				m_out->wasm = m_m.finish();
			}

		private:
			// Memory access at absolute addresses. Addr pushes the address operand and returns the memarg offset:
			// VURegs fields go from its address in a local, which the prologue loads from memory so that the engine
			// keeps it in a register (a constant would be folded back into a 32-bit address built at every access).
			u32 Addr(u32 addr)
			{
				if (addr - A(R) < sizeof(VURegs))
				{
					m_f.local_get(m_rb);
					return addr - A(R);
				}
				m_f.i32_const(0);
				return addr;
			}
			void LdV(u32 addr)
			{
				const u32 o = Addr(addr);
				m_f.v128_load(o);
			}
			void Ld32(u32 addr)
			{
				const u32 o = Addr(addr);
				m_f.i32_load(o);
			}
			void Ld16u(u32 addr)
			{
				const u32 o = Addr(addr);
				m_f.i32_load16_u(o);
			}
			void Ld16s(u32 addr)
			{
				const u32 o = Addr(addr);
				m_f.i32_load16_s(o);
			}
			template <typename F>
			void StV(u32 addr, F&& value)
			{
				const u32 o = Addr(addr);
				value();
				m_f.v128_store(o);
			}
			template <typename F>
			void St32(u32 addr, F&& value)
			{
				const u32 o = Addr(addr);
				value();
				m_f.i32_store(o);
			}
			template <typename F>
			void St16(u32 addr, F&& value)
			{
				const u32 o = Addr(addr);
				value();
				m_f.i32_store16(o);
			}
			void St32c(u32 addr, u32 v) { St32(addr, [&] { m_f.i32_const(static_cast<s32>(v)); }); }
			void Splat(u32 v) { m_f.v128_pooled_i32x4(v, v, v, v); }
			void Simd(Simd o) { m_f.simd(o); }
			static u32 DestMask(u32 dest, u32 lane) { return (dest & (8 >> lane)) ? 0xffffffffu : 0; }
			void DestConst(u32 dest)
			{
				m_f.v128_pooled_i32x4(DestMask(dest, 0), DestMask(dest, 1), DestMask(dest, 2), DestMask(dest, 3));
			}
			// Stores the vector in local v to VF/ACC at addr with the dest mask.
			void StoreMerged(u32 addr, u32 v, u32 dest)
			{
				if ((dest & 0xf) == 0)
					return;
				StV(addr, [&] {
					m_f.local_get(v);
					if ((dest & 0xf) != 0xf)
					{
						LdV(addr);
						DestConst(dest);
						Simd(Simd::V128Bitselect);
					}
				});
			}

			void Clamp(u32 v); // local v = vuDouble(v)
			void ScalarDouble(); // vuDouble on the i32 bits on the stack
			void EmitLoopBack(u32 exit_id, u32 last_start);
			void EmitLoopState(); // the loop exit's ApplyExit and NormalizeRings, for the pass that just ended
			void EmitEdge(u32 exit_id, u32 next);
			void EmitEdgeState(const BlockExit& x);
			void RotateSnapshots(u32 readpos, u32 count);
			void CountPass() // a block or a loop's pass completed without leaving generated code
			{
				m_f.local_get(m_acc);
				m_f.i32_const(static_cast<s32>(m_block_n));
				m_f.i32_add();
				m_f.local_set(m_acc);
				m_f.local_get(m_edges);
				m_f.i32_const(1);
				m_f.i32_add();
				m_f.local_set(m_edges);
			}

			// VF0-VF31, ACC and VI0-VI15 as the block's native code uses them: in locals (RegUse). VI locals hold
			// the 16-bit value; memory only ever gets its low half (i32.store16), as the interpreter writes US[0].
			u32 VFLocal(u32 r)
			{
				if (!m_vf[r])
					m_vf[r] = m_f.local(ValType::V128) + 1;
				return m_vf[r] - 1;
			}
			u32 VILocal(u32 r)
			{
				if (!m_vi[r])
					m_vi[r] = m_f.local(ValType::I32) + 1;
				return m_vi[r] - 1;
			}
			u32 VFAddr(u32 r) { return r == ACC_INDEX ? A(R.ACC) : A(R.VF[r]); }
			void VF(u32 r) // pushes VF[r] (or ACC)
			{
				m_use.vf_used |= u64(1) << r;
				m_f.local_get(VFLocal(r));
			}
			void VFLane(u32 r, u32 lane) // pushes VF[r].UL[lane]
			{
				VF(r);
				m_f.simd_lane(Simd::I32x4ExtractLane, static_cast<u8>(lane));
			}
			void VFSplat(u32 r, u32 lane) // pushes VF[r].UL[lane] in every lane
			{
				VF(r);
				u8 l[16];
				for (u32 i = 0; i < 16; i++)
					l[i] = static_cast<u8>(lane * 4 + (i & 3));
				m_f.local_tee(m_splat);
				m_f.local_get(m_splat);
				m_f.shuffle(l);
			}
			// VF[r] (or ACC) = local v, lanes outside dest kept. clean: the lanes of v known to be vuDouble's fixed
			// points (Clean).
			void SetVF(u32 r, u32 v, u32 dest, u32 clean = 0)
			{
				if ((dest & 0xf) == 0)
					return;
				m_use.vf_used |= u64(1) << r;
				m_use.vf_written |= u64(1) << r;
				m_clean[r] = static_cast<u8>((m_clean[r] & ~dest & 0xf) | (clean & dest & 0xf));
				m_cc[r] = 0;
				m_f.local_get(v);
				if ((dest & 0xf) != 0xf)
				{
					m_f.local_get(VFLocal(r));
					DestConst(dest);
					Simd(Simd::V128Bitselect);
				}
				m_f.local_set(VFLocal(r));
			}
			void VI(u32 r) // pushes VI[r] zero-extended
			{
				m_use.vi_used |= 1u << r;
				m_f.local_get(VILocal(r));
			}
			void VIs(u32 r) // pushes VI[r] sign-extended
			{
				VI(r);
				m_f.i32_extend16_s();
			}
			template <typename F>
			void SetVI(u32 r, F&& value) // VI[r] = low 16 bits of value()
			{
				m_use.vi_used |= 1u << r;
				m_use.vi_written |= 1u << r;
				value();
				m_f.i32_const(0xffff);
				m_f.i32_and();
				m_f.local_set(VILocal(r));
			}
			// Clean registers (the design's section 13, step 3): lanes known to be fixed points of vuDouble (results of
			// FMAC ops, which went through the result clamp), whose operand clamp can go, and clamped copies of
			// registers (m_cc) that stay valid until the register is written. Compile-time state along the block's
			// straight-line code; a helper call forgets it; at a loop's head only copies of registers the block never
			// writes are kept, made before the loop (BeginLoop).
			void ForgetClean()
			{
				std::memset(m_clean, 0, sizeof(m_clean));
				std::memset(m_cc, 0, sizeof(m_cc));
			}
			void MakeClampedCopy(u32 r)
			{
				// Locals are reused from block to block: a block's copies are dead once it is left.
				if (m_cc_next == m_cc_pool.size())
					m_cc_pool.push_back(m_f.local(ValType::V128));
				const u32 cc = m_cc_pool[m_cc_next++];
				VF(r);
				m_f.local_set(cc);
				Clamp(cc);
				m_cc[r] = cc + 1;
			}
			// local dst = vuDouble(VF[r]) on the lanes in need (dest bits).
			void ClampedVF(u32 r, u32 need, u32 dst)
			{
				m_use.vf_fmac_read |= u64(1) << r;
				if ((m_clean[r] & need & 0xf) == (need & 0xf))
					VF(r);
				else
				{
					if (!m_cc[r])
						MakeClampedCopy(r);
					m_f.local_get(m_cc[r] - 1);
				}
				m_f.local_set(dst);
			}
			// local dst = vuDouble(VF[r].UL[lane]) in every lane.
			void ClampedSplat(u32 r, u32 lane, u32 dst)
			{
				m_use.vf_fmac_read |= u64(1) << r;
				if (m_clean[r] & (8 >> lane))
					VFSplat(r, lane);
				else
				{
					if (!m_cc[r])
						MakeClampedCopy(r);
					u8 l[16];
					for (u32 i = 0; i < 16; i++)
						l[i] = static_cast<u8>(lane * 4 + (i & 3));
					m_f.local_get(m_cc[r] - 1);
					m_f.local_get(m_cc[r] - 1);
					m_f.shuffle(l);
				}
				m_f.local_set(dst);
			}

			void LoadRegs()
			{
				for (u32 r = 0; r <= ACC_INDEX; r++)
				{
					if (m_use.vf_used & (u64(1) << r))
					{
						LdV(VFAddr(r));
						m_f.local_set(VFLocal(r));
					}
				}
				for (u32 r = 0; r < 16; r++)
				{
					if (m_use.vi_used & (1u << r))
					{
						Ld16u(A(R.VI[r]));
						m_f.local_set(VILocal(r));
					}
				}
			}
			void StoreRegs()
			{
				for (u32 r = 0; r <= ACC_INDEX; r++)
				{
					if (m_use.vf_written & (u64(1) << r))
						StV(VFAddr(r), [&] { m_f.local_get(VFLocal(r)); });
				}
				for (u32 r = 0; r < 16; r++)
				{
					if (m_use.vi_written & (1u << r))
						St16(A(R.VI[r]), [&] { m_f.local_get(VILocal(r)); });
				}
			}

			// The pending macflag, statusflag and clipflag and the committed VI[MAC], VI[STATUS] and VI[CLIP]
			// live in locals while generated code runs; memory has them at every exit and around helper calls.
			void LoadFlags()
			{
				const u32 locals[6] = {m_pmac, m_pstat, m_pclip, m_vmac, m_vstat, m_vclip};
				const u32 addrs[6] = {A(R.macflag), A(R.statusflag), A(R.clipflag), A(R.VI[REG_MAC_FLAG]),
					A(R.VI[REG_STATUS_FLAG]), A(R.VI[REG_CLIP_FLAG])};
				for (u32 i = 0; i < 6; i++)
				{
					Ld32(addrs[i]);
					m_f.local_set(locals[i]);
				}
				// All explicit: no raw value, the last write-back's low 4 bits are VI[STATUS]'s own.
				m_f.local_get(m_vstat);
				m_f.local_set(m_lxstat);
				for (u32 l : {m_pr, m_lr})
				{
					NeutralRaw();
					m_f.local_set(l);
				}
				ResetSticky();
			}
			void StoreFlags()
			{
				St32(A(R.macflag), [&] { PushMac(m_pr, m_pmac); });
				St32(A(R.statusflag), [&] { PushStat(m_pr, m_pstat); });
				St32(A(R.clipflag), [&] { m_f.local_get(m_pclip); });
				St32(A(R.VI[REG_MAC_FLAG]), [&] { PushMac(m_lr, m_vmac); });
				St32(A(R.VI[REG_STATUS_FLAG]), [&] { PushVstat(); });
				St32(A(R.VI[REG_CLIP_FLAG]), [&] { m_f.local_get(m_vclip); });
			}
			// A raw value that sets no flag: 1.0 in every lane.
			void NeutralRaw() { Splat(0x3f800000u); }
			// The sticky accumulators with nothing written back.
			void ResetSticky()
			{
				Splat(EXP);
				m_f.local_set(m_amin);
				Splat(0);
				m_f.local_set(m_amax);
				Splat(0);
				m_f.local_set(m_aor);
				Splat(0xffffffffu);
				m_f.local_set(m_aun);
				m_f.i32_const(0);
				m_f.local_set(m_axs);
			}
			void FlagBytes(u32 r); // m_v[4] = the per-lane flag bytes of the raw value in local r (Flags' old form)
			// Pushes MacBits(local r) | local x, and StatBits(local r) | local x.
			void PushMac(u32 r, u32 x);
			void PushStat(u32 r, u32 x);
			void PushVstat(); // VI[STATUS]
			// xgkickenable changes only in generated code (XGKICK) and in the helpers, which reload it after them.
			void LoadKick()
			{
				if (!m_vu)
					return;
				Ld32(A(R.xgkickenable));
				m_f.local_set(m_kick);
			}
			void Flags(u32 r, u32 dest); // MAC and status from the raw result in local r (VU_MAC_UPDATE, VU_STAT_UPDATE)
			void Flushes(const Plan& p);
			void FmacWriteback(u32 slot, int flagreg);
			void KickCatchUp(u32 delta);
			bool Upper(const Plan& p, u32 delta);
			bool Lower(const Plan& p, u32 delta);
			void Backup(const Plan& p);
			void NoteHelper()
			{
				m_use.helpers = true;
				if (m_use.block_helpers.size() <= m_k)
					m_use.block_helpers.resize(m_k + 1);
				m_use.block_helpers[m_k] = 1;
				m_out->helper_ops++;
			}
			void HelperUpper(const Plan& p, u32 delta)
			{
				if (m_vu)
					KickCatchUp(delta); // op bodies see the kick as the interpreter leaves it
				StoreRegs();
				StoreFlags();
				m_f.i32_const(static_cast<s32>(p.ins.upper));
				m_f.i32_const(static_cast<s32>(delta));
				m_f.call(m_h_upper);
				LoadRegs();
				LoadFlags();
				LoadKick();
				ForgetClean();
				NoteHelper();
			}
			void HelperLower(const Plan& p, u32 delta)
			{
				// Ops that call _vuBackupVI read the window state; the model knows it.
				if (BackupVIRegister(p.ins) >= 0)
				{
					m_f.i32_const(0);
					m_f.i32_const(p.info.vi_backup_cycles);
					m_f.i32_store8(A(R.VIBackupCycles));
					St32c(A(R.VIRegNumber), p.info.vi_backup_cycles ? p.info.vi_backup_reg : p.pre.VIRegNumber);
				}
				if (m_vu)
					KickCatchUp(delta); // op bodies see the kick as the interpreter leaves it
				StoreRegs();
				StoreFlags();
				m_f.i32_const(static_cast<s32>(p.ins.lower));
				m_f.i32_const(static_cast<s32>(delta));
				m_f.call(m_h_lower);
				LoadRegs();
				LoadFlags();
				LoadKick();
				ForgetClean();
				NoteHelper();
			}

			CompiledBlock* m_out;
			VURegs& R;
			int m_vu;
			Module m_m;
			Function m_f;
			u32 m_type_run;
			u32 m_h_kick, m_h_upper, m_h_lower;
			u32 m_v[8];
			u32 m_i[6];
			u32 m_hs, m_hn, m_hc_old, m_hc_new; // the upper/lower hazard copies
			u32 m_dx, m_de; // ScalarDouble
			// The MAC and status flags are kept lazily (section 7.3): a flag-setting op only records its raw result,
			// with the lanes outside dest set to 1.0, which sets no flag (Flags), and the flags are computed where they
			// are observed: at FMAND/FMEQ/FMOR and FSAND/FSEQ/FSOR, around op bodies and at the exits (StoreFlags).
			// A value is "raw | explicit": the MAC flag is MacBits(raw) | the explicit bits (the bits above 16 and
			// what op bodies, FSSET, DIV and OPMULA/OPMSUB leave), the status StatBits(raw) | the explicit bits.
			//   pending: m_pr | m_pmac, m_pstat; ring slot s: m_snapr[s] | m_snap[s][0], m_snap[s][1].
			//   VI[MAC]: m_lr | m_vmac, the last entry written back. VI[STATUS] (VstatValue): m_vstat's bits 4-11,
			//   the low 4 of the last entry written back (m_lr | m_lxstat), and as sticky bits the low 4 of every
			//   entry written back since m_vstat was set, OR-ed as lane accumulators of the raw values (m_amin: the
			//   minimum exponent, m_amax: the maximum, m_aor: the signs, m_aun: the minimum |x| - 1, under 0x7fffff
			//   for a denormal) and m_axs (the explicit low 4 bits).
			u32 m_pmac, m_pstat, m_pclip, m_vmac, m_vstat, m_vclip; // LoadFlags
			u32 m_pr, m_lr, m_lxstat, m_amin, m_amax, m_aor, m_aun, m_axs;
			u32 m_snap[4][3]; // per ring slot of the model: the FMAC entry's macflag, statusflag and clipflag
			u32 m_snapr[4]; // ... and its raw result
			u32 m_snapr_tmp[4];
			const BlockExit* m_loop = nullptr; // BeginLoop
			u32 m_base = 0; // VU1.cycle at the entry of the current block, or of the current pass of a loop
			u32 m_budget = 0; // g_budget_end
			u32 m_looped = 0; // nonzero once a pass went back to the entry
			u32 m_snap_tmp[12] = {};
			u32 m_next = 0; // the block the dispatch loop goes to
			u32 m_exit = 0; // the exit the shared exit code returns
			u32 m_cond1 = 0; // the condition of a branch whose delay slot holds a branch (SaveBranchCondition)
			u32 m_stub_level = 0; // levels (Function::depth) of the exits' block, the dispatch loop and a block's loop
			u32 m_dispatch_level = 0;
			u32 m_loop_level = 0;
			u32 m_acc = 0; // g_chain_instrs
			u32 m_edges = 0; // g_chain_edges
			u32 m_rb = 0, m_mb = 0; // VURegs and the VU data memory (Addr)
			u32 m_kick = 0; // VU1.xgkickenable, as generated code and the helpers leave it (LoadKick)
			u32 m_k = 0; // the block being emitted
			u32 m_block_n = 0; // its instructions
			s32 m_exit_kick = -1; // the cycle (delta) at which exits feed the kick for the block's last instruction
			std::vector<u32> m_last_starts; // per block of the module (BeginModule)
			std::vector<u32> m_cc_pool; // locals for clamped copies
			u32 m_cc_next = 0;
			RegUse m_use;
			u32 m_vf[ACC_INDEX + 1] = {}; // local index + 1, 0 until used
			u8 m_clean[ACC_INDEX + 1] = {}; // dest-style lane bits (x = 8)
			u32 m_cc[ACC_INDEX + 1] = {}; // local index + 1 of a clamped copy, 0 if none
			u32 m_vi[16] = {};
			u32 m_splat = 0;
			u32 m_mem;
		};

		static_assert(sizeof(fmacPipe) == 48 && sizeof(ialuPipe) == 24);

		void Emitter::ScalarDouble()
		{
			// vuDouble (VUops.cpp:443-460) with the overflow clamp on: exponent 0 gives the signed zero, exponent 255
			// the signed maximum.
			const u32 x = m_dx, e = m_de;
			m_f.local_set(x);
			m_f.local_get(x);
			m_f.i32_const(static_cast<s32>(SIGN));
			m_f.i32_and();
			m_f.local_get(x);
			m_f.i32_const(static_cast<s32>(SIGN));
			m_f.i32_and();
			m_f.i32_const(0x7f7fffff);
			m_f.i32_or();
			m_f.local_get(x);
			m_f.local_get(x);
			m_f.i32_const(static_cast<s32>(EXP));
			m_f.i32_and();
			m_f.local_tee(e);
			m_f.i32_const(static_cast<s32>(EXP));
			m_f.i32_eq();
			m_f.select(); // exponent 255 ? sign | max : x
			m_f.local_get(e);
			m_f.i32_eqz();
			m_f.select(); // exponent 0 ? sign : ...
		}

		void Emitter::EmitLoopBack(u32 exit_id, u32 last_start)
		{
			// The dispatcher's work between the loop exit and the same block again (VU1Jit.cpp, Dispatch and Run).
			// Its budget test is made here. ApplyExit and NormalizeRings only matter to the exit that finally leaves:
			// the timing fields they write are read by nothing else, and the next pass writes them again (the loop
			// exit has no entry left from the pass's entry, CompileBlock). So EmitExit makes them, once, for the
			// last pass (EmitLoopState). The flags' snapshots move with the ring, as NormalizeRings moves them.
			const PipeImage& m = m_loop->pipes;
			const s64 pass = static_cast<s64>(m.cycle - TIMING_MODEL_BASE);

			// Dispatch enters a block only if its last instruction starts inside the budget.
			m_f.local_get(m_base);
			m_f.i64_const(pass + last_start);
			m_f.i64_add();
			m_f.local_get(m_budget);
			m_f.i64_ge_u();
			m_f.if_unlikely();
			EmitExit(exit_id);
			m_f.end();

			RotateSnapshots(m.fmacreadpos, m.fmaccount);

			// The next pass: its base, for the helpers and Run too.
			m_f.local_get(m_base);
			m_f.i64_const(pass);
			m_f.i64_add();
			m_f.local_set(m_base);
			m_f.i32_const(0);
			m_f.local_get(m_base);
			m_f.i64_store(A(m_vu ? g_block_base : g_block_base0));
			m_f.i32_const(1);
			m_f.local_set(m_looped);
			CountPass();
			m_f.br(m_f.depth() - m_loop_level);
		}

		void Emitter::RotateSnapshots(u32 readpos, u32 count)
		{
			// NormalizeRings for the snapshot locals: entry i of the ring order goes to slot i.
			if (!readpos)
				return;
			for (u32 i = 0; i < count; i++)
			{
				for (u32 j = 0; j < 3; j++)
				{
					m_f.local_get(m_snap[(readpos + i) & 3][j]);
					m_f.local_set(m_snap_tmp[i * 3 + j]);
				}
			}
			for (u32 i = 0; i < count; i++)
			{
				m_f.local_get(m_snapr[(readpos + i) & 3]);
				m_f.local_set(m_snapr_tmp[i]);
			}
			for (u32 i = 0; i < count; i++)
			{
				for (u32 j = 0; j < 3; j++)
				{
					m_f.local_get(m_snap_tmp[i * 3 + j]);
					m_f.local_set(m_snap[i][j]);
				}
				m_f.local_get(m_snapr_tmp[i]);
				m_f.local_set(m_snapr[i]);
			}
		}

		void Emitter::EmitEdge(u32 exit_id, u32 next)
		{
			// The dispatcher's work between exit_id and the block of the module it goes to (VU1Jit.cpp, Dispatch and
			// Run), as EmitLoopBack does for a loop: its budget test, then ApplyExit and NormalizeRings for the timing
			// fields that the next exit's ApplyExit does not write again (EmitEdgeState), and the next block's base.
			// The registers and the flags stay in locals. Fields ApplyExit writes at every exit (the cycle, the ring
			// positions, the pipes' enables, the VI backup window, TPC and the control state) are left to the exit
			// that finally leaves; nothing reads them in memory before (op bodies that call _vuBackupVI get the window
			// from HelperLower).
			const BlockExit x = m_out->exits[exit_id];
			const s64 pass = static_cast<s64>(x.pipes.cycle - TIMING_MODEL_BASE);

			m_f.local_get(m_base);
			m_f.i64_const(pass + m_last_starts[next]);
			m_f.i64_add();
			m_f.local_get(m_budget);
			m_f.i64_ge_u();
			m_f.if_unlikely();
			EmitExit(exit_id);
			m_f.end();

			if (m_loop)
			{
				// The timing fields of the entries the previous pass left, as the exit would write them.
				m_f.local_get(m_looped);
				m_f.if_();
				EmitLoopState();
				m_f.end();
			}
			EmitEdgeState(x);

			m_f.local_get(m_base);
			m_f.i64_const(pass);
			m_f.i64_add();
			m_f.local_set(m_base);
			m_f.i32_const(0);
			m_f.local_get(m_base);
			m_f.i64_store(A(m_vu ? g_block_base : g_block_base0));
			CountPass();
			if (next > m_k)
				m_f.br(m_f.depth() - BlockLevel(next));
			else
			{
				m_f.i32_const(static_cast<s32>(next));
				m_f.local_set(m_next);
				m_f.br(m_f.depth() - m_dispatch_level);
			}
		}

		void Emitter::EmitEdgeState(const BlockExit& x)
		{
			// ApplyExit then NormalizeRings for x, on the entries pending at x: the next block's entries, whose fields
			// its exits leave to memory. Those pending since this block's entry (or its pass's) are in memory in their
			// ring slot (NormalizeRings, an edge or EmitLoopState put them there) and move to their place in the ring
			// order; those this block added get every field from the model. Earlier slots are written first, and
			// every move goes to a lower slot, so no entry is overwritten before it moves.
			const PipeImage& m = x.pipes;
			const auto st64 = [&](u32 addr, u64 model_time) { // base + (model_time - TIMING_MODEL_BASE)
				m_f.i32_const(0);
				m_f.local_get(m_base);
				m_f.i64_const(static_cast<s64>(model_time - TIMING_MODEL_BASE));
				m_f.i64_add();
				m_f.i64_store(addr);
			};
			for (u32 i = 0; i < m.fmaccount; i++)
			{
				const u32 s = (m.fmacreadpos + i) & 3;
				const fmacPipe& p = m.fmac[s];
				const fmacPipe& q = R.fmac[i];
				if (p.sCycle < TIMING_MODEL_BASE)
				{
					pxAssert(s >= i);
					if (s != i)
					{
						for (u32 o = 0; o < sizeof(fmacPipe); o += 16)
							StV(A(q) + o, [&] { LdV(A(R.fmac[s]) + o); });
					}
					continue;
				}
				St32c(A(q.regupper), p.regupper);
				St32c(A(q.reglower), p.reglower);
				St32c(A(q.flagreg), static_cast<u32>(p.flagreg));
				St32c(A(q.xyzwupper), p.xyzwupper);
				St32c(A(q.xyzwlower), p.xyzwlower);
				st64(A(q.sCycle), p.sCycle);
				St32c(A(q.Cycle), p.Cycle);
			}
			for (u32 i = 0; i < m.ialucount; i++)
			{
				const u32 s = (m.ialureadpos + i) & 3;
				const ialuPipe& p = m.ialu[s];
				if (p.sCycle < TIMING_MODEL_BASE)
				{
					pxAssert(s >= i);
					if (s != i)
					{
						for (u32 o = 0; o < sizeof(ialuPipe); o += 8)
						{
							m_f.i32_const(0);
							m_f.i32_const(0);
							m_f.i64_load(A(R.ialu[s]) + o);
							m_f.i64_store(A(R.ialu[i]) + o);
						}
					}
					continue;
				}
				St32c(A(R.ialu[i].reg), static_cast<u32>(p.reg));
				st64(A(R.ialu[i].sCycle), p.sCycle);
				St32c(A(R.ialu[i].Cycle), p.Cycle);
			}
			if (m.fdiv.enable && m.fdiv.sCycle >= TIMING_MODEL_BASE)
			{
				st64(A(R.fdiv.sCycle), m.fdiv.sCycle);
				St32c(A(R.fdiv.Cycle), m.fdiv.Cycle);
			}
			if (m.efu.enable)
			{
				if (m.efu.sCycle >= TIMING_MODEL_BASE)
				{
					st64(A(R.efu.sCycle), m.efu.sCycle);
					St32c(A(R.efu.Cycle), m.efu.Cycle);
				}
				else if (x.efu_cycle_adjust)
				{
					St32(A(R.efu.Cycle), [&] {
						Ld32(A(R.efu.Cycle));
						m_f.i32_const(x.efu_cycle_adjust);
						m_f.i32_add();
					});
				}
			}
			RotateSnapshots(m.fmacreadpos, m.fmaccount);
		}

		void Emitter::EmitLoopState()
		{
			// ApplyExit (VU1Jit.cpp) for the loop exit at the end of the previous pass, whose base is this pass's
			// minus the pass's cycles, then NormalizeRings: entry i of the exit goes to ring slot i. Every entry is
			// one the previous pass added, so every field comes from the model. Fields ApplyExit then writes again
			// for the exit that leaves (TPC, the control state, the cycle) are left to it.
			const PipeImage& m = m_loop->pipes;
			const s64 pass = static_cast<s64>(m.cycle - TIMING_MODEL_BASE);
			const auto st64 = [&](u32 addr, u64 model_time) { // previous base + (model_time - TIMING_MODEL_BASE)
				m_f.i32_const(0);
				m_f.local_get(m_base);
				m_f.i64_const(static_cast<s64>(model_time - TIMING_MODEL_BASE) - pass);
				m_f.i64_add();
				m_f.i64_store(addr);
			};
			for (u32 i = 0; i < m.fmaccount; i++)
			{
				const fmacPipe& p = m.fmac[(m.fmacreadpos + i) & 3];
				const fmacPipe& q = R.fmac[i];
				St32c(A(q.regupper), p.regupper);
				St32c(A(q.reglower), p.reglower);
				St32c(A(q.flagreg), static_cast<u32>(p.flagreg));
				St32c(A(q.xyzwupper), p.xyzwupper);
				St32c(A(q.xyzwlower), p.xyzwlower);
				st64(A(q.sCycle), p.sCycle);
				St32c(A(q.Cycle), p.Cycle);
			}
			for (u32 i = 0; i < m.ialucount; i++)
			{
				const ialuPipe& p = m.ialu[(m.ialureadpos + i) & 3];
				St32c(A(R.ialu[i].reg), static_cast<u32>(p.reg));
				st64(A(R.ialu[i].sCycle), p.sCycle);
				St32c(A(R.ialu[i].Cycle), p.Cycle);
			}
			if (m.fdiv.enable)
			{
				st64(A(R.fdiv.sCycle), m.fdiv.sCycle);
				St32c(A(R.fdiv.Cycle), m.fdiv.Cycle);
			}
			if (m.efu.enable)
			{
				st64(A(R.efu.sCycle), m.efu.sCycle);
				St32c(A(R.efu.Cycle), m.efu.Cycle);
			}
		}

		void Emitter::EmitExit(u32 exit_id)
		{
			// Every exit leaves the registers and the flags in memory (LoadRegs, LoadFlags), with the snapshots of the
			// FMAC entries pending there in their ring slots (the shared code after the dispatch loop, EndModule), and
			// what only some exits need here; ApplyExit writes the rest of the timing state.
			if (m_exit_kick >= 0)
				KickCatchUp(static_cast<u32>(m_exit_kick));
			if (m_loop)
			{
				m_f.local_get(m_looped);
				m_f.if_();
				EmitLoopState();
				m_f.end();
			}
			m_f.i32_const(static_cast<s32>(exit_id));
			m_f.local_set(m_exit);
			m_f.br(m_f.depth() - m_stub_level);
		}

		void Emitter::Clamp(u32 v)
		{
			// vuDouble (VUops.cpp:443-459) with the overflow clamp on, and VU_MAC_UPDATE's result (VUflags.cpp:15-46):
			// exponent 0 gives the signed zero, exponent 255 the signed maximum.
			const u32 e = m_v[6], sgn = m_v[7];
			m_f.local_get(v);
			Splat(EXP);
			Simd(Simd::V128And);
			m_f.local_set(e);
			m_f.local_get(v);
			Splat(SIGN);
			Simd(Simd::V128And);
			m_f.local_tee(sgn);
			Splat(0x7f7fffffu);
			Simd(Simd::V128Or);
			m_f.local_get(v);
			m_f.local_get(e);
			Splat(EXP);
			Simd(Simd::I32x4Eq);
			Simd(Simd::V128Bitselect); // exponent 255 ? sign | max : v
			m_f.local_set(v);
			m_f.local_get(sgn);
			m_f.local_get(v);
			m_f.local_get(e);
			Splat(0);
			Simd(Simd::I32x4Eq);
			Simd(Simd::V128Bitselect); // exponent 0 ? sign : ...
			m_f.local_set(v);
		}

		void Emitter::Flags(u32 r, u32 dest)
		{
			// VU_MAC_UPDATE on the dest lanes, VU_MACx_CLEAR on the others, then VU_STAT_UPDATE, lazily: the raw
			// result with 1.0 outside dest, the bits above MAC's 16 kept, no explicit status bits.
			m_f.local_get(r);
			if ((dest & 0xf) != 0xf)
			{
				NeutralRaw();
				DestConst(dest);
				Simd(Simd::V128Bitselect);
			}
			m_f.local_set(m_pr);
			m_f.local_get(m_pmac);
			m_f.i32_const(static_cast<s32>(0xffff0000u));
			m_f.i32_and();
			m_f.local_set(m_pmac);
			m_f.i32_const(0);
			m_f.local_set(m_pstat);
		}

		void Emitter::FlagBytes(u32 r)
		{
			// Per lane of the raw value: zero (exponent 0), sign, underflow (exponent 0, mantissa not 0), overflow
			// (exponent 255) (VU_MAC_UPDATE). Bytes: zero xyzw, sign xyzw, underflow xyzw, overflow xyzw.
			const u32 e = m_v[6], zm = m_v[7], n = m_v[4];
			m_f.local_get(r);
			Splat(EXP);
			Simd(Simd::V128And);
			m_f.local_set(e);
			m_f.local_get(e);
			Splat(0);
			Simd(Simd::I32x4Eq);
			m_f.local_tee(zm);
			m_f.local_get(r);
			m_f.i32_const(31);
			Simd(Simd::I32x4ShrS);
			Simd(Simd::I16x8NarrowI32x4S);
			// underflow: zero and a mantissa
			m_f.local_get(zm);
			m_f.local_get(r);
			Splat(0x007fffffu);
			Simd(Simd::V128And);
			Splat(0);
			Simd(Simd::I32x4Eq);
			Simd(Simd::V128AndNot);
			m_f.local_get(e);
			Splat(EXP);
			Simd(Simd::I32x4Eq);
			Simd(Simd::I16x8NarrowI32x4S);
			Simd(Simd::I8x16NarrowI16x8S);
			m_f.local_set(n);
		}

		void Emitter::PushMac(u32 r, u32 x)
		{
			// MAC bit order: x is bit 3 of each nibble.
			FlagBytes(r);
			m_f.local_get(m_v[4]);
			m_f.local_get(m_v[4]);
			static constexpr u8 rev[16] = {3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12};
			m_f.shuffle(rev);
			Simd(Simd::I8x16Bitmask);
			m_f.local_get(x);
			m_f.i32_or();
		}

		void Emitter::PushStat(u32 r, u32 x)
		{
			// One bit per group with any lane set (VU_STAT_UPDATE).
			FlagBytes(r);
			m_f.local_get(m_v[4]);
			Splat(0);
			Simd(Simd::I32x4Ne);
			Simd(Simd::I32x4Bitmask);
			m_f.local_get(x);
			m_f.i32_or();
		}

		void Emitter::PushVstat()
		{
			// _vuFMACflush's update, (status & 0xFF0) | low4 | low4 << 6, applied for every entry written back since
			// m_vstat was set: m_vstat's bits 4-11, the last entry's low 4 bits, and the OR of all their low 4 bits
			// shifted to the sticky ones.
			m_f.local_get(m_vstat);
			m_f.i32_const(0xFF0);
			m_f.i32_and();
			PushStat(m_lr, m_lxstat);
			m_f.i32_const(0xF);
			m_f.i32_and();
			m_f.i32_or();
			// zero: an exponent 0 in some lane
			m_f.local_get(m_amin);
			Splat(0);
			Simd(Simd::I32x4Eq);
			Simd(Simd::V128AnyTrue);
			// sign
			m_f.local_get(m_aor);
			Simd(Simd::I32x4Bitmask);
			m_f.i32_const(0);
			m_f.i32_ne();
			m_f.i32_const(1);
			m_f.i32_shl();
			m_f.i32_or();
			// underflow: 0 < |x| < 0x800000
			m_f.local_get(m_aun);
			Splat(0x007fffffu);
			Simd(Simd::I32x4LtU);
			Simd(Simd::V128AnyTrue);
			m_f.i32_const(2);
			m_f.i32_shl();
			m_f.i32_or();
			// overflow: an exponent 255
			m_f.local_get(m_amax);
			Splat(EXP);
			Simd(Simd::I32x4Eq);
			Simd(Simd::V128AnyTrue);
			m_f.i32_const(3);
			m_f.i32_shl();
			m_f.i32_or();
			m_f.local_get(m_axs);
			m_f.i32_or();
			m_f.i32_const(6);
			m_f.i32_shl();
			m_f.i32_or();
		}

		void Emitter::FmacWriteback(u32 slot, int flagreg)
		{
			// _vuFMACflush / _vuFlushAll for one entry (VUops.cpp:51-63), on the flags in locals (lazily, see m_pr).
			const u32* e = m_snap[slot];
			const u32 r = m_snapr[slot];
			if (flagreg & (1 << REG_CLIP_FLAG))
			{
				m_f.local_get(e[2]);
				m_f.local_set(m_vclip);
			}
			if (flagreg & (1 << REG_STATUS_FLAG))
			{
				// FSSET's entry: (status & 0x30) | (sf & 0xFC0) | (sf & 0xF), from here on the base of the sticky bits.
				const u32 sf = m_dx;
				PushStat(r, e[1]);
				m_f.local_set(sf);
				PushVstat();
				m_f.i32_const(0x30);
				m_f.i32_and();
				m_f.local_get(sf);
				m_f.i32_const(0xFC0);
				m_f.i32_and();
				m_f.i32_or();
				m_f.local_get(sf);
				m_f.i32_const(0xF);
				m_f.i32_and();
				m_f.i32_or();
				m_f.local_set(m_vstat);
				ResetSticky();
			}
			else
			{
				// (status & 0xFF0) | low4 | low4 << 6: the entry's flags go into the sticky accumulators.
				m_f.local_get(r);
				Splat(EXP);
				Simd(Simd::V128And);
				m_f.local_tee(m_v[4]);
				m_f.local_get(m_amin);
				Simd(Simd::I32x4MinU);
				m_f.local_set(m_amin);
				m_f.local_get(m_v[4]);
				m_f.local_get(m_amax);
				Simd(Simd::I32x4MaxU);
				m_f.local_set(m_amax);
				m_f.local_get(r);
				m_f.local_get(m_aor);
				Simd(Simd::V128Or);
				m_f.local_set(m_aor);
				m_f.local_get(r);
				Splat(0x7fffffffu);
				Simd(Simd::V128And);
				Splat(1);
				Simd(Simd::I32x4Sub);
				m_f.local_get(m_aun);
				Simd(Simd::I32x4MinU);
				m_f.local_set(m_aun);
				m_f.local_get(e[1]);
				m_f.i32_const(0xF);
				m_f.i32_and();
				m_f.local_get(m_axs);
				m_f.i32_or();
				m_f.local_set(m_axs);
			}
			// The last entry written back: VI[MAC] and the low 4 bits of VI[STATUS].
			m_f.local_get(r);
			m_f.local_set(m_lr);
			m_f.local_get(e[0]);
			m_f.local_set(m_vmac);
			m_f.local_get(e[1]);
			m_f.local_set(m_lxstat);
		}

		void Emitter::Flushes(const Plan& p)
		{
			// _vuTestPipes (VUops.cpp:188-209): FMAC entries in ring order, then FDIV, then EFU (IALU has no data).
			for (u32 k = 0; k < p.info.fmac_flushed; k++)
			{
				const u32 slot = (p.pre.fmacreadpos + k) & 3;
				FmacWriteback(slot, p.pre.fmac[slot].flagreg);
			}
			if (p.info.fdiv_flushed)
			{
				St32(A(R.VI[REG_Q]), [&] { Ld32(A(R.fdiv.reg)); });
				m_f.local_get(m_vstat);
				m_f.i32_const(0xFCF);
				m_f.i32_and();
				Ld32(A(R.fdiv.statusflag));
				m_f.i32_const(0xC30);
				m_f.i32_and();
				m_f.i32_or();
				m_f.local_set(m_vstat);
			}
			if (p.info.efu_flushed)
				St32(A(R.VI[REG_P]), [&] { Ld32(A(R.efu.reg)); });
		}

		void Emitter::FlushAllAtEnd(const Plan& p, const int end_flagreg[4])
		{
			// _vuFlushAll (VUops.cpp:130-186).
			if (p.info.end_fdiv)
			{
				St32(A(R.VI[REG_Q]), [&] { Ld32(A(R.fdiv.reg)); });
				m_f.local_get(m_vstat);
				m_f.i32_const(0xFCF);
				m_f.i32_and();
				Ld32(A(R.fdiv.statusflag));
				m_f.i32_const(0xC30);
				m_f.i32_and();
				m_f.i32_or();
				m_f.local_set(m_vstat);
			}
			if (p.info.end_efu)
				St32(A(R.VI[REG_P]), [&] { Ld32(A(R.efu.reg)); });
			for (u32 k = 0; k < p.info.end_fmac_count; k++)
			{
				const u32 slot = (p.info.end_fmac_read + k) & 3;
				FmacWriteback(slot, end_flagreg[slot]);
			}
		}

		void Emitter::KickCatchUp(u32 delta)
		{
			if (!m_vu)
				return;
			m_f.local_get(m_kick);
			m_f.if_unlikely();
			m_f.i32_const(static_cast<s32>(delta));
			m_f.call(m_h_kick);
			LoadKick(); // the kick may have ended
			m_f.end();
		}

		static int DebugMask()
		{
			static const int mask = [] { const char* e = std::getenv("VU1JIT_HELPERS"); return e ? std::atoi(e) : 0; }();
			return mask;
		}

		// Whether every pipe entry pending in m was added by the block (none left from its entry): then the block's
		// loop exit is fully described by the model (EmitLoopState).
		static bool OnlyNewEntries(const PipeImage& m)
		{
			for (u32 i = 0; i < m.fmaccount; i++)
			{
				if (m.fmac[(m.fmacreadpos + i) & 3].sCycle < TIMING_MODEL_BASE)
					return false;
			}
			for (u32 i = 0; i < m.ialucount; i++)
			{
				if (m.ialu[(m.ialureadpos + i) & 3].sCycle < TIMING_MODEL_BASE)
					return false;
			}
			return (!m.fdiv.enable || m.fdiv.sCycle >= TIMING_MODEL_BASE) && (!m.efu.enable || m.efu.sCycle >= TIMING_MODEL_BASE);
		}

		// Loops in generated code (EmitLoopBack). Off with VU1JIT_NOLOOP, to bisect a difference, and with the
		// dispatcher's VU1JIT_BLOCKCHECK, which replays one pass of a block on the interpreter.
		static bool LoopsEnabled()
		{
			static const bool enabled = !std::getenv("VU1JIT_NOLOOP") && !std::getenv("VU1JIT_BLOCKCHECK");
			return enabled;
		}

		bool Emitter::Upper(const Plan& p, u32 delta)
		{
			if ((DebugMask() & 1) && p.ins.up != U::NOP)
			{
				HelperUpper(p, delta);
				return true;
			}
			const u32 code = p.ins.upper;
			const u32 fd = (code >> 6) & 0x1f, fs = (code >> 11) & 0x1f, ft = (code >> 16) & 0x1f;
			const u32 dest = (code >> 21) & 0xf;
			enum K { NONE, ADD, SUB, MUL, MADD, MSUB } k = NONE;
			bool acc = false;
			int bc = -1; // 0-3 lane of ft, 4 I, 5 Q, -1 vector
			switch (p.ins.up)
			{
				case U::NOP: return true;
#define VU1JIT_BC(name, kind, isacc) \
	case U::name##x: k = kind; acc = isacc; bc = 0; break; \
	case U::name##y: k = kind; acc = isacc; bc = 1; break; \
	case U::name##z: k = kind; acc = isacc; bc = 2; break; \
	case U::name##w: k = kind; acc = isacc; bc = 3; break; \
	case U::name##q: k = kind; acc = isacc; bc = 5; break; \
	case U::name: k = kind; acc = isacc; bc = -1; break;
				VU1JIT_BC(ADD, ADD, false)
				VU1JIT_BC(SUB, SUB, false)
				VU1JIT_BC(MUL, MUL, false)
				VU1JIT_BC(MADD, MADD, false)
				VU1JIT_BC(MSUB, MSUB, false)
				VU1JIT_BC(ADDA, ADD, true)
				VU1JIT_BC(SUBA, SUB, true)
				VU1JIT_BC(MULA, MUL, true)
				VU1JIT_BC(MADDA, MADD, true)
				VU1JIT_BC(MSUBA, MSUB, true)
#undef VU1JIT_BC
				case U::ADDi:
					if (CHECK_VUADDSUBHACK)
						break; // vuADD_TriAceHack
					k = ADD; bc = 4; break;
				case U::SUBi: k = SUB; bc = 4; break;
				case U::MULi: k = MUL; bc = 4; break;
				case U::MADDi: k = MADD; bc = 4; break;
				case U::MSUBi: k = MSUB; bc = 4; break;
				case U::ADDAi: k = ADD; acc = true; bc = 4; break;
				case U::SUBAi: k = SUB; acc = true; bc = 4; break;
				case U::MULAi: k = MUL; acc = true; bc = 4; break;
				case U::MADDAi: k = MADD; acc = true; bc = 4; break;
				case U::MSUBAi: k = MSUB; acc = true; bc = 4; break;
				case U::FTOI0: case U::FTOI4: case U::FTOI12: case U::FTOI15:
				{
					// floatToInt (VUops.cpp:878-891)
					if (ft == 0)
						return true;
					const u32 off = (p.ins.up == U::FTOI0) ? 0 : (p.ins.up == U::FTOI4) ? 4 : (p.ins.up == U::FTOI12) ? 12 : 15;
					const u32 u = m_v[0], r = m_v[1];
					VF(fs);
					if (off)
					{
						Splat(0x3f800000u + (off << 23));
						Simd(Simd::F32x4Mul);
					}
					m_f.local_tee(u);
					Splat(0x7fffffffu);
					m_f.local_get(u);
					m_f.i32_const(31);
					Simd(Simd::I32x4ShrU);
					Simd(Simd::I32x4Add); // the saturated value by sign
					m_f.local_get(u);
					Simd(Simd::I32x4TruncSatF32x4S);
					m_f.local_get(u);
					Splat(EXP);
					Simd(Simd::V128And);
					Splat(0x4f000000u);
					Simd(Simd::I32x4GeU);
					Simd(Simd::V128Bitselect);
					m_f.local_set(r);
					SetVF(ft, r, dest);
					m_out->native_ops++;
					return true;
				}
				case U::ITOF0: case U::ITOF4: case U::ITOF12: case U::ITOF15:
				{
					// intToFloat (VUops.cpp:898-904)
					if (ft == 0)
						return true;
					const u32 off = (p.ins.up == U::ITOF0) ? 0 : (p.ins.up == U::ITOF4) ? 4 : (p.ins.up == U::ITOF12) ? 12 : 15;
					const u32 r = m_v[1];
					VF(fs);
					Simd(Simd::F32x4ConvertI32x4S);
					if (off)
					{
						Splat(0x3f800000u - (off << 23));
						Simd(Simd::F32x4Mul);
					}
					m_f.local_set(r);
					SetVF(ft, r, dest);
					m_out->native_ops++;
					return true;
				}
				case U::ABS:
				{
					if (ft == 0)
						return true;
					const u32 r = m_v[1];
					const u32 clean = m_clean[fs]; // |x| is clean when x is
					VF(fs);
					Splat(0x7fffffffu);
					Simd(Simd::V128And);
					m_f.local_set(r);
					SetVF(ft, r, dest, clean);
					m_out->native_ops++;
					return true;
				}
				case U::CLIP:
				{
					// _vuCLIP (VUops.cpp:912-928)
					const u32 w = m_i[0], pm = m_i[1], nm = m_i[2], fsv = m_v[0];
					VFLane(ft, 3);
					m_f.local_tee(w);
					m_f.i32_const(0x7fffffff);
					m_f.i32_and();
					m_f.i32_const(0x007fffff);
					m_f.local_get(w);
					m_f.i32_const(static_cast<s32>(EXP));
					m_f.i32_and();
					m_f.select();
					Simd(Simd::I32x4Splat);
					m_f.local_set(m_v[1]);
					VF(fs);
					m_f.local_tee(fsv);
					m_f.local_get(m_v[1]);
					Simd(Simd::I32x4GtS);
					Simd(Simd::I32x4Bitmask);
					m_f.local_set(pm);
					m_f.local_get(fsv);
					Splat(SIGN);
					Simd(Simd::V128Xor);
					m_f.local_get(m_v[1]);
					Simd(Simd::I32x4GtS);
					Simd(Simd::I32x4Bitmask);
					m_f.local_set(nm);
					{
						m_f.local_get(m_pclip);
						m_f.i32_const(6);
						m_f.i32_shl();
						// pos x,y,z -> bits 0,2,4; neg -> 1,3,5
						for (u32 lane = 0; lane < 3; lane++)
						{
							m_f.local_get(pm);
							m_f.i32_const(1 << lane);
							m_f.i32_and();
							if (lane)
							{
								m_f.i32_const(static_cast<s32>(lane));
								m_f.i32_shl();
							}
							m_f.i32_or();
							m_f.local_get(nm);
							m_f.i32_const(1 << lane);
							m_f.i32_and();
							m_f.i32_const(static_cast<s32>(lane + 1));
							m_f.i32_shl();
							m_f.i32_or();
						}
						m_f.i32_const(0xFFFFFF);
						m_f.i32_and();
						m_f.local_set(m_pclip);
					}
					m_out->native_ops++;
					return true;
				}
				case U::MAX:
				case U::MAXx:
				case U::MAXy:
				case U::MAXz:
				case U::MAXw:
				case U::MAXi:
				case U::MINI:
				case U::MINIx:
				case U::MINIy:
				case U::MINIz:
				case U::MINIw:
				case U::MINIi:
				{
					// applyMinMax / applyMinMaxBroadcast with fp_max / fp_min (VUops.cpp:788-842): signed integer
					// compares of the raw bits, reversed when both are negative. No flags; fd 0 writes nothing.
					if (fd == 0)
						return true;
					const bool max = p.ins.up == U::MAX || p.ins.up == U::MAXx || p.ins.up == U::MAXy ||
					                 p.ins.up == U::MAXz || p.ins.up == U::MAXw || p.ins.up == U::MAXi;
					const u32 a = m_v[0], b = m_v[1], r = m_v[3];
					// Each result lane is one of its operands' lanes: clean where both are (Clean).
					u32 clean = m_clean[fs];
					VF(fs);
					m_f.local_set(a);
					switch (p.ins.up)
					{
						case U::MAX:
						case U::MINI:
							clean &= m_clean[ft];
							VF(ft);
							break;
						case U::MAXi:
						case U::MINIi:
							clean = 0;
							m_f.i32_const(0);
							m_f.simd_mem(Simd::V128Load32Splat, A(R.VI[REG_I]), 2);
							break;
						default:
						{
							const u32 lane = (p.ins.up == U::MAXx || p.ins.up == U::MINIx) ? 0 :
							                 (p.ins.up == U::MAXy || p.ins.up == U::MINIy) ? 1 :
							                 (p.ins.up == U::MAXz || p.ins.up == U::MINIz) ? 2 :
							                                                                 3;
							if (!(m_clean[ft] & (8 >> lane)))
								clean = 0;
							VFSplat(ft, lane);
							break;
						}
					}
					m_f.local_set(b);
					// both negative ? (max ? min_s : max_s) : (max ? max_s : min_s)
					m_f.local_get(a);
					m_f.local_get(b);
					Simd(max ? Simd::I32x4MinS : Simd::I32x4MaxS);
					m_f.local_get(a);
					m_f.local_get(b);
					Simd(max ? Simd::I32x4MaxS : Simd::I32x4MinS);
					m_f.local_get(a);
					m_f.local_get(b);
					Simd(Simd::V128And);
					m_f.i32_const(31);
					Simd(Simd::I32x4ShrS);
					Simd(Simd::V128Bitselect);
					m_f.local_set(r);
					SetVF(fd, r, dest, clean);
					m_out->native_ops++;
					return true;
				}
				case U::OPMULA:
				case U::OPMSUB:
				{
					// _vuOPMULA / _vuOPMSUB (VUops.cpp:844-873): x, y and z of fs.yzx * ft.zxy (subtracted from ACC for
					// OPMSUB), whatever the dest field says. MAC's w bits stay as the previous op left them, and the
					// status is taken from all four lanes (VU_STAT_UPDATE).
					const bool sub = p.ins.up == U::OPMSUB;
					const u32 a = m_v[0], b = m_v[1], c = m_v[2], r = m_v[3], w = m_i[0];
					static constexpr u8 yzx[16] = {4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 12, 13, 14, 15};
					static constexpr u8 zxy[16] = {8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15};
					ClampedVF(fs, 0xE, a);
					ClampedVF(ft, 0xE, b);
					if (sub)
					{
						ClampedVF(ACC_INDEX, 0xE, c);
						m_f.local_get(c);
					}
					m_f.local_get(a);
					m_f.local_get(a);
					m_f.shuffle(yzx);
					m_f.local_get(b);
					m_f.local_get(b);
					m_f.shuffle(zxy);
					Simd(Simd::F32x4Mul);
					if (sub)
						Simd(Simd::F32x4Sub);
					m_f.local_set(r);
					// Lazily (see m_pr): the raw x, y and z with the previous raw w, MAC's w bits and the bits above 16
					// kept explicit, and the status of those w bits explicit.
					m_f.local_get(r);
					m_f.local_get(m_pr);
					DestConst(0xE);
					Simd(Simd::V128Bitselect);
					m_f.local_set(m_pr);
					m_f.local_get(m_pmac);
					m_f.i32_const(static_cast<s32>(0xffff1111u));
					m_f.i32_and();
					m_f.local_tee(m_pmac);
					m_f.i32_const(0x1111);
					m_f.i32_and();
					m_f.local_set(w);
					// Z.w is bit 0, S.w bit 4, U.w bit 8, O.w bit 12
					m_f.i32_const(0);
					for (u32 g = 0; g < 4; g++)
					{
						m_f.local_get(w);
						if (g)
						{
							m_f.i32_const(static_cast<s32>(3 * g));
							m_f.i32_shr_u();
						}
						m_f.i32_const(1 << g);
						m_f.i32_and();
						m_f.i32_or();
					}
					m_f.local_set(m_pstat);
					if (!sub || fd != 0) // fd 0 writes RDzero, which nothing reads
					{
						Clamp(r);
						SetVF(sub ? fd : ACC_INDEX, r, 0xE, 0xf);
					}
					m_out->native_ops++;
					return true;
				}
				default: break;
			}
			if (k == NONE)
			{
				HelperUpper(p, delta);
				return true;
			}

			// applyBinaryMACOp / applyTernaryMACOp and their broadcasts (VUops.cpp:516-552, 681-710).
			// Only the dest lanes of the operands matter: the others are neither stored nor flagged.
			const u32 a = m_v[0], b = m_v[1], c = m_v[2], r = m_v[3];
			ClampedVF(fs, dest, a);
			if (bc < 0)
				ClampedVF(ft, dest, b);
			else if (bc < 4)
				ClampedSplat(ft, static_cast<u32>(bc), b);
			else
			{
				m_f.i32_const(0);
				m_f.simd_mem(Simd::V128Load32Splat, A(R.VI[(bc == 4) ? REG_I : REG_Q]), 2);
				m_f.local_set(b);
				Clamp(b);
			}
			if (k == MADD || k == MSUB)
			{
				ClampedVF(ACC_INDEX, dest, c);
				m_f.local_get(c);
				m_f.local_get(a);
				m_f.local_get(b);
				Simd(Simd::F32x4Mul);
				Simd(k == MADD ? Simd::F32x4Add : Simd::F32x4Sub);
			}
			else
			{
				m_f.local_get(a);
				m_f.local_get(b);
				Simd(k == ADD ? Simd::F32x4Add : k == SUB ? Simd::F32x4Sub : Simd::F32x4Mul);
			}
			m_f.local_set(r);
			Flags(r, dest);
			if (acc || fd != 0) // fd 0 writes RDzero, which nothing reads
			{
				Clamp(r);
				SetVF(acc ? ACC_INDEX : fd, r, dest, 0xf);
			}
			m_out->native_ops++;
			return true;
		}

		void Emitter::Backup(const Plan& p)
		{
			// _vuBackupVI (VUops.cpp:420-434): the window's cycles and register are the model's; only the value is data.
			if (p.info.backup_vi < 0 || p.info.backup_chained)
				return;
			St32(A(R.VIOldValue), [&] { VI(p.info.backup_vi); });
		}

		bool Emitter::Lower(const Plan& p, u32 delta)
		{
			const u32 code = p.ins.lower;
			const u32 ft = (code >> 16) & 0x1f, fs = (code >> 11) & 0x1f;
			const u32 it = ft & 0xf, is = fs & 0xf, id = ((code >> 6) & 0x1f) & 0xf;
			const u32 dest = (code >> 21) & 0xf;
			const s32 imm11 = (code & 0x400) ? static_cast<s32>(0xfffffc00u | (code & 0x3ff)) : static_cast<s32>(code & 0x3ff);
			const s32 imm15 = static_cast<s32>(((code >> 10) & 0x7800) | (code & 0x7ff));
			const u32 v = m_v[0];
			const auto vi_op = [&](u32 dst, auto&& value) {
				Backup(p);
				SetVI(dst, value);
				m_out->native_ops++;
			};
			// VU memory address of a qword index on the stack (GET_VU_MEM: & 0x3fff after * 16 for VU1, & 0xfff for VU0,
			// whose 0x4000 mapping EmitInstruction left to the interpreter), from the data memory's base in a local (Addr).
			const auto qaddr = [&] {
				m_f.i32_const(4);
				m_f.i32_shl();
				m_f.i32_const(m_vu ? 0x3ff0 : 0xff0);
				m_f.i32_and();
				m_f.local_get(m_mb);
				m_f.i32_add();
			};
			const auto load_q = [&](u32 to_vf) { // VF[to_vf] <- mem[addr on stack], dest merged
				m_f.v128_load(0);
				m_f.local_set(v);
				SetVF(to_vf, v, dest);
			};
			const auto store_q = [&](u32 from_vf) { // mem[addr in m_i[0]] <- VF[from_vf], dest merged
				if (dest == 0)
					return;
				m_f.local_get(m_i[0]);
				VF(from_vf);
				if (dest != 0xf)
				{
					m_f.local_get(m_i[0]);
					m_f.v128_load(0);
					DestConst(dest);
					Simd(Simd::V128Bitselect);
				}
				m_f.v128_store(0);
			};

			if (((DebugMask() & 2) && !IsBranch(p.ins.lo)) || (m_vu == 0 && IsVu0Memory(p) && !Vu0MemoryNative(p))) // GET_VU_MEM
			{
				HelperLower(p, delta);
				return true;
			}
			switch (p.ins.lo)
			{
				case L::MOVE:
					if (ft == 0 || dest == 0)
						return true;
					VF(fs);
					m_f.local_set(v);
					SetVF(ft, v, dest);
					m_out->native_ops++;
					return true;
				case L::MR32:
				{
					if (ft == 0)
						return true;
					static constexpr u8 rot[16] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3};
					VF(fs);
					m_f.shuffle1(rot, v);
					m_f.local_set(v);
					SetVF(ft, v, dest);
					m_out->native_ops++;
					return true;
				}
				case L::LQ:
					if (ft == 0)
						return true;
					VIs(is);
					m_f.i32_const(imm11);
					m_f.i32_add();
					qaddr();
					load_q(ft);
					m_out->native_ops++;
					return true;
				case L::SQ:
					VIs(it);
					m_f.i32_const(imm11);
					m_f.i32_add();
					qaddr();
					m_f.local_set(m_i[0]);
					store_q(fs);
					m_out->native_ops++;
					return true;
				case L::LQI:
					Backup(p);
					if (ft)
					{
						VI(is);
						qaddr();
						load_q(ft);
					}
					if (fs != 0)
						SetVI(is, [&] { VI(is); m_f.i32_const(1); m_f.i32_add(); });
					m_out->native_ops++;
					return true;
				case L::LQD:
					Backup(p);
					if (is != 0)
						SetVI(is, [&] { VI(is); m_f.i32_const(1); m_f.i32_sub(); });
					if (ft)
					{
						VI(is);
						qaddr();
						load_q(ft);
					}
					m_out->native_ops++;
					return true;
				case L::SQI:
					Backup(p);
					VI(it);
					qaddr();
					m_f.local_set(m_i[0]);
					store_q(fs);
					if (ft != 0)
						SetVI(it, [&] { VI(it); m_f.i32_const(1); m_f.i32_add(); });
					m_out->native_ops++;
					return true;
				case L::SQD:
					Backup(p);
					if (ft != 0)
						SetVI(it, [&] { VI(it); m_f.i32_const(1); m_f.i32_sub(); });
					VI(it);
					qaddr();
					m_f.local_set(m_i[0]);
					store_q(fs);
					m_out->native_ops++;
					return true;
				case L::IADD:
				case L::ISUB:
				case L::IAND:
				case L::IOR:
					if (id == 0)
						return true;
					vi_op(id, [&] {
						VI(is);
						VI(it);
						switch (p.ins.lo)
						{
							case L::IADD:
								m_f.i32_add();
								break;
							case L::ISUB:
								m_f.i32_sub();
								break;
							case L::IAND:
								m_f.i32_and();
								break;
							default:
								m_f.i32_or();
								break;
						}
					});
					return true;
				case L::IADDI:
				{
					if (it == 0)
						return true;
					const s32 imm5 = static_cast<s32>(((code >> 6) & 0x10) ? (0xfff0u | ((code >> 6) & 0xf)) : ((code >> 6) & 0xf));
					vi_op(it, [&] { VI(is); m_f.i32_const(static_cast<s16>(imm5)); m_f.i32_add(); });
					return true;
				}
				case L::IADDIU:
				case L::ISUBIU:
					if (it == 0)
						return true;
					vi_op(it, [&] {
						VI(is);
						m_f.i32_const(imm15);
						if (p.ins.lo == L::IADDIU)
							m_f.i32_add();
						else
							m_f.i32_sub();
					});
					return true;
				case L::FCAND:
				case L::FCEQ:
				case L::FCOR:
					SetVI(1, [&] {
						m_f.local_get(m_vclip);
						m_f.i32_const(0xFFFFFF);
						m_f.i32_and();
						m_f.i32_const(static_cast<s32>(code & 0xFFFFFF));
						if (p.ins.lo == L::FCAND)
						{
							m_f.i32_and();
							m_f.i32_const(0);
							m_f.i32_ne();
						}
						else if (p.ins.lo == L::FCEQ)
							m_f.i32_eq();
						else
						{
							m_f.i32_or();
							m_f.i32_const(0xFFFFFF);
							m_f.i32_eq();
						}
					});
					m_out->native_ops++;
					return true;
				case L::DIV:
				case L::SQRT:
				case L::RSQRT:
				{
					// _vuDIV, _vuSQRT, _vuRSQRT (VUops.cpp:934-1015): vuDouble'd operands (zero for exponent 0, so
					// "== 0.0" is an exponent test), the pending status's D and I bits, q through vuDouble. Never
					// m_i[4] or m_i[5]: this may be a branch's delay slot.
					const u32 fsf = (code >> 21) & 3, ftf = (code >> 23) & 3;
					const u32 t = m_i[0], s = m_i[1], st = m_i[2];
					VFLane(ft, ftf);
					m_f.local_set(t);
					if (p.ins.lo != L::SQRT)
					{
						VFLane(fs, fsf);
						m_f.local_set(s);
					}
					m_f.local_get(m_pstat);
					m_f.i32_const(~0x30);
					m_f.i32_and();
					m_f.local_set(st);
					const auto exp_zero = [&](u32 local) {
						m_f.local_get(local);
						m_f.i32_const(static_cast<s32>(EXP));
						m_f.i32_and();
						m_f.i32_eqz();
					};
					const auto set_status = [&](u32 bits) {
						m_f.local_get(st);
						m_f.i32_const(static_cast<s32>(bits));
						m_f.i32_or();
						m_f.local_set(st);
					};
					const auto set_q = [&](auto&& value) { St32(A(R.q), value); };
					// The sign of fs ^ ft picks between two constants.
					const auto signed_const = [&](u32 negative, u32 positive) {
						set_q([&] {
							m_f.i32_const(static_cast<s32>(negative));
							m_f.i32_const(static_cast<s32>(positive));
							m_f.local_get(t);
							m_f.local_get(s);
							m_f.i32_xor();
							m_f.i32_const(0);
							m_f.i32_lt_s();
							m_f.select();
						});
					};
					// ft < 0.0 after vuDouble: the sign bit with a non-zero exponent.
					const auto negative_status = [&] {
						m_f.local_get(t);
						m_f.i32_const(0);
						m_f.i32_lt_s();
						exp_zero(t);
						m_f.i32_eqz();
						m_f.i32_and();
						m_f.if_();
						set_status(0x10);
						m_f.end();
					};
					// |vuDouble(ft)| as a float: vuDouble then the sign cleared (fabs).
					const auto abs_ft = [&] {
						m_f.local_get(t);
						ScalarDouble();
						m_f.i32_const(0x7fffffff);
						m_f.i32_and();
						m_f.f32_reinterpret_i32();
					};
					if (p.ins.lo == L::SQRT)
					{
						negative_status();
						set_q([&] {
							abs_ft();
							m_f.f32_sqrt();
							m_f.i32_reinterpret_f32();
							ScalarDouble();
						});
					}
					else
					{
						exp_zero(t);
						m_f.if_();
						if (p.ins.lo == L::DIV)
						{
							// 0x10 for 0/0, 0x20 for x/0
							m_f.i32_const(0x10);
							m_f.i32_const(0x20);
							exp_zero(s);
							m_f.select();
							m_f.local_get(st);
							m_f.i32_or();
							m_f.local_set(st);
							signed_const(0xFF7FFFFFu, 0x7F7FFFFFu);
						}
						else
						{
							set_status(0x20);
							exp_zero(s);
							m_f.if_();
							set_status(0x10);
							signed_const(0x80000000u, 0);
							m_f.else_();
							signed_const(0xFF7FFFFFu, 0x7F7FFFFFu);
							m_f.end();
						}
						m_f.else_();
						if (p.ins.lo == L::RSQRT)
							negative_status();
						set_q([&] {
							m_f.local_get(s);
							ScalarDouble();
							m_f.f32_reinterpret_i32();
							if (p.ins.lo == L::DIV)
							{
								m_f.local_get(t);
								ScalarDouble();
								m_f.f32_reinterpret_i32();
							}
							else
							{
								abs_ft();
								m_f.f32_sqrt();
							}
							m_f.f32_div();
							m_f.i32_reinterpret_f32();
							ScalarDouble();
						});
						m_f.end();
					}
					m_f.local_get(st);
					m_f.local_set(m_pstat);
					m_out->native_ops++;
					return true;
				}
				case L::WAITQ:
				case L::WAITP:
					return true; // _vuWAITQ, _vuWAITP: empty bodies, the wait is timing (the model)
				case L::XTOP:
				case L::XITOP:
					// _vuXTOP, _vuXITOP (VUops.cpp:1818-1827, 1954-1963): MTVU's copy of VIF1's TOP/ITOP.
					if (it == 0)
						return true;
					if (m_vu == 0)
					{
						HelperLower(p, delta); // VIF0's registers
						return true;
					}
					SetVI(it, [&] { Ld32(A(p.ins.lo == L::XTOP ? vu1Thread.vifRegs.top : vu1Thread.vifRegs.itop)); });
					m_out->native_ops++;
					return true;
				case L::ILW:
				case L::ILWR:
				{
					// _vuILW, _vuILWR (VUops.cpp:1239-1279): the halfword of the last lane in dest wins.
					if (it == 0 || dest == 0)
						return true;
					const u32 lane = (dest & 1) ? 3 : (dest & 2) ? 2 :
					                              (dest & 4)     ? 1 :
					                                               0;
					SetVI(it, [&] {
						if (p.ins.lo == L::ILW)
						{
							VIs(is);
							m_f.i32_const(imm11);
							m_f.i32_add();
						}
						else
							VI(is);
						qaddr();
						m_f.i32_load16_u(lane * 4);
					});
					m_out->native_ops++;
					return true;
				}
				case L::ISW:
				case L::ISWR:
				{
					// _vuISW, _vuISWR (VUops.cpp:1254-1290): the halfword and a zero halfword in each lane of dest.
					if (dest == 0)
						return true;
					if (p.ins.lo == L::ISW)
					{
						VIs(is);
						m_f.i32_const(imm11);
						m_f.i32_add();
					}
					else
						VI(is);
					qaddr();
					m_f.local_set(m_i[0]);
					for (u32 lane = 0; lane < 4; lane++)
					{
						if (!(dest & (8 >> lane)))
							continue;
						m_f.local_get(m_i[0]);
						VI(it);
						m_f.i32_store(lane * 4);
					}
					m_out->native_ops++;
					return true;
				}
				case L::MFIR:
				{
					// _vuMFIR (VUops.cpp:1100-1109): VI sign-extended into each lane of dest.
					if (ft == 0)
						return true;
					VIs(is);
					Simd(Simd::I32x4Splat);
					m_f.local_set(v);
					SetVF(ft, v, dest);
					m_out->native_ops++;
					return true;
				}
				case L::MTIR:
					// _vuMTIR (VUops.cpp:1111-1119)
					if (it == 0)
						return true;
					vi_op(it, [&] { VFLane(fs, (code >> 21) & 3); });
					return true;
				case L::MFP:
				case L::RGET:
				case L::RNEXT:
				{
					// _vuMFP, _vuRGET, _vuRNEXT (VUops.cpp:1335-1345, 1648-1657): P or R into each lane of dest;
					// RNEXT advances the LFSR first (AdvanceLFSR, VUops.cpp:1310-1318), not with Ft 0.
					if (ft == 0)
						return true;
					const u32 reg = (p.ins.lo == L::MFP) ? REG_P : REG_R;
					if (p.ins.lo == L::RNEXT)
					{
						St32(A(R.VI[REG_R]), [&] {
							const u32 r = m_i[0];
							Ld32(A(R.VI[REG_R]));
							m_f.local_tee(r);
							m_f.i32_const(1);
							m_f.i32_shl();
							m_f.local_get(r);
							m_f.i32_const(4);
							m_f.i32_shr_u();
							m_f.local_get(r);
							m_f.i32_const(22);
							m_f.i32_shr_u();
							m_f.i32_xor();
							m_f.i32_const(1);
							m_f.i32_and();
							m_f.i32_xor();
							m_f.i32_const(0x7fffff);
							m_f.i32_and();
							m_f.i32_const(0x3f800000);
							m_f.i32_or();
						});
					}
					m_f.i32_const(0);
					m_f.simd_mem(Simd::V128Load32Splat, A(R.VI[reg]), 2);
					m_f.local_set(v);
					SetVF(ft, v, dest);
					m_out->native_ops++;
					return true;
				}
				case L::RINIT:
				case L::RXOR:
					// _vuRINIT, _vuRXOR (VUops.cpp:1320-1350)
					St32(A(R.VI[REG_R]), [&] {
						VFLane(fs, (code >> 21) & 3);
						if (p.ins.lo == L::RXOR)
						{
							Ld32(A(R.VI[REG_R]));
							m_f.i32_xor();
						}
						m_f.i32_const(0x007fffff);
						m_f.i32_and();
						m_f.i32_const(0x3f800000);
						m_f.i32_or();
					});
					m_out->native_ops++;
					return true;
				case L::FSAND:
				case L::FSEQ:
				case L::FSOR:
				{
					// _vuFSAND, _vuFSEQ, _vuFSOR (VUops.cpp:1359-1393) on the committed status.
					if (it == 0)
						return true;
					const u32 imm = (((code >> 21) & 1) << 11) | (code & 0x7ff);
					SetVI(it, [&] {
						PushVstat();
						m_f.i32_const(0xFFF);
						m_f.i32_and();
						m_f.i32_const(static_cast<s32>(imm));
						if (p.ins.lo == L::FSAND)
							m_f.i32_and();
						else if (p.ins.lo == L::FSOR)
							m_f.i32_or();
						else
							m_f.i32_eq();
					});
					m_out->native_ops++;
					return true;
				}
				case L::FSSET:
				{
					// _vuFSSET (VUops.cpp:1395-1401) on the pending status.
					const u32 imm = (((code >> 21) & 1) << 11) | (code & 0x7ff);
					m_f.i32_const(static_cast<s32>(imm & 0xFC0));
					m_f.local_get(m_pstat);
					m_f.i32_const(0x3F);
					m_f.i32_and();
					m_f.i32_or();
					m_f.local_set(m_pstat);
					m_out->native_ops++;
					return true;
				}
				case L::FMAND:
				case L::FMEQ:
				case L::FMOR:
					// _vuFMAND, _vuFMEQ, _vuFMOR (VUops.cpp:1403-1427) on the committed MAC flag.
					if (it == 0)
						return true;
					SetVI(it, [&] {
						PushMac(m_lr, m_vmac);
						m_f.i32_const(0xFFFF);
						m_f.i32_and();
						VI(is);
						if (p.ins.lo == L::FMAND)
							m_f.i32_and();
						else if (p.ins.lo == L::FMOR)
							m_f.i32_or();
						else
							m_f.i32_eq();
					});
					m_out->native_ops++;
					return true;
				case L::FCSET:
					// _vuFCSET (VUops.cpp:1454-1457) on the pending clip flag.
					m_f.i32_const(static_cast<s32>(code & 0xFFFFFF));
					m_f.local_set(m_pclip);
					m_out->native_ops++;
					return true;
				case L::XGKICK:
				{
					// _vuXGKICK (VUops.cpp:1932-1952). The busy-kick exit before this instruction (EmitInstruction)
					// leaves while a kick runs, so none does here and nothing is flushed. MTVU: no VPU_STAT.
					if (m_vu == 0)
					{
						HelperLower(p, delta);
						return true;
					}
					const u32 addr = m_i[0];
					VI(is);
					m_f.i32_const(0x3ff);
					m_f.i32_and();
					m_f.i32_const(4);
					m_f.i32_shl();
					m_f.local_set(addr);
					St32c(A(R.xgkickenable), 1);
					m_f.i32_const(1);
					m_f.local_set(m_kick);
					St32(A(R.xgkickaddr), [&] { m_f.local_get(addr); });
					St32(A(R.xgkickdiff), [&] {
						m_f.i32_const(0x4000);
						m_f.local_get(addr);
						m_f.i32_sub();
					});
					St32c(A(R.xgkicksizeremaining), 0);
					St32c(A(R.xgkickendpacket), 0);
					m_f.i32_const(0);
					m_f.i32_const(0);
					m_f.i64_load(A(g_block_base));
					m_f.i64_const(delta);
					m_f.i64_add();
					m_f.i64_store(A(R.xgkicklastcycle));
					St32c(A(R.xgkickcyclecount), 1);
					m_out->native_ops++;
					return true;
				}
				case L::FCGET:
					if (it == 0)
						return true;
					SetVI(it, [&] { m_f.local_get(m_vclip); m_f.i32_const(0x0FFF); m_f.i32_and(); });
					m_out->native_ops++;
					return true;
				case L::B:
					m_f.i32_const(1);
					m_f.local_set(m_i[4]);
					m_out->native_ops++;
					return true;
				case L::BAL:
					if (it && p.pre_ctl.branch == 1)
					{
						// In the delay slot of a branch (PlanBlock): if that branch is taken, the link is the instruction
						// after its target's first (_vuBAL, VUops.cpp:1611-1619). Its condition is in m_cond1.
						SetVI(it, [&] {
							m_f.i32_const(static_cast<s32>((p.pre_ctl.branchpc + 8) / 8));
							m_f.i32_const(static_cast<s32>((p.pc + 16) / 8));
							m_f.local_get(m_cond1);
							m_f.select();
						});
					}
					else if (it)
						SetVI(it, [&] { m_f.i32_const(static_cast<s32>((p.pc + 16) / 8)); });
					m_f.i32_const(1);
					m_f.local_set(m_i[4]);
					m_out->native_ops++;
					return true;
				case L::JR:
				case L::JALR:
					VI(is);
					m_f.i32_const(3);
					m_f.i32_shl();
					m_f.local_set(m_i[5]);
					if (p.ins.lo == L::JALR && it)
						SetVI(it, [&] { m_f.i32_const(static_cast<s32>((p.pc + 16) / 8)); });
					m_out->native_ops++;
					return true;
				case L::IBEQ:
				case L::IBNE:
				case L::IBLTZ:
				case L::IBGTZ:
				case L::IBLEZ:
				case L::IBGEZ:
				{
					// _vuIBxx (VUops.cpp:1489-1600): s16 compares, VIOldValue in the VI branch delay window.
					const bool window = p.info.vi_backup_cycles > 0;
					const auto src = [&](u32 reg) {
						if (window && p.info.vi_backup_reg == reg)
							Ld16s(A(R.VIOldValue));
						else
							VIs(reg);
					};
					if (p.ins.lo == L::IBEQ || p.ins.lo == L::IBNE)
					{
						src(it);
						src(is);
						if (p.ins.lo == L::IBEQ)
							m_f.i32_eq();
						else
							m_f.i32_ne();
					}
					else
					{
						src(is);
						m_f.i32_const(0);
						switch (p.ins.lo)
						{
							case L::IBLTZ: m_f.i32_lt_s(); break;
							case L::IBGTZ: m_f.i32_gt_s(); break;
							case L::IBLEZ: m_f.i32_le_s(); break;
							default: m_f.i32_ge_s(); break;
						}
					}
					m_f.local_set(m_i[4]);
					m_out->native_ops++;
					return true;
				}
				default:
					HelperLower(p, delta);
					return true;
			}
		}

		void Emitter::EmitVu0MappingTest(const Plan& p)
		{
			// The address is the qword index times 16 (_vuLQ to _vuISWR, VUops.cpp:1156-1290): bit 14 is bit 10 of the
			// index, before the op changes its register (LQD and SQD decrement it first).
			const u32 code = p.ins.lower;
			const u32 ft = (code >> 16) & 0x1f, fs = (code >> 11) & 0x1f;
			const u32 it = ft & 0xf, is = fs & 0xf;
			const s32 imm11 = (code & 0x400) ? static_cast<s32>(0xfffffc00u | (code & 0x3ff)) : static_cast<s32>(code & 0x3ff);
			switch (p.ins.lo)
			{
				case L::LQ:
				case L::ILW:
				case L::ISW:
					VI(is);
					m_f.i32_const(imm11);
					m_f.i32_add();
					break;
				case L::SQ:
					VI(it);
					m_f.i32_const(imm11);
					m_f.i32_add();
					break;
				case L::LQI:
				case L::ILWR:
				case L::ISWR:
					VI(is);
					break;
				case L::SQI:
					VI(it);
					break;
				case L::LQD:
					VI(is);
					if (is != 0)
					{
						m_f.i32_const(1);
						m_f.i32_sub();
					}
					break;
				case L::SQD:
					VI(it);
					if (ft != 0)
					{
						m_f.i32_const(1);
						m_f.i32_sub();
					}
					break;
				default:
					pxFailRel("VU0 JIT: not a memory op");
			}
			m_f.i32_const(0x400);
			m_f.i32_and();
		}

		void Emitter::EmitInstruction(const Plan& p, s32 busy_kick_exit, s32 busy_kick_untaken, bool last)
		{
			const StepInfo& info = p.info;
			const u32 delta = p.cyc + info.op_cycle; // R.cycle at the op, relative to the entry

			// The kick is fed only where the result can differ from feeding it at every instruction (section 7.7):
			// before VU memory changes, at an XGKICK, before op bodies, and at the last instruction before any exit.
			// Each feed adds the cycles since the previous one, so the bytes and tags the GS receives are the same.
			// The block's last instruction is fed by the exits after it (m_exit_kick), at its own cycle, which is the
			// same unless its ops write VU memory or start a kick; going to the next block feeds nothing.
			if (m_vu && busy_kick_exit >= 0)
			{
				// A kick still running at an XGKICK, once fed up to this cycle: the interpreter takes the instruction,
				// whose own feed then adds nothing (section 7.7). In the delay slot of a conditional branch, the exit
				// depends on whether it was taken (m_i[4]).
				KickCatchUp(delta);
				m_f.local_get(m_kick);
				m_f.if_unlikely();
				if (busy_kick_untaken >= 0)
				{
					m_f.local_get(m_i[4]);
					m_f.if_();
					EmitExit(static_cast<u32>(busy_kick_exit));
					m_f.else_();
					EmitExit(static_cast<u32>(busy_kick_untaken));
					m_f.end();
				}
				else
					EmitExit(static_cast<u32>(busy_kick_exit));
				m_f.end();
			}
			if (!m_vu && busy_kick_exit >= 0)
			{
				// VU1's registers through VU0's 0x4000 mapping: the interpreter takes the instruction (GET_VU_MEM waits
				// for the VU thread there).
				EmitVu0MappingTest(p);
				m_f.if_unlikely();
				EmitExit(static_cast<u32>(busy_kick_exit)); // never in a delay slot (Vu0MemoryNative)
				m_f.end();
			}

			Flushes(p);
			const bool fed = busy_kick_exit >= 0 || (!info.ibit && (GetLowerFlags(p.ins.lo) & (LOWER_STORE | LOWER_XGKICK)));
			if (m_vu && busy_kick_exit < 0 && fed)
				KickCatchUp(delta);
			if (m_vu && last && !fed)
				m_exit_kick = static_cast<s32>(delta); // from here on, EmitExit feeds it

			if (info.ibit)
			{
				Upper(p, delta);
				St32c(A(R.VI[REG_I]), p.ins.lower);
			}
			else
			{
				// The upper op's result is hidden from the lower op (VU1microInterp.cpp:130-185).
				const u32 old_vf = info.old_vf;
				const bool lower = info.lower_runs;
				if (lower && old_vf)
				{
					VF(old_vf);
					m_f.local_set(m_hs);
				}
				if (lower && info.old_clip)
				{
					m_f.local_get(m_vclip);
					m_f.local_set(m_hc_old);
				}
				Upper(p, delta);
				if (lower)
				{
					if (old_vf)
					{
						VF(old_vf);
						m_f.local_set(m_hn);
						m_use.vf_written |= u64(1) << old_vf; // the lower op's helper must find the old value in memory
						m_f.local_get(m_hs);
						m_f.local_set(VFLocal(old_vf));
					}
					if (info.old_clip)
					{
						m_f.local_get(m_vclip);
						m_f.local_set(m_hc_new);
						m_f.local_get(m_hc_old);
						m_f.local_set(m_vclip);
					}
					Lower(p, delta);
					if (old_vf)
					{
						m_f.local_get(m_hn);
						m_f.local_set(VFLocal(old_vf));
					}
					if (info.old_clip)
					{
						m_f.local_get(m_hc_new);
						m_f.local_set(m_vclip);
					}
				}
			}

			// Pipe entries (VU1microInterp.cpp:187-192): their value snapshots, taken after both ops.
			if (info.fmac_added)
			{
				const u32* e = m_snap[p.pre.fmacwritepos];
				const u32 from[3] = {m_pmac, m_pstat, m_pclip};
				for (u32 j = 0; j < 3; j++)
				{
					m_f.local_get(from[j]);
					m_f.local_set(e[j]);
				}
				m_f.local_get(m_pr);
				m_f.local_set(m_snapr[p.pre.fmacwritepos]);
			}
			if (info.fdiv_added)
			{
				St32(A(R.fdiv.reg), [&] { Ld32(A(R.q)); });
				St32(A(R.fdiv.statusflag), [&] { PushStat(m_pr, m_pstat); });
			}
			if (info.efu_added)
				St32(A(R.efu.reg), [&] { Ld32(A(R.p)); });
		}

		void Emitter::EmitBranchEnd(const Plan& branch, u32 exit_taken, u32 exit_fall, u32 exit_dyn, bool loop, u32 last_start,
			s32 next_taken, s32 next_fall, const std::vector<std::pair<u32, s32>>& jumps)
		{
			const u16 flags = GetLowerFlags(branch.ins.lo);
			if (flags & LOWER_DYNAMIC)
			{
				// TPC is the target as the interpreter leaves it (VI * 8, unmasked): only an exact match goes to a block
				// of the module, whose exits write TPC again.
				St32(A(R.VI[REG_TPC]), [&] { m_f.local_get(m_i[5]); });
				for (const auto& [target, next] : jumps)
				{
					m_f.local_get(m_i[5]);
					m_f.i32_const(static_cast<s32>(target));
					m_f.i32_eq();
					m_f.if_();
					EmitEdge(exit_dyn, static_cast<u32>(next));
					m_f.end();
				}
				EmitExit(exit_dyn);
			}
			else if (flags & LOWER_COND)
			{
				m_f.local_get(m_i[4]);
				m_f.if_();
				if (loop)
					EmitLoopBack(exit_taken, last_start);
				else
					EmitEdgeOrExit(exit_taken, next_taken);
				m_f.end();
				EmitEdgeOrExit(exit_fall, next_fall);
			}
			else if (loop)
				EmitLoopBack(exit_taken, last_start);
			else
				EmitEdgeOrExit(exit_taken, next_taken);
		}

		// Blocks of a module going to each other inside generated code (EmitEdge). Off with VU1JIT_NOCHAIN, to bisect
		// a difference: every block then is a module of its own, as before modules held several.
		static bool ChainEnabled()
		{
			static const bool enabled = !std::getenv("VU1JIT_NOCHAIN");
			return enabled;
		}

		// Caps per module (section 4.3): its compile time grows with its size.
		static constexpr u32 MODULE_BLOCKS = 1024;
		static constexpr u32 MODULE_INSTRS = 8192;
		// VU0: a module from every entry, each holding all it reaches (no ModuleSource::known), so smaller ones. Its micro
		// memory holds 512 instructions; random code (the fuzzer) reaches the same blocks under many signatures.
		static constexpr u32 MODULE_BLOCKS0 = 64;
		static constexpr u32 MODULE_INSTRS0 = 1024;

		// One block: the timing model's plan for its instructions, and how it ends.
		struct BlockPlan
		{
			u32 pc = 0;
			Signature sig = {};
			std::vector<Plan> plans;
			bool stop_before = false; // the last planned instruction is followed by one generated code does not run
			bool ended = false;
			bool branch_end = false;
			bool wrapped = false;
			bool mbit_end = false;
			int end_flagreg[4] = {}; // the model after the program end, for _vuFlushAll's write-backs
			PipeImage stop_img = {};
			Control stop_ctl;
			u32 stop_pc = 0;
			PipeImage final_img = {};
			Control final_ctl;
			s32 efu_entry_cycle = 0;
			bool efu_at_entry = false;
			bool loop = false; // the taken branch comes back to the entry with its signature: a loop in generated code
			BlockExit loop_exit;
			// The exits that may go to another block of the module: a static branch target, the fall-through and a
			// JR/JALR, and the block of the module each goes to (an index, -1 for the dispatcher).
			BlockExit taken, fall, dyn;
			bool has_taken = false;
			bool has_fall = false;
			bool has_dyn = false;
			s32 next_taken = -1;
			s32 next_fall = -1;
			std::vector<std::pair<u32, s32>> jumps; // learned JR/JALR targets (TPC) that are blocks of the module
			// A block entered in a branch delay slot (from the module only): its one instruction, then the branch to
			// ds_target (the taken exit).
			bool ds = false;
			u32 ds_target = 0;
			// A block that ends with a branch in the delay slot of the branch before it (the interpreter's
			// takedelaybranch): its exits for both taken, the first only, the second only and neither (bds_has: which
			// can happen), and the blocks of the module they go to.
			bool bds = false;
			bool bds_cond1 = false;
			bool bds_cond2 = false;
			BlockExit bds_exits[4];
			bool bds_has[4] = {};
			s32 bds_next[4] = {-1, -1, -1, -1};

			u32 Instrs() const { return static_cast<u32>(plans.size()); }
			u32 LastStart() const { return plans.back().cyc; }
		};

		BlockExit MakeExit(int vu, const BlockPlan& b, ExitKind kind, ExitReason reason, u32 instrs, const PipeImage& img,
			const Control& ctl, u32 tpc)
		{
			BlockExit x;
			x.kind = kind;
			x.reason = reason;
			x.instrs = instrs;
			x.last_cyc = instrs > 0 ? b.plans[instrs - 1].cyc : 0;
			x.tpc = tpc;
			x.ebit = ctl.ebit;
			x.pipes = img;
			x.block_pc = b.pc;
			x.mbit = vu == 0 && instrs > 0 && b.plans[instrs - 1].ins.MBit(); // set at its fetch (VU0microInterp.cpp:40)
			if (b.efu_at_entry && img.efu.enable && img.efu.sCycle < TIMING_MODEL_BASE)
				x.efu_cycle_adjust = static_cast<s32>(img.efu.Cycle) - b.efu_entry_cycle;
			if (kind == ExitKind::Boundary && ctl.ebit == 0 && ctl.branch == 0)
			{
				VURegs tmp;
				std::memset(static_cast<void*>(&tmp), 0, sizeof(tmp));
				std::memcpy(tmp.fmac, img.fmac, sizeof(tmp.fmac));
				tmp.fmacreadpos = img.fmacreadpos;
				tmp.fmacwritepos = img.fmacwritepos;
				tmp.fmaccount = img.fmaccount;
				tmp.fdiv = img.fdiv;
				tmp.efu = img.efu;
				std::memcpy(tmp.ialu, img.ialu, sizeof(tmp.ialu));
				tmp.ialureadpos = img.ialureadpos;
				tmp.ialuwritepos = img.ialuwritepos;
				tmp.ialucount = img.ialucount;
				tmp.VIBackupCycles = img.VIBackupCycles;
				tmp.VIRegNumber = img.VIRegNumber;
				tmp.cycle = img.cycle;
				x.chain = DeriveSignature(tmp, &x.sig) == Refusal::None;
			}
			return x;
		}

		// Whether generated code may go from exit x to the block at its TPC itself: the dispatcher would enter that
		// block next with x's signature and no branch or E bit pending. A TPC past the end of micro memory is left to
		// the dispatcher, which masks it (the interpreter keeps it unmasked until the next fetch).
		bool CanChain(int vu, const BlockExit& x)
		{
			return x.kind == ExitKind::Boundary && (x.reason == ExitReason::Block || x.reason == ExitReason::Cap) && x.chain &&
			       !x.dynamic_tpc && x.ebit == 0 && x.tpc < (vu ? VU1_PROGSIZE : VU0_PROGSIZE) && !(vu == 0 && x.mbit);
		}

		// A static branch in the delay slot of a static branch compiled (VU1). Off with VU1JIT_NOBDS, to bisect.
		static bool BranchInDelayEnabled()
		{
			static const bool enabled = !std::getenv("VU1JIT_NOBDS");
			return enabled;
		}

		// Runs the timing model over the block at pc for an entry with signature sig and no branch or E bit pending,
		// or (delay_slot) in the delay slot of a branch to delay_slot->branchpc. False when there is nothing to
		// compile: the first instruction must run on the interpreter.
		bool PlanBlock(int vu, u32 pc, const Signature& sig, BlockPlan* out, const Control* delay_slot = nullptr)
		{
			BlockPlan& b = *out;
			b = {};
			b.pc = pc;
			b.sig = sig;
			b.ds = delay_slot != nullptr;
			b.ds_target = delay_slot ? delay_slot->branchpc : 0;
			VURegs& R = vu ? VU1 : VU0;
			const u32 progsize = vu ? VU1_PROGSIZE : VU0_PROGSIZE;
			TimingModel model(vu);
			model.Load(sig, delay_slot ? *delay_slot : Control{});
			b.efu_entry_cycle = static_cast<s32>(model.State().efu.Cycle);
			b.efu_at_entry = model.State().efu.enable != 0;

			std::vector<Plan>& plans = b.plans;
			plans.reserve(64);
			u32 cur = pc;
			u32 cyc = 0;
			for (;;)
			{
				Plan p;
				p.pc = cur;
				const u32* words = reinterpret_cast<const u32*>(&R.Micro[cur]);
				p.ins = Decode(words[0], words[1]);
				p.pre_ctl = model.GetControl();
				p.pre = CaptureImage(model.State());
				p.cyc = cyc;
				// A branch or an XGKICK in this delay slot: the branch before it (the one that set it) must be static.
				bool after_static_branch = false;
				if (vu == 1 && BranchInDelayEnabled() && p.pre_ctl.branch == 1 && !plans.empty())
				{
					const Instruction& prev = plans.back().ins;
					after_static_branch = !prev.IBit() && IsBranch(prev.lo) && !(GetLowerFlags(prev.lo) & LOWER_DYNAMIC);
				}
				if (!Allowed(p, vu, after_static_branch))
				{
					if (plans.empty())
						return false; // a delay slot entry whose instruction the interpreter runs
					if (p.pre_ctl.branch != 0)
					{
						// Not in a branch delay slot: stop before the branch.
						const Plan& br = plans.back();
						b.stop_img = br.pre;
						b.stop_ctl = br.pre_ctl;
						b.stop_pc = br.pc;
						plans.pop_back();
					}
					else
					{
						b.stop_img = p.pre;
						b.stop_ctl = p.pre_ctl;
						b.stop_pc = p.pc;
					}
					b.stop_before = true;
					break;
				}
				StepInput in;
				in.pc = cur;
				in.lower = words[0];
				in.upper = words[1];
				in.fbrst = 0; // no D/T bits here
				in.branch_taken = true;
				in.jump_target = 0;
				if (!model.Step(in, &p.info))
					return false;
				cyc += p.info.cycles;
				plans.push_back(p);
				if (p.info.end)
				{
					b.ended = true;
					for (u32 i = 0; i < 4; i++)
						b.end_flagreg[i] = model.State().fmac[i].flagreg;
					break;
				}
				if (p.pre_ctl.branch == 1)
				{
					b.branch_end = true; // the delay slot of the previous instruction
					break;
				}
				if (vu == 0 && p.ins.MBit())
				{
					b.mbit_end = true; // InterpVU0::Execute stops after it (VU0microInterp.cpp:268)
					cur += 8;
					break;
				}
				cur += 8;
				if (cur >= progsize)
				{
					b.wrapped = true;
					break;
				}
				if (plans.size() >= BLOCK_CAP && model.GetControl().branch == 0)
					break;
			}
			if (plans.empty())
				return false;

			b.final_img = CaptureImage(model.State());
			b.final_ctl = model.GetControl();
			const u32 n = b.Instrs();
			const Plan& last = plans.back();
			if (b.ds)
			{
				// The delay slot ran: the branch goes to its target (VU1microInterp.cpp:194-199).
				b.taken = MakeExit(vu, b, ExitKind::Boundary, ExitReason::Block, n, b.final_img, b.final_ctl, b.ds_target);
				b.has_taken = true;
			}
			else if (b.branch_end && !last.ins.IBit() && IsBranch(last.ins.lo))
			{
				// A branch in the delay slot of the branch before it (_setBranch, VUops.cpp:1473-1477, and the step's
				// end, VU1microInterp.cpp:194-206). If the first is taken, the second's delay slot is the first's target
				// (one instruction, then the second's target if it is taken too); if not, the second is an ordinary
				// branch. The timing does not depend on which: the model ran it with both taken.
				const Plan& b1 = plans[n - 2];
				b.bds = true;
				b.bds_cond1 = (GetLowerFlags(b1.ins.lo) & LOWER_COND) != 0;
				b.bds_cond2 = (GetLowerFlags(last.ins.lo) & LOWER_COND) != 0;
				const u32 t1 = StaticBranchTarget(b1.pc, b1.ins.lower) & (progsize - 1);
				const u32 t2 = StaticBranchTarget(last.pc, last.ins.lower) & (progsize - 1);
				Control clean = b.final_ctl;
				clean.branch = 0;
				clean.branchpc = 0;
				clean.takedelaybranch = false;
				clean.delaybranchpc = 0;
				const auto make = [&](u32 tpc, bool pending) {
					BlockExit x = MakeExit(vu, b, ExitKind::Boundary, ExitReason::Block, n, b.final_img, clean, tpc);
					if (pending)
					{
						x.branch = 1;
						x.branchpc = t2;
						x.sig_valid = x.chain;
						x.chain = false; // the dispatcher steps the delay slot on the interpreter
					}
					return x;
				};
				b.bds_exits[0] = make(t1, true);
				b.bds_has[0] = true;
				b.bds_exits[1] = make(t1, false);
				b.bds_has[1] = b.bds_cond2;
				b.bds_exits[2] = make(last.pc + 8, true);
				b.bds_has[2] = b.bds_cond1;
				b.bds_exits[3] = make(last.pc + 8, false);
				b.bds_has[3] = b.bds_cond1 && b.bds_cond2;
			}
			else if (b.branch_end)
			{
				// A branch's exits, and whether the taken one comes back to this block with the same signature: a loop
				// generated code runs by itself.
				const Plan& branch = plans[n - 2];
				const u16 flags = GetLowerFlags(branch.ins.lo);
				if (!(flags & LOWER_DYNAMIC))
				{
					b.taken = MakeExit(vu, b, ExitKind::Boundary, ExitReason::Block, n, b.final_img, b.final_ctl,
						StaticBranchTarget(branch.pc, branch.ins.lower) & (progsize - 1));
					b.has_taken = true;
					const BlockExit& t = b.taken;
					b.loop = vu == 1 && LoopsEnabled() && t.chain && t.tpc == pc && t.sig == sig && OnlyNewEntries(t.pipes);
					if (b.loop)
						b.loop_exit = t;
				}
				if (flags & LOWER_COND)
				{
					b.fall = MakeExit(vu, b, ExitKind::Boundary, ExitReason::Block, n, b.final_img, b.final_ctl, last.pc + 8);
					b.has_fall = true;
				}
				if (flags & LOWER_DYNAMIC)
				{
					b.dyn = MakeExit(vu, b, ExitKind::Boundary, ExitReason::Block, n, b.final_img, b.final_ctl, 0);
					b.has_dyn = true;
				}
			}
			else if (!b.ended && !b.stop_before)
			{
				b.fall = MakeExit(vu, b, ExitKind::Boundary, (b.wrapped || b.mbit_end) ? ExitReason::Block : ExitReason::Cap, n,
					b.final_img, b.final_ctl, last.pc + 8);
				b.has_fall = true;
			}
			return true;
		}

		// Block k of the module (layout order); pos maps BlockPlan::next_* to layout positions. Exits are numbered in
		// the order this makes them, the same in both emissions.
		// VU0 slices: an instruction with no branch or E bit pending before it may be left before (a delay slot may not:
		// the interpreter stops before it with the branch or the end pending, which an exit does not describe).
		bool SliceExitable(const Plan& p)
		{
			return p.pre_ctl.branch == 0 && p.pre_ctl.ebit == 0;
		}

		// The cycles left at the entry at or below which generated code leaves before instruction i: when it would start
		// past the slice; or, when delay slots follow it (a branch's, an E bit's, an E bit in an E bit's delay slot),
		// when the last of them would: the interpreter may then stop among them, so it runs instruction i and those after
		// it itself (the dispatcher steps them).
		u32 SliceThreshold(const BlockPlan& b, u32 i)
		{
			u32 j = i + 1;
			while (j < b.Instrs() && !SliceExitable(b.plans[j]))
				j++;
			return b.plans[j - 1].cyc;
		}

		void EmitBlock(Emitter& em, int vu, const BlockPlan& b, u32 k, const std::vector<s32>& pos, std::vector<BlockExit>& exits)
		{
			const auto add = [&](const BlockExit& x) {
				exits.push_back(x);
				exits.back().block = k;
				return static_cast<u32>(exits.size() - 1);
			};
			const auto next = [&](s32 i) { return i >= 0 ? pos[i] : -1; };
			const u32 progsize = vu ? VU1_PROGSIZE : VU0_PROGSIZE;
			const u32 n = b.Instrs();
			const Plan& last = b.plans.back();
			em.BeginBlock(k, n);
			if (b.loop)
				em.BeginLoop(b.loop_exit);
			for (u32 i = 0; i < n; i++)
			{
				const Plan& p = b.plans[i];
				// VU0 runs in slices of the EE's cycles (InterpVU0::Execute steps while VU0.cycle - start < cycles): leave
				// before the first instruction that would start past the slice, as the interpreter stops there.
				if (vu == 0 && g_vu0_slices && i > 0 && SliceExitable(p))
					em.EmitBudgetExit(SliceThreshold(b, i),
						add(MakeExit(vu, b, ExitKind::Boundary, ExitReason::Budget, i, p.pre, p.pre_ctl, p.pc)));
				s32 busy = -1, busy_untaken = -1;
				const bool vu0_check = vu == 0 && Vu0MemoryCheck(p);
				if ((vu && !p.ins.IBit() && p.ins.lo == L::XGKICK && !p.info.discarded) || vu0_check)
				{
					// The interpreter's state before this instruction: TPC is its own address. In a branch's delay slot
					// the branch is pending if it was taken (busy), not if a conditional one was not (busy_untaken).
					BlockExit x = MakeExit(vu, b, ExitKind::Boundary, vu0_check ? ExitReason::Fallback : ExitReason::BusyKick, i,
						p.pre, p.pre_ctl, p.pc);
					if (p.pre_ctl.branch == 1)
					{
						x.branch = 1;
						x.branchpc = p.pre_ctl.branchpc;
					}
					busy = static_cast<s32>(add(x));
					if (p.pre_ctl.branch == 1 && (GetLowerFlags(b.plans[i - 1].ins.lo) & LOWER_COND))
					{
						x.branch = 0;
						x.branchpc = 0;
						busy_untaken = static_cast<s32>(add(x));
					}
				}
				em.EmitInstruction(p, busy, busy_untaken, i + 1 == n);
				if (i + 1 == n && b.ended)
					em.FlushAllAtEnd(p, b.end_flagreg);
				if (i + 2 == n && b.bds)
					em.SaveBranchCondition();
			}

			if (b.ended)
			{
				em.EmitExit(add(MakeExit(vu, b, ExitKind::End, ExitReason::Block, n, b.final_img, b.final_ctl, last.pc + 8)));
			}
			else if (b.ds)
			{
				em.EmitEdgeOrExit(add(b.taken), next(b.next_taken));
			}
			else if (b.bds)
			{
				s32 ids[4], nexts[4];
				for (u32 j = 0; j < 4; j++)
				{
					ids[j] = b.bds_has[j] ? static_cast<s32>(add(b.bds_exits[j])) : -1;
					nexts[j] = b.bds_has[j] ? next(b.bds_next[j]) : -1;
				}
				em.EmitBranchInDelayEnd(b.bds_cond1, b.bds_cond2, ids, nexts);
			}
			else if (b.branch_end)
			{
				const Plan& branch = b.plans[n - 2];
				const u32 exit_taken = add(MakeExit(vu, b, ExitKind::Boundary, ExitReason::Block, n, b.final_img, b.final_ctl,
					StaticBranchTarget(branch.pc, branch.ins.lower) & (progsize - 1)));
				const u32 exit_fall = add(MakeExit(vu, b, ExitKind::Boundary, ExitReason::Block, n, b.final_img, b.final_ctl, last.pc + 8));
				BlockExit dyn = MakeExit(vu, b, ExitKind::Boundary, ExitReason::Block, n, b.final_img, b.final_ctl, 0);
				dyn.dynamic_tpc = true;
				const u32 exit_dyn = add(dyn);
				std::vector<std::pair<u32, s32>> jumps;
				for (const auto& [target, i] : b.jumps)
					jumps.emplace_back(target, next(i));
				em.EmitBranchEnd(branch, exit_taken, exit_fall, exit_dyn, b.loop, last.cyc, next(b.next_taken), next(b.next_fall),
					jumps);
			}
			else if (b.stop_before)
			{
				em.EmitExit(add(MakeExit(vu, b, ExitKind::Boundary, ExitReason::Fallback, n, b.stop_img, b.stop_ctl, b.stop_pc)));
			}
			else
			{
				em.EmitEdgeOrExit(add(b.fall), next(b.next_fall));
			}
			if (b.loop)
				em.EndLoop();
		}
	} // namespace

	bool CompileBlock(int vu, u32 pc, const Signature& sig, CompiledBlock* out)
	{
		*out = {};
		BlockPlan b;
		if (!PlanBlock(vu, pc, sig, &b))
			return false;
		const std::vector<s32> pos{0};
		const std::vector<u32> last_starts{b.LastStart()};

		// Emitted twice: the first time records which registers the native code uses (RegUse), the second keeps
		// them in locals. Both make the same exits.
		const auto emit = [&](Emitter& em) {
			out->exits.clear();
			em.EmitPrologue();
			em.BeginModule(last_starts);
			EmitBlock(em, vu, b, 0, pos, out->exits);
			em.EndModule();
		};
		RegUse use;
		{
			Emitter first(out, vu, nullptr);
			emit(first);
			use = first.Use();
		}
		out->native_ops = 0;
		out->helper_ops = 0;
		Emitter em(out, vu, &use);
		emit(em);
		em.Finish();
		out->instrs = b.Instrs();
		out->last_start = b.LastStart();
		out->first_need = SliceThreshold(b, 0);
		out->loops = b.loop;
		return true;
	}

	bool CompileModule(int vu, u32 pc, const Signature& sig, const ModuleSource& source, CompiledModule* out)
	{
		*out = {};
		const u32 default_blocks = vu ? MODULE_BLOCKS : MODULE_BLOCKS0;
		const u32 max_blocks = source.block_limit ? std::min(source.block_limit, default_blocks) : default_blocks;
		const u32 max_instrs = vu ? MODULE_INSTRS : MODULE_INSTRS0;
		std::vector<BlockPlan> blocks(1);
		if (!PlanBlock(vu, pc, sig, &blocks[0]))
			return false;
		u32 total = blocks[0].Instrs();
		for (const auto& [rpc, rsig] : source.roots)
		{
			bool have = false;
			for (const BlockPlan& b : blocks)
				have |= !b.ds && b.pc == rpc && b.sig == rsig;
			BlockPlan nb;
			if (have || blocks.size() >= max_blocks || !PlanBlock(vu, rpc, rsig, &nb) || total + nb.Instrs() > max_instrs)
				continue;
			total += nb.Instrs();
			blocks.push_back(std::move(nb));
		}
		const size_t roots = blocks.size();

		// Discovery (section 4.3): the blocks the exits go to, breadth first, unless another module holds them (when
		// source.known says so) or there is nothing to compile there.
		// A block is (PC, signature) and, for one entered in a branch delay slot, the branch's target.
		struct Key
		{
			u32 pc;
			Signature sig;
			bool ds;
			u32 ds_target;
		};
		std::vector<Key> refused;
		const bool chain = ChainEnabled();
		const auto successor = [&](const BlockExit& x) -> s32 {
			const bool ds = x.branch != 0;
			if (!chain || (ds && vu == 0))
				return -1;
			if (ds ? !(x.kind == ExitKind::Boundary && x.reason == ExitReason::Block && x.sig_valid && x.ebit == 0 &&
						 x.tpc < VU1_PROGSIZE) :
					 !CanChain(vu, x))
				return -1;
			const u32 ds_target = ds ? x.branchpc : 0;
			for (size_t i = 0; i < blocks.size(); i++)
			{
				if (blocks[i].pc == x.tpc && blocks[i].sig == x.sig && blocks[i].ds == ds && blocks[i].ds_target == ds_target)
					return static_cast<s32>(i);
			}
			for (const Key& r : refused)
			{
				if (r.pc == x.tpc && r.sig == x.sig && r.ds == ds && r.ds_target == ds_target)
					return -1;
			}
			if (blocks.size() >= max_blocks || (!ds && source.known && source.known(source.ctx, x.tpc, x.sig)))
				return -1;
			Control delay_slot;
			delay_slot.branch = 1;
			delay_slot.branchpc = ds_target;
			BlockPlan nb;
			if (!PlanBlock(vu, x.tpc, x.sig, &nb, ds ? &delay_slot : nullptr) || total + nb.Instrs() > max_instrs)
			{
				refused.push_back(Key{x.tpc, x.sig, ds, ds_target});
				return -1;
			}
			total += nb.Instrs();
			blocks.push_back(std::move(nb));
			return static_cast<s32>(blocks.size() - 1);
		};
		for (size_t i = 0; i < blocks.size(); i++)
		{
			for (u32 j = 0; j < 4; j++)
			{
				if (!blocks[i].bds_has[j])
					continue;
				const BlockExit x = blocks[i].bds_exits[j];
				const s32 t = successor(x);
				blocks[i].bds_next[j] = t;
			}
			if (blocks[i].has_taken && !blocks[i].loop)
			{
				const BlockExit x = blocks[i].taken;
				const s32 t = successor(x);
				blocks[i].next_taken = t;
			}
			if (blocks[i].has_fall)
			{
				const BlockExit x = blocks[i].fall;
				const s32 t = successor(x);
				blocks[i].next_fall = t;
			}
			const std::vector<u32>* targets =
				(blocks[i].has_dyn && source.jump_targets) ? source.jump_targets(source.ctx, blocks[i].pc, blocks[i].sig) : nullptr;
			if (targets)
			{
				// The targets the JR/JALR went to so far, as static exits: the jump's exit state with that TPC.
				const std::vector<u32> copy = *targets;
				for (const u32 target : copy)
				{
					BlockExit x = blocks[i].dyn;
					x.tpc = target;
					const s32 t = successor(x);
					if (t >= 0)
						blocks[i].jumps.emplace_back(target, t);
				}
			}
		}

		// Layout: reverse postorder from the entry, the taken branch visited first, so that most exits go forward
		// (a br) and a fall-through usually goes to the next block; then the same from each other root.
		const u32 nb = static_cast<u32>(blocks.size());
		std::vector<u32> order;
		order.reserve(nb);
		{
			std::vector<u8> seen(nb, 0);
			for (size_t r = 0; r < roots; r++)
			{
				if (seen[r])
					continue;
				std::vector<u32> post;
				std::vector<std::pair<u32, u32>> stack; // block, successors visited
				stack.emplace_back(static_cast<u32>(r), 0);
				seen[r] = 1;
				while (!stack.empty())
				{
					const u32 v = stack.back().first;
					const u32 c = stack.back().second;
					if (c < 6 + blocks[v].jumps.size())
					{
						stack.back().second++;
						const BlockPlan& b = blocks[v];
						const s32 s = c == 0 ? b.next_taken : c == 1 ? b.next_fall :
						                                  c < 6      ? b.bds_next[c - 2] :
						                                               b.jumps[c - 6].second;
						if (s >= 0 && !seen[s])
						{
							seen[s] = 1;
							stack.emplace_back(static_cast<u32>(s), 0);
						}
						continue;
					}
					post.push_back(v);
					stack.pop_back();
				}
				order.insert(order.end(), post.rbegin(), post.rend());
			}
		}
		pxAssert(order.size() == nb);
		// What the dispatcher tests before it enters each block, and so EmitEdge before it goes there (VU0 with slices:
		// the cycles its first instructions need, as the dispatcher's first_need).
		const auto need = [&](const BlockPlan& b) { return (vu == 0 && g_vu0_slices) ? SliceThreshold(b, 0) : b.LastStart(); };
		std::vector<s32> pos(nb);
		std::vector<u32> last_starts(nb);
		for (u32 k = 0; k < nb; k++)
		{
			pos[order[k]] = static_cast<s32>(k);
			last_starts[k] = need(blocks[order[k]]);
		}

		CompiledBlock scratch;
		const auto emit = [&](Emitter& em) {
			scratch.exits.clear();
			em.EmitPrologue();
			em.BeginModule(last_starts);
			for (u32 k = 0; k < nb; k++)
				EmitBlock(em, vu, blocks[order[k]], k, pos, scratch.exits);
			em.EndModule();
		};
		RegUse use;
		{
			Emitter first(&scratch, vu, nullptr);
			emit(first);
			use = first.Use();
		}
		scratch.native_ops = 0;
		scratch.helper_ops = 0;
		Emitter em(&scratch, vu, &use);
		emit(em);
		em.Finish();

		out->wasm = std::move(scratch.wasm);
		out->exits = std::move(scratch.exits);
		out->native_ops = scratch.native_ops;
		out->helper_ops = scratch.helper_ops;
		out->blocks.reserve(nb);
		for (u32 k = 0; k < nb; k++)
		{
			const BlockPlan& b = blocks[order[k]];
			out->blocks.push_back(CompiledModule::Block{b.pc, b.sig, b.Instrs(), need(b), !b.ds});
			out->edges += (b.next_taken >= 0) + (b.next_fall >= 0) + static_cast<u32>(b.jumps.size());
			for (const s32 s : b.bds_next)
				out->edges += s >= 0;
		}
		return true;
	}
} // namespace VU1Jit
