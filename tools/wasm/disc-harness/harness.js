// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Disc harness: drives pcsx2-wasm/web/disc-worker.js the way the emulator will, without the
// emulator. A shared WebAssembly.Memory stands in for the wasm heap, requester workers (requester.js,
// or requester.wasm when built) stand in for PCSX2's disc reader thread, and this page plays the
// TabStationToo page: it only posts messages and grows the memory. See docs/web-port/DISC-DESIGN.md.
//
// Serve the repository root so the page can load the worker, and name the disc with --disc:
//   python3 tools/wasm/serve.py --port 8110 --disc <image> .
//   http://127.0.0.1:8110/tools/wasm/disc-harness/index.html?disc=/dev-disc/<name>
// Everything is reachable from the console as window.discHarness; results land in .results.

(function () {
	'use strict';

	const params = new URLSearchParams(location.search);
	const DISC_WORKER_URL = new URL('../../../pcsx2-wasm/web/disc-worker.js', location.href).href;

	const KB = 1024;
	const MB = 1024 * KB;
	const WASM_PAGE = 64 * KB;
	const INITIAL_PAGES = 1280; // 80 MiB (room for every requester's destination), grown during the tests
	const MAXIMUM_PAGES = 32768; // 2 GiB, as the emulator's ALLOW_MEMORY_GROWTH limit
	const SCRATCH = 0x1000; // 8 bytes for requester.wasm's disc_open()
	const BLOCK = 0x100000;
	const SLOTS = 8;
	const SLOT_BYTES = 32;
	const DST_BASE = 0x200000;
	const DST_STRIDE = 0x1200000; // 18 MiB per requester: room for a 16 MiB request
	const REQUESTERS = 4;
	const BLOCK_MAGIC = 0x4b534454;
	const OPFS_NAME = 'disc-harness.iso';
	const SYNTHETIC_SIZE = 64 * MB + 1000; // not a multiple of 2048, for the short read at the end

	const E = { IO: -1, INVALID: -2, NO_DISC: -3, STALE: -4, CANCELLED: -5, NETWORK: -6, TIMEOUT: -7 };

	const $ = (sel) => document.querySelector(sel);
	const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
	const hex = (buf) => Array.from(new Uint8Array(buf), (b) => b.toString(16).padStart(2, '0')).join('');

	// ------------------------------------------------------------------ log and results

	const results = [];

	function log(text) {
		const line = `[${(performance.now() / 1000).toFixed(2)}] ${text}`;
		const pre = $('#log');
		pre.textContent += line + '\n';
		pre.scrollTop = pre.scrollHeight;
		console.log(line);
	}

	function record(test, ctx, pass, detail) {
		const row = { test, source: ctx.source || '', read: ctx.read || '', impl: ctx.impl || '', loop: ctx.loop || '', pass, detail };
		results.push(row);
		const tr = document.createElement('tr');
		tr.className = pass === true ? 'pass' : pass === false ? 'fail' : 'info';
		for (const v of [test, row.source, row.read, row.impl, row.loop, pass === true ? 'pass' : pass === false ? 'FAIL' : '', JSON.stringify(detail)]) {
			const td = document.createElement('td');
			td.textContent = v;
			tr.appendChild(td);
		}
		$('#results tbody').appendChild(tr);
		log(`${pass === false ? 'FAIL' : pass === true ? 'pass' : 'info'} ${test} ${row.source} ${row.read} ${JSON.stringify(detail)}`);
		return pass;
	}

	// ------------------------------------------------------------------ workers

	class DiscWorker {
		constructor() {
			this.worker = new Worker(DISC_WORKER_URL);
			this.seq = 0;
			this.waiting = new Map();
			this.statuses = [];
			this.status = null;
			this.worker.onmessage = (e) => {
				const m = e.data;
				if (m.type === 'status') {
					this.status = m.status;
					this.statuses.push({ t: performance.now(), state: m.status.state, error: m.status.error });
				} else if (m.type === 'reply') {
					this.status = m.status;
					this.waiting.get(m.id)?.(m);
					this.waiting.delete(m.id);
				}
			};
			this.worker.onerror = (e) => log(`disc worker error: ${e.message}`);
		}

		call(msg) {
			const id = ++this.seq;
			return new Promise((resolve) => {
				this.waiting.set(id, resolve);
				this.worker.postMessage({ ...msg, id });
			});
		}

		terminate() {
			this.worker.terminate();
		}
	}

	class Requester {
		constructor(index) {
			this.dst = DST_BASE + index * DST_STRIDE;
			this.worker = new Worker('requester.js');
			this.seq = 0;
			this.waiting = new Map();
			this.worker.onmessage = (e) => {
				const m = e.data;
				const w = this.waiting.get(m.id);
				this.waiting.delete(m.id);
				if (m.ok) w.resolve(m.value);
				else w.reject(new Error(m.error));
			};
		}

		call(cmd, args = {}) {
			const id = ++this.seq;
			return new Promise((resolve, reject) => {
				this.waiting.set(id, { resolve, reject });
				this.worker.postMessage({ dst: this.dst, ...args, cmd, id });
			});
		}

		terminate() {
			this.worker.terminate();
		}
	}

	class OpfsTool {
		constructor() {
			this.worker = new Worker('opfs-tool.js');
			this.seq = 0;
			this.waiting = new Map();
			this.onprogress = null;
			this.worker.onmessage = (e) => {
				const m = e.data;
				if (m.progress !== undefined) {
					this.onprogress?.(m);
					return;
				}
				const w = this.waiting.get(m.id);
				this.waiting.delete(m.id);
				if (m.ok) w.resolve(m.value);
				else w.reject(new Error(m.error));
			};
		}

		call(cmd, args = {}) {
			const id = ++this.seq;
			return new Promise((resolve, reject) => {
				this.waiting.set(id, { resolve, reject });
				this.worker.postMessage({ ...args, cmd, id });
			});
		}
	}

	// ------------------------------------------------------------------ setup

	const h = {
		memory: null,
		disc: null,
		requesters: [],
		opfs: null,
		wasmModule: null,
		impl: params.get('impl') === 'wasm' ? 'wasm' : 'js',
		loop: params.get('loop') === 'blocking' ? 'blocking' : 'auto',
		discUrl: params.get('disc') ? new URL(params.get('disc'), location.href).href : null,
		discSize: 0,
		picked: null,
		discBlob: null, // the whole disc fetched into a Blob (Chrome keeps large blobs on disk)
		synthetic: null,
		generation: 0,
		results,
		log,
	};

	function initBlock(memory) {
		const i32 = new Int32Array(memory.buffer);
		const w = BLOCK >> 2;
		for (let k = 0; k < (64 + SLOTS * SLOT_BYTES) / 4; k++)
			Atomics.store(i32, w + k, 0);
		Atomics.store(i32, w + 0, BLOCK_MAGIC | 0);
		Atomics.store(i32, w + 1, 1);
		Atomics.store(i32, w + 2, SLOTS);
		Atomics.store(i32, w + 3, SLOT_BYTES);
	}

	async function loadWasmModule() {
		if (h.wasmModule)
			return h.wasmModule;
		try {
			const res = await fetch('requester.wasm', { cache: 'no-store' });
			if (res.ok)
				h.wasmModule = await WebAssembly.compile(await res.arrayBuffer());
		} catch {}
		return h.wasmModule;
	}

	// Fresh memory, block, disc worker and requesters.
	async function setup(options = {}) {
		teardown();
		h.impl = options.impl || h.impl;
		h.loop = options.loop || h.loop;
		h.memory = new WebAssembly.Memory({ initial: INITIAL_PAGES, maximum: MAXIMUM_PAGES, shared: true });
		initBlock(h.memory);
		h.disc = new DiscWorker();
		const attached = await h.disc.call({ type: 'attach', memory: h.memory, block: BLOCK, loop: h.loop === 'blocking' ? 'blocking' : 'auto' });
		if (!attached.ok)
			throw new Error(`attach failed: ${attached.error.message}`);
		const module = h.impl === 'wasm' ? await loadWasmModule() : null;
		if (h.impl === 'wasm' && !module)
			throw new Error('requester.wasm is missing: run tools/wasm/disc-harness/build-requester.sh');
		h.requesters = [];
		for (let k = 0; k < REQUESTERS; k++) {
			const r = new Requester(k);
			await r.call('init', { memory: h.memory, block: BLOCK, scratch: SCRATCH, wasmModule: module });
			h.requesters.push(r);
		}
		return { loop: attached.status.loop, features: attached.status.features, impl: h.impl };
	}

	function teardown() {
		h.disc?.terminate();
		h.disc = null;
		for (const r of h.requesters)
			r.terminate();
		h.requesters = [];
	}

	function syntheticBlob() {
		if (!h.synthetic) {
			const words = new Uint32Array(Math.ceil(SYNTHETIC_SIZE / 4));
			for (let i = 0; i < words.length; i++)
				words[i] = Math.imul(i + 1, 2654435761);
			h.synthetic = new Blob([new Uint8Array(words.buffer, 0, SYNTHETIC_SIZE)]);
		}
		return h.synthetic;
	}

	async function opfsHandle() {
		const root = await navigator.storage.getDirectory();
		return root.getFileHandle(OPFS_NAME);
	}

	// Opens a source in the disc worker. kind: url | blob | opfs-file | opfs-sah | picked | disc-blob.
	async function open(kind, read = 'auto') {
		let msg;
		if (kind === 'url')
			msg = { url: h.discUrl };
		else if (kind === 'blob')
			msg = { file: syntheticBlob(), name: 'synthetic' };
		else if (kind === 'opfs-file')
			msg = { file: await (await opfsHandle()).getFile() };
		else if (kind === 'opfs-sah')
			msg = { handle: await opfsHandle() };
		else if (kind === 'picked')
			msg = { file: h.picked };
		else if (kind === 'disc-blob')
			msg = { file: h.discBlob, name: 'disc (fetched Blob)' };
		else
			msg = kind; // a raw message, for error tests
		const reply = await h.disc.call({ type: 'open', read, ...msg });
		if (reply.ok) {
			const info = await h.requesters[0].call('open', { impl: h.impl });
			h.generation = info.generation;
			return { ...reply, info };
		}
		return reply;
	}

	const req = (k = 0) => h.requesters[k];
	const read = (args, k = 0) => req(k).call('read', { impl: h.impl, generation: h.generation, ...args });

	async function referenceHash(offset, length) {
		const res = await fetch(h.discUrl, { headers: { Range: `bytes=${offset}-${offset + length - 1}` }, cache: 'no-store' });
		const buf = await res.arrayBuffer();
		return { status: res.status, length: buf.byteLength, sha256: hex(await crypto.subtle.digest('SHA-256', buf)) };
	}

	async function urlSize() {
		const res = await fetch(h.discUrl, { method: 'HEAD', cache: 'no-store' });
		return Number(res.headers.get('Content-Length'));
	}

	// ------------------------------------------------------------------ tests

	async function testEnvironment() {
		const est = await navigator.storage.estimate();
		const detail = {
			userAgent: navigator.userAgent,
			crossOriginIsolated: self.crossOriginIsolated,
			waitAsync: typeof Atomics.waitAsync,
			fileReaderSyncOnPage: typeof FileReaderSync, // WORKERFS needs it on the FS thread (the page)
			memoryBufferStable: (() => { const m = new WebAssembly.Memory({ initial: 1, maximum: 2, shared: true }); return m.buffer === m.buffer; })(),
			showOpenFilePicker: typeof self.showOpenFilePicker,
			opfsQuotaGB: +(est.quota / 2 ** 30).toFixed(1),
			opfsUsageMB: +(est.usage / MB).toFixed(1),
		};
		return record('environment', {}, self.crossOriginIsolated, detail);
	}

	// The real disc: sector 16 holds the ISO 9660 primary volume descriptor; ranges at the start,
	// middle and end must hash like the server's own answer (and curl's).
	async function testIso(kind, readMode, expect = {}) {
		const ctx = { source: kind, read: readMode, impl: h.impl, loop: h.loop };
		const opened = await open(kind, readMode);
		if (!opened.ok)
			return record('iso-open', ctx, false, opened.error);
		const size = opened.info.size;
		let expectedSize = h.discUrl ? await urlSize() : size;
		// A partial OPFS copy (importToOpfs(limit)) is checked against the URL up to its own size.
		const partial = kind.startsWith('opfs') && size < expectedSize;
		if (partial)
			expectedSize = (await (await opfsHandle()).getFile()).size;
		record('iso-open', ctx, size === expectedSize && size === opened.status.disc.size,
			{ size, expectedSize, partial, generation: opened.info.generation, read: opened.status.disc.read, kind: opened.status.disc.kind });

		const pvd = await read({ length: 2048, offset: 16 * 2048, bytes: 6 });
		const tag = String.fromCharCode(...pvd.bytes.slice(1, 6));
		record('iso-pvd', ctx, pvd.result === 2048 && pvd.bytes[0] === 1 && tag === 'CD001', { result: pvd.result, type: pvd.bytes[0], tag, ms: +pvd.ms.toFixed(3) });

		const ranges = [
			['start-64K', 0, 64 * KB],
			['sector16', 32768, 2048],
			['middle-128K', 1600000000, 128 * KB],
			['odd-offset', 3000000001, 4097],
			['last-2048', size - 2048, 2048],
			['past-end', size - 1000, 2048],
		].filter(([, off]) => off < size);
		for (const [name, offset, length] of ranges) {
			const got = await read({ length, offset, hash: true });
			const want = Math.min(length, size - offset);
			const ref = h.discUrl ? await referenceHash(offset, want) : null;
			const pass = got.result === want && (!ref || ref.sha256 === got.sha256) &&
				(partial || !expect[name] || got.sha256.startsWith(expect[name]));
			record('iso-range', ctx, pass, { name, offset, length, result: got.result, sha256: got.sha256.slice(0, 16),
				server: ref?.sha256.slice(0, 16), curl: partial ? null : expect[name] || null });
		}
		const atEnd = await read({ length: 2048, offset: size });
		const pastEnd = await read({ length: 2048, offset: size + 4096 });
		record('iso-eof', ctx, atEnd.result === 0 && pastEnd.result === 0, { atEnd: atEnd.result, pastEnd: pastEnd.result });
		return true;
	}

	// The synthetic Blob: every byte is checked, including the short read at its end.
	async function testPattern(readMode, count = 300) {
		const ctx = { source: 'blob', read: readMode, impl: h.impl, loop: h.loop };
		const opened = await open('blob', readMode);
		if (!opened.ok)
			return record('pattern-open', ctx, false, opened.error);
		const size = opened.info.size;
		const r = await req(0).call('bench', { impl: h.impl, generation: h.generation, size, length: 64 * KB + 7, count, align: 1, verifyPattern: true, seed: 7 });
		record('pattern-random', ctx, r.errors === 0 && r.mismatches === 0, { reads: r.count, mismatches: r.mismatches, errors: r.errors, medianMs: r.medianMs });
		const tail = await read({ length: 4096, offset: size - 1000, pattern: true });
		record('pattern-short-read', ctx, tail.result === 1000 && tail.mismatchAt === -1, { result: tail.result, mismatchAt: tail.mismatchAt });
		return true;
	}

	// Memory growth while reads are in flight, then a read into the new pages.
	async function testGrowth(kind, readMode) {
		const ctx = { source: kind, read: readMode, impl: h.impl, loop: h.loop };
		const opened = await open(kind, readMode);
		if (!opened.ok)
			return record('growth-open', ctx, false, opened.error);
		const size = opened.info.size;
		const before = h.memory.buffer.byteLength;
		const background = req(1).call('bench', { impl: h.impl, generation: h.generation, size, length: 64 * KB, count: 400, seed: 3,
			verifyPattern: kind === 'blob' });
		await sleep(30);
		h.memory.grow(1024); // +64 MiB
		const after = h.memory.buffer.byteLength;
		const bg = await background;
		const low = await read({ length: 64 * KB, offset: 12345 * 2048, hash: true });
		const highDst = before + 8 * MB; // only reachable after the growth
		const high = await read({ dst: highDst, length: 64 * KB, offset: 12345 * 2048, hash: true });
		const info = await req(2).call('bufferInfo');
		return record('growth', ctx, bg.errors === 0 && bg.mismatches === 0 && high.result === 64 * KB && high.sha256 === low.sha256,
			{ before, after, readsDuringGrowth: bg.count, errors: bg.errors, mismatches: bg.mismatches, highDst, high: high.result,
				sameHash: high.sha256 === low.sha256, requesterSeesBytes: info.byteLength });
	}

	// Four requesters at once (the reader thread plus, say, a game-list scan), each verified.
	async function testConcurrency(kind, readMode, count = 100) {
		const ctx = { source: kind, read: readMode, impl: h.impl, loop: h.loop };
		const opened = await open(kind, readMode);
		if (!opened.ok)
			return record('concurrency-open', ctx, false, opened.error);
		const size = opened.info.size;
		if (kind === 'blob') {
			const t0 = performance.now();
			const all = await Promise.all(h.requesters.map((r, k) => r.call('bench', { impl: h.impl, generation: h.generation, size, length: 32 * KB + 3, count, align: 1, seed: 100 + k, verifyPattern: true })));
			const ms = performance.now() - t0;
			const bad = all.reduce((a, r) => a + r.errors + r.mismatches, 0);
			return record('concurrency', ctx, bad === 0, { requesters: all.length, readsEach: count, bad, wallMs: +ms.toFixed(1), maxInflight: h.disc.status?.stats.maxInflight });
		}
		// Real disc: the same ranges read by one requester, then by four in parallel, must hash alike.
		const ranges = [];
		for (let k = 0; k < 24; k++)
			ranges.push([Math.floor(((k * 2654435761) % 1e9) / 1e9 * (size - MB) / 2048) * 2048, 256 * KB]);
		const reference = await req(0).call('hashRanges', { impl: h.impl, generation: h.generation, ranges });
		const t0 = performance.now();
		const all = await Promise.all(h.requesters.map((r, k) => r.call('hashRanges', {
			impl: h.impl, generation: h.generation, ranges: k % 2 ? [...ranges].reverse() : ranges })));
		const ms = performance.now() - t0;
		const want = new Map(reference.map((x) => [x.offset, x.sha256]));
		const bad = all.flat().filter((x) => x.result !== 256 * KB || want.get(x.offset) !== x.sha256).length;
		await h.disc.call({ type: 'status' });
		return record('concurrency', ctx, bad === 0, { requesters: all.length, readsEach: ranges.length, bad, wallMs: +ms.toFixed(1),
			mbPerS: +((all.length * ranges.length * 256 * KB) / MB / (ms / 1000)).toFixed(1), maxInflight: h.disc.status.stats.maxInflight });
	}

	// Requests the worker must refuse, cancellation, stop, and a changed file.
	async function testErrors() {
		const ctx = { impl: h.impl, loop: h.loop };
		const blobOpen = await open('blob');
		const size = blobOpen.info.size;
		const memEnd = h.memory.buffer.byteLength;
		const nullDst = await read({ dst: 0, length: 2048, offset: 0 });
		record('error-null-destination', { ...ctx, source: 'blob' }, nullDst.result === E.INVALID, { result: nullDst.result });
		const outside = await read({ dst: memEnd - 1024, length: 2048, offset: 0 });
		record('error-destination-outside-memory', { ...ctx, source: 'blob' }, outside.result === E.INVALID, { result: outside.result });
		const tooLong = await read({ length: 16 * MB + 1, offset: 0 });
		record('error-request-too-long', { ...ctx, source: 'blob' }, tooLong.result === E.INVALID, { result: tooLong.result });
		const stale = await read({ length: 2048, offset: 0, generation: h.generation + 1000 });
		record('error-stale-generation', { ...ctx, source: 'blob' }, stale.result === E.STALE, { result: stale.result });
		const oldGen = h.generation;
		await h.disc.call({ type: 'close' });
		const closed = await read({ length: 2048, offset: 0, generation: oldGen });
		record('error-no-disc', { ...ctx, source: 'blob' }, closed.result === E.NO_DISC, { result: closed.result, state: h.disc.status.state });
		await open('blob');
		const reopened = await read({ length: 2048, offset: 0, generation: oldGen });
		record('error-stale-after-reopen', { ...ctx, source: 'blob' }, reopened.result === E.STALE && h.generation !== oldGen,
			{ result: reopened.result, oldGeneration: oldGen, newGeneration: h.generation });
		const zero = await read({ length: 0, offset: 0 });
		record('zero-length', { ...ctx, source: 'blob' }, zero.result === 0, { result: zero.result });
		void size;

		// An open that fails keeps the disc that was open (a failed swap must not leave the emulator
		// without a readable disc).
		const keptGen = h.generation;
		const badOpen = await h.disc.call({ type: 'open' });
		const stillThere = await read({ length: 2048, offset: 16 * 2048, generation: keptGen });
		record('failed-open-keeps-disc', { ...ctx, source: 'blob' }, !badOpen.ok && badOpen.status.state === 'ready' &&
			stillThere.result === 2048 && badOpen.status.disc?.generation === keptGen,
			{ error: badOpen.error, state: badOpen.status.state, read: stillThere.result });

		if (h.discUrl) {
			const hadDisc = !!h.disc.status.disc;
			const notFound = await h.disc.call({ type: 'open', url: new URL('/dev-disc/not-there.iso', location.href).href });
			record('error-url-404', { ...ctx, source: 'url' }, !notFound.ok && notFound.status.state === (hadDisc ? 'ready' : 'idle'), { error: notFound.error });
			// An open overtaken by a newer one (the URL needs a request, the Blob opens at once) fails rather
			// than replying ok.
			const first = h.disc.call({ type: 'open', url: h.discUrl });
			const second = h.disc.call({ type: 'open', file: syntheticBlob(), name: 'synthetic' });
			const [firstReply, secondReply] = await Promise.all([first, second]);
			record('superseded-open-fails', { ...ctx, source: 'url' }, !firstReply.ok && firstReply.error?.code === 'CANCELLED' &&
				secondReply.ok && firstReply.status.disc?.name === 'synthetic',
				{ first: firstReply.error, second: secondReply.ok, disc: firstReply.status.disc?.name });
			const refused = await h.disc.call({ type: 'open', url: 'http://127.0.0.1:8119/nothing.iso' });
			record('error-url-unreachable', { ...ctx, source: 'url' }, !refused.ok && refused.error.code === 'NETWORK', { error: refused.error });
			const notRanged = await h.disc.call({ type: 'open', url: new URL('index.html', location.href).href });
			record('url-small-file', { ...ctx, source: 'url' }, notRanged.ok, { ok: notRanged.ok, size: notRanged.status.disc?.size });

			// Cancel a slow (16 MiB) in-flight URL read from the requester side.
			await open('url');
			const t0 = performance.now();
			const cancelled = await read({ length: 16 * MB, offset: 100 * MB, cancelAfterMs: 2 });
			record('cancel-busy-url-read', { ...ctx, source: 'url' }, cancelled.result === E.CANCELLED,
				{ result: cancelled.result, ms: +(performance.now() - t0).toFixed(1) });
			const after = await read({ length: 2048, offset: 16 * 2048, bytes: 6 });
			record('read-after-cancel', { ...ctx, source: 'url' }, after.result === 2048, { result: after.result });

			// Stop while a read is in flight: the requester wakes with CANCELLED, later reads fail fast.
			const pending = read({ length: 16 * MB, offset: 300 * MB }, 1);
			await sleep(3);
			const t1 = performance.now();
			const stopped = await h.disc.call({ type: 'stop' });
			const inflight = await pending;
			const afterStop = await read({ length: 2048, offset: 0 });
			record('stop-in-flight', { ...ctx, source: 'url' }, stopped.ok && inflight.result === E.CANCELLED && afterStop.result === E.CANCELLED,
				{ inflight: inflight.result, afterStop: afterStop.result, afterStopMs: +afterStop.ms.toFixed(3), state: stopped.status.state,
					stopToWakeMs: +(performance.now() - t1).toFixed(1) });
			await setup({});
		}

		// A File whose file shrinks, then disappears, after it was picked (an OPFS file of synthetic
		// bytes stands in for the user's file). Chrome then fails the reads (NotReadableError,
		// NotFoundError); the worker answers IO and reports the 'error' state.
		h.opfs ||= new OpfsTool();
		for (const readMode of ['sync', 'async']) {
			const name = 'disc-harness-changing.bin';
			const ctxF = { ...ctx, source: 'opfs-file', read: readMode };
			await h.opfs.call('makeSynthetic', { name, size: 4 * MB });
			const root = await navigator.storage.getDirectory();
			const file = await (await root.getFileHandle(name)).getFile();
			await open({ file, name }, readMode);
			const ok = await read({ length: 64 * KB, offset: 2 * MB });
			await h.opfs.call('truncate', { name, size: MB });
			const shrunk = await read({ length: 64 * KB, offset: 2 * MB });
			await sleep(20);
			const st = h.disc.status;
			record('error-file-shrunk', ctxF, ok.result === 64 * KB && shrunk.result === E.IO && st.state === 'error',
				{ before: ok.result, after: shrunk.result, state: st.state, error: st.error });
			await h.opfs.call('remove', { name });
			const gone = await read({ length: 2048, offset: 0 });
			record('error-file-deleted', ctxF, gone.result === E.IO && h.disc.status.state === 'error',
				{ result: gone.result, state: h.disc.status.state, error: h.disc.status.error });
			const checked = await h.disc.call({ type: 'check' });
			record('check-file-gone', ctxF, !checked.ok && checked.error?.code === 'IO' && checked.status.state === 'error',
				{ ok: checked.ok, error: checked.error, state: checked.status.state });
			await open('blob', readMode);
			const again = await read({ length: 2048, offset: 0 });
			record('recover-after-new-open', ctxF, again.result === 2048 && h.disc.status.state === 'ready' && !h.disc.status.error,
				{ result: again.result, state: h.disc.status.state });
		}
		return true;
	}

	// A request whose requester ran out of time (S_CANCEL = 2, as the C++ reader's deadline does) is
	// answered TIMEOUT and makes the source unavailable: the next reads fail at once instead of waiting,
	// until a probe finds the source answering again (here it always answers, so after about 1 s).
	async function testUnavailable() {
		if (!h.discUrl)
			return;
		const ctx = { source: 'url', read: 'async', impl: h.impl, loop: h.loop };
		await open('url');
		const timedOut = await read({ length: 16 * MB, offset: 200 * MB, cancelAfterMs: 20, cancelValue: 2 });
		await sleep(20);
		const st = h.disc.status;
		record('deadline-cancel-times-out', ctx, timedOut.result === E.TIMEOUT && st.state === 'error' && st.error?.code === 'TIMEOUT',
			{ result: timedOut.result, state: st.state, error: st.error });
		const fast = await read({ length: 2048, offset: 16 * 2048 });
		record('unavailable-fails-fast', ctx, fast.result === E.TIMEOUT && fast.ms < 50, { result: fast.result, ms: +fast.ms.toFixed(2) });
		const t0 = performance.now();
		let recovered = false;
		while (performance.now() - t0 < 6000) {
			await sleep(100);
			await h.disc.call({ type: 'status' });
			if (h.disc.status.state === 'ready') {
				recovered = true;
				break;
			}
		}
		const after = await read({ length: 2048, offset: 16 * 2048, bytes: 6 });
		record('unavailable-recovers', ctx, recovered && after.result === 2048 && !h.disc.status.error,
			{ afterMs: Math.round(performance.now() - t0), result: after.result, state: h.disc.status.state });
		const check = await h.disc.call({ type: 'check' });
		record('check-ok', ctx, check.ok && check.status.state === 'ready', { ok: check.ok, error: check.error });
	}

	// The page's side of a worker restart (app.js onDiscWorkerDied): CAS BUSY -> PENDING in every slot.
	function handBack() {
		const i32 = new Int32Array(h.memory.buffer);
		let n = 0;
		for (let s = 0; s < SLOTS; s++) {
			if (Atomics.compareExchange(i32, (BLOCK >> 2) + 16 + s * (SLOT_BYTES >> 2), 3, 2) === 3)
				n++;
		}
		return n;
	}

	function slotStates() {
		const i32 = new Int32Array(h.memory.buffer);
		return Array.from({ length: SLOTS }, (_, s) => Atomics.load(i32, (BLOCK >> 2) + 16 + s * (SLOT_BYTES >> 2)));
	}

	const lockName = () => `disc-harness-${Date.now()}-${Math.random().toString(36).slice(2)}`;

	// Resolves with the ms it took to be granted the lock a terminated worker held.
	async function whenGone(name, t0) {
		const granted = navigator.locks.request(name, () => performance.now() - t0);
		return Promise.race([granted, sleep(10000).then(() => { throw new Error(`the lock ${name} was not released`); })]);
	}

	async function attachWorker(lock, resume) {
		h.disc = new DiscWorker();
		const msg = { type: 'attach', memory: h.memory, block: BLOCK, loop: h.loop === 'blocking' ? 'blocking' : 'auto', lock };
		if (resume)
			msg.resume = resume;
		return h.disc.call(msg);
	}

	// A worker that dies is replaced by one that resumes its disc under the same generation, once the page
	// knows the old one is gone (it was granted the lock the old one held): requests posted meanwhile, and
	// ones the dead worker had taken (handed back), are served by the new one.
	async function testRestart() {
		const ctx = { impl: h.impl, loop: h.loop };
		h.disc.terminate();
		let lock = lockName();
		const attached = await attachWorker(lock);
		record('life-lock-held', ctx, attached.ok && attached.status.lock === true, { lock: attached.status.lock });

		const opened = await open('blob');
		const gen = h.generation;
		const size = opened.info.size;
		let t0 = performance.now();
		h.disc.terminate();
		const waiting = read({ length: 64 * KB + 7, offset: 5 * MB + 3, pattern: true, generation: gen });
		const goneMs = await whenGone(lock, t0);
		record('life-lock-released-on-terminate', ctx, goneMs < 2000, { ms: +goneMs.toFixed(1) });
		const handed = handBack();
		lock = lockName();
		const resumed = await attachWorker(lock, { file: syntheticBlob(), name: 'synthetic', generation: gen, size });
		const got = await waiting;
		const again = await read({ length: 2048, offset: 0, generation: gen });
		record('resume-keeps-generation', { ...ctx, source: 'blob' }, resumed.ok && resumed.status.disc?.generation === gen &&
			got.result === 64 * KB + 7 && got.mismatchAt === -1 && again.result === 2048,
			{ generation: gen, resumedGeneration: resumed.status.disc?.generation, waitingRead: got.result, mismatchAt: got.mismatchAt,
				handedBack: handed, readAfter: again.result });

		if (h.discUrl) {
			const urlOpen = await open('url');
			const genU = h.generation;
			const busy = read({ length: 16 * MB, offset: 700 * MB, hash: true, generation: genU });
			await sleep(5);
			const atDeath = slotStates();
			t0 = performance.now();
			h.disc.terminate();
			await whenGone(lock, t0);
			const handedBack = handBack();
			lock = lockName();
			const r = await attachWorker(lock, { url: h.discUrl, generation: genU, size: urlOpen.info.size });
			const result = await busy;
			const ref = await referenceHash(700 * MB, 16 * MB);
			record('resume-serves-handed-back-read', { ...ctx, source: 'url' }, r.ok && r.status.disc?.generation === genU && handedBack === 1 &&
				result.result === 16 * MB && result.sha256 === ref.sha256,
				{ slotsAtDeath: atDeath, handedBack, result: result.result, sameHash: result.sha256 === ref.sha256, ms: Math.round(performance.now() - t0) });
		}

		// A resume that does not match the block (here another size) leaves the new worker attached without
		// a disc, and the old generation's reads fail rather than read something else.
		await open('blob');
		const genB = h.generation;
		t0 = performance.now();
		h.disc.terminate();
		await whenGone(lock, t0);
		lock = lockName();
		const wrong = await attachWorker(lock, { file: syntheticBlob(), name: 'synthetic', generation: genB, size: size + 1 });
		const stale = await read({ length: 2048, offset: 0, generation: genB });
		record('resume-mismatch-has-no-disc', ctx, wrong.ok && !wrong.status.disc && wrong.status.state === 'idle' && !!wrong.status.error &&
			(stale.result === E.STALE || stale.result === E.NO_DISC),
			{ state: wrong.status.state, error: wrong.status.error, read: stale.result });
		await setup({});
	}

	// Latency of single reads (one requester, so no queueing) and sequential streaming.
	async function bench(kind, readMode, options = {}) {
		const ctx = { source: kind, read: readMode, impl: h.impl, loop: h.loop };
		const opened = await open(kind, readMode);
		if (!opened.ok)
			return record('bench-open', ctx, false, opened.error);
		const size = opened.info.size;
		const plan = options.plan || [[2 * KB, 400], [64 * KB, 300], [128 * KB, 200], [MB, 60]];
		const out = {};
		for (const [length, count] of plan) {
			const r = await req(0).call('bench', { impl: h.impl, generation: h.generation, size, length, count, seed: length, verifyPattern: kind === 'blob' });
			out[length] = r;
			record('bench-random', ctx, r.errors === 0 && r.mismatches === 0, { length, count: r.count, minMs: r.minMs, medianMs: r.medianMs,
				p95Ms: r.p95Ms, maxMs: r.maxMs, mbPerS: r.mbPerS });
		}
		const seqLength = 128 * KB;
		const seq = await req(0).call('bench', { impl: h.impl, generation: h.generation, size, length: seqLength, count: options.seqCount || 256,
			pattern: 'sequential', start: options.seqStart ?? Math.floor(size / 3 / 2048) * 2048, verifyPattern: kind === 'blob' });
		record('bench-sequential', ctx, seq.errors === 0, { length: seqLength, count: seq.count, medianMs: seq.medianMs, p95Ms: seq.p95Ms, mbPerS: seq.mbPerS });
		if (options.parallel) {
			const t0 = performance.now();
			const all = await Promise.all(h.requesters.map((r, k) => r.call('bench', { impl: h.impl, generation: h.generation, size, length: MB, count: 30, seed: 900 + k })));
			const ms = performance.now() - t0;
			const bytes = all.reduce((a, r) => a + r.bytes, 0);
			record('bench-parallel-1M', ctx, all.every((r) => r.errors === 0), { requesters: all.length, medianMs: all.map((r) => r.medianMs),
				mbPerS: +(bytes / MB / (ms / 1000)).toFixed(1) });
		}
		return out;
	}

	// ------------------------------------------------------------------ OPFS copy

	async function hasOpfsCopy() {
		try {
			const f = await (await opfsHandle()).getFile();
			return !h.discSize || f.size === h.discSize || f.size > 0;
		} catch {
			return false;
		}
	}

	async function importToOpfs(limit = 0) {
		h.opfs ||= new OpfsTool();
		h.opfs.onprogress = (m) => log(`OPFS import: ${(m.progress / MB).toFixed(0)} MiB in ${(m.ms / 1000).toFixed(1)} s`);
		let r;
		try {
			r = await h.opfs.call('import', { url: h.discUrl, name: OPFS_NAME, limit });
		} catch (err) {
			record('opfs-import', { source: 'url->opfs' }, false, { error: err.message });
			throw err;
		}
		record('opfs-import', { source: 'url->opfs' }, r.bytes === (limit || await urlSize()), {
			bytes: r.bytes, seconds: +(r.ms / 1000).toFixed(1), mbPerS: r.mbPerS, quotaGB: +(r.quota / 2 ** 30).toFixed(1),
			usageBeforeMB: +(r.usageBefore / MB).toFixed(0), usageAfterMB: +(r.usageAfter / MB).toFixed(0) });
		return r;
	}

	// A disk-backed Blob of the whole disc, as close to a picked File as a page can make without a
	// picker: Chrome pages blobs this large out to its blob storage on disk.
	async function loadDiscBlob() {
		const t0 = performance.now();
		const res = await fetch(h.discUrl, { cache: 'no-store' });
		h.discBlob = await res.blob();
		const ms = performance.now() - t0;
		return record('fetch-to-blob', { source: 'url->blob' }, h.discBlob.size === await urlSize(),
			{ bytes: h.discBlob.size, seconds: +(ms / 1000).toFixed(1), mbPerS: +(h.discBlob.size / MB / (ms / 1000)).toFixed(0) });
	}

	// How fast the page can pull the URL at all (no storing), to put the OPFS copy time in context.
	async function streamSpeed(limit = 1024 * MB) {
		const t0 = performance.now();
		const res = await fetch(h.discUrl, { cache: 'no-store' });
		const reader = res.body.getReader();
		let n = 0;
		let chunks = 0;
		while (n < limit) {
			const { done, value } = await reader.read();
			if (done)
				break;
			n += value.byteLength;
			chunks++;
		}
		reader.cancel().catch(() => {});
		const ms = performance.now() - t0;
		return record('url-stream-speed', { source: 'url' }, null, { bytes: n, chunks, avgChunkKB: +(n / chunks / KB).toFixed(0),
			seconds: +(ms / 1000).toFixed(2), mbPerS: +(n / MB / (ms / 1000)).toFixed(0) });
	}

	async function opfsWriteSpeed(mb = 1024) {
		h.opfs ||= new OpfsTool();
		const r = await h.opfs.call('writeSpeed', { mb });
		return record('opfs-write-speed', { source: 'memory->opfs' }, null, r);
	}

	async function removeOpfsCopy() {
		h.opfs ||= new OpfsTool();
		try {
			await h.opfs.call('remove', { name: OPFS_NAME });
		} catch {}
		return h.opfs.call('list');
	}

	// ------------------------------------------------------------------ suites

	async function runCore(options = {}) {
		const s = await setup(options);
		log(`setup: loop=${s.loop} impl=${s.impl} waitAsync=${s.features.waitAsync}`);
		await testEnvironment();
		await testPattern('sync');
		await testPattern('async');
		await testGrowth('blob', 'sync');
		await testConcurrency('blob', 'sync');
		if (h.discUrl) {
			await testIso('url', 'async', options.expect);
			await testIso('url', 'sync', options.expect);
			await testGrowth('url', 'async');
			await testConcurrency('url', 'async');
		}
		if (h.picked)
			await testIso('picked', 'sync', options.expect);
		if (h.discBlob) {
			await testIso('disc-blob', 'sync', options.expect);
			await testIso('disc-blob', 'async', options.expect);
			await testGrowth('disc-blob', 'sync');
		}
		if (await hasOpfsCopy()) {
			await testIso('opfs-file', 'sync', options.expect);
			await testIso('opfs-file', 'async', options.expect);
			await testIso('opfs-sah', 'sync', options.expect);
		}
		await testErrors();
		await testUnavailable();
		await testRestart();
		const failed = results.filter((r) => r.pass === false).length;
		log(`core done: ${results.length} rows, ${failed} failed`);
		return { rows: results.length, failed };
	}

	async function runBench(options = {}) {
		await setup(options);
		const sources = [];
		if (h.discUrl)
			sources.push(['url', 'async'], ['url', 'sync']);
		if (await hasOpfsCopy())
			sources.push(['opfs-file', 'sync'], ['opfs-file', 'async'], ['opfs-sah', 'sync']);
		if (h.picked)
			sources.push(['picked', 'sync'], ['picked', 'async']);
		if (h.discBlob)
			sources.push(['disc-blob', 'sync'], ['disc-blob', 'async']);
		sources.push(['blob', 'sync'], ['blob', 'async']);
		for (const [kind, mode] of options.sources || sources)
			await bench(kind, mode, { parallel: kind === 'url' && mode === 'async', ...options });
		return results.filter((r) => r.test.startsWith('bench'));
	}

	// ------------------------------------------------------------------ page

	$('#disc-url').textContent = h.discUrl || 'none (add ?disc=/dev-disc/<name>; see serve.py --disc)';
	$('#worker-url').textContent = DISC_WORKER_URL;
	$('#pick').addEventListener('change', (e) => {
		h.picked = e.target.files[0] || null;
		if (h.picked)
			log(`picked ${h.picked.name} (${h.picked.size} bytes)`);
	});
	const busy = async (fn) => {
		for (const b of document.querySelectorAll('button'))
			b.disabled = true;
		try {
			await fn();
		} catch (err) {
			log(`error: ${err.stack || err}`);
		} finally {
			for (const b of document.querySelectorAll('button'))
				b.disabled = false;
		}
	};
	$('#run-core').addEventListener('click', () => busy(() => runCore({ impl: $('#impl').value, loop: $('#loop').value })));
	$('#run-bench').addEventListener('click', () => busy(() => runBench({ impl: $('#impl').value, loop: $('#loop').value })));
	$('#import').addEventListener('click', () => busy(importToOpfs));
	$('#remove').addEventListener('click', () => busy(async () => log(`OPFS now: ${JSON.stringify(await removeOpfsCopy())}`)));

	Object.assign(h, { setup, teardown, open, read, testEnvironment, testIso, testPattern, testGrowth, testConcurrency, testErrors,
		testUnavailable, testRestart,
		bench, importToOpfs, removeOpfsCopy, hasOpfsCopy, loadDiscBlob, streamSpeed, opfsWriteSpeed, runCore, runBench, referenceHash, E });
	window.discHarness = h;
	log(`ready; crossOriginIsolated=${self.crossOriginIsolated}`);
})();
