// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The requester side of the disc request block (docs/web-port/DISC-DESIGN.md), in C, as PCSX2's
// disc reader thread will run it. Built standalone (no libc) for the disc harness by
// build-requester.sh, so the harness exercises real memory.atomic.wait32/notify against the disc
// worker's Atomics.waitAsync/notify. Phase 2 does the same with emscripten_futex_wait/wake.

#include <stdint.h>

enum
{
	H_MAGIC, H_VERSION, H_SLOTS, H_SLOT_BYTES, H_STATE, H_DOORBELL, H_GENERATION, H_SIZE_LO, H_SIZE_HI,
	H_MAX_REQUEST, H_ERROR, HEADER_WORDS = 16
};
enum { S_STATE, S_RESULT, S_DST, S_LENGTH, S_OFFSET_LO, S_OFFSET_HI, S_GENERATION, S_CANCEL };
enum { SLOT_FREE, SLOT_CLAIMED, SLOT_PENDING, SLOT_BUSY, SLOT_DONE };
enum { BLOCK_DETACHED, BLOCK_NO_DISC, BLOCK_READY, BLOCK_STOPPED };

#define E_NO_DISC (-3)
#define E_CANCELLED (-5)
#define E_TIMEOUT (-7)
#define E_NO_SLOT (-8)

#define CANCEL_DEADLINE 2 // S_CANCEL when the requester's own timeout ran out (WebDisc::CANCEL_DEADLINE)

#define WAIT_SLICE_NS (500 * 1000 * 1000LL)

typedef volatile int32_t word;

static int32_t load(word* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
static void store(word* p, int32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static int32_t cas(word* p, int32_t expected, int32_t desired) { return __sync_val_compare_and_swap(p, expected, desired); }

// Returns the generation of the open disc and writes its size to *size_out, or E_NO_DISC. The
// generation is read before and after the size (seqlock), so a disc change in between is caught.
int64_t disc_open(word* block, uint64_t* size_out)
{
	for (int tries = 0; tries < 100; tries++)
	{
		const int32_t gen = load(&block[H_GENERATION]);
		if (load(&block[H_STATE]) != BLOCK_READY)
			return E_NO_DISC;
		const uint64_t size = (uint64_t)(uint32_t)load(&block[H_SIZE_LO]) |
							  ((uint64_t)(uint32_t)load(&block[H_SIZE_HI]) << 32);
		if (load(&block[H_GENERATION]) == gen)
		{
			*size_out = size;
			return (uint32_t)gen;
		}
	}
	return E_NO_DISC;
}

// Reads `length` bytes at `offset` of disc `generation` into `dst`. Returns the bytes read (fewer
// only at the end of the disc) or a negative E_* code. cancel_after_ms > 0 cancels the request
// once it has waited that long (test hook; the C++ reader cancels from another thread instead),
// setting S_CANCEL to cancel_value: 1, or CANCEL_DEADLINE for a timeout (answered TIMEOUT).
int32_t disc_read(word* block, uint32_t dst, uint32_t length, uint32_t offset_lo, uint32_t offset_hi,
	uint32_t generation, int32_t cancel_after_ms, int32_t cancel_value)
{
	const int32_t block_state = load(&block[H_STATE]);
	if (block_state != BLOCK_READY)
		return block_state == BLOCK_STOPPED ? E_CANCELLED : E_NO_DISC;

	const uint32_t slots = (uint32_t)load(&block[H_SLOTS]);
	const uint32_t slot_words = (uint32_t)load(&block[H_SLOT_BYTES]) / 4;
	word* slot = 0;
	for (uint32_t i = 0; i < slots && !slot; i++)
	{
		word* candidate = block + HEADER_WORDS + i * slot_words;
		if (cas(&candidate[S_STATE], SLOT_FREE, SLOT_CLAIMED) == SLOT_FREE)
			slot = candidate;
	}
	if (!slot)
		return E_NO_SLOT; // the C++ reader has one slot per thread and never sees this

	store(&slot[S_RESULT], 0);
	store(&slot[S_DST], (int32_t)dst);
	store(&slot[S_LENGTH], (int32_t)length);
	store(&slot[S_OFFSET_LO], (int32_t)offset_lo);
	store(&slot[S_OFFSET_HI], (int32_t)offset_hi);
	store(&slot[S_GENERATION], (int32_t)generation);
	store(&slot[S_CANCEL], 0);
	store(&slot[S_STATE], SLOT_PENDING);
	__atomic_fetch_add(&block[H_DOORBELL], 1, __ATOMIC_SEQ_CST);
	__builtin_wasm_memory_atomic_notify((int32_t*)&block[H_DOORBELL], 1);

	int32_t waited_ms = 0;
	for (;;)
	{
		const int32_t s = load(&slot[S_STATE]);
		if (s == SLOT_DONE)
			break;
		const int64_t slice = cancel_after_ms > 0 ? (int64_t)cancel_after_ms * 1000000 : WAIT_SLICE_NS;
		if (__builtin_wasm_memory_atomic_wait32((int32_t*)&slot[S_STATE], s, slice) != 2)
			continue; // woken or changed: look again
		waited_ms += (int32_t)(slice / 1000000);
		const int32_t now = load(&block[H_STATE]);
		const int cancel = cancel_after_ms > 0 && waited_ms >= cancel_after_ms;
		if (now == BLOCK_READY && !cancel)
			continue;
		// Withdraw a request the worker has not taken yet; a BUSY one always ends in DONE.
		if (cas(&slot[S_STATE], SLOT_PENDING, SLOT_FREE) == SLOT_PENDING)
			return cancel ? (cancel_value == CANCEL_DEADLINE ? E_TIMEOUT : E_CANCELLED) : now == BLOCK_STOPPED ? E_CANCELLED : E_NO_DISC;
		if (cancel && !load(&slot[S_CANCEL]))
		{
			store(&slot[S_CANCEL], cancel_value ? cancel_value : 1);
			__atomic_fetch_add(&block[H_DOORBELL], 1, __ATOMIC_SEQ_CST);
			__builtin_wasm_memory_atomic_notify((int32_t*)&block[H_DOORBELL], 1);
		}
	}
	const int32_t result = load(&slot[S_RESULT]);
	store(&slot[S_STATE], SLOT_FREE);
	return result;
}
