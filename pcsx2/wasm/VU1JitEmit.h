// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// VU1 JIT code generation (docs/web-port/VU1-JIT-DESIGN.md sections 4, 5, 7). A block is a straight run of
// instructions from an entry (PC, pipeline signature) to a branch's delay slot, the program end, or the first
// instruction generated code does not run. The timing model decides every stall, write-back and pipe entry at
// compile time; generated code does only the data work. It keeps the registers it uses (VF, ACC, VI0-VI15), the
// pending and committed flags and the pipe entries' flag snapshots in locals, and writes them to the interpreter's
// own state in VURegs at every exit and before calling an op body without native code (which it reloads after), so
// every exit leaves a state the interpreter continues from. The timing fields of the pipes are written at the exit,
// from the model (ApplyExit).
//
// On VU1 a module holds the blocks reachable from its entry block, through branches and fall-throughs whose target
// (PC, signature) no other module holds: one function, whose argument is the block to start at. A block's exit to
// another block of the module does the dispatcher's work itself (its budget test, ApplyExit's and NormalizeRings'
// effects on the timing fields that the next exit does not write again, the next block's base) and branches there
// without leaving generated code. A block whose branch goes back to its own entry with the entry's signature (a
// vertex loop) runs inside a wasm loop and leaves the timing fields to the exit that finally leaves (EmitLoopBack).

#pragma once

#include "VU1JitAnalysis.h"

#include <vector>

namespace VU1Jit
{
	enum class ExitKind : u8
	{
		Boundary, // the state is complete at the start of the instruction at tpc
		End, // the program ended
	};

	enum class ExitReason : u8
	{
		Fallback, // an instruction generated code does not run
		Budget,
		BusyKick, // an XGKICK found the previous kick running: its cycle cost is data dependent
		DTBit,
		UnknownTarget,
		Cap,
		Block, // the block ended (branch, fall-through): the next one may follow at once
		Count
	};

	// The timing fields the model holds at an exit; times relative to TIMING_MODEL_BASE.
	struct PipeImage
	{
		fmacPipe fmac[4];
		u32 fmacreadpos, fmacwritepos, fmaccount;
		fdivPipe fdiv;
		efuPipe efu;
		ialuPipe ialu[4];
		u32 ialureadpos, ialuwritepos, ialucount;
		u8 VIBackupCycles;
		u32 VIRegNumber;
		u64 cycle;
	};

	struct BlockExit
	{
		ExitKind kind = ExitKind::Boundary;
		ExitReason reason = ExitReason::Block;
		u32 instrs = 0; // instructions generated code ran before it
		u32 last_cyc = 0; // cycles from the block's entry to the start of the last of them (VU0: VU0Jit's tracking)
		u32 tpc = 0; // TPC as the interpreter leaves it (unmasked), unless dynamic_tpc
		bool dynamic_tpc = false; // JR/JALR: generated code stored TPC
		u32 ebit = 0;
		PipeImage pipes = {};
		s32 efu_cycle_adjust = 0; // an EFU entry pending since the entry: efu.Cycle changes (VUops.cpp:270)
		bool mbit = false; // VU0: the last instruction had the M bit (VUFLAG_MFLAGSET)
		bool chain = false; // sig is valid: the dispatcher may enter the next block without deriving it
		Signature sig = {};
		// A branch is pending (a branch in a branch delay slot, VU1): the instruction at tpc is the delay slot of a
		// branch to branchpc. The dispatcher then steps the interpreter; sig_valid says whether sig describes the pipes,
		// for a block of the module entered in that delay slot.
		u32 branch = 0;
		u32 branchpc = 0;
		bool sig_valid = false;
		u32 block_pc = 0; // the entry of the block it leaves
		u32 block = 0; // ... and its index in the module
	};

	struct CompiledBlock
	{
		std::vector<u8> wasm;
		std::vector<BlockExit> exits;
		u32 instrs = 0;
		u32 last_start = 0; // cycles from the entry to the start of the last instruction (the budget test)
		// VU0 slices (g_vu0_slices): the dispatcher enters the block only with more cycles left than this; generated
		// code leaves at the first instruction past the slice (0 when the first instruction runs alone).
		u32 first_need = 0;
		u32 native_ops = 0;
		u32 helper_ops = 0;
		bool loops = false; // a taken branch back to the entry with the entry's signature stays in generated code
	};

	// Compiles the block at pc (byte address) for an entry with signature sig and no branch or E bit pending,
	// reading the VU's micro memory (vu: 0 or 1). False when there is nothing to compile: the first instruction must run on the interpreter.
	bool CompileBlock(int vu, u32 pc, const Signature& sig, CompiledBlock* out);

	// A module: blocks that branch to each other inside generated code. Its function's argument is the index of
	// the block to start at; exits are numbered across the module.
	struct CompiledModule
	{
		struct Block
		{
			u32 pc;
			Signature sig;
			u32 instrs;
			u32 last_start;
			bool entry; // false: a block entered in a branch delay slot, from the module only
		};
		std::vector<u8> wasm;
		std::vector<BlockExit> exits;
		std::vector<Block> blocks; // blocks[0] is the entry
		u32 native_ops = 0;
		u32 helper_ops = 0;
		u32 edges = 0; // block exits that go to a block of the module
	};

	struct ModuleSource
	{
		// Whether another module already holds the block at (pc, sig): CompileModule then leaves it to the
		// dispatcher. Null: every block reachable goes into the module.
		bool (*known)(void* ctx, u32 pc, const Signature& sig) = nullptr;
		// The targets seen at run time of the JR or JALR that ends the block at (pc, sig) (TPC values, unmasked):
		// generated code goes to those of them that are blocks of the module itself. Null: none.
		const std::vector<u32>* (*jump_targets)(void* ctx, u32 pc, const Signature& sig) = nullptr;
		void* ctx = nullptr;
		// More blocks (PC, signature) to start from: merging modules into one.
		std::vector<std::pair<u32, Signature>> roots;
		u32 block_limit = 0; // zero keeps the emitter's normal bound
	};

	// Compiles the block at pc for sig and the blocks reachable from it, through static branches, fall-throughs
	// and learned jump targets, up to the module's caps. False when there is nothing to compile (as CompileBlock).
	// VU0 (vu 0): no block entered in a delay slot, and no edge after an M bit, which ends InterpVU0::Execute's loop.
	// CompiledModule::Block::last_start is then what the dispatcher tests before it enters the block: first_need
	// with g_vu0_slices, as CompiledBlock.
	bool CompileModule(int vu, u32 pc, const Signature& sig, const ModuleSource& source, CompiledModule* out);

	// After generated code returned exit x: writes the timing state, TPC and the control state. base is VU1.cycle at
	// the entry.
	void ApplyExit(VURegs& R, const BlockExit& x, u64 base);

	// Rotates the FMAC and IALU rings so that their read positions are 0, as blocks expect at their entry. Ring
	// positions are not observable (the interpreter only uses them relative to each other).
	void NormalizeRings(VURegs& R);

	// VU1.cycle at the entry of the running block, for the helpers. A block that loops (CompiledBlock::loops) moves it
	// to the start of each pass, so after it returns it is the base of the pass that exited.
	extern u64 g_block_base;
	extern u64 g_block_base0; // the same for VU0
	// VU0's slice (InterpVU0::Execute's cycles): VU0.cycle at the start of its Execute plus its cycles. With
	// g_vu0_slices, VU0 blocks leave before the first instruction that would start at or after it, so a block runs in
	// part of a slice instead of the interpreter stepping it.
	extern u64 g_budget_end0;
	extern bool g_vu0_slices;

	// The dispatcher's budget (VU1): VU1.cycle at the start of its Execute plus its cycles. Generated code leaves
	// before a block (or a loop's pass) whose last instruction would start at or after it, as the dispatcher would
	// not enter it.
	extern u64 g_budget_end;
	// Written by every exit of a VU1 module: the instructions of the blocks and loop passes it completed before the
	// block that exited, and how many times it went from a block to the next (or back to its start) without leaving.
	extern u32 g_chain_instrs;
	extern u32 g_chain_edges;
	extern u32 g_chain_instrs0; // the same for VU0 modules (EE thread)
	extern u32 g_chain_edges0;

	// Debugging: log every block and interpreter step of the dispatcher.
	extern bool g_trace;
} // namespace VU1Jit
