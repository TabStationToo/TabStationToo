// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Disc harness: a requester worker standing in for PCSX2's disc reader thread. It posts requests
// into the request block and sleeps in Atomics.wait, exactly as the C++ reader will (see
// requester.c, used instead when requester.wasm is built). Timings are taken here, so they include
// the whole round trip through the disc worker and nothing of the page.

'use strict';

const HEADER_WORDS = 16;
const H_SLOTS = 2, H_SLOT_BYTES = 3, H_STATE = 4, H_DOORBELL = 5, H_GENERATION = 6, H_SIZE_LO = 7, H_SIZE_HI = 8;
const S_STATE = 0, S_RESULT = 1, S_DST = 2, S_LENGTH = 3, S_OFFSET_LO = 4, S_OFFSET_HI = 5, S_GENERATION = 6, S_CANCEL = 7;
const SLOT_FREE = 0, SLOT_CLAIMED = 1, SLOT_PENDING = 2, SLOT_DONE = 4;
const BLOCK_READY = 2, BLOCK_STOPPED = 3;
const E_NO_DISC = -3, E_CANCELLED = -5, E_TIMEOUT = -7, E_NO_SLOT = -8;
const CANCEL_DEADLINE = 2; // S_CANCEL value of a requester whose own timeout ran out (WebDisc::CANCEL_DEADLINE)
const WAIT_SLICE_MS = 500;

let memory = null;
let heap = null;
let i32 = null;
let u8 = null;
let block = 0;
let wasm = null; // requester.wasm exports, when the page passed the module
let scratch = 0; // 8 bytes the wasm disc_open() writes the size to

function views() {
	const b = memory.buffer;
	if (b !== heap) {
		heap = b;
		i32 = new Int32Array(b);
		u8 = new Uint8Array(b);
	}
}

// ------------------------------------------------------------------ the protocol (JS version)

function jsOpen() {
	views();
	const h = block >> 2;
	for (let tries = 0; tries < 100; tries++) {
		const gen = Atomics.load(i32, h + H_GENERATION) >>> 0;
		if (Atomics.load(i32, h + H_STATE) !== BLOCK_READY)
			return { generation: E_NO_DISC, size: 0 };
		const size = (Atomics.load(i32, h + H_SIZE_LO) >>> 0) + (Atomics.load(i32, h + H_SIZE_HI) >>> 0) * 2 ** 32;
		if ((Atomics.load(i32, h + H_GENERATION) >>> 0) === gen)
			return { generation: gen, size };
	}
	return { generation: E_NO_DISC, size: 0 };
}

// cancelValue: what S_CANCEL is set to after cancelAfterMs: 1 (a cancel) or CANCEL_DEADLINE (a timeout,
// which the worker answers with TIMEOUT, as the C++ reader's READ_TIMEOUT_MS does).
function jsRead(dst, length, offset, generation, cancelAfterMs, cancelValue = 1) {
	views();
	const h = block >> 2;
	const blockState = Atomics.load(i32, h + H_STATE);
	if (blockState !== BLOCK_READY)
		return blockState === BLOCK_STOPPED ? E_CANCELLED : E_NO_DISC;
	const slots = Atomics.load(i32, h + H_SLOTS);
	const slotWords = Atomics.load(i32, h + H_SLOT_BYTES) / 4;
	let base = -1;
	for (let s = 0; s < slots && base < 0; s++) {
		const b = h + HEADER_WORDS + s * slotWords;
		if (Atomics.compareExchange(i32, b + S_STATE, SLOT_FREE, SLOT_CLAIMED) === SLOT_FREE)
			base = b;
	}
	if (base < 0)
		return E_NO_SLOT;
	Atomics.store(i32, base + S_RESULT, 0);
	Atomics.store(i32, base + S_DST, dst | 0);
	Atomics.store(i32, base + S_LENGTH, length | 0);
	Atomics.store(i32, base + S_OFFSET_LO, (offset % 2 ** 32) | 0);
	Atomics.store(i32, base + S_OFFSET_HI, Math.floor(offset / 2 ** 32) | 0);
	Atomics.store(i32, base + S_GENERATION, generation | 0);
	Atomics.store(i32, base + S_CANCEL, 0);
	Atomics.store(i32, base + S_STATE, SLOT_PENDING);
	Atomics.add(i32, h + H_DOORBELL, 1);
	Atomics.notify(i32, h + H_DOORBELL, 1);

	let waited = 0;
	for (;;) {
		const s = Atomics.load(i32, base + S_STATE);
		if (s === SLOT_DONE)
			break;
		const slice = cancelAfterMs > 0 ? cancelAfterMs : WAIT_SLICE_MS;
		if (Atomics.wait(i32, base + S_STATE, s, slice) !== 'timed-out')
			continue;
		waited += slice;
		const now = Atomics.load(i32, h + H_STATE);
		const cancel = cancelAfterMs > 0 && waited >= cancelAfterMs;
		if (now === BLOCK_READY && !cancel)
			continue;
		if (Atomics.compareExchange(i32, base + S_STATE, SLOT_PENDING, SLOT_FREE) === SLOT_PENDING)
			return cancel ? (cancelValue === CANCEL_DEADLINE ? E_TIMEOUT : E_CANCELLED) : now === BLOCK_STOPPED ? E_CANCELLED : E_NO_DISC;
		if (cancel && !Atomics.load(i32, base + S_CANCEL)) {
			Atomics.store(i32, base + S_CANCEL, cancelValue);
			Atomics.add(i32, h + H_DOORBELL, 1);
			Atomics.notify(i32, h + H_DOORBELL, 1);
		}
	}
	const result = Atomics.load(i32, base + S_RESULT);
	Atomics.store(i32, base + S_STATE, SLOT_FREE);
	return result;
}

// ------------------------------------------------------------------ either implementation

function discOpen(impl) {
	if (impl === 'wasm' && wasm) {
		const gen = Number(wasm.disc_open(block, scratch));
		views();
		const size = gen < 0 ? 0 : Number(new BigUint64Array(heap, scratch, 1)[0]);
		return { generation: gen, size };
	}
	return jsOpen();
}

function discRead(impl, dst, length, offset, generation, cancelAfterMs = 0, cancelValue = 1) {
	if (impl === 'wasm' && wasm)
		return wasm.disc_read(block, dst, length, offset % 2 ** 32, Math.floor(offset / 2 ** 32), generation, cancelAfterMs, cancelValue);
	return jsRead(dst, length, offset, generation, cancelAfterMs, cancelValue);
}

// ------------------------------------------------------------------ helpers

// The synthetic test disc: byte p is byte (p & 3) of the little-endian word (p >> 2 + 1) * 2654435761.
const patternByte = (p) => (Math.imul(Math.floor(p / 4) + 1, 2654435761) >>> ((p % 4) * 8)) & 255;

function checkPattern(dst, offset, n) {
	views();
	for (let k = 0; k < n; k++) {
		if (u8[dst + k] !== patternByte(offset + k))
			return offset + k;
	}
	return -1;
}

async function sha256(dst, n) {
	views();
	const copy = u8.slice(dst, dst + n); // digest() refuses shared memory
	const d = new Uint8Array(await crypto.subtle.digest('SHA-256', copy));
	return Array.from(d, (b) => b.toString(16).padStart(2, '0')).join('');
}

function summary(ms) {
	const s = [...ms].sort((a, b) => a - b);
	const pick = (q) => s[Math.min(s.length - 1, Math.floor(q * s.length))];
	const sum = s.reduce((a, b) => a + b, 0);
	return {
		count: s.length,
		minMs: +s[0].toFixed(4), medianMs: +pick(0.5).toFixed(4), p95Ms: +pick(0.95).toFixed(4),
		maxMs: +s[s.length - 1].toFixed(4), meanMs: +(sum / s.length).toFixed(4),
	};
}

// Deterministic pseudo-random numbers, so runs can be repeated.
function rng(seed) {
	let x = seed >>> 0 || 1;
	return () => {
		x ^= x << 13; x >>>= 0;
		x ^= x >>> 17;
		x ^= x << 5; x >>>= 0;
		return x / 2 ** 32;
	};
}

// ------------------------------------------------------------------ commands

const commands = {
	async init(msg) {
		memory = msg.memory;
		block = msg.block;
		scratch = msg.scratch || 0;
		if (msg.wasmModule) {
			const instance = await WebAssembly.instantiate(msg.wasmModule, { env: { memory } });
			wasm = instance.exports;
		}
		views();
		return { wasm: !!wasm };
	},

	open(msg) {
		return discOpen(msg.impl);
	},

	async read(msg) {
		const t0 = performance.now();
		const result = discRead(msg.impl, msg.dst, msg.length, msg.offset, msg.generation, msg.cancelAfterMs || 0, msg.cancelValue || 1);
		const ms = performance.now() - t0;
		const out = { result, ms };
		if (msg.hash && result > 0)
			out.sha256 = await sha256(msg.dst, result);
		if (msg.bytes && result > 0) {
			views();
			out.bytes = Array.from(u8.subarray(msg.dst, msg.dst + Math.min(result, msg.bytes)));
		}
		if (msg.pattern && result > 0)
			out.mismatchAt = checkPattern(msg.dst, msg.offset, result);
		return out;
	},

	// Reads a list of ranges and returns each result and hash: {ranges: [[offset, length]...]}.
	async hashRanges(msg) {
		const out = [];
		for (const [offset, length] of msg.ranges) {
			const result = discRead(msg.impl, msg.dst, length, offset, msg.generation);
			out.push({ offset, length, result, sha256: result > 0 ? await sha256(msg.dst, result) : null });
		}
		return out;
	},

	// Timed loop: count reads of `length` bytes at offsets chosen by `pattern` ('random' over the
	// disc, aligned to `align`, or 'sequential' from `start`). Optionally verifies the synthetic
	// pattern. Returns latency statistics and the throughput of the whole loop.
	async bench(msg) {
		const next = rng(msg.seed || 1);
		const align = msg.align || 2048;
		const span = Math.max(align, msg.size - msg.length);
		const ms = [];
		let bytes = 0;
		let errors = 0;
		let firstError = 0;
		let mismatches = 0;
		let offset = msg.start || 0;
		const t0 = performance.now();
		for (let k = 0; k < msg.count; k++) {
			if (msg.pattern !== 'sequential')
				offset = Math.floor(next() * span / align) * align;
			const a = performance.now();
			const result = discRead(msg.impl, msg.dst, msg.length, offset, msg.generation);
			ms.push(performance.now() - a);
			if (result < 0) {
				errors++;
				firstError ||= result;
			} else {
				bytes += result;
				if (msg.verifyPattern && checkPattern(msg.dst, offset, result) >= 0)
					mismatches++;
			}
			offset += msg.length;
		}
		const totalMs = performance.now() - t0;
		return { ...summary(ms), bytes, errors, firstError, mismatches, totalMs: +totalMs.toFixed(2),
			mbPerS: +(bytes / 1048576 / (totalMs / 1000)).toFixed(1) };
	},

	sha256(msg) {
		return sha256(msg.dst, msg.length);
	},

	bufferInfo() {
		return { byteLength: memory.buffer.byteLength, stable: memory.buffer === memory.buffer };
	},
};

self.onmessage = async (event) => {
	const msg = event.data;
	try {
		const value = await commands[msg.cmd](msg);
		self.postMessage({ id: msg.id, ok: true, value });
	} catch (err) {
		self.postMessage({ id: msg.id, ok: false, error: String(err?.stack || err) });
	}
};
