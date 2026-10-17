// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// TabStationToo disc worker: reads the disc image on demand for the emulator. Plain modern JS, no
// build step. The design, the measurements behind it and the C++ side are in
// docs/web-port/DISC-DESIGN.md.
//
// A 3-8 GB disc image cannot be loaded into the emulator's memory. PCSX2's disc reader thread (a
// pthread, so a Web Worker) writes a byte-range request into a small request block in the shared
// wasm memory, rings the block's doorbell and sleeps in Atomics.wait. This worker holds the disc (a
// File or Blob, a FileSystemFileHandle, or a URL served with byte ranges), wakes on the doorbell,
// reads the range, copies it straight into the wasm memory and wakes the reader. Nothing here runs
// on the page's thread, which only posts the messages below.
//
// Messages from the page. Each may carry an `id`; the worker answers it with
// {type: 'reply', id, ok, error?, status}.
//   {type: 'attach', memory, block, loop?, lock?, resume?}
//                                            memory: the emulator's WebAssembly.Memory (shared);
//                                            block: address of the request block, set up by C++;
//                                            loop: 'auto' (Atomics.waitAsync when the browser has
//                                            it) or 'blocking' (Atomics.wait, the fallback).
//                                            lock: a Web Lock name this worker holds for as long as
//                                            it lives, taken before it touches the memory, so the page
//                                            can tell when it is really gone (status.lock says if
//                                            it got it).
//                                            resume: {file | handle | url, name?, read?, generation,
//                                            size}: a worker that replaces a dead one goes on serving
//                                            the disc in the block under the same generation, so a
//                                            running game keeps reading it. If the block does not
//                                            hold that disc, or the source does not open at that
//                                            size, the reply is still ok but status.disc is null
//                                            (and status.error says why).
//   {type: 'open', file | handle | url, name?, read?}
//                                            file: File or Blob; handle: FileSystemFileHandle
//                                            (OPFS files are read through a sync access handle);
//                                            url: absolute, served with byte ranges.
//                                            read: 'auto' (default), 'sync' or 'async'.
//                                            The current disc keeps being served until the new one
//                                            has opened: an open that fails leaves it in place. An
//                                            open overtaken by a newer open or a close fails.
//   {type: 'close'}                          forget the disc; reads fail with NO_DISC
//   {type: 'check'}                          read the start of the disc now (5 s at most): ok when
//                                            the source still answers, as the page asks before each
//                                            boot. Clears the error state on success.
//   {type: 'stop'}                           fail every request and stop serving, then the page
//                                            terminates the worker
//   {type: 'status'}                         just the reply
// Messages to the page: {type: 'status', status} whenever the state or the error changes. A status
// is {state, disc, error, stats, loop, features, lock}; state is one of STATE_* below, disc is
// {kind, name, size, generation, read} or null, error is {code, message} or null.
//
// A source that times out or cannot be reached (TIMEOUT, NETWORK) is marked unavailable: further
// requests fail at once with the same code, instead of each waiting for the source, while a probe
// reads its first byte every few seconds; once it answers, reads go to it again (state ready).

'use strict';

// ------------------------------------------------------------------ request block (version 1)
// Little-endian 32-bit words, always accessed with Atomics. The C++ side owns the block (zeroed,
// 64-byte aligned) and writes MAGIC, VERSION, SLOTS and SLOT_BYTES before posting its address.

const BLOCK_MAGIC = 0x4b534454; // 'TDSK'
const BLOCK_VERSION = 1;
const HEADER_WORDS = 16;
const MIN_SLOT_WORDS = 8;
const MAX_SLOTS = 16;

// Header words.
const H_MAGIC = 0;
const H_VERSION = 1;
const H_SLOTS = 2;
const H_SLOT_BYTES = 3;
const H_STATE = 4; // BLOCK_*, written here
const H_DOORBELL = 5; // incremented and notified by requesters; this worker waits on it
const H_GENERATION = 6; // bumped on every open, close and stop
const H_SIZE_LO = 7; // disc size in bytes, valid while H_STATE is BLOCK_READY
const H_SIZE_HI = 8;
const H_MAX_REQUEST = 9; // largest length a request may ask for
const H_ERROR = 10; // last error result (negative) since the disc opened, 0 if none

const BLOCK_DETACHED = 0;
const BLOCK_NO_DISC = 1;
const BLOCK_READY = 2;
const BLOCK_STOPPED = 3;

// Slot words. The requester owns a slot from FREE -> CLAIMED until it sets it back to FREE.
const S_STATE = 0;
const S_RESULT = 1; // bytes read (short only at the end of the disc), or a negative E_* code
const S_DST = 2; // wasm address of the destination
const S_LENGTH = 3;
const S_OFFSET_LO = 4;
const S_OFFSET_HI = 5;
const S_GENERATION = 6; // H_GENERATION the requester opened the disc with
const S_CANCEL = 7; // set non-zero (then ring the doorbell) to cancel a BUSY request: CANCEL_*

const CANCEL_REQUESTED = 1; // answered CANCELLED
const CANCEL_DEADLINE = 2; // the requester's timeout ran out: answered TIMEOUT, and the source is unavailable

const SLOT_FREE = 0;
const SLOT_CLAIMED = 1;
const SLOT_PENDING = 2; // posted; the requester may still withdraw it with CAS PENDING -> FREE
const SLOT_BUSY = 3; // taken by this worker; it always ends in DONE
const SLOT_DONE = 4;

const E_IO = -1; // the source failed (file unreadable, HTTP error)
const E_INVALID = -2; // bad request: destination outside the memory or null, length too large
const E_NO_DISC = -3;
const E_STALE = -4; // the disc changed since the requester opened it
const E_CANCELLED = -5;
const E_NETWORK = -6;
const E_TIMEOUT = -7;

const ERROR_NAMES = {
	[E_IO]: 'IO', [E_INVALID]: 'INVALID', [E_NO_DISC]: 'NO_DISC', [E_STALE]: 'STALE',
	[E_CANCELLED]: 'CANCELLED', [E_NETWORK]: 'NETWORK', [E_TIMEOUT]: 'TIMEOUT',
};

// ------------------------------------------------------------------ settings

const MAX_REQUEST = 16 * 1024 * 1024;
const URL_TIMEOUT_MS = 30000;
// A synchronous XHR blocks this worker, so cancels cannot reach it: end it before the C++ reader's own
// 10 s deadline (WebDisc::READ_TIMEOUT_MS), and never retry a timeout.
const SYNC_URL_TIMEOUT_MS = 8000;
const URL_OPEN_TIMEOUT_MS = 15000;
const CHECK_TIMEOUT_MS = 5000;
const PROBE_FIRST_MS = 1000; // an unavailable source is probed after this, then every PROBE_INTERVAL_MS
const PROBE_INTERVAL_MS = 2000;
const PROBE_TIMEOUT_MS = 5000;
const URL_RETRY_DELAYS_MS = [250, 1000]; // network errors and 408/429/5xx are retried this often
// Without Atomics.waitAsync the loop blocks in Atomics.wait; it returns to the event loop at least
// this often so that page messages (open, stop) are handled.
const BLOCKING_POLL_MS = 50;
const BLOCKING_CANCEL_POLL_MS = 5;

const STATE_DETACHED = 'detached'; // no request block yet
const STATE_IDLE = 'idle'; // attached, no disc
const STATE_OPENING = 'opening';
const STATE_READY = 'ready';
const STATE_ERROR = 'error'; // a disc is open but its last read failed (see status.error)
const STATE_STOPPED = 'stopped';

const FEATURES = {
	waitAsync: typeof Atomics.waitAsync === 'function',
	fileReaderSync: typeof FileReaderSync === 'function',
	syncXhr: typeof XMLHttpRequest === 'function',
};

// ------------------------------------------------------------------ state

let memory = null; // {buffer} source: the WebAssembly.Memory, or a SharedArrayBuffer wrapped
let heap = null; // the buffer the views below were made from
let u8 = null;
let i32 = null;
let hdr = 0; // word index of the block header
let slotCount = 0;
let slotWords = MIN_SLOT_WORDS;

let state = STATE_DETACHED;
let source = null; // the open disc (see the *Source classes)
let generation = 0;
let openSeq = 0; // makes a slow open lose against a newer open or close
let loopMode = null; // 'async' | 'blocking'
let running = false;
let attaching = false;
let lockHeld = false;
let lastError = null;
const inflight = []; // per slot: the request this worker is serving, or null

const stats = { reads: 0, bytes: 0, errors: 0, inflight: 0, maxInflight: 0, busyMs: 0 };

// ------------------------------------------------------------------ memory views

// Memory growth replaces memory.buffer with a larger SharedArrayBuffer over the same memory; views
// on the old one stay valid but cannot reach the new pages. Check before every use.
function refreshViews() {
	const buffer = memory.buffer;
	if (buffer !== heap) {
		heap = buffer;
		u8 = new Uint8Array(buffer);
		i32 = new Int32Array(buffer);
	}
}

const word = (index) => Atomics.load(i32, hdr + index);
const slotBase = (slot) => hdr + HEADER_WORDS + slot * slotWords;

// ------------------------------------------------------------------ status

function snapshot() {
	return {
		state,
		disc: source ? {
			kind: source.kind, name: source.name, size: source.size, generation, read: source.mode,
		} : null,
		error: lastError,
		stats: { ...stats },
		loop: loopMode,
		features: FEATURES,
		lock: lockHeld,
	};
}

function postStatus() {
	self.postMessage({ type: 'status', status: snapshot() });
}

function setState(next, error) {
	const errorChanged = error !== undefined && (error?.code !== lastError?.code || error?.message !== lastError?.message);
	if (error !== undefined)
		lastError = error;
	if (next !== state || errorChanged) {
		state = next;
		postStatus();
	}
}

class DiscError extends Error {
	constructor(code, message) {
		super(message);
		this.code = code;
	}
}

function describe(err) {
	if (err instanceof DiscError)
		return { code: ERROR_NAMES[err.code] || 'IO', message: err.message };
	return { code: 'IO', message: String(err?.message || err) };
}

const asDiscError = (err) => (err instanceof DiscError ? err : new DiscError(E_IO, String(err?.message || err)));

function withTimeout(promise, ms, onTimeout) {
	let timer = 0;
	const timeout = new Promise((resolve, reject) => {
		timer = setTimeout(() => reject(onTimeout()), ms);
	});
	return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

// ------------------------------------------------------------------ disc sources
// read(offset, length, request) returns a Uint8Array (or a promise of one) to copy into the
// memory, or a number of bytes it already wrote there. offset + length never passes size.

// Maps a DOMException from reading a File to the page's message. Chrome raises NotReadableError
// when the file changed or moved since it was picked, NotFoundError when it was deleted.
function fileError(err) {
	const name = err?.name || '';
	if (name === 'NotReadableError' || name === 'NotFoundError' || name === 'SecurityError')
		return new DiscError(E_IO, 'The disc file can no longer be read. It may have been moved, changed or deleted: pick it again.');
	return new DiscError(E_IO, `Reading the disc file failed (${name || err}).`);
}

class BlobSource {
	constructor(blob, name, mode) {
		this.kind = blob instanceof File ? 'file' : 'blob';
		this.blob = blob;
		this.name = name || blob.name || 'disc';
		this.size = blob.size;
		this.mode = mode === 'async' || !FEATURES.fileReaderSync ? 'async' : 'sync';
		this.reader = this.mode === 'sync' ? new FileReaderSync() : null;
	}

	read(offset, length) {
		const slice = this.blob.slice(offset, offset + length);
		if (this.reader) {
			try {
				return new Uint8Array(this.reader.readAsArrayBuffer(slice));
			} catch (err) {
				throw fileError(err);
			}
		}
		return slice.arrayBuffer().then((ab) => new Uint8Array(ab), (err) => { throw fileError(err); });
	}

	// The first sector, asynchronously even in sync mode, so a hung read cannot hold the worker.
	async probe(ms) {
		const n = Math.min(this.size, 2048);
		const read = this.blob.slice(0, n).arrayBuffer().catch((err) => { throw fileError(err); });
		const ab = await withTimeout(read, ms, () => new DiscError(E_TIMEOUT, 'Reading the disc file took too long.'));
		if (ab.byteLength < n)
			throw new DiscError(E_IO, 'The disc file can no longer be read. It may have been moved, changed or deleted: pick it again.');
	}

	close() {}
}

// An OPFS file read through a FileSystemSyncAccessHandle, straight into the wasm memory.
class AccessHandleSource {
	constructor(handle, access, name) {
		this.kind = 'opfs';
		this.access = access;
		this.name = name || handle.name;
		this.size = access.getSize();
		this.mode = 'sync';
	}

	read(offset, length, request) {
		try {
			return this.access.read(u8.subarray(request.dst, request.dst + length), { at: offset });
		} catch (err) {
			throw fileError(err);
		}
	}

	async probe() {
		let size;
		try {
			size = this.access.getSize();
		} catch (err) {
			throw fileError(err);
		}
		if (size !== this.size)
			throw new DiscError(E_IO, 'The disc file can no longer be read. It may have been moved, changed or deleted: pick it again.');
	}

	close() {
		try {
			this.access.close();
		} catch {}
	}
}

const isRetryableStatus = (s) => s === 408 || s === 429 || s >= 500;
const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function httpError(status, url) {
	if (status === 404 || status === 410)
		return new DiscError(E_IO, `The disc is not on the server any more (HTTP ${status}).`);
	return new DiscError(E_IO, `The server answered HTTP ${status} for ${url}.`);
}

// Checks a 206 answer's Content-Range ("bytes first-last/size") against the request.
function checkContentRange(header, offset) {
	const m = /^bytes (\d+)-(\d+)\/(\d+|\*)$/.exec(header || '');
	if (!m || Number(m[1]) !== offset)
		throw new DiscError(E_IO, 'The server sent a different byte range than the one asked for.');
}

class UrlSource {
	constructor(url, name, size, mode) {
		this.kind = 'url';
		this.url = url;
		this.name = name;
		this.size = size;
		this.mode = mode === 'sync' && FEATURES.syncXhr ? 'sync' : 'async';
	}

	// Byte 0, with a timeout: the size, and whether the server honours ranges.
	static async firstByte(url, ms) {
		const controller = new AbortController();
		let timedOut = false;
		const timer = setTimeout(() => { timedOut = true; controller.abort(); }, ms);
		try {
			return await fetch(url, { headers: { Range: 'bytes=0-0' }, cache: 'no-store', signal: controller.signal });
		} catch (err) {
			if (timedOut)
				throw new DiscError(E_TIMEOUT, 'The disc server did not answer in time.');
			throw new DiscError(E_NETWORK, `The disc server cannot be reached (${err.message || err}).`);
		} finally {
			clearTimeout(timer);
		}
	}

	// Asks for byte 0 to learn the size and to check that the server honours ranges.
	static async open(url, name, mode) {
		const res = await UrlSource.firstByte(url, URL_OPEN_TIMEOUT_MS);
		const m = res.status === 206 ? /^bytes 0-0\/(\d+)$/.exec(res.headers.get('Content-Range') || '') : null;
		if (!m) {
			// Never read the body of any other answer: a server that ignores ranges answers 200 with
			// the whole disc, which arrayBuffer() would download into memory after the error.
			res.body?.cancel().catch(() => {});
			if (res.status === 200)
				throw new DiscError(E_IO, 'The server does not support byte ranges, so the disc cannot be read on demand.');
			if (res.status !== 206)
				throw httpError(res.status, url);
			throw new DiscError(E_IO, 'The server did not report the disc size (Content-Range).');
		}
		await res.arrayBuffer().catch(() => null); // the one byte: finish the request
		const fallback = decodeURIComponent(new URL(url).pathname.split('/').pop() || 'disc');
		return new UrlSource(url, name || fallback, Number(m[1]), mode);
	}

	read(offset, length, request) {
		return this.mode === 'sync' ? this.readSync(offset, length) : this.readAsync(offset, length, request);
	}

	// The server still answers, with the same disc.
	async probe(ms) {
		const res = await UrlSource.firstByte(this.url, ms);
		const m = res.status === 206 ? /^bytes 0-0\/(\d+)$/.exec(res.headers.get('Content-Range') || '') : null;
		if (!m) {
			res.body?.cancel().catch(() => {});
			throw res.status === 206 || res.status === 200
				? new DiscError(E_IO, 'The disc server no longer answers with byte ranges.')
				: httpError(res.status, this.url);
		}
		await res.arrayBuffer().catch(() => null);
		if (Number(m[1]) !== this.size)
			throw new DiscError(E_IO, 'The disc on the server changed since it was opened.');
	}

	readSync(offset, length) {
		for (let attempt = 0; ; attempt++) {
			const xhr = new XMLHttpRequest();
			xhr.open('GET', this.url, false);
			xhr.responseType = 'arraybuffer';
			xhr.timeout = SYNC_URL_TIMEOUT_MS;
			xhr.setRequestHeader('Range', `bytes=${offset}-${offset + length - 1}`);
			let failure;
			try {
				xhr.send();
			} catch (err) {
				if (err?.name === 'TimeoutError')
					throw new DiscError(E_TIMEOUT, 'The disc server did not answer in time.');
				failure = new DiscError(E_NETWORK, 'The connection to the disc server failed.');
			}
			if (!failure) {
				if (xhr.status === 416)
					return new Uint8Array(0);
				if (xhr.status === 206) {
					checkContentRange(xhr.getResponseHeader('Content-Range'), offset);
					return new Uint8Array(xhr.response, 0, Math.min(xhr.response.byteLength, length));
				}
				failure = httpError(xhr.status, this.url);
				if (!isRetryableStatus(xhr.status))
					throw failure;
			}
			if (attempt >= URL_RETRY_DELAYS_MS.length)
				throw failure; // no sleeping on this thread: sync retries go out at once
		}
	}

	async readAsync(offset, length, request) {
		for (let attempt = 0; ; attempt++) {
			const controller = new AbortController();
			let timedOut = false;
			const timer = setTimeout(() => { timedOut = true; controller.abort(); }, URL_TIMEOUT_MS);
			request.abort = () => controller.abort();
			let failure;
			try {
				const res = await fetch(this.url, {
					headers: { Range: `bytes=${offset}-${offset + length - 1}` },
					cache: 'no-store',
					signal: controller.signal,
				});
				if (res.status === 416)
					return new Uint8Array(0);
				if (res.status === 206) {
					try {
						checkContentRange(res.headers.get('Content-Range'), offset);
					} catch (err) {
						res.body?.cancel().catch(() => {});
						throw err;
					}
					const ab = await res.arrayBuffer();
					return new Uint8Array(ab, 0, Math.min(ab.byteLength, length));
				}
				res.body?.cancel().catch(() => {});
				failure = httpError(res.status, this.url);
				if (!isRetryableStatus(res.status))
					throw failure;
			} catch (err) {
				if (err instanceof DiscError)
					throw err;
				if (request.cancelled)
					throw new DiscError(E_CANCELLED, 'Cancelled.');
				failure = timedOut
					? new DiscError(E_TIMEOUT, 'The disc server did not answer in time.')
					: new DiscError(E_NETWORK, `The connection to the disc server failed (${err.message || err}).`);
			} finally {
				clearTimeout(timer);
				request.abort = null;
			}
			if (attempt >= URL_RETRY_DELAYS_MS.length)
				throw failure;
			await delay(URL_RETRY_DELAYS_MS[attempt]);
			if (request.cancelled)
				throw new DiscError(E_CANCELLED, 'Cancelled.');
		}
	}

	close() {}
}

async function openSource(msg) {
	const mode = msg.read === 'sync' || msg.read === 'async' ? msg.read : 'auto';
	if (msg.file instanceof Blob)
		return new BlobSource(msg.file, msg.name, mode === 'auto' ? 'sync' : mode);
	if (msg.handle && typeof msg.handle.getFile === 'function') {
		// OPFS handles open a sync access handle (read-only mode lets other tabs read too, Chrome
		// 121+). Handles from showOpenFilePicker() cannot, and are read as the File they point to.
		if (typeof msg.handle.createSyncAccessHandle === 'function' && mode !== 'async') {
			for (const options of [{ mode: 'read-only' }, undefined]) {
				try {
					const access = await msg.handle.createSyncAccessHandle(options);
					return new AccessHandleSource(msg.handle, access, msg.name);
				} catch {}
			}
		}
		let file;
		try {
			file = await msg.handle.getFile();
		} catch (err) {
			throw err?.name === 'NotAllowedError'
				? new DiscError(E_IO, 'Permission to read the disc file was not granted.')
				: fileError(err);
		}
		return new BlobSource(file, msg.name || file.name, mode === 'auto' ? 'sync' : mode);
	}
	if (typeof msg.url === 'string') {
		let url;
		try {
			url = new URL(msg.url, self.location.href).href;
		} catch {
			throw new DiscError(E_INVALID, 'The disc URL is not valid.');
		}
		return UrlSource.open(url, msg.name, mode === 'auto' ? 'async' : mode);
	}
	throw new DiscError(E_INVALID, 'Nothing to open: pass a file, a handle or a url.');
}

// ------------------------------------------------------------------ serving requests

// Ends a request: the result, then DONE, then a wake-up for the requester sleeping on the state.
function finish(request, result) {
	if (request.done)
		return;
	request.done = true;
	if (inflight[request.slot] === request)
		inflight[request.slot] = null;
	stats.inflight--;
	stats.busyMs += performance.now() - request.started;
	if (result >= 0) {
		stats.reads++;
		stats.bytes += result;
	} else {
		stats.errors++;
		if (result !== E_CANCELLED && result !== E_STALE && result !== E_NO_DISC)
			Atomics.store(i32, hdr + H_ERROR, result);
	}
	refreshViews();
	const base = slotBase(request.slot);
	Atomics.store(i32, base + S_RESULT, result);
	Atomics.store(i32, base + S_STATE, SLOT_DONE);
	Atomics.notify(i32, base + S_STATE);
}

function fail(request, err) {
	if (request.done)
		return;
	const code = err instanceof DiscError ? err.code : E_IO;
	finish(request, code);
	if (code === E_CANCELLED || code === E_STALE || !source || request.source !== source)
		return;
	if (code === E_TIMEOUT || code === E_NETWORK)
		markUnavailable(source, err);
	else
		setState(STATE_ERROR, describe(err));
}

// The source timed out or cannot be reached: fail its requests at once (with the same code) until a
// probe finds it answering again. Without this every read, and PCSX2 retries a failed read up to 256
// times, would wait for the source again: up to the C++ reader's 10 s deadline each, with the
// emulator frozen.
function markUnavailable(src, err) {
	if (src !== source)
		return;
	const first = !src.unavailable;
	src.unavailable = { code: err.code };
	setState(STATE_ERROR, describe(err));
	if (first)
		scheduleProbe(src, PROBE_FIRST_MS);
}

function scheduleProbe(src, ms) {
	setTimeout(async () => {
		if (src !== source || !src.unavailable || state === STATE_STOPPED)
			return;
		let err = null;
		try {
			await src.probe(PROBE_TIMEOUT_MS);
		} catch (e) {
			err = asDiscError(e);
		}
		if (src !== source || !src.unavailable)
			return;
		if (err && (err.code === E_TIMEOUT || err.code === E_NETWORK)) {
			scheduleProbe(src, PROBE_INTERVAL_MS);
			return;
		}
		// Answering again, or failing for a reason that reads will now report themselves.
		src.unavailable = null;
		if (err)
			setState(STATE_ERROR, describe(err));
		else if (state === STATE_ERROR)
			setState(STATE_READY, null);
	}, ms);
}

// Copies what a source returned into the destination (unless the request was cancelled meanwhile)
// and finishes the request. `expected` is already clamped to the disc size, so getting less means
// the disc changed under us: Firefox, for one, answers a File that shrank with short reads, not
// errors.
function complete(request, data, expected) {
	if (request.done)
		return;
	const got = typeof data === 'number' ? data : data.byteLength;
	if (got < expected) {
		fail(request, new DiscError(E_IO, request.source?.kind === 'url'
			? 'The disc on the server changed (it is shorter than when it was opened).'
			: 'The disc file can no longer be read. It may have been moved, changed or deleted: pick it again.'));
		return;
	}
	if (typeof data !== 'number') {
		refreshViews();
		if (request.dst + expected > u8.byteLength) {
			finish(request, E_INVALID);
			return;
		}
		u8.set(got > expected ? data.subarray(0, expected) : data, request.dst);
	}
	finish(request, expected);
	if (request.source === source && source) {
		source.unavailable = null;
		if (state === STATE_ERROR)
			setState(STATE_READY, null);
	}
}

// Validates and starts the request in a slot this worker just moved to BUSY. Returns a promise when
// the read is asynchronous.
function start(slot) {
	// The views were refreshed when the scan began; the memory may have grown since, before this
	// request was posted into the new pages, and the checks below must see the new size.
	refreshViews();
	const base = slotBase(slot);
	const request = {
		slot,
		dst: Atomics.load(i32, base + S_DST) >>> 0,
		length: Atomics.load(i32, base + S_LENGTH) >>> 0,
		offset: (Atomics.load(i32, base + S_OFFSET_LO) >>> 0) + (Atomics.load(i32, base + S_OFFSET_HI) >>> 0) * 2 ** 32,
		generation: Atomics.load(i32, base + S_GENERATION) >>> 0,
		source,
		started: performance.now(),
		done: false,
		cancelled: false,
		abort: null,
	};
	inflight[slot] = request;
	stats.inflight++;
	stats.maxInflight = Math.max(stats.maxInflight, stats.inflight);

	// A request a dead worker handed back after its requester had already given up on it.
	const cancelled = Atomics.load(i32, base + S_CANCEL);
	if (cancelled !== 0)
		return finish(request, cancelled === CANCEL_DEADLINE ? E_TIMEOUT : E_CANCELLED);
	if (!source) // nothing open (an open in progress keeps serving the disc it replaces)
		return finish(request, E_NO_DISC);
	if (request.generation !== generation)
		return finish(request, E_STALE);
	if (request.length > MAX_REQUEST || request.dst === 0 || request.dst + request.length > u8.byteLength ||
		!Number.isSafeInteger(request.offset))
		return finish(request, E_INVALID);
	const length = Math.min(request.length, Math.max(0, source.size - request.offset));
	if (length === 0)
		return finish(request, 0);
	if (source.unavailable)
		return finish(request, source.unavailable.code);

	let data;
	try {
		data = source.read(request.offset, length, request);
	} catch (err) {
		return fail(request, err);
	}
	if (data instanceof Promise)
		return data.then((d) => complete(request, d, length), (err) => fail(request, err));
	complete(request, data, length);
}

// Ends BUSY requests whose requester set S_CANCEL (aborting their fetch). One that ran out of time
// makes its source unavailable.
function checkCancels() {
	refreshViews();
	for (let slot = 0; slot < slotCount; slot++) {
		const request = inflight[slot];
		if (!request || request.done)
			continue;
		const reason = Atomics.load(i32, slotBase(slot) + S_CANCEL);
		if (reason === 0)
			continue;
		request.cancelled = true;
		request.abort?.();
		if (reason !== CANCEL_DEADLINE) {
			finish(request, E_CANCELLED);
			continue;
		}
		finish(request, E_TIMEOUT);
		const src = request.source;
		if (src)
			markUnavailable(src, new DiscError(E_TIMEOUT, src.kind === 'url' ? 'The disc server did not answer in time.'
				: 'Reading the disc file took too long.'));
	}
}

// The loop died: give back the requests this worker took (BUSY -> PENDING), so that the worker the page
// starts next serves them. Their late results are dropped (complete() and fail() skip done requests),
// and a synchronous read cannot be in progress while this runs.
function handBackBusy() {
	refreshViews();
	for (let slot = 0; slot < slotCount; slot++) {
		const request = inflight[slot];
		if (!request || request.done)
			continue;
		request.done = true;
		request.cancelled = true;
		request.abort?.();
		inflight[slot] = null;
		stats.inflight--;
		Atomics.compareExchange(i32, slotBase(slot) + S_STATE, SLOT_BUSY, SLOT_PENDING);
	}
}

// One pass over the slots: takes PENDING requests (CAS, so a requester withdrawing at the same time
// wins or loses cleanly) and handles cancellations of BUSY ones. In blocking mode asynchronous
// reads are awaited one at a time, with a timer watching for their cancellation meanwhile.
async function scan(serial) {
	checkCancels();
	let found = false;
	for (let slot = 0; slot < slotCount; slot++) {
		const base = slotBase(slot);
		if (Atomics.load(i32, base + S_STATE) !== SLOT_PENDING ||
			Atomics.compareExchange(i32, base + S_STATE, SLOT_PENDING, SLOT_BUSY) !== SLOT_PENDING)
			continue;
		found = true;
		const pending = start(slot);
		if (serial && pending) {
			const watch = setInterval(checkCancels, BLOCKING_CANCEL_POLL_MS);
			await pending;
			clearInterval(watch);
		}
	}
	return found;
}

async function serveAsync() {
	while (running) {
		const seen = word(H_DOORBELL);
		await scan(false);
		if (!running)
			break;
		const wait = Atomics.waitAsync(i32, hdr + H_DOORBELL, seen);
		if (wait.async)
			await wait.value;
	}
}

// Fallback without Atomics.waitAsync: block in Atomics.wait, and return to the event loop on every
// timeout or idle wake-up, and at least every BLOCKING_POLL_MS, so page messages still get through.
const yieldChannel = new MessageChannel();
const yieldQueue = [];
yieldChannel.port1.onmessage = () => yieldQueue.shift()?.();
const yieldToEventLoop = () => new Promise((resolve) => {
	yieldQueue.push(resolve);
	yieldChannel.port2.postMessage(0);
});

async function serveBlocking() {
	let lastYield = performance.now();
	while (running) {
		const seen = word(H_DOORBELL);
		const found = await scan(true);
		if (!running)
			break;
		// Yield at least every BLOCKING_POLL_MS even while requests keep coming: synchronous reads
		// never return to the event loop, and a steady stream would otherwise hold page messages
		// (open, stop) back until it ends.
		const left = BLOCKING_POLL_MS - (performance.now() - lastYield);
		if (left > 0 && word(H_DOORBELL) !== seen)
			continue;
		const result = left > 0 ? Atomics.wait(i32, hdr + H_DOORBELL, seen, left) : 'timed-out';
		if (result === 'timed-out' || !found) {
			await yieldToEventLoop();
			lastYield = performance.now();
		}
	}
}

// Fails every PENDING and BUSY request with `code` (they belong to a disc that is going away).
function failAll(code) {
	refreshViews();
	for (let slot = 0; slot < slotCount; slot++) {
		const request = inflight[slot];
		if (request && !request.done) {
			request.cancelled = true;
			request.abort?.();
			finish(request, code);
		}
		const base = slotBase(slot);
		if (Atomics.compareExchange(i32, base + S_STATE, SLOT_PENDING, SLOT_BUSY) === SLOT_PENDING) {
			const stale = { slot, started: performance.now(), done: false };
			stats.inflight++;
			finish(stale, code);
		}
	}
}

function bumpGeneration() {
	generation = (generation + 1) >>> 0 || 1;
	Atomics.store(i32, hdr + H_GENERATION, generation | 0);
}

// Takes the disc away: new requests see NO_DISC (and a new generation) before in-flight ones fail.
function dropSource(code) {
	Atomics.store(i32, hdr + H_STATE, BLOCK_NO_DISC);
	bumpGeneration();
	failAll(code);
	source?.close();
	source = null;
}

// ------------------------------------------------------------------ page messages

// A Web Lock held until this worker ends: the page requests the same name after terminate(), and is
// granted it only once nothing of this worker runs any more, synchronous reads into the memory included.
function holdLifeLock(name) {
	const locks = self.navigator?.locks;
	if (typeof name !== 'string' || !name || !locks || typeof locks.request !== 'function')
		return Promise.resolve(false);
	return new Promise((resolve) => {
		locks.request(name, () => {
			resolve(true);
			return new Promise(() => {});
		}).catch(() => resolve(false));
	});
}

const blockSize = () => (word(H_SIZE_LO) >>> 0) + (word(H_SIZE_HI) >>> 0) * 2 ** 32;

// Takes over the disc a dead worker served: same source, same size, same generation, so the requests
// it left behind (and the emulator's open disc) stay valid. Nothing is published: the block already
// says ready.
async function resume(r) {
	if (word(H_STATE) !== BLOCK_READY || (word(H_GENERATION) >>> 0) !== (r.generation >>> 0) || blockSize() !== r.size)
		throw new DiscError(E_STALE, 'The disc in the request block is not the one to resume.');
	const next = await openSource(r);
	if (next.size !== r.size) {
		next.close();
		throw new DiscError(E_IO, 'The disc changed since it was opened.');
	}
	source = next;
	generation = r.generation >>> 0;
}

async function attach(msg) {
	if (state !== STATE_DETACHED || attaching)
		throw new DiscError(E_INVALID, 'The disc worker is already attached; start a new one.');
	const mem = msg.memory;
	if (mem instanceof WebAssembly.Memory)
		memory = mem;
	else if (typeof SharedArrayBuffer === 'function' && mem instanceof SharedArrayBuffer)
		memory = { buffer: mem };
	else
		throw new DiscError(E_INVALID, 'attach needs the shared WebAssembly.Memory.');
	if (!(memory.buffer instanceof SharedArrayBuffer))
		throw new DiscError(E_INVALID, 'The memory is not shared (is the page cross-origin isolated?).');
	const block = msg.block >>> 0;
	refreshViews();
	if (block === 0 || block % 64 !== 0 || block + HEADER_WORDS * 4 > u8.byteLength)
		throw new DiscError(E_INVALID, 'The request block address is not valid.');
	hdr = block >> 2;
	const slots = word(H_SLOTS) >>> 0;
	const slotBytes = word(H_SLOT_BYTES) >>> 0;
	if (word(H_MAGIC) >>> 0 !== BLOCK_MAGIC || word(H_VERSION) !== BLOCK_VERSION)
		throw new DiscError(E_INVALID, 'The request block was not set up (magic or version mismatch).');
	if (slots < 1 || slots > MAX_SLOTS || slotBytes < MIN_SLOT_WORDS * 4 || slotBytes % 4 !== 0 ||
		block + HEADER_WORDS * 4 + slots * slotBytes > u8.byteLength)
		throw new DiscError(E_INVALID, 'The request block layout is not valid.');
	slotCount = slots;
	slotWords = slotBytes / 4;
	inflight.length = slotCount;
	inflight.fill(null);

	attaching = true;
	let resumeError = null;
	try {
		// Nothing below writes into the memory before the lock is held.
		lockHeld = await holdLifeLock(msg.lock);
		refreshViews();
		Atomics.store(i32, hdr + H_MAX_REQUEST, MAX_REQUEST);
		if (msg.resume) {
			try {
				await resume(msg.resume);
			} catch (err) {
				resumeError = asDiscError(err);
			}
		}
		if (!source) {
			generation = word(H_GENERATION) >>> 0;
			Atomics.store(i32, hdr + H_ERROR, 0);
			Atomics.store(i32, hdr + H_STATE, BLOCK_NO_DISC);
			bumpGeneration();
		}
	} finally {
		attaching = false;
	}

	loopMode = msg.loop === 'blocking' || !FEATURES.waitAsync ? 'blocking' : 'async';
	running = true;
	setState(source ? STATE_READY : STATE_IDLE, resumeError ? describe(resumeError) : null);
	(loopMode === 'async' ? serveAsync() : serveBlocking()).catch(loopDied);
}

// If the loop ever dies, this worker serves nothing more. It hands back what it took and says
// 'stopped'; the page then terminates it and starts a worker that resumes the disc. The block stays
// ready meanwhile, so requesters wait (each at most its 10 s deadline) rather than fail at once, which
// would end in a read error for the game: PCSX2's retries take far less than a restart.
function loopDied(err) {
	if (state === STATE_STOPPED)
		return;
	running = false;
	openSeq++;
	try {
		handBackBusy();
	} finally {
		setState(STATE_STOPPED, describe(err));
	}
}

// The disc being served (if any) stays in service while the new one opens: a failed open must not
// leave the emulator without a readable disc. Once the new source is open, the switch is one
// synchronous step: the old disc's requests fail STALE, and readers that check the block (WebDisc::Query)
// see a new generation on each side of the size, so a ready state between two equal generations always
// comes with its own size.
async function open(msg) {
	if (state === STATE_DETACHED || state === STATE_STOPPED || attaching)
		throw new DiscError(E_INVALID, 'Attach the disc worker to the emulator first.');
	const seq = ++openSeq;
	const previous = state;
	setState(STATE_OPENING);
	let next;
	try {
		next = await openSource(msg);
	} catch (err) {
		if (seq === openSeq && state === STATE_OPENING)
			setState(source ? (previous === STATE_ERROR ? STATE_ERROR : STATE_READY) : STATE_IDLE, describe(err));
		throw err;
	}
	if (seq !== openSeq || state === STATE_STOPPED) {
		next.close();
		throw new DiscError(E_CANCELLED, 'A newer request replaced this one before the disc opened.');
	}
	dropSource(E_STALE);
	source = next;
	Atomics.store(i32, hdr + H_SIZE_LO, (source.size % 2 ** 32) | 0);
	Atomics.store(i32, hdr + H_SIZE_HI, Math.floor(source.size / 2 ** 32) | 0);
	Atomics.store(i32, hdr + H_ERROR, 0);
	bumpGeneration();
	Atomics.store(i32, hdr + H_STATE, BLOCK_READY);
	setState(STATE_READY, null);
}

function close() {
	if (state === STATE_DETACHED || state === STATE_STOPPED)
		return;
	openSeq++;
	dropSource(E_NO_DISC);
	setState(STATE_IDLE, null);
}

// Reads the start of the disc now. The page asks before every boot, so that a disc that went away
// is reported on the slate instead of hanging the boot in PCSX2's disc detection.
async function check() {
	if (state === STATE_DETACHED || state === STATE_STOPPED || attaching)
		throw new DiscError(E_INVALID, 'Attach the disc worker to the emulator first.');
	const src = source;
	if (!src)
		throw new DiscError(E_NO_DISC, 'No disc is open.');
	try {
		await src.probe(CHECK_TIMEOUT_MS);
	} catch (e) {
		const err = asDiscError(e);
		if (src === source) {
			if (err.code === E_TIMEOUT || err.code === E_NETWORK)
				markUnavailable(src, err);
			else
				setState(STATE_ERROR, describe(err));
		}
		throw err;
	}
	if (src === source) {
		src.unavailable = null;
		if (state === STATE_ERROR)
			setState(STATE_READY, null);
	}
}

// Stops serving for good: pending and busy requests fail CANCELLED, and the block says stopped, so
// requesters never wait on it again.
function shutDown(error) {
	if (state === STATE_STOPPED)
		return;
	running = false;
	if (state !== STATE_DETACHED) {
		openSeq++;
		try {
			dropSource(E_CANCELLED);
		} finally {
			Atomics.store(i32, hdr + H_STATE, BLOCK_STOPPED);
			Atomics.add(i32, hdr + H_DOORBELL, 1);
			Atomics.notify(i32, hdr + H_DOORBELL); // ends the loop's wait
		}
	}
	setState(STATE_STOPPED, error);
}

function stop() {
	shutDown(undefined);
}

self.onmessage = async (event) => {
	const msg = event.data || {};
	let error = null;
	try {
		switch (msg.type) {
			case 'attach': await attach(msg); break;
			case 'open': await open(msg); break;
			case 'close': close(); break;
			case 'check': await check(); break;
			case 'stop': stop(); break;
			case 'status': break;
			default: throw new DiscError(E_INVALID, `Unknown message type '${msg.type}'.`);
		}
	} catch (err) {
		error = describe(err);
	}
	if (msg.id !== undefined)
		self.postMessage({ type: 'reply', id: msg.id, ok: !error, error, status: snapshot() });
};

postStatus();
