// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// The browser's disc: a request block in the shared wasm memory that the page's disc worker
// (pcsx2-wasm/web/disc-worker.js) serves. A reader pthread posts a byte range into a slot, rings the
// doorbell and sleeps on the slot; the worker reads the range from the user's file (or a URL) and
// copies it straight into the destination. The protocol, its measurements and the page's side are
// in docs/web-port/DISC-DESIGN.md (section 4.1 is the block layout, section 5 this interface).

#include "common/Pcsx2Defs.h"

#include <atomic>
#include <cstddef>
#include <string_view>

class Error;

namespace WebDisc
{
	static constexpr u32 MAGIC = 0x4b534454; // "TDSK"
	static constexpr u32 VERSION = 1;
	static constexpr u32 SLOTS = 4;
	static constexpr u32 MAX_REQUEST = 16 * 1024 * 1024;

	/// How long one read may take before it is cancelled. A read from a picked file takes under a
	/// millisecond; this only ends reads from a stalled server or drive, which would otherwise hold the
	/// CPU thread (ThreadedFileReader::ReadSync reads under its mutex) for up to three 30 s attempts.
	/// A read cancelled this way fails with E_TIMEOUT, and the worker then fails further reads from that
	/// source at once until it answers again, so a stall costs this once rather than on every retry.
	static constexpr u32 READ_TIMEOUT_MS = 10000;

	enum BlockState : s32
	{
		BLOCK_DETACHED = 0,
		BLOCK_NO_DISC = 1,
		BLOCK_READY = 2,
		BLOCK_STOPPED = 3,
	};

	enum SlotState : s32
	{
		SLOT_FREE = 0,
		SLOT_CLAIMED = 1,
		SLOT_PENDING = 2,
		SLOT_BUSY = 3,
		SLOT_DONE = 4,
	};

	/// Slot::cancel values: why the requester gave up on a busy request.
	enum CancelReason : s32
	{
		CANCEL_REQUESTED = 1, // Stop or the caller's cancel flag: the worker answers E_CANCELLED
		CANCEL_DEADLINE = 2, // the read took longer than its timeout: the worker answers E_TIMEOUT
	};

	enum Result : s32
	{
		E_IO = -1,
		E_INVALID = -2,
		E_NO_DISC = -3,
		E_STALE = -4,
		E_CANCELLED = -5,
		E_NETWORK = -6,
		E_TIMEOUT = -7,
	};

	struct Slot
	{
		std::atomic<s32> state;
		std::atomic<s32> result;
		std::atomic<u32> dst;
		std::atomic<u32> length;
		std::atomic<u32> offset_lo;
		std::atomic<u32> offset_hi;
		std::atomic<u32> generation;
		std::atomic<s32> cancel;
	};

	struct alignas(64) RequestBlock
	{
		std::atomic<u32> magic{MAGIC};
		std::atomic<u32> version{VERSION};
		std::atomic<u32> slots{SLOTS};
		std::atomic<u32> slot_bytes{sizeof(Slot)};
		std::atomic<s32> state{BLOCK_DETACHED};
		std::atomic<s32> doorbell{0};
		std::atomic<u32> generation{0};
		std::atomic<u32> size_lo{0};
		std::atomic<u32> size_hi{0};
		std::atomic<u32> max_request{0};
		std::atomic<s32> error{0};
		u32 reserved[5] = {};
		Slot slot[SLOTS] = {};
	};
	static_assert(sizeof(std::atomic<u32>) == 4 && std::atomic<u32>::is_always_lock_free);
	static_assert(sizeof(Slot) == 32 && offsetof(RequestBlock, slot) == 64);

	/// The block (static storage, so it is set up before any code runs). Its address is what
	/// pcsx2web_disc_block() gives the page.
	RequestBlock* GetBlock();

	/// Disc paths the page owns: "/disc/<name>". The page writes a 0-byte placeholder there so that
	/// FileSystem::FileExists() and VMManager's checks pass; the bytes come from the disc worker.
	bool IsDiscPath(std::string_view path);

	struct Info
	{
		u32 generation;
		u64 size;
	};

	/// Reads the generation, state and size consistently. False, with an error, when the page has no
	/// disc open. Not on the browser's main thread: it waits a few milliseconds for a disc that is
	/// being published.
	bool Query(Info* info, Error* error);

	/// Reads length bytes at offset into dst for the disc opened at `generation`. Blocks the calling
	/// pthread, never the browser's main thread. Returns the bytes read (fewer only at the end of the
	/// disc) or a Result code. The read is cancelled when `cancel` becomes true, after timeout_ms, or
	/// after AbortReads(); dst is never written once this returns.
	s32 Read(u32 generation, void* dst, u64 offset, u32 length, const std::atomic<bool>* cancel = nullptr,
		u32 timeout_ms = READ_TIMEOUT_MS);

	/// Cancels every read in progress and fails new ones (E_CANCELLED) until ResumeReads(). Only touches
	/// atomics, so the page's thread may call it (pcsx2web_stop() does, so that Stop never waits on a
	/// slow read; pcsx2web_boot_disc() resumes reads before it asks for the boot).
	void AbortReads();
	void ResumeReads();
	bool ReadsAborted();

	/// A short description of a Result code, for logs.
	const char* ResultName(s32 result);
} // namespace WebDisc
