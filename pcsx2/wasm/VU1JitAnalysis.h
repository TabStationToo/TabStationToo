// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU1 JIT (docs/web-port/VU1-JIT-DESIGN.md): instruction decoding, pipeline signatures and the timing model.
//
// The timing model works out at compile time every timing decision that _vu1Exec (VU1microInterp.cpp:38-247) makes
// for one instruction: stalls, pipe write-backs, new pipe entries, the VI branch delay window, the branch and E-bit
// countdowns. It runs the interpreter's own primitives (_vuTestPipes, _vuTest*Stalls, _vuAdd*Stalls, _vuClearFMAC,
// _vuFlushAll, _vuBackupVI) on a scratch VURegs, and the interpreter's register-info functions
// (VU1regs_*_OPCODE). Only _vu1Exec's orchestration is copied, in TimingModel::Step, and the timing oracle
// (VU1JitFuzz.cpp) tests that copy against the interpreter, instruction by instruction (section 9.2).

#pragma once

#include "VU.h"
#include "VUops.h"

#include <string>

namespace VU1Jit
{
	//////////////////////////////////////////////////////////////////////////
	// Decoding: mirrors the VU1 opcode tables (VUops.cpp:3612-3790, _vuTablesMess).

#define VU1JIT_UPPER_OPS(X) \
	X(ADDx) X(ADDy) X(ADDz) X(ADDw) X(SUBx) X(SUBy) X(SUBz) X(SUBw) \
	X(MADDx) X(MADDy) X(MADDz) X(MADDw) X(MSUBx) X(MSUBy) X(MSUBz) X(MSUBw) \
	X(MAXx) X(MAXy) X(MAXz) X(MAXw) X(MINIx) X(MINIy) X(MINIz) X(MINIw) \
	X(MULx) X(MULy) X(MULz) X(MULw) X(MULq) X(MAXi) X(MULi) X(MINIi) \
	X(ADDq) X(MADDq) X(ADDi) X(MADDi) X(SUBq) X(MSUBq) X(SUBi) X(MSUBi) \
	X(ADD) X(MADD) X(MUL) X(MAX) X(SUB) X(MSUB) X(OPMSUB) X(MINI) \
	X(ADDAx) X(SUBAx) X(MADDAx) X(MSUBAx) X(ITOF0) X(FTOI0) X(MULAx) X(MULAq) X(ADDAq) X(SUBAq) X(ADDA) X(SUBA) \
	X(ADDAy) X(SUBAy) X(MADDAy) X(MSUBAy) X(ITOF4) X(FTOI4) X(MULAy) X(ABS) X(MADDAq) X(MSUBAq) X(MADDA) X(MSUBA) \
	X(ADDAz) X(SUBAz) X(MADDAz) X(MSUBAz) X(ITOF12) X(FTOI12) X(MULAz) X(MULAi) X(ADDAi) X(SUBAi) X(MULA) X(OPMULA) \
	X(ADDAw) X(SUBAw) X(MADDAw) X(MSUBAw) X(ITOF15) X(FTOI15) X(MULAw) X(CLIP) X(MADDAi) X(MSUBAi) X(NOP)

#define VU1JIT_LOWER_OPS(X) \
	X(LQ) X(SQ) X(ILW) X(ISW) X(IADDIU) X(ISUBIU) \
	X(FCEQ) X(FCSET) X(FCAND) X(FCOR) X(FSEQ) X(FSSET) X(FSAND) X(FSOR) X(FMEQ) X(FMAND) X(FMOR) X(FCGET) \
	X(B) X(BAL) X(JR) X(JALR) X(IBEQ) X(IBNE) X(IBLTZ) X(IBGTZ) X(IBLEZ) X(IBGEZ) \
	X(IADD) X(ISUB) X(IADDI) X(IAND) X(IOR) \
	X(MOVE) X(LQI) X(DIV) X(MTIR) X(RNEXT) X(MFP) X(XTOP) X(XGKICK) X(ESADD) X(EATANxy) X(ESQRT) X(ESIN) \
	X(MR32) X(SQI) X(SQRT) X(MFIR) X(RGET) X(XITOP) X(ERSADD) X(EATANxz) X(ERSQRT) X(EATAN) \
	X(LQD) X(RSQRT) X(ILWR) X(RINIT) X(ELENG) X(ESUM) X(ERCPR) X(EEXP) \
	X(SQD) X(WAITQ) X(ISWR) X(RXOR) X(ERLENG) X(WAITP)

#define VU1JIT_ENUM_ENTRY(name) name,
	enum class Upper : u8
	{
		Unknown,
		VU1JIT_UPPER_OPS(VU1JIT_ENUM_ENTRY)
		Count
	};

	enum class Lower : u8
	{
		Unknown,
		Immediate, // the lower word of an I-bit instruction: the value loaded into I
		VU1JIT_LOWER_OPS(VU1JIT_ENUM_ENTRY)
		Count
	};
#undef VU1JIT_ENUM_ENTRY

	// Properties of lower op bodies that matter for timing and control flow (VUops.cpp).
	enum LowerFlags : u16
	{
		LOWER_BRANCH = 1 << 0, // calls _setBranch
		LOWER_COND = 1 << 1, // ... only when its condition holds (IBxx)
		LOWER_DYNAMIC = 1 << 2, // ... with a target from a VI register (JR, JALR)
		LOWER_LINK = 1 << 3, // BAL, JALR
		LOWER_STORE = 1 << 4, // writes VU data memory (SQ, SQD, SQI, ISW, ISWR)
		LOWER_XGKICK = 1 << 5,
		LOWER_EFU = 1 << 6,
		LOWER_VI_OLD = 1 << 7, // reads VIOldValue in the VI branch delay window (IBxx, VUops.cpp:1486-1595)
		LOWER_FDIV = 1 << 8, // DIV, SQRT, RSQRT, WAITQ
	};

	struct Instruction
	{
		u32 lower = 0;
		u32 upper = 0;
		Upper up = Upper::Unknown;
		Lower lo = Lower::Unknown;

		bool IBit() const { return (upper & 0x80000000u) != 0; }
		bool EBit() const { return (upper & 0x40000000u) != 0; }
		bool MBit() const { return (upper & 0x20000000u) != 0; }
		bool DBit() const { return (upper & 0x10000000u) != 0; }
		bool TBit() const { return (upper & 0x08000000u) != 0; }

		// Both words are ops the interpreter knows. Unknown words leave _VURegsNum uninitialised in the
		// interpreter (VUops.cpp:3603-3613): the JIT never compiles or models them.
		bool IsValid() const { return up != Upper::Unknown && lo != Lower::Unknown; }
	};

	Instruction Decode(u32 lower, u32 upper);
	const char* UpperName(Upper op);
	const char* LowerName(Lower op);
	u16 GetLowerFlags(Lower op);

	// The opcode bits of an op (the other fields zero), for building instructions.
	u32 UpperOpcodeBits(Upper op);
	u32 LowerOpcodeBits(Lower op);

	// The register _vuBackupVI is called with by the lower op's body (VUops.cpp:1014-1233), or -1.
	int BackupVIRegister(const Instruction& ins);

	// B, BAL and IBxx: _branchAddr (VUops.cpp:1463-1468) for the instruction at byte address pc.
	u32 StaticBranchTarget(u32 pc, u32 lower);

	// "MULAx.xyzw vf0, vf1x / LQ.xyzw vf2, 0(vi1)", for logs.
	std::string Disassemble(u32 lower, u32 upper);

	//////////////////////////////////////////////////////////////////////////
	// Signatures (section 4.2): the part of the interpreter's state that decides timing and is static along a path.

	enum SignatureFmacFlags : u8
	{
		SIG_FMAC_CLIP = 1 << 0, // flagreg has REG_CLIP_FLAG: the write-back sets VI[CLIP]
		SIG_FMAC_STATUS = 1 << 1, // flagreg has REG_STATUS_FLAG: the FSSET form of the status write-back
	};

	struct Signature
	{
		u8 fmac_count;
		u8 ialu_count;
		u8 fdiv; // cycles until the FDIV write-back, 0 when idle
		u8 efu; // cycles until the EFU write-back (after any efu.Cycle -= 1), 0 when idle
		u8 vi_backup_cycles;
		u8 vi_backup_reg; // 0 when vi_backup_cycles is 0
		u8 pad[2];

		// Pending FMAC entries from the read position, in ring order. remaining: cycles until the write-back.
		// xyzw fields are 0 when their register is 0: the interpreter leaves them as stack garbage for ops that
		// write no VF, and stalls never test them then (VUops.cpp:221-222).
		struct Fmac
		{
			u8 remaining;
			u8 regupper;
			u8 xyzwupper;
			u8 reglower;
			u8 xyzwlower;
			u8 flags; // SignatureFmacFlags
		} fmac[4];

		struct Ialu
		{
			u8 remaining;
			u8 pad;
			u16 reg; // VI register mask
		} ialu[4];

		bool IsClean() const;
		u32 Hash() const;
		std::string ToString() const;
	};
	static_assert(sizeof(Signature) == 48);

	bool operator==(const Signature& a, const Signature& b);
	inline bool operator!=(const Signature& a, const Signature& b) { return !(a == b); }

	// Why a state has no signature: the interpreter then steps until it has one (section 4.2).
	enum class Refusal : u8
	{
		None,
		FmacRing, // count above 4, or read and write positions that disagree with the count
		FmacStale, // entries left from before VU1.cycle = 0 (MTVU_VU_EXECUTE, MTVU.cpp:145): one that starts after the
		           // current cycle, or entries out of completion order
		FmacRange, // more cycles remaining than the latency, or none
		IaluRing,
		IaluStale,
		IaluRange,
		FdivStale,
		FdivRange,
		EfuStale,
		EfuRange,
		ViBackup, // VIBackupCycles above 2 or a register above 15
		Count
	};
	const char* RefusalName(Refusal r);

	Refusal DeriveSignature(const VURegs& vu, Signature* out);

	//////////////////////////////////////////////////////////////////////////
	// The timing model (section 4.2)

	// Branch and E-bit state between instructions (VURegs fields). takedelaybranch is always false at an
	// instruction boundary (_vu1Exec clears it in the step that sets it), but it is kept for the oracle.
	struct Control
	{
		u32 branch = 0; // 2 just set, 1 in the delay slot, 0 none (counted down at the end of each step)
		u32 branchpc = 0;
		bool takedelaybranch = false;
		u32 delaybranchpc = 0;
		u32 ebit = 0; // the same countdown for the program end
	};
	Control GetControl(const VURegs& vu);

	struct StepInput
	{
		u32 pc = 0; // byte address of the instruction (TPC masked, as InterpVU1::Step does)
		u32 lower = 0;
		u32 upper = 0;
		u32 fbrst = 0; // vu1Thread.vuFBRST of the run (D and T bits)
		bool branch_taken = true; // outcome of a conditional branch (IBxx): data, not timing
		u32 jump_target = 0; // JR/JALR target (VI * 8, unmasked)
		u32 kick_bump = 0; // cycles a busy XGKICK's flush added (VUops.cpp:1887-1888): only the oracle uses it,
		                   // generated code leaves to the interpreter before such an XGKICK (section 7.7)
	};

	struct StepInfo
	{
		u32 cycles; // cycle delta of the whole instruction, the _vuFlushAll at a program end included
		u32 op_cycle; // delta to its _vuTestPipes point: 1 + stalls
		u32 upper_stall;
		u32 lower_stall;

		// Entries that existed before the instruction and were written back at its _vuTestPipes point, FMAC and
		// IALU in ring order (_vuTestPipes order: FMAC, FDIV, EFU, IALU; VUops.cpp:188-208).
		u8 fmac_flushed;
		u8 ialu_flushed;
		bool fdiv_flushed;
		bool efu_flushed;

		// The same at the second _vuTestPipes of a busy XGKICK (only with StepInput::kick_bump).
		u8 kick_fmac_flushed;
		u8 kick_ialu_flushed;
		bool kick_fdiv_flushed;
		bool kick_efu_flushed;

		bool ibit;
		bool lower_runs; // the lower op's body runs: no I bit and not discarded
		bool discarded; // upper and lower write the same VF, or CLIP and FCSET (VU1microInterp.cpp:130-158)
		u8 old_vf; // nonzero: the lower op reads this VF, which the upper op writes, and sees its old value
		bool old_clip; // the lower op reads the clip flag the upper op writes, and sees its old value

		u8 vi_backup_cycles; // after the decrement, before the ops: what IBxx see
		u8 vi_backup_reg;
		s8 backup_vi; // the register this instruction's _vuBackupVI used, -1 if none
		bool backup_chained; // _vuBackupVI kept the older value (repeat write of the same register)

		bool fmac_added;
		u8 fmac_added_flags; // SignatureFmacFlags of the new entry
		bool fdiv_added;
		bool efu_added;
		bool ialu_added;

		bool fdiv_wait; // an FDIV-pipe op found FDIV busy
		bool efu_wait; // an EFU-pipe op found EFU busy (and decremented efu.Cycle, VUops.cpp:270)
		bool ialu_wait; // a branch found a pending IALU entry for a register it reads

		bool branch_set; // a branch op set a branch
		bool delay_branch; // ... inside a branch delay slot (takedelaybranch, VUops.cpp:1472-1476)
		bool ebit_fetched;
		bool ebit_rearmed; // an E bit fetched while the E-bit countdown ran: the program runs one more instruction
		bool dt_stop; // a D or T bit stopped the program (vuFBRST)
		bool end; // the program ended after this instruction (_vuFlushAll ran)
	};

	class TimingModel
	{
	public:
		TimingModel();

		// Starts from a signature and a control state, at an arbitrary time base.
		void Load(const Signature& sig, const Control& ctl);

		// Runs one instruction. Returns false (the model is then undefined) for an instruction with unknown
		// words, which is never modelled.
		bool Step(const StepInput& in, StepInfo* out);

		// The state after the steps so far. GetSignature fails only if the model itself is wrong.
		bool GetSignature(Signature* out) const;
		Control GetControl() const;
		u32 GetTPC() const; // as the interpreter leaves it: unmasked
		u64 GetCycles() const; // since Load

	private:
		VURegs m_vu; // scratch: only the timing fields are meaningful
	};
} // namespace VU1Jit
