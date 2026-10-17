// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// TabStationToo page logic (the pcsx2-wasm frontend). Plain modern JS, no build step. The UI follows docs/web-port/UI-SPEC.md.
//
// Threading: pcsx2-wasm is linked with PROXY_TO_PTHREAD, so C main() and the emulator threads are
// workers and this file is all that runs on the browser main thread. Everything here must stay
// non-blocking: the exports we call only touch atomics and lock-free queues, and Emscripten proxies
// the emulator's file I/O to this thread, so it has to keep returning to the event loop.

(function () {
	'use strict';

	// ------------------------------------------------------------------ constants

	const JS_FILE = 'pcsx2-wasm.js';
	const WASM_FILE = 'pcsx2-wasm.wasm';
	const DATA_FILE = 'pcsx2-wasm.data';

	const STATE = { WAITING: 0, STARTING: 1, RUNNING: 2, STOPPED: 3, FAILED: 4 };
	const STATE_NAMES = ['waiting', 'starting', 'running', 'stopped', 'failed'];

	const POLL_MS = 500;
	const LOG_MAX_LINES = 2000;
	const LOG_TAIL_LINES = 40;
	const LOG_EXCERPT_LINES = 6;
	const LOG_FLUSH_MS = 120;
	// The runtime normally initializes within seconds of the core download; past this, no error
	// event may ever come (for example a worker that never reports "loaded").
	const LOAD_WATCHDOG_MS = 60000;

	const BIOS_DIR = '/bios';
	const KB = 1024;
	const MB = 1024 * KB;
	const GB = 1024 * MB;
	const BIOS_USUAL_MIN = 4 * MB; // PCSX2's own auto-detection range (BiosTools.cpp)
	const BIOS_USUAL_MAX = 8 * MB;
	const BIOS_REJECT_BELOW = 256 * KB;
	const BIOS_REJECT_ABOVE = 32 * MB;

	const IDB_NAME = 'pcsx2-web';
	const IDB_STORE = 'bios';
	const IDB_KEY = 'last';
	const PREF_CRISP = 'pcsx2web.crisp';
	const PREF_KEEP_CHROME = 'pcsx2web.keepChrome';
	const PREF_HINTED = 'pcsx2web.firstBootHint';

	// The repository is private for now: before this page is served to anyone else (GPL), make it
	// public or point this at a public copy of the source.
	const SOURCE_URL = 'https://github.com/Aegyptiacus11/pcsx2-web/tree/wasm';
	const DUMP_URL = 'https://pcsx2.net/docs/setup/bios/';
	const SVG_NS = 'http://www.w3.org/2000/svg';
	const icon = (name) => 'icons.svg#i-' + name;

	const CHROME_IDLE_MS = 2500;
	const CHROME_MOVE_PX = 4;
	const SUBTITLE_MS = 4000;
	const HINT_MS = 8000;
	const COPY_FEEDBACK_MS = 2000;
	const SLOW_SPEED = 50;
	const SLOW_HINT_AFTER_MS = 5000;
	const STARTING_REASSURE_S = 10;
	const STARTING_LOG_S = 15;

	// Frame descriptor (5 x u32) written by the GS thread; see pcsx2web_frame().
	const FD_SEQ = 0;
	const FD_WIDTH = 1;
	const FD_HEIGHT = 2;
	const FD_PIXELS = 3;
	const FD_WORDS = 5;
	const MAX_FRAME_DIM = 8192;

	// KeyboardEvent.code -> PCSX2 host key name. The C++ side owns the name -> code table.
	// Escape and Tab are deliberately missing: they release the keyboard and never reach the PS2.
	const KEY_NAMES = (() => {
		const m = {
			ArrowUp: 'Up', ArrowDown: 'Down', ArrowLeft: 'Left', ArrowRight: 'Right',
			Enter: 'Return', NumpadEnter: 'Return', Backspace: 'Backspace',
			Space: 'Space',
			ShiftLeft: 'Shift', ShiftRight: 'Shift', ControlLeft: 'Control', ControlRight: 'Control',
			AltLeft: 'Alt', AltRight: 'Alt',
			Insert: 'Insert', Delete: 'Delete', Home: 'Home', End: 'End', PageUp: 'PageUp', PageDown: 'PageDown',
		};
		for (let c = 65; c <= 90; c++) {
			const letter = String.fromCharCode(c);
			m['Key' + letter] = letter;
		}
		for (let d = 0; d <= 9; d++)
			m['Digit' + d] = String(d);
		for (let f = 1; f <= 12; f++)
			m['F' + f] = 'F' + f;
		return m;
	})();
	const MODIFIER_NAMES = new Set(['Shift', 'Control', 'Alt']);
	// Forwarded to the PS2, but the browser default is kept: F5 reloads, F11 is the browser's
	// fullscreen and F12 opens the developer tools.
	const KEEP_DEFAULT = new Set(['F5', 'F11', 'F12']);
	// The physical keys of the default port 1 pad mapping (see the Keys drawer). A page shortcut
	// never takes one of these.
	const PAD_CODES = new Set([
		'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Enter', 'NumpadEnter', 'Backspace',
		'KeyQ', 'KeyE', 'Digit1', 'Digit2', 'Digit3', 'Digit4',
		'KeyW', 'KeyA', 'KeyS', 'KeyD', 'KeyT', 'KeyF', 'KeyG', 'KeyH',
		'KeyI', 'KeyJ', 'KeyK', 'KeyL',
	]);

	const params = new URLSearchParams(location.search);
	const MIRROR_CONSOLE = params.get('console') === '1';
	const VU1JIT_PARAM = params.get('vu1jit');

	// ------------------------------------------------------------------ copy

	const COPY = {
		intakeTitle: 'Start with your BIOS.',
		intakeBody: 'TabStationToo needs the BIOS from a PS2 you own, usually a 4\u00a0MB .bin file. Then add a disc image of one of your games, or boot the console’s own menus.',
		choose: 'Choose BIOS file…',
		dropHere: 'or drop it here',
		staysHere: 'It stays in this browser. Nothing is uploaded.',
		dumpLink: 'How to dump your BIOS',
		touchDownload: 'The emulator (14\u00a0MB) downloads after you choose it.',
		touchWarning: 'Made for a computer with a keyboard. Phones may not have the memory to run it.',
		preparing: 'Preparing the emulator',
		runtimeStarting: 'Starting the runtime…',
		drop: 'Drop to use this BIOS.',
		dropBlocked: 'Stop the PS2 first to change the BIOS.',
		tooSmall: (size) => `That file is too small to be a PS2 BIOS (${size}; a BIOS is usually 4\u00a0MB).`,
		tooBig: (size) => `That file is ${size}. A BIOS is 4 to 8\u00a0MB.`,
		unreadable: 'Couldn’t read that file. Try choosing it again.',
		fetchFailed: (path, why) => `Couldn’t fetch the BIOS from ${path} (${why}).`,
		otherOrigin: '?bios= only accepts addresses on this server.',
		stored: { idb: 'Remembered in this browser', visit: 'This visit only', url: 'Loaded from ?bios=, not saved' },
		boot: 'Boot',
		another: 'Use another BIOS…',
		saveFailed: 'Couldn’t save the BIOS in this browser. It will last for this visit.',
		queued: 'Booting when ready',
		queuedCancel: 'Press again to cancel.',
		starting: 'Starting the PS2…',
		firstFrame: 'Waiting for the first frame…',
		reassure: 'Still starting. The first screen can take a few seconds here.',
		noAnswer: 'The emulator hasn’t answered. The log may say why.',
		openLog: 'Open the log',
		alreadyStarting: 'The emulator is already starting.',
		refused: 'The emulator refused to boot. The log has details.',
		captured: 'Keys go to the PS2',
		released: 'Click the screen to play',
		releasedFocused: 'Press Enter to play',
		firstHint: (confirm, jp) => `Arrows move · ${confirm} confirms (${jp} on Japanese consoles) · ? shows every key`,
		slowHint: (pct) => `Running at ${pct}%. Heavy scenes are slow in the browser for now.`,
		stopped: 'Stopped.',
		bootAgain: 'Boot again',
		failed: 'The PS2 didn’t start.',
		tryAgain: 'Try again',
		fatal: 'The emulator stopped working.',
		fatalBody: 'Reload to start over.',
		fatalSaved: 'Your BIOS is still saved.',
		reload: 'Reload',
		copyDetails: 'Copy details',
		downloadFailed: 'Couldn’t download the emulator.',
		downloadBody: 'Check the connection to this server, then reload.',
		buildBroken: 'This build of the emulator is incomplete.',
		unsupported: 'This page can’t run the emulator.',
		causeHeaders: 'The server has to send cross-origin isolation headers, and this one doesn’t.',
		causeFile: 'You opened the file from disk. Start the dev server instead.',
		causeInsecure: 'This address isn’t a secure context. Use http://127.0.0.1 or HTTPS.',
		causeWasm: 'This browser can’t run WebAssembly.',
		causeThreads: 'This browser doesn’t support shared-memory threads.',
		tryBrowser: 'Try a recent Chrome or Edge on a computer.',
		pass: 'Pass',
		fail: 'Fail',
		forgotten: 'BIOS forgotten. It’s been removed from this browser.',
		copy: 'Copy',
		copied: 'Copied',
		copyFailed: 'Couldn’t copy',
		fullscreen: 'Fullscreen',
		exitFullscreen: 'Exit fullscreen',
		fullscreenUnavailable: 'Fullscreen isn’t available here.',
		announce: {
			ready: 'Ready to boot.',
			queued: 'Booting when ready.',
			starting: 'Starting.',
			running: 'Running. Keys go to the PS2.',
			runningReleased: 'Running. Click the screen to play.',
			released: 'Keyboard released.',
			stopped: 'Stopped.',
			failed: 'The PS2 didn’t start.',
			dead: 'The emulator stopped working.',
		},
	};

	// ------------------------------------------------------------------ elements

	const $ = (id) => document.getElementById(id);
	const els = {
		body: document.body,
		marquee: document.querySelector('.marquee'),
		credits: document.querySelector('.credits'),
		aboutBtn: $('about-btn'),
		about: $('about'),
		theatre: $('theatre'),
		stageBox: $('stage-box'),
		screen: $('screen'),
		canvas: $('canvas'),
		hairline: $('hairline'),
		hairlineFill: $('hairline-fill'),
		slateInner: $('slate-inner'),
		slateTitle: $('slate-title'),
		slateBody: $('slate-body'),
		slateExcerpt: $('slate-excerpt'),
		slateActions: $('slate-actions'),
		slatePrimary: $('slate-primary'),
		slatePrimaryIcon: $('slate-primary-icon'),
		slatePrimaryLabel: $('slate-primary-label'),
		slatePrimaryCap: $('slate-primary-cap'),
		slateAside: $('slate-aside'),
		slateAlt: $('slate-alt'),
		slateOpenLog: $('slate-openlog'),
		slateCopy: $('slate-copy'),
		slateCopyLabel: $('slate-copy-label'),
		slateError: $('slate-error'),
		slateDev: $('slate-dev'),
		devChecks: $('dev-checks'),
		devCmd: $('dev-cmd'),
		devCopy: $('dev-copy'),
		devCopyLabel: $('dev-copy-label'),
		slateFine: $('slate-fine'),
		sill: $('sill'),
		loadLine: $('load-line'),
		captureText: $('capture-text'),
		capture: $('capture'),
		subtitle: $('subtitle'),
		readout: $('readout'),
		rDash: $('r-dash'),
		rSpeedWrap: $('r-speed-wrap'),
		rSpeed: $('r-speed'),
		rRest: $('r-rest'),
		rVps: $('r-vps'),
		rFps: $('r-fps'),
		rFrame: $('r-frame'),
		transport: $('transport'),
		btnStop: $('btn-stop'),
		biosBtn: $('bios-btn'),
		biosBtnName: $('bios-btn-name'),
		btnCrisp: $('btn-crisp'),
		btnKeys: $('btn-keys'),
		btnLog: $('btn-log'),
		btnFs: $('btn-fs'),
		btnFsIcon: $('btn-fs-icon'),
		btnFsLabel: $('btn-fs-label'),
		btnFsTip: $('btn-fs-tip'),
		keysDrawer: $('keys-drawer'),
		keysClose: $('keys-close'),
		keepChrome: $('keep-chrome'),
		logDrawer: $('log-drawer'),
		log: $('log'),
		logEmpty: $('log-empty'),
		logCount: $('log-count'),
		logLatest: $('log-latest'),
		logCopy: $('log-copy'),
		logCopyLabel: $('log-copy-label'),
		logClear: $('log-clear'),
		logClose: $('log-close'),
		biosMenu: $('bios-menu'),
		menuName: $('menu-name'),
		menuWhere: $('menu-where'),
		menuItems: $('menu-items'),
		menuChoose: $('menu-choose'),
		menuForget: $('menu-forget'),
		menuReason: $('menu-reason'),
		menuConfirm: $('menu-confirm'),
		menuForgetYes: $('menu-forget-yes'),
		menuKeep: $('menu-keep'),
		announce: $('announce'),
		biosInput: $('bios-input'),
	};
	const ctx2d = els.canvas.getContext('2d', { alpha: false });

	// ------------------------------------------------------------------ page state

	const media = (q) => (typeof matchMedia === 'function' ? matchMedia(q) : { matches: false });
	const touchOnlyQuery = media('(pointer: coarse) and (hover: none)');
	const hoverQuery = media('(hover: hover)');
	const reducedMotionQuery = media('(prefers-reduced-motion: reduce)');

	const page = {
		phase: 'loading', // loading | ready | unsupported | dead
		runtimeRequested: false, // false while a touch-only device waits for a BIOS
		vmState: STATE.WAITING,
		speed: 0,
		vps: 0,
		displayFps: 0,
		bios: null, // { name, size, origin: 'file'|'idb'|'url', remembered, saveFailed }
		pendingBiosBytes: null, // held until the runtime (and its FS) is ready
		writtenBiosName: null,
		biosOp: 0, // generation token: a newer pick, fetch or Forget wins over an older one
		biosReading: false,
		urlBiosName: null, // the ?bios= file while it is being fetched
		autoBoot: false, // a queued boot: runs as soon as the runtime and the BIOS are ready
		bootsAccepted: 0,
		bootAt: 0, // performance.now() of the accepted boot
		runStartedAt: 0, // performance.now() of the first frame of this run
		ranMs: 0,
		frameSinceBoot: false,
		seqAtBoot: 0,
		wasmLoaded: false,
		outcome: null, // null | 'stopped' | 'failed': what the slate shows after a run
		error: '', // the slate's coral line; stays until the next attempt
		errorIsDisc: false, // page.error is about the disc: a BIOS arriving does not clear it
		fatal: null, // { kind, what, detail, line }
		env: null, // environmentProblem() when unsupported
		drag: '', // '' | 'ok' | 'blocked'
		drawer: '', // '' | 'keys' | 'log'
		fsByPage: false, // the page itself is leaving fullscreen
		keepChrome: false,
		slowSince: 0,
		slowHintShown: false,
		startNotice: 0, // the starting slate's 10 s / 15 s lines announced so far (1, 2)
		lastState: null,
	};

	const progress = {
		wasm: { loaded: 0, total: 0, done: false },
		data: { loaded: 0, total: 0, done: false, seen: false },
		depsLeft: 0,
	};

	const frame = {
		ptr: 0,
		lastSeq: 0,
		dropStreak: 0,
		presented: 0,
		dropped: 0,
		width: 0,
		height: 0,
		imageData: null,
		presentedAtLastPoll: 0,
		lastPollTime: 0,
	};

	let api = null; // bound exports, set once the runtime is initialized
	let wasmMemory = null; // the WebAssembly.Memory object (stable; its .buffer is not)
	const keyCodes = new Map(); // host key name -> code from pcsx2web_key_code
	const heldPhysical = new Map(); // KeyboardEvent.code -> host code
	const heldHostRefs = new Map(); // host code -> number of physical keys holding it
	const swallowed = new Set(); // KeyboardEvent.code of page shortcuts: their keyup is never forwarded
	let pollTimer = 0;
	let rafId = 0;
	let watchdogTimer = 0;

	// ------------------------------------------------------------------ logging

	const logLines = []; // { text, cls }
	let logPending = [];
	let logFlushTimer = 0;
	let logTotal = 0;

	// CSI sequences (colours, cursor moves) and two-byte escapes.
	const ANSI_RE = /\x1b\[[0-?]*[ -/]*[@-~]|\x1b[@-Z\\-_]/g;
	const SGR_RE = /\x1b\[([0-9;]*)m/g;
	const CTRL_RE = /[\x00-\x08\x0b-\x1f\x7f]/g;

	function classifyAnsi(raw) {
		if (raw.indexOf('\x1b') < 0)
			return null;
		SGR_RE.lastIndex = 0;
		let m;
		while ((m = SGR_RE.exec(raw)) !== null) {
			for (const p of m[1].split(';')) {
				const n = Number(p);
				if (n === 31 || n === 91) return 'err';
				if (n === 33 || n === 93) return 'warn';
				if (n === 32 || n === 92) return 'ok';
				if ((n >= 34 && n <= 36) || (n >= 94 && n <= 96)) return 'info';
			}
		}
		return null;
	}

	function appendLog(text, stream, level) {
		const str = String(text);
		const parts = str.indexOf('\n') >= 0 ? str.split('\n') : [str];
		for (const raw of parts) {
			const clean = raw.replace(ANSI_RE, '').replace(CTRL_RE, '');
			if (!clean && parts.length > 1)
				continue;
			let cls = stream === 'page' ? (level === 'error' ? 'err' : level === 'warn' ? 'warn' : 'page') : classifyAnsi(raw);
			if (!cls && stream === 'stderr')
				cls = 'stderr';
			const entry = { text: stream === 'page' ? '[page] ' + clean : clean, cls, page: stream === 'page' };
			logLines.push(entry);
			logPending.push(entry);
			logTotal++;
			if (MIRROR_CONSOLE)
				(stream === 'stderr' ? console.warn : console.log)(entry.text);
		}
		if (logLines.length > LOG_MAX_LINES + 200)
			logLines.splice(0, logLines.length - LOG_MAX_LINES);
		if (logPending.length > LOG_MAX_LINES)
			logPending = logPending.slice(-LOG_MAX_LINES);
		if (!logFlushTimer)
			logFlushTimer = setTimeout(flushLog, LOG_FLUSH_MS);
	}

	function pageLog(text, level) {
		appendLog(text, 'page', level);
		if (level === 'error')
			console.error('[pcsx2-web]', text);
		else if (level === 'warn')
			console.warn('[pcsx2-web]', text);
	}

	function makeLogLine(entry) {
		const div = document.createElement('div');
		if (entry.cls)
			div.className = 'l-' + entry.cls;
		div.textContent = entry.text || ' ';
		return div;
	}

	function logIsOpen() {
		return page.drawer === 'log';
	}

	function logAtBottom() {
		const box = els.log;
		return box.scrollTop + box.clientHeight >= box.scrollHeight - 24;
	}

	function renderLogCount() {
		const shown = Math.min(logLines.length, LOG_MAX_LINES);
		rich(els.logCount, [{ num: shown.toLocaleString('en-US') }, shown === 1 ? ' line' : ' lines']);
		els.logEmpty.hidden = shown > 0;
	}

	function flushLog() {
		logFlushTimer = 0;
		renderLogCount();
		if (!logIsOpen()) {
			logPending = [];
			return;
		}
		const box = els.log;
		const atBottom = logAtBottom();
		const frag = document.createDocumentFragment();
		for (const entry of logPending)
			frag.appendChild(makeLogLine(entry));
		logPending = [];
		box.appendChild(frag);
		let excess = box.childElementCount - LOG_MAX_LINES;
		while (excess-- > 0)
			box.firstElementChild.remove();
		if (atBottom)
			box.scrollTop = box.scrollHeight;
		else
			els.logLatest.hidden = false;
	}

	function renderFullLog() {
		const box = els.log;
		box.textContent = '';
		const frag = document.createDocumentFragment();
		for (const entry of logLines.slice(-LOG_MAX_LINES))
			frag.appendChild(makeLogLine(entry));
		logPending = [];
		box.appendChild(frag);
		box.scrollTop = box.scrollHeight;
		els.logLatest.hidden = true;
		renderLogCount();
	}

	function logTail(n) {
		return logLines.slice(-n).map((l) => l.text).join('\n');
	}

	// The emulator's own last lines for the slate: no page bookkeeping, no timestamps, no blanks.
	const STAMP_RE = /^\[\s*\d+\.\d+\]\s?/;
	function emulatorTail(n) {
		const out = [];
		for (let i = logLines.length - 1; i >= 0 && out.length < n; i--) {
			if (logLines[i].page)
				continue;
			const text = logLines[i].text.replace(STAMP_RE, '');
			if (text.trim())
				out.push(text);
		}
		return out.reverse().join('\n');
	}

	// ------------------------------------------------------------------ errors

	function errorText(err) {
		if (err === undefined || err === null)
			return 'Unknown error';
		if (typeof err === 'string')
			return err;
		// A pthread's worker.onerror rethrows the worker's ErrorEvent; its .error is null there.
		if (typeof ErrorEvent !== 'undefined' && err instanceof ErrorEvent)
			return err.error ? errorText(err.error) : `${err.message}${err.filename ? ` (${err.filename}:${err.lineno})` : ''}`;
		if (err instanceof Error)
			return err.stack && !String(err.stack).startsWith(err.message) ? `${err.message}\n${err.stack}` : String(err.stack || err.message);
		try {
			return JSON.stringify(err);
		} catch {
			return String(err);
		}
	}

	// The slate's coral line, which stays until the next attempt. The slate is not a live region,
	// so the line is announced too.
	function showError(text, isDisc = false) {
		page.error = text;
		page.errorIsDisc = isDisc;
		announce(text);
	}

	// The emulator cannot continue: stop calling into it and say why on the slate.
	// kind: 'download' | 'build' | 'init' | 'crash'. line: a short mono line for download failures.
	function fatal(kind, what, err, line) {
		if (page.phase === 'dead')
			return;
		const detail = errorText(err);
		pageLog(`${what}: ${detail}`, 'error');
		page.phase = 'dead';
		page.fatal = { kind, what, detail, line: line || '' };
		page.autoBoot = false;
		clearTimeout(watchdogTimer);
		stopLoops();
		releaseAllKeys(false);
		hideMenu();
		render();
	}

	function fatalDetails() {
		const f = page.fatal || {};
		return [
			`TabStationToo: ${f.what || 'error'}`,
			f.detail || '',
			`Page state: ${page.lastState}, VM state: ${STATE_NAMES[page.vmState] || page.vmState}`,
			navigator.userAgent,
			'',
			logTail(LOG_TAIL_LINES),
		].join('\n');
	}

	// ------------------------------------------------------------------ environment check

	function environmentProblem() {
		const wasm = typeof WebAssembly === 'object';
		const atomics = typeof Atomics === 'object';
		const threads = atomics && typeof SharedArrayBuffer === 'function';
		const secure = !!window.isSecureContext;
		const isolated = !!window.crossOriginIsolated;
		const checks = [
			{ label: 'WebAssembly', pass: wasm },
			{ label: 'Shared-memory threads', pass: threads },
			{ label: 'Secure context', pass: secure },
			{ label: 'Cross-origin isolated', pass: isolated },
		];
		let cause = null;
		if (location.protocol === 'file:')
			cause = COPY.causeFile;
		else if (!wasm)
			cause = `${COPY.causeWasm} ${COPY.tryBrowser}`;
		else if (!atomics)
			cause = `${COPY.causeThreads} ${COPY.tryBrowser}`;
		else if (!secure)
			cause = COPY.causeInsecure;
		else if (!isolated)
			cause = COPY.causeHeaders;
		else if (!threads)
			cause = `${COPY.causeThreads} ${COPY.tryBrowser}`;
		return cause ? { cause, checks } : null;
	}

	// ------------------------------------------------------------------ Emscripten Module

	function findMemoryIn(obj) {
		if (!obj || typeof obj !== 'object')
			return null;
		for (const value of Object.values(obj)) {
			if (value instanceof WebAssembly.Memory)
				return value;
		}
		return null;
	}

	function httpError(status, statusText, file) {
		const err = new Error(`HTTP ${status} ${statusText || ''} for ${file}`.replace('  ', ' '));
		err.httpStatus = status;
		return err;
	}

	function downloadReason(err) {
		return err && err.httpStatus ? `HTTP ${err.httpStatus}` : 'network error';
	}

	// Fetches the .wasm ourselves to report download progress, while keeping streaming
	// compilation. Pthread workers do not use this: they get the compiled module by postMessage.
	function instantiateWasm(imports, receiveInstance) {
		const url = new URL(WASM_FILE, document.baseURI).href;
		const report = () => renderProgress();
		(async () => {
			for (const ns of Object.values(imports))
				wasmMemory = wasmMemory || findMemoryIn(ns);

			const response = await fetch(url, { credentials: 'same-origin' });
			if (!response.ok)
				throw httpError(response.status, response.statusText, WASM_FILE);
			progress.wasm.total = Number(response.headers.get('Content-Length')) || 0;

			let source = response;
			if (response.body && typeof TransformStream === 'function') {
				const counter = new TransformStream({
					transform(chunk, controller) {
						progress.wasm.loaded += chunk.byteLength;
						report();
						controller.enqueue(chunk);
					},
				});
				source = new Response(response.body.pipeThrough(counter), {
					headers: { 'Content-Type': 'application/wasm' },
				});
			}

			let result;
			try {
				result = await WebAssembly.instantiateStreaming(source, imports);
			} catch (err) {
				// A compile or link error will not get better with a second try.
				if (err instanceof WebAssembly.CompileError || err instanceof WebAssembly.LinkError)
					throw err;
				pageLog(`Streaming compilation failed (${err && err.message}); retrying without streaming.`, 'warn');
				const retry = await fetch(url, { credentials: 'same-origin' });
				if (!retry.ok)
					throw httpError(retry.status, retry.statusText, WASM_FILE);
				const bytes = await retry.arrayBuffer();
				progress.wasm.loaded = progress.wasm.total = bytes.byteLength;
				result = await WebAssembly.instantiate(bytes, imports);
			}
			progress.wasm.done = true;
			if (progress.wasm.total)
				progress.wasm.loaded = progress.wasm.total;
			page.wasmLoaded = true;
			wasmMemory = wasmMemory || findMemoryIn(result.instance.exports);
			watchdogTimer = setTimeout(() => {
				if (page.phase === 'loading')
					fatal('crash', 'The emulator failed while loading', `The runtime did not initialize within ${LOAD_WATCHDOG_MS / 1000} s of the core download.`);
			}, LOAD_WATCHDOG_MS);
			report();
			render();
			receiveInstance(result.instance, result.module);
		})().catch((err) => {
			if (err instanceof WebAssembly.CompileError || err instanceof WebAssembly.LinkError)
				fatal('build', `Could not compile ${WASM_FILE}`, err);
			else
				fatal('download', `Could not load the emulator core (${WASM_FILE})`, err, `${WASM_FILE}: ${downloadReason(err)}`);
		});
		return {}; // asynchronous instantiation
	}

	function onSetStatus(text) {
		if (!text)
			return;
		// Emitted by the file packager while it fetches pcsx2-wasm.data.
		if (/^Downloading/.test(text)) {
			progress.data.seen = true;
			const m = /\((\d+)\/(\d+)\)/.exec(text);
			if (m) {
				progress.data.loaded = Number(m[1]);
				progress.data.total = Number(m[2]);
				progress.data.done = progress.data.total > 0 && progress.data.loaded >= progress.data.total;
			}
			renderProgress();
		}
	}

	function onRunDependencies(left) {
		progress.depsLeft = left;
		if (left === 0 && progress.data.seen) {
			progress.data.done = true;
			if (progress.data.total)
				progress.data.loaded = progress.data.total;
		}
		renderProgress();
	}

	function bindApi() {
		const M = window.Module;
		const fns = ['pcsx2web_boot', 'pcsx2web_stop', 'pcsx2web_state', 'pcsx2web_speed', 'pcsx2web_vps',
			'pcsx2web_key_code', 'pcsx2web_key', 'pcsx2web_frame'];
		const missing = fns.filter((n) => typeof M['_' + n] !== 'function').map((n) => '_' + n + ' (EMSCRIPTEN_KEEPALIVE export)');
		if (typeof M.ccall !== 'function')
			missing.push('Module.ccall (add ccall to -sEXPORTED_RUNTIME_METHODS)');
		if (!M.FS || typeof M.FS.writeFile !== 'function')
			missing.push('Module.FS (add FS to -sEXPORTED_RUNTIME_METHODS)');
		if (!wasmMemory) {
			if (M.wasmMemory instanceof WebAssembly.Memory)
				wasmMemory = M.wasmMemory;
			else if (globalThis.wasmMemory instanceof WebAssembly.Memory)
				wasmMemory = globalThis.wasmMemory;
		}
		if (!wasmMemory && !(M.HEAPU8 || globalThis.HEAPU8))
			missing.push('the wasm memory (export wasmMemory or HEAPU8 in -sEXPORTED_RUNTIME_METHODS)');
		if (missing.length)
			throw new Error('Missing from the build:\n  ' + missing.join('\n  '));

		return {
			boot: (name) => M.ccall('pcsx2web_boot', 'number', ['string'], [name]),
			stop: () => M._pcsx2web_stop(),
			state: () => M._pcsx2web_state(),
			speed: () => M._pcsx2web_speed(),
			vps: () => M._pcsx2web_vps(),
			keyCode: (name) => M.ccall('pcsx2web_key_code', 'number', ['string'], [name]),
			key: (code, pressed) => M._pcsx2web_key(code, pressed ? 1 : 0),
			// Pointers are returned as signed i32; make them unsigned.
			frame: () => M._pcsx2web_frame() >>> 0,
		};
	}

	// The wasm memory buffer, looked up again on every call: with ALLOW_MEMORY_GROWTH on shared
	// memory, growth (by any thread) replaces memory.buffer, and views on the old one stay short.
	function heapBuffer() {
		if (wasmMemory)
			return wasmMemory.buffer;
		const heap = window.Module.HEAPU8 || globalThis.HEAPU8; // may lag behind growth
		return heap ? heap.buffer : null;
	}

	function onRuntimeInitialized() {
		if (page.phase === 'dead')
			return;
		clearTimeout(watchdogTimer);
		try {
			api = bindApi();
			frame.ptr = api.frame();
			if (!frame.ptr || frame.ptr % 4 !== 0)
				throw new Error(`pcsx2web_frame() returned an unusable pointer (${frame.ptr})`);
			resolveKeyCodes();
			ensureBiosDir();
		} catch (err) {
			fatal('build', 'This build of the emulator is missing part of its page interface', err);
			return;
		}

		progress.wasm.done = true;
		if (progress.data.seen) {
			progress.data.done = true;
			if (progress.data.total)
				progress.data.loaded = progress.data.total;
		}

		page.phase = 'ready';
		pageLog('Runtime ready.');
		startVu1JitTesting();
		if (page.pendingBiosBytes && page.bios) {
			writeBiosToFs(page.bios.name, page.pendingBiosBytes);
			page.pendingBiosBytes = null;
		}
		renderProgress();
		frame.lastPollTime = performance.now();
		pollTimer = setInterval(poll, POLL_MS);
		rafId = requestAnimationFrame(drawLoop);
		poll();
		maybeAutoBoot();
	}

	// ------------------------------------------------------------------ VU1 JIT testing

	// Testing only, with no interface (pcsx2-wasm/README.md): ?vu1jit=1 boots with the VU1 JIT, which is off by
	// default, and prints its counters to the browser console every 10 s; ?vu1jit=fuzz:SEED:COUNT[:timing|jit|both]
	// runs the JIT's fuzzer on a worker, without a VM, and logs its result.
	const VU1JIT_STATS_NAMES = ['selftest', 'executes', 'programs', 'compiles', 'entries', 'instructions',
		'fallbackSteps', 'refused', 'lockstepRuns', 'lockstepDiffs', 'oracleChecked', 'oracleDiffs', 'eeClears'];
	const VU1JIT_FUZZ_STATES = ['idle', 'running', 'passed', 'differences', 'bad spec'];
	const vu1jit = { mode: null };

	function vu1jitStats() {
		const M = window.Module;
		const buffer = heapBuffer();
		if (!buffer || !M || typeof M._pcsx2web_vu1jit_stats !== 'function')
			return null;
		const words = new Uint32Array(buffer, M._pcsx2web_vu1jit_stats() >>> 0, VU1JIT_STATS_NAMES.length);
		const out = {};
		VU1JIT_STATS_NAMES.forEach((name, i) => { out[name] = Atomics.load(words, i); });
		return out;
	}

	function vu1jitFuzzStatus() {
		const M = window.Module;
		const buffer = heapBuffer();
		if (!buffer || !M || typeof M._pcsx2web_vu1jit_fuzz_status !== 'function')
			return null;
		const ptr = M._pcsx2web_vu1jit_fuzz_status() >>> 0;
		const words = new Uint32Array(buffer, ptr, 4);
		const state = Atomics.load(words, 0);
		// TextDecoder does not take views of shared memory: copy the bytes first.
		const bytes = new Uint8Array(buffer, ptr + 24, 512).slice();
		const end = bytes.indexOf(0);
		return {
			state: VU1JIT_FUZZ_STATES[state] || String(state),
			done: Atomics.load(words, 1),
			total: Atomics.load(words, 2),
			differences: Atomics.load(words, 3),
			summary: new TextDecoder().decode(bytes.subarray(0, end < 0 ? bytes.length : end)),
		};
	}

	function startVu1JitTesting() {
		const M = window.Module;
		if (!VU1JIT_PARAM)
			return;
		if (typeof M._pcsx2web_vu1jit_enable !== 'function') {
			pageLog('?vu1jit= needs a build with the VU1 JIT testing exports.', 'warn');
			return;
		}
		if (VU1JIT_PARAM === '1') {
			vu1jit.mode = 'on';
			M._pcsx2web_vu1jit_enable(1);
			pageLog('VU1 JIT testing: the JIT runs in this session (?vu1jit=1). It is off by default.');
			setInterval(() => console.log('[vu1jit]', JSON.stringify(vu1jitStats())), 10000);
		} else if (VU1JIT_PARAM.startsWith('fuzz:')) {
			vu1jit.mode = 'fuzz';
			const spec = VU1JIT_PARAM.slice(5);
			if (M.ccall('pcsx2web_vu1jit_fuzz', 'number', ['string'], [spec]) !== 0) {
				pageLog(`VU1 JIT fuzzer: could not start '${spec}'.`, 'warn');
				return;
			}
			pageLog(`VU1 JIT fuzzer: running ${spec} on a worker.`);
			const timer = setInterval(() => {
				const st = vu1jitFuzzStatus();
				if (!st || st.state === 'running')
					return;
				clearInterval(timer);
				const line = `VU1 JIT fuzzer, ${st.state}: ${st.summary}`;
				pageLog(line, st.state === 'passed' ? undefined : 'warn');
				console.log('[vu1jit]', line);
			}, 500);
		} else {
			pageLog(`Unknown ?vu1jit= value '${VU1JIT_PARAM}' (use 1, or fuzz:SEED:COUNT[:timing|jit|both]).`, 'warn');
		}
	}

	function onExit(code) {
		fatal('crash', `The emulator exited (code ${code})`, `main() returned or exit(${code}) was called.`);
	}

	function stopLoops() {
		clearInterval(pollTimer);
		pollTimer = 0;
		cancelAnimationFrame(rafId);
		rafId = 0;
	}

	function loadRuntime() {
		window.Module = {
			print: (text) => appendLog(text, 'stdout'),
			printErr: (text) => appendLog(text, 'stderr'),
			setStatus: onSetStatus,
			monitorRunDependencies: onRunDependencies,
			instantiateWasm,
			onRuntimeInitialized,
			onAbort: (what) => fatal('crash', 'The emulator aborted', what),
			onExit,
			preRun: [mountMemoryCards],
			postRun: [onRuntimeReadyForDiscs],
		};

		const script = document.createElement('script');
		script.src = JS_FILE;
		script.async = true;
		script.addEventListener('error', async () => {
			let reason = 'not found';
			try {
				const r = await fetch(JS_FILE, { method: 'HEAD', cache: 'no-store' });
				reason = r.ok ? 'failed to load' : `HTTP ${r.status}`;
			} catch {
				reason = 'network error';
			}
			fatal('download', `Could not load ${JS_FILE}`,
				`${JS_FILE} could not be loaded next to this page. Build the pcsx2-wasm target ` +
				'(ninja -C build-wasm pcsx2-wasm) and serve <build>/bin.', `${JS_FILE}: ${reason}`);
		});
		document.body.appendChild(script);
	}

	// Loads the runtime once. Touch-only devices wait for a BIOS first (the download is large).
	function ensureRuntime() {
		if (page.runtimeRequested || page.phase !== 'loading')
			return;
		page.runtimeRequested = true;
		loadRuntime();
		render();
	}

	window.addEventListener('error', (e) => {
		// Benign: layout settled over two frames (see the ResizeObserver in wireUi()).
		if (!e.error && /ResizeObserver loop/.test(e.message || '')) {
			e.preventDefault();
			return;
		}
		if (page.phase === 'dead' || page.phase === 'unsupported')
			return;
		// A rethrown worker error arrives as an ErrorEvent inside the window's ErrorEvent.
		const inner = (typeof ErrorEvent !== 'undefined' && e.error instanceof ErrorEvent) ? e.error : null;
		const fromRuntime = (e.filename && e.filename.indexOf(JS_FILE) >= 0) ||
			(inner && inner.filename && inner.filename.indexOf(JS_FILE) >= 0) ||
			(typeof WebAssembly === 'object' && e.error instanceof WebAssembly.RuntimeError);
		if (fromRuntime && (api || page.phase === 'loading')) {
			const what = inner ? inner
				: (typeof Event !== 'undefined' && e.error instanceof Event)
					? `A pthread worker failed to load ${JS_FILE} (see the log for "worker sent an error")`
					: (e.error || e.message);
			fatal('crash', api ? 'The emulator crashed' : 'The emulator failed while loading', what);
		} else {
			pageLog(`Unexpected error: ${errorText(inner || e.error || e.message)}`, 'error');
		}
	});

	window.addEventListener('unhandledrejection', (e) => {
		if (page.phase === 'dead' || page.phase === 'unsupported')
			return;
		const text = errorText(e.reason);
		// The file packager rejects when pcsx2-wasm.data cannot be fetched ("404: <url>" or
		// "Network Error: <name>"); the runtime never starts then.
		if (page.phase === 'loading' && page.runtimeRequested) {
			if (text.indexOf(DATA_FILE) >= 0) {
				const status = /^(?:Error: )?(\d{3}):/.exec(text);
				fatal('download', 'The emulator failed while loading', text, `${DATA_FILE}: ${status ? 'HTTP ' + status[1] : 'network error'}`);
			} else {
				fatal('crash', 'The emulator failed while loading', text);
			}
		} else {
			pageLog(`Unexpected error: ${text}`, 'error');
		}
	});

	// ------------------------------------------------------------------ frames

	function drawLoop() {
		rafId = requestAnimationFrame(drawLoop);
		if (!frame.ptr || page.phase !== 'ready')
			return;
		const buf = heapBuffer();
		if (!buf || frame.ptr + FD_WORDS * 4 > buf.byteLength)
			return;

		// Fresh views every frame: never keep a view across frames (memory can grow).
		const desc = new Uint32Array(buf, frame.ptr, FD_WORDS);
		const seq = Atomics.load(desc, FD_SEQ);
		if (seq === 0 || seq === frame.lastSeq)
			return;

		const width = Atomics.load(desc, FD_WIDTH);
		const height = Atomics.load(desc, FD_HEIGHT);
		const pixels = Atomics.load(desc, FD_PIXELS);
		const bytes = width * height * 4;
		if (!width || !height || width > MAX_FRAME_DIM || height > MAX_FRAME_DIM || !pixels || pixels + bytes > buf.byteLength) {
			frame.lastSeq = seq; // nothing sensible to show for this one
			return;
		}

		// ImageData cannot wrap SharedArrayBuffer memory: copy into its own (non-shared) array.
		if (!frame.imageData || frame.imageData.width !== width || frame.imageData.height !== height)
			frame.imageData = new ImageData(width, height);
		frame.imageData.data.set(new Uint8Array(buf, pixels, bytes));

		// The GS thread cycles through three buffers and rewrites the buffer of frame N only for
		// frame N+3, which it starts after publishing N+2. So the copy is intact unless seq moved
		// on by more than 1 meanwhile. Exception: width/height/pixels are stored just before seq,
		// so a read racing a publish can pair the new size with the old buffer; when the size
		// differs from the last frame shown, only accept a copy during which seq did not move.
		// After a few drops in a row, accept one step anyway so the display cannot starve.
		const advanced = (Atomics.load(desc, FD_SEQ) - seq) >>> 0;
		const sameSize = width === frame.width && height === frame.height;
		const tolerance = sameSize || frame.dropStreak >= 3 ? 1 : 0;
		if (advanced > tolerance) {
			frame.dropStreak++;
			frame.dropped++;
			return; // lastSeq unchanged: the next animation frame picks up the newest frame
		}
		frame.dropStreak = 0;
		frame.lastSeq = seq;

		if (els.canvas.width !== width || els.canvas.height !== height) {
			els.canvas.width = width;
			els.canvas.height = height;
		}
		ctx2d.putImageData(frame.imageData, 0, 0);
		frame.presented++;
		frame.width = width;
		frame.height = height;

		if (!page.frameSinceBoot && seq !== page.seqAtBoot) {
			page.frameSinceBoot = true;
			render();
		}
	}

	function currentSeq() {
		const buf = heapBuffer();
		if (!buf || !frame.ptr || frame.ptr + FD_WORDS * 4 > buf.byteLength)
			return 0;
		return Atomics.load(new Uint32Array(buf, frame.ptr, FD_WORDS), FD_SEQ);
	}

	// ------------------------------------------------------------------ polling, boot, stop

	function poll() {
		if (page.phase !== 'ready' || !api)
			return;
		let state, speed, vps;
		try {
			state = api.state();
			speed = api.speed();
			vps = api.vps();
		} catch (err) {
			fatal('crash', 'The emulator stopped responding', err);
			return;
		}

		const now = performance.now();
		const dt = now - frame.lastPollTime;
		if (dt > 0)
			page.displayFps = ((frame.presented - frame.presentedAtLastPoll) * 1000) / dt;
		frame.lastPollTime = now;
		frame.presentedAtLastPoll = frame.presented;

		page.speed = Number.isFinite(speed) ? speed : 0;
		page.vps = Number.isFinite(vps) ? vps : 0;

		const prev = page.vmState;
		if (state !== prev)
			onVmStateChange(prev, state);
		if (page.phase !== 'ready')
			return; // the state change was fatal
		// A hidden tab gets no animation frames, so drawLoop() cannot see the first frame; the
		// slate would count up to "hasn't answered" while the PS2 runs fine.
		if (!page.frameSinceBoot && page.vmState === STATE.RUNNING && document.visibilityState === 'hidden' &&
			currentSeq() !== page.seqAtBoot)
			page.frameSinceBoot = true;

		checkSlowSpeed(now);
		render();
	}

	function onVmStateChange(prev, state) {
		page.vmState = state;
		pageLog(`VM state: ${STATE_NAMES[prev] || prev} -> ${STATE_NAMES[state] || state}`);
		if (state !== STATE.RUNNING)
			releaseAllKeys(true);
		if (state === STATE.STARTING || state === STATE.RUNNING) {
			page.outcome = null;
		} else if (state === STATE.STOPPED) {
			if (page.bootsAccepted)
				page.outcome = 'stopped';
		} else if (state === STATE.FAILED) {
			// FAILED without any accepted boot is Main.cpp's InitializeConfig()/CPUThreadInitialize()
			// failure: no BIOS can fix that.
			if (!page.bootsAccepted) {
				fatal('init', 'PCSX2 failed to initialize', 'InitializeConfig() or CPUThreadInitialize() failed before any boot; see the log.');
				return;
			}
			page.outcome = 'failed';
		}
	}

	function canBoot() {
		return page.phase === 'ready' && !!page.bios && !!page.writtenBiosName && !page.biosReading && discBootable() &&
			(page.vmState === STATE.WAITING || page.vmState === STATE.STOPPED || page.vmState === STATE.FAILED);
	}

	function canStop() {
		return page.phase === 'ready' && (page.vmState === STATE.STARTING || page.vmState === STATE.RUNNING);
	}

	// The Boot button and Enter on the screen. While the runtime is still loading this queues the
	// boot, and pressing again cancels it.
	function boot() {
		page.error = '';
		// Queued (the runtime is still loading, or the ?bios= file is still on its way): pressing
		// again cancels the boot, and pressing once more queues it again.
		const fetching = !page.bios && !!page.urlBiosName;
		if ((page.phase === 'loading' && (page.bios || fetching)) || (page.phase === 'ready' && fetching)) {
			page.autoBoot = !page.autoBoot;
			ensureRuntime();
			render();
			return;
		}
		if (page.phase === 'ready' && (page.vmState === STATE.STARTING || page.vmState === STATE.RUNNING)) {
			subtitle(COPY.alreadyStarting);
			return;
		}
		startVm();
	}

	// checked: the disc was just read (checkDiscThenBoot), or there is none.
	function startVm(checked = false) {
		if (!canBoot())
			return;
		if (!checked && discNeedsCheck()) {
			checkDiscThenBoot();
			return;
		}
		releaseAllKeys(true);
		let result;
		try {
			result = bootRequest();
		} catch (err) {
			fatal('crash', 'Calling pcsx2web_boot failed', err);
			return;
		}
		if (result !== 0) {
			let state = page.vmState;
			try {
				state = api.state();
			} catch {
				// handled by the next poll
			}
			pageLog(`pcsx2web_boot("${page.bios.name}") returned ${result}.`, 'warn');
			if (state === STATE.STARTING || state === STATE.RUNNING) {
				subtitle(COPY.alreadyStarting);
				poll();
				return;
			}
			// The name is sanitized, so a refusal while idle means the frontend failed to
			// initialize (Main.cpp's s_fatal_error); every later boot would be refused too.
			fatal('init', 'pcsx2web_boot refused the boot', 'pcsx2web_boot returned -1 while the VM was idle: PCSX2 failed to initialize.');
			return;
		}
		pageLog(`Boot requested with ${BIOS_DIR}/${page.bios.name}`);
		page.bootsAccepted++;
		page.bootAt = performance.now();
		page.frameSinceBoot = false;
		page.seqAtBoot = currentSeq();
		page.slowSince = 0;
		page.ranMs = 0;
		page.startNotice = 0;
		// pcsx2web_boot returns 0 only after its CAS to STARTING, so the request is already
		// acknowledged. A later FAILED or STOPPED from poll() is then a real transition, even when
		// the VM fails before the first poll.
		onVmStateChange(page.vmState, STATE.STARTING);
		els.screen.focus({ preventScroll: true });
		render();
		setTimeout(poll, 50);
	}

	function stop() {
		if (!canStop())
			return;
		releaseAllKeys(true);
		try {
			api.stop();
		} catch (err) {
			fatal('crash', 'Calling pcsx2web_stop failed', err);
			return;
		}
		pageLog('Stop requested.');
		els.screen.focus({ preventScroll: true });
		setTimeout(poll, 50);
	}

	function maybeAutoBoot() {
		if (page.autoBoot && canBoot()) {
			page.autoBoot = false;
			startVm();
		}
	}

	function checkSlowSpeed(now) {
		if (page.slowHintShown || !page.frameSinceBoot || page.vmState !== STATE.RUNNING)
			return;
		if (page.speed > 0 && page.speed < SLOW_SPEED) {
			if (!page.slowSince)
				page.slowSince = now;
			else if (now - page.slowSince >= SLOW_HINT_AFTER_MS && !subtitleOpen) {
				page.slowHintShown = true;
				subtitle(COPY.slowHint(Math.round(page.speed)));
			}
		} else {
			page.slowSince = 0;
		}
	}

	// ------------------------------------------------------------------ BIOS

	function formatSize(bytes) {
		if (bytes >= GB)
			return `${(bytes / GB).toFixed(1)}\u00a0GB`;
		if (bytes >= MB)
			return `${(bytes / MB).toFixed(bytes % MB === 0 ? 0 : 1)}\u00a0MB`;
		return `${Math.max(1, Math.round(bytes / KB))}\u00a0KB`;
	}

	function sizeParts(bytes) {
		const [num, unit] = formatSize(bytes).split('\u00a0');
		return [{ num, unit }];
	}

	// Discs in decimal units, as file managers and disc sizes usually give them (3,199,631,360 bytes is
	// 3.2 GB); BIOS dumps stay binary (4 MB).
	function formatDiscSize(bytes) {
		if (bytes >= 1e9)
			return `${(bytes / 1e9).toFixed(1)}\u00a0GB`;
		if (bytes >= 1e6)
			return `${(bytes / 1e6).toFixed(bytes >= 1e8 ? 0 : 1)}\u00a0MB`;
		return `${Math.max(1, Math.round(bytes / 1e3))}\u00a0KB`;
	}

	function discSizeParts(bytes) {
		const [num, unit] = formatDiscSize(bytes).split('\u00a0');
		return [{ num, unit }];
	}

	// The model number reads better as the title: scph39001.bin -> SCPH-39001.
	function biosTitle(name) {
		let base = String(name).replace(/\.(bin|rom0?|bios)$/i, '');
		const m = /^scph[-_ ]?(\d{5})(.*)$/i.exec(base);
		if (m)
			base = `SCPH-${m[1]}${m[2]}`;
		return base.replace(/_+/g, ' ').trim() || name;
	}

	function storedText(b) {
		if (b.origin === 'url')
			return COPY.stored.url;
		return b.remembered ? COPY.stored.idb : COPY.stored.visit;
	}

	// A plain file name for /bios: no separators or control characters, and short enough in UTF-8
	// for pcsx2web_boot (which takes at most 255 bytes). Keeps the end, where the extension is.
	const utf8 = new TextEncoder();
	function sanitizeBiosName(name) {
		let n = String(name || '').replace(/[/\\\x00-\x1f\x7f]/g, '_').trim();
		while (utf8.encode(n).length > 200)
			n = Array.from(n).slice(1).join('');
		if (!n || n === '.' || n === '..')
			n = 'bios.bin';
		return n;
	}

	// Returns the slate's error line, or null. Sizes outside 4-8 MB are allowed but logged.
	function checkBiosSize(size, name) {
		if (size < BIOS_REJECT_BELOW)
			return COPY.tooSmall(formatSize(size));
		if (size > BIOS_REJECT_ABOVE)
			return COPY.tooBig(formatSize(size));
		if (size < BIOS_USUAL_MIN || size > BIOS_USUAL_MAX)
			pageLog(`${name} is ${formatSize(size)}; PS2 BIOS dumps are usually 4 MB. Trying it anyway.`, 'warn');
		return null;
	}

	function canChangeBios() {
		return (page.phase === 'loading' || page.phase === 'ready') &&
			page.vmState !== STATE.STARTING && page.vmState !== STATE.RUNNING;
	}

	function ensureBiosDir() {
		const FS = window.Module.FS;
		if (!FS.analyzePath(BIOS_DIR).exists)
			FS.mkdir(BIOS_DIR);
	}

	function writeBiosToFs(name, bytes) {
		const FS = window.Module.FS;
		try {
			ensureBiosDir();
			if (page.writtenBiosName && page.writtenBiosName !== name) {
				try {
					FS.unlink(`${BIOS_DIR}/${page.writtenBiosName}`);
				} catch {
					// already gone
				}
			}
			FS.writeFile(`${BIOS_DIR}/${name}`, bytes);
			page.writtenBiosName = name;
			pageLog(`BIOS written to ${BIOS_DIR}/${name} (${bytes.length.toLocaleString('en-US')} bytes).`);
		} catch (err) {
			page.writtenBiosName = null;
			showError(COPY.unreadable);
			pageLog(`Could not write the BIOS into the emulator: ${errorText(err)}`, 'warn');
		}
	}

	function removeBiosFromFs() {
		if (!page.writtenBiosName || page.phase !== 'ready')
			return;
		try {
			window.Module.FS.unlink(`${BIOS_DIR}/${page.writtenBiosName}`);
		} catch {
			// already gone
		}
		page.writtenBiosName = null;
	}

	function setBios(name, bytes, origin, remembered) {
		page.bios = { name, size: bytes.length, origin, remembered, saveFailed: false };
		page.outcome = null;
		// A disc error stays: with ?bios= and ?disc= the BIOS often arrives after the disc failed, and the
		// error is then the only sign of why the queued boot did not run.
		if (!page.errorIsDisc)
			page.error = '';
		if (page.phase === 'ready') {
			writeBiosToFs(name, bytes);
			page.pendingBiosBytes = null;
		} else {
			page.pendingBiosBytes = bytes;
		}
		render();
	}

	function pickBiosCandidate(files) {
		const list = Array.from(files || []);
		return list.find((f) => f.size >= BIOS_USUAL_MIN && f.size <= BIOS_USUAL_MAX) || list[0] || null;
	}

	async function acceptBiosFile(file) {
		if (!file)
			return;
		if (!canChangeBios()) {
			subtitle(COPY.dropBlocked);
			return;
		}
		const name = sanitizeBiosName(file.name);
		const problem = checkBiosSize(file.size, name);
		if (problem) {
			showError(problem);
			pageLog(`${name}: ${problem}`, 'warn');
			render();
			return;
		}
		// A newer pick, fetch or Forget bumps the token; an older, slower read then loses.
		const op = ++page.biosOp;
		page.autoBoot = false;
		page.urlBiosName = null;
		page.biosReading = true;
		let bytes;
		try {
			bytes = new Uint8Array(await file.arrayBuffer());
		} catch (err) {
			if (op === page.biosOp) {
				page.biosReading = false;
				showError(COPY.unreadable);
				pageLog(`Could not read ${name}: ${errorText(err)}`, 'warn');
				render();
			}
			return;
		}
		if (op !== page.biosOp)
			return;
		page.biosReading = false;
		// A boot may have started while the file was read: do not unlink or replace its BIOS.
		if (!canChangeBios()) {
			subtitle(COPY.dropBlocked);
			render();
			return;
		}
		setBios(name, bytes, 'file', false);
		ensureRuntime();
		focusPrimary();
		const saved = await idbPut({ name, size: bytes.length, lastModified: file.lastModified || 0, savedAt: Date.now(), data: bytes.buffer });
		if (op !== page.biosOp) {
			// Forgotten meanwhile: make sure the late write does not bring it back next visit.
			if (!page.bios && saved)
				idbDelete();
			return;
		}
		if (page.bios && page.bios.name === name) {
			if (saved) {
				page.bios.remembered = true;
			} else {
				page.bios.saveFailed = true;
				if (!page.error)
					announce(COPY.saveFailed);
			}
			render();
		}
	}

	function forgetBios() {
		if (!canChangeBios()) {
			subtitle(COPY.dropBlocked);
			return;
		}
		// Synchronous first, so Boot is gone before the (possibly slow) IndexedDB delete.
		page.biosOp++;
		page.biosReading = false;
		removeBiosFromFs();
		page.bios = null;
		page.pendingBiosBytes = null;
		page.autoBoot = false;
		page.urlBiosName = null;
		page.outcome = null;
		page.error = '';
		hideMenu();
		render();
		subtitle(COPY.forgotten);
		focusPrimary();
		idbDelete();
	}

	async function loadBiosFromUrl(param) {
		let url;
		try {
			url = new URL(param, location.href);
		} catch {
			showError(COPY.fetchFailed(param, 'not a valid address'));
			render();
			return;
		}
		if (url.origin !== location.origin) {
			showError(COPY.otherOrigin);
			pageLog(`Refusing to fetch a BIOS from another origin: ${url.href}`, 'warn');
			render();
			return;
		}
		let base = url.pathname.split('/').pop() || '';
		try {
			base = decodeURIComponent(base);
		} catch {
			// keep it encoded
		}
		const name = sanitizeBiosName(base);
		const op = ++page.biosOp;
		// ?bios= starts queued: it boots as soon as the runtime is ready, unless cancelled.
		page.urlBiosName = name;
		page.autoBoot = true;
		render();
		pageLog(`Fetching BIOS from ${url.pathname}`);
		let bytes;
		try {
			const response = await fetch(url.href, { credentials: 'same-origin', cache: 'no-store' });
			if (!response.ok)
				throw httpError(response.status, response.statusText, url.pathname);
			bytes = new Uint8Array(await response.arrayBuffer());
		} catch (err) {
			if (op === page.biosOp) {
				page.urlBiosName = null;
				page.autoBoot = false;
				showError(COPY.fetchFailed(url.pathname, downloadReason(err)));
				pageLog(`Could not fetch the BIOS from ${url.pathname}: ${errorText(err)}`, 'warn');
				render();
			}
			return;
		}
		if (op !== page.biosOp)
			return;
		page.urlBiosName = null;
		const problem = checkBiosSize(bytes.length, name);
		if (problem) {
			page.autoBoot = false;
			showError(problem);
			render();
			return;
		}
		// Not remembered in IndexedDB: ?bios= is for automated runs.
		setBios(name, bytes, 'url', false);
		maybeAutoBoot();
	}

	// IndexedDB: the last BIOS, so the user does not pick it on every visit. Failures (private
	// windows, blocked storage) only mean the BIOS is not remembered.
	function idbOpen() {
		return new Promise((resolve, reject) => {
			if (!('indexedDB' in window)) {
				reject(new Error('IndexedDB is not available'));
				return;
			}
			const req = indexedDB.open(IDB_NAME, 1);
			req.onupgradeneeded = () => req.result.createObjectStore(IDB_STORE);
			req.onsuccess = () => resolve(req.result);
			req.onerror = () => reject(req.error);
			req.onblocked = () => reject(new Error('IndexedDB open blocked'));
		});
	}

	async function idbRequest(mode, fn) {
		const db = await idbOpen();
		try {
			return await new Promise((resolve, reject) => {
				const tx = db.transaction(IDB_STORE, mode);
				const req = fn(tx.objectStore(IDB_STORE));
				let result;
				req.onsuccess = () => { result = req.result; };
				tx.oncomplete = () => resolve(result);
				tx.onerror = () => reject(tx.error || req.error);
				tx.onabort = () => reject(tx.error || new Error('IndexedDB transaction aborted'));
			});
		} finally {
			db.close();
		}
	}

	async function idbGet() {
		try {
			return await idbRequest('readonly', (s) => s.get(IDB_KEY));
		} catch (err) {
			pageLog(`Could not read the remembered BIOS: ${err && err.message}`, 'warn');
			return null;
		}
	}

	async function idbPut(record) {
		try {
			await idbRequest('readwrite', (s) => s.put(record, IDB_KEY));
			return true;
		} catch (err) {
			pageLog(`Could not remember the BIOS in this browser: ${err && err.message}`, 'warn');
			return false;
		}
	}

	async function idbDelete() {
		try {
			await idbRequest('readwrite', (s) => s.delete(IDB_KEY));
		} catch (err) {
			pageLog(`Could not forget the BIOS: ${err && err.message}`, 'warn');
		}
	}

	async function restoreRememberedBios() {
		const op = page.biosOp;
		const rec = await idbGet();
		if (!rec || !rec.data || page.bios || op !== page.biosOp)
			return;
		const bytes = new Uint8Array(rec.data);
		const name = sanitizeBiosName(rec.name);
		if (checkBiosSize(bytes.length, name))
			return;
		setBios(name, bytes, 'idb', true);
		ensureRuntime();
		pageLog(`Using the BIOS remembered in this browser: ${name}`);
		// Only if the user has not put focus anywhere yet.
		if (!document.activeElement || document.activeElement === document.body)
			focusPrimary();
	}

	// ------------------------------------------------------------------ discs (UI-SPEC.md §10)
	// The disc worker (disc-worker.js) reads the user's file, or a URL, for the emulator's disc reader
	// (docs/web-port/DISC-DESIGN.md): nothing is copied first. Once the worker has opened the disc, a
	// 0-byte placeholder at /disc/<name> stands for it, and pcsx2web_boot_disc() boots that path.

	const DISC_DIR = '/disc';
	const DISC_WORKER_FILE = 'disc-worker.js';
	const DISC_IDB_NAME = 'pcsx2-web-disc';
	const DISC_IDB_STORE = 'disc';
	const DISC_IDB_KEY = 'last';
	// The layouts InputIsoFile::Detect() tries, as [sector size, data offset, block offset]: the ISO 9660
	// primary volume descriptor ("CD001") is in sector 16, and PCSX2 wants at least 17 sectors.
	const DISC_LAYOUTS = [[2048, 0, 24], [2336, 0, 16], [2352, 0, 0], [2448, 0, 0],
		[2048, 150 * 2048, 24], [2352, 150 * 2048, 0], [2448, 150 * 2048, 0]];
	const DISC_HEADER_BYTES = 150 * 2048 + 17 * 2448;
	const DISC_MIN_SIZE = 17 * 2048;
	const DISC_UNSUPPORTED_RE = /\.(chd|cso|zso|gz|dump)$/i;
	const DISC_BOOT_FULL = 1; // pcsx2web_boot_disc(): the BIOS intro instead of fast boot
	const DISC_POLL_MS = 500;
	const DISC_ERROR_MS = 8000;
	const DISC_WORKER_RESTARTS = 3;
	// Without Web Locks the page cannot know when a terminated worker is gone; it waits this long. That is
	// safe for the sources it sends (a File or a URL: their bytes reach the memory only through the
	// worker's JS, which terminate() stops at once), not for OPFS sync access handles, which it never sends.
	const DISC_WORKER_GONE_MS = 1000;
	const DISC_WORKER_LOCK = 'tabstationtoo-disc-worker-';
	// Request block words (DISC-DESIGN.md 4.1), for failing what a dead worker left waiting.
	const DB_SLOTS = 2;
	const DB_SLOT_BYTES = 3;
	const DB_STATE = 4;
	const DB_DOORBELL = 5;
	const DB_HEADER_WORDS = 16;
	const DB_STATE_STOPPED = 3;
	const DB_STATE_READY = 2;
	const DB_GENERATION = 6;
	const DB_SIZE_LO = 7;
	const DB_SIZE_HI = 8;
	const DS_PENDING = 2;
	const DS_BUSY = 3;
	const DS_DONE = 4;
	const DS_RESULT = 1;
	const DE_CANCELLED = -5;
	// pcsx2web_game(): seq, disc CRC, ELF CRC, serial[32], title[256].
	const GAME_SERIAL_AT = 12;
	const GAME_SERIAL_BYTES = 32;
	const GAME_TITLE_BYTES = 256;

	const DISC_COPY = {
		choose: 'Choose a disc…',
		another: 'Use another disc…',
		remove: 'Remove the disc',
		where: { file: 'Read from your file', handle: 'Remembered in this browser', url: 'Loaded from ?disc=, not saved' },
		withBios: (bios) => `With ${bios}.`,
		savesKept: 'Saves go to a memory card kept in this browser.',
		savesVisit: 'Saves last for this visit only.',
		savesOtherTab: 'Saves last for this visit only: TabStationToo is open in another tab.',
		lastDisc: 'Last disc: ',
		openAgain: 'Open it again',
		waiting: ' is chosen. Add a BIOS to boot it.',
		blocked: 'Stop the PS2 first to change the disc.',
		blockedAny: 'Stop the PS2 first to change the disc or BIOS.',
		drop: 'Drop to use this disc or BIOS.',
		notDisc: 'That file isn’t a disc image TabStationToo can read. Use an .iso, .bin or .img of a PS2 disc.',
		compressed: 'Compressed images (.chd, .cso, .zso, .gz) and block dumps aren’t supported yet. Use an .iso.',
		unreadable: 'Couldn’t read the disc file. It may have been moved, changed or deleted; choose it again.',
		denied: (name) => `The browser didn’t allow reading ${name}. Choose it again.`,
		fetchFailed: (path, why) => `Couldn’t open the disc at ${path} (${why}).`,
		otherOrigin: '?disc= only accepts addresses on this server.',
		lost: 'The disc can’t be read any more. Stop, then choose the file again.',
		lostServer: 'The disc server stopped answering, so the game can’t read its disc.',
		lostUrl: 'The game can’t read its disc from the server any more.',
		readable: 'The disc can be read again. If the game doesn’t go on, stop it and boot again.',
		readerFailed: 'The disc reader stopped working. Reload the page to read discs again.',
		removed: 'Disc removed. Boot opens the console’s menus.',
		memcardFailed: 'Couldn’t keep the memory card in this browser. Saves last for this visit.',
		announceReady: 'Disc ready.',
		announceRemoved: 'Disc removed.',
	};

	const discEls = {
		slateDisc: $('slate-disc'),
		slateRemove: $('slate-remove'),
		input: $('disc-input'),
		btn: $('disc-btn'),
		btnName: $('disc-btn-name'),
		menu: $('disc-menu'),
		menuName: $('disc-menu-name'),
		menuWhere: $('disc-menu-where'),
		menuChoose: $('disc-menu-choose'),
		menuRemove: $('disc-menu-remove'),
		menuReason: $('disc-menu-reason'),
	};

	const disc = {
		worker: null,
		workerLock: '', // the Web Lock the worker holds while it lives ('' if it cannot hold one)
		attached: false,
		dead: false, // no disc reader in this page (missing exports, or it kept failing)
		restarting: false, // a worker died; the next one is not attached yet
		restarts: 0,
		checking: false, // the disc is being read before a boot (checkDiscThenBoot)
		block: 0, // address of the request block (pcsx2web_disc_block)
		gamePtr: 0, // address of the game details (pcsx2web_game)
		status: null, // the worker's last status
		calls: new Map(), // reply id -> resolve
		nextCall: 1,
		// The chosen disc: { name, path, size, kind: 'file'|'handle'|'url', file, url, handle, title,
		// opened, remembered }. A newer choice replaces it only once the worker has opened the new one.
		current: null,
		opening: null, // a disc the worker is opening (or will open once attached)
		op: 0, // generation token: a newer choice or Remove wins over an older, slower one
		urlPending: false, // ?disc= is still being checked: a queued boot waits for it
		remembered: null, // the last picked disc's handle (Chromium): { handle, name, size, lastModified, title }
		runIsDisc: false, // the current or last boot booted a disc
		runTitle: '', // the game's title as PCSX2 reported it during that boot
		runSerial: '',
		runGeneration: 0, // the request block's generation the current or last disc boot read
		gameSeq: 0,
		errorShown: false,
		lastVm: STATE.WAITING,
		timer: 0,
	};

	function discBootable() {
		return !disc.urlPending && !disc.opening && !disc.checking && !disc.restarting && (!disc.current || disc.current.opened);
	}

	// The game's title for a file name: no extension, no trailing region or language tags.
	function discTitleFromName(name) {
		let t = String(name).replace(/\.[a-z0-9]{1,4}$/i, '');
		let prev;
		do {
			prev = t;
			t = t.replace(/\s*(\([^()]*\)|\[[^[\]]*\])\s*$/, '');
		} while (t !== prev);
		return t.replace(/_+/g, ' ').trim() || String(name);
	}

	function discDisplayTitle(d) {
		return d.title || discTitleFromName(d.name);
	}

	function discWhere(d) {
		if (d.kind === 'url')
			return DISC_COPY.where.url;
		return d.remembered ? DISC_COPY.where.handle : DISC_COPY.where.file;
	}

	// A plain file name for /disc, short enough for pcsx2web_boot_disc's 512-byte path.
	function sanitizeDiscName(name) {
		let n = String(name || '').replace(/[/\\\x00-\x1f\x7f]/g, '_').trim();
		while (utf8.encode(n).length > 400)
			n = Array.from(n).slice(1).join('');
		if (!n || n === '.' || n === '..')
			n = 'disc.iso';
		return n;
	}

	// The layout PCSX2 will detect in a disc image's first bytes, or null.
	function discLayout(bytes) {
		for (const [size, offset, blockofs] of DISC_LAYOUTS) {
			const at = offset + 16 * size + 25 - blockofs;
			if (at + 5 <= bytes.length && bytes[at] === 0x43 && bytes[at + 1] === 0x44 && bytes[at + 2] === 0x30 &&
				bytes[at + 3] === 0x30 && bytes[at + 4] === 0x31)
				return size;
		}
		return null;
	}

	async function isDiscImage(file) {
		if (file.size < DISC_MIN_SIZE)
			return false;
		const bytes = new Uint8Array(await file.slice(0, Math.min(file.size, DISC_HEADER_BYTES)).arrayBuffer());
		return discLayout(bytes) !== null;
	}

	function discError(text) {
		showError(text, true);
		pageLog(text, 'warn');
		render();
	}

	// Any file from a drop or the BIOS picker: a disc image goes to the disc, anything else to the BIOS.
	async function acceptFile(file, handlePromise) {
		if (!file)
			return;
		if (!canChangeBios()) {
			subtitle(DISC_COPY.blockedAny);
			return;
		}
		if (DISC_UNSUPPORTED_RE.test(file.name)) {
			discError(DISC_COPY.compressed);
			return;
		}
		let isDisc;
		try {
			isDisc = await isDiscImage(file);
		} catch (err) {
			pageLog(`Could not read ${file.name}: ${errorText(err)}`, 'warn');
			discError(COPY.unreadable);
			return;
		}
		if (isDisc) {
			chooseDisc({ kind: 'file', file, name: file.name, size: file.size }, await (handlePromise || null));
		} else if (file.size > BIOS_REJECT_ABOVE) {
			discError(DISC_COPY.notDisc);
		} else {
			acceptBiosFile(file);
		}
	}

	// A file from the disc picker (or reopened from its handle).
	async function acceptDiscFile(file, handle) {
		if (!file)
			return;
		if (!canChangeBios()) {
			subtitle(DISC_COPY.blocked);
			return;
		}
		if (DISC_UNSUPPORTED_RE.test(file.name)) {
			discError(DISC_COPY.compressed);
			return;
		}
		let ok;
		try {
			ok = await isDiscImage(file);
		} catch (err) {
			pageLog(`Could not read ${file.name}: ${errorText(err)}`, 'warn');
			discError(DISC_COPY.unreadable);
			return;
		}
		if (!ok) {
			discError(DISC_COPY.notDisc);
			return;
		}
		chooseDisc({ kind: 'file', file, name: file.name, size: file.size }, handle);
	}

	// Drops: the handle (Chromium) is asked for during the drop event, the only time it is available.
	function acceptDrop(dt) {
		const files = Array.from((dt && dt.files) || []);
		if (!files.length)
			return;
		const items = Array.from((dt && dt.items) || []).filter((i) => i.kind === 'file');
		const handles = items.map((i) => (typeof i.getAsFileSystemHandle === 'function'
			? i.getAsFileSystemHandle().catch(() => null) : Promise.resolve(null)));
		const file = files.find((f) => f.size > BIOS_REJECT_ABOVE) || pickBiosCandidate(files);
		const handle = handles[files.indexOf(file)] || null;
		acceptFile(file, handle && handle.then((h) => (h && h.kind === 'file' ? h : null)));
	}

	function chooseDisc(desc, handle) {
		if (!canChangeBios()) {
			subtitle(DISC_COPY.blocked);
			return;
		}
		const op = ++disc.op;
		const d = {
			name: sanitizeDiscName(desc.name), path: '', size: desc.size, kind: handle ? 'handle' : desc.kind,
			file: desc.file || null, url: desc.url || null, handle: handle || null, title: '', opened: false,
			remembered: false, sourceName: desc.name,
		};
		d.path = `${DISC_DIR}/${d.name}`;
		const r = disc.remembered;
		if (r && r.name === desc.name && r.size === desc.size && r.title)
			d.title = r.title;
		page.error = '';
		disc.opening = d;
		pageLog(`Disc chosen: ${desc.name} (${desc.size.toLocaleString('en-US')} bytes${desc.url ? `, ${desc.url}` : ''}).`);
		openDiscInWorker(d, op);
		render();
	}

	function discFailureText(d, error) {
		const message = (error && error.message) || '';
		if (d.kind === 'url') {
			const http = /HTTP (\d{3})/.exec(message);
			const code = error && error.code;
			const why = http ? `HTTP ${http[1]}` : /byte ranges/.test(message) ? 'no byte ranges'
				: code === 'TIMEOUT' ? 'no answer in time' : code === 'IO' && /changed/.test(message) ? 'the disc changed' : 'network error';
			return DISC_COPY.fetchFailed(displayPath(d.url), why);
		}
		if (/Permission/i.test(message))
			return DISC_COPY.denied(d.sourceName || d.name);
		return DISC_COPY.unreadable;
	}

	async function openDiscInWorker(d, op) {
		if (!disc.attached)
			return; // opened once the worker is attached
		const msg = d.url ? { type: 'open', url: d.url, name: d.name } : { type: 'open', file: d.file, name: d.name };
		const reply = await discCall(msg);
		if (op !== disc.op || disc.opening !== d)
			return; // a newer choice or Remove won
		disc.opening = null;
		if (!reply.ok) {
			// The worker keeps serving the disc it had, so the previous choice stands.
			pageLog(`Could not open the disc ${d.name}: ${reply.error ? reply.error.message : 'unknown error'}`, 'warn');
			page.autoBoot = false;
			discError(discFailureText(d, reply.error));
			return;
		}
		const prev = disc.current;
		d.size = (reply.status && reply.status.disc && reply.status.disc.size) || d.size;
		try {
			const FS = window.Module.FS;
			if (!FS.analyzePath(DISC_DIR).exists)
				FS.mkdir(DISC_DIR);
			if (prev && prev.path !== d.path && FS.analyzePath(prev.path).exists)
				FS.unlink(prev.path);
			FS.writeFile(d.path, new Uint8Array(0));
		} catch (err) {
			pageLog(`Could not write the disc placeholder ${d.path}: ${errorText(err)}`, 'warn');
			discError(DISC_COPY.unreadable);
			return;
		}
		d.opened = true;
		disc.current = d;
		page.outcome = null;
		if (d.handle)
			rememberDisc(d);
		else if (d.kind !== 'url')
			forgetRememberedDisc(); // the last disc is this one now, and it cannot be reopened
		pageLog(`Disc ready: ${d.path} (${formatDiscSize(d.size).replace(' ', ' ')}).`);
		render();
		if (vmLive())
			return; // a restarted reader opened it again: the running game cannot use it (reviewRunningDisc)
		announce(DISC_COPY.announceReady);
		if (focusIsFree())
			focusPrimary();
		maybeAutoBoot();
	}

	function removeDisc() {
		if (!canChangeBios()) {
			subtitle(DISC_COPY.blocked);
			return;
		}
		disc.op++;
		const d = disc.current;
		disc.current = null;
		disc.opening = null;
		disc.urlPending = false;
		if (disc.attached)
			discCall({ type: 'close' });
		if (d && page.phase === 'ready') {
			try {
				window.Module.FS.unlink(d.path);
			} catch {
				// already gone
			}
		}
		forgetRememberedDisc();
		page.outcome = null;
		page.error = '';
		hideDiscMenu();
		render();
		subtitle(DISC_COPY.removed);
		announce(DISC_COPY.announceRemoved);
		focusPrimary();
	}

	async function openDiscPicker() {
		if (!canChangeBios()) {
			subtitle(DISC_COPY.blocked);
			return;
		}
		hideDiscMenu();
		if (typeof window.showOpenFilePicker !== 'function') {
			discEls.input.click();
			return;
		}
		let handle;
		try {
			[handle] = await window.showOpenFilePicker({
				id: 'tabstationtoo-disc',
				types: [{ description: 'PS2 disc images', accept: { 'application/octet-stream': ['.iso', '.bin', '.img'] } }],
			});
		} catch (err) {
			if (err && err.name === 'AbortError')
				return;
			pageLog(`The file picker failed (${err && err.message}); using the plain one.`, 'warn');
			discEls.input.click();
			return;
		}
		let file;
		try {
			file = await handle.getFile();
		} catch (err) {
			pageLog(`Could not read the picked disc: ${errorText(err)}`, 'warn');
			discError(DISC_COPY.unreadable);
			return;
		}
		acceptDiscFile(file, handle);
	}

	// "Open it again": one press, the browser's permission prompt, then the disc. Never automatic.
	async function reopenLastDisc() {
		const rec = disc.remembered;
		if (!rec)
			return;
		if (!canChangeBios()) {
			subtitle(DISC_COPY.blocked);
			return;
		}
		let permission = 'denied';
		try {
			permission = await rec.handle.queryPermission({ mode: 'read' });
			if (permission !== 'granted')
				permission = await rec.handle.requestPermission({ mode: 'read' });
		} catch (err) {
			pageLog(`Could not ask for permission to read ${rec.name}: ${errorText(err)}`, 'warn');
		}
		if (permission !== 'granted') {
			discError(DISC_COPY.denied(rec.name));
			return;
		}
		let file;
		try {
			file = await rec.handle.getFile();
		} catch (err) {
			pageLog(`Could not reopen ${rec.name}: ${errorText(err)}`, 'warn');
			discError(DISC_COPY.unreadable);
			return;
		}
		acceptDiscFile(file, rec.handle);
	}

	// A same-origin URL as the slate shows it: the path, decoded.
	function displayPath(href) {
		let path = href;
		try {
			path = new URL(href, location.href).pathname;
			path = decodeURIComponent(path);
		} catch {
			// keep what there is
		}
		return path;
	}

	async function loadDiscFromUrl(param) {
		let url;
		try {
			url = new URL(param, location.href);
		} catch {
			disc.urlPending = false;
			page.autoBoot = false;
			discError(DISC_COPY.fetchFailed(param, 'not a valid address'));
			return;
		}
		if (url.origin !== location.origin) {
			disc.urlPending = false;
			page.autoBoot = false;
			discError(DISC_COPY.otherOrigin);
			return;
		}
		let base = url.pathname.split('/').pop() || '';
		try {
			base = decodeURIComponent(base);
		} catch {
			// keep it encoded
		}
		let bytes;
		let size = 0;
		try {
			const res = await fetch(url.href, { headers: { Range: `bytes=0-${DISC_HEADER_BYTES - 1}` }, cache: 'no-store' });
			if (res.status !== 206) {
				if (res.body)
					res.body.cancel().catch(() => {});
				if (res.ok)
					throw new Error('The server does not support byte ranges.');
				throw httpError(res.status, res.statusText, url.pathname);
			}
			const range = /\/(\d+)$/.exec(res.headers.get('Content-Range') || '');
			size = range ? Number(range[1]) : 0;
			bytes = new Uint8Array(await res.arrayBuffer());
		} catch (err) {
			disc.urlPending = false;
			page.autoBoot = false;
			pageLog(`Could not fetch the disc from ${url.pathname}: ${errorText(err)}`, 'warn');
			discError(DISC_COPY.fetchFailed(displayPath(url.href), err && err.httpStatus ? `HTTP ${err.httpStatus}`
				: /byte ranges/.test(String(err && err.message)) ? 'no byte ranges' : 'network error'));
			return;
		}
		disc.urlPending = false;
		if (size < DISC_MIN_SIZE || discLayout(bytes) === null) {
			page.autoBoot = false;
			discError(DISC_COPY.notDisc);
			return;
		}
		chooseDisc({ kind: 'url', url: url.href, name: base, size }, null);
	}

	// ---- the disc worker

	function discCall(msg) {
		return new Promise((resolve) => {
			if (!disc.worker) {
				resolve({ ok: false, error: { code: 'CANCELLED', message: 'The disc reader is not running.' } });
				return;
			}
			const id = disc.nextCall++;
			disc.calls.set(id, resolve);
			disc.worker.postMessage({ ...msg, id });
		});
	}

	// Module.postRun: after onRuntimeInitialized, so the exports are bound. The tick also keeps the memory
	// card, so it runs even when the disc reader cannot.
	function onRuntimeReadyForDiscs() {
		if (page.phase !== 'ready' || !api)
			return;
		clearInterval(disc.timer);
		disc.timer = setInterval(discTick, DISC_POLL_MS);
		startDiscWorker(null);
	}

	function vmLive() {
		return page.vmState === STATE.STARTING || page.vmState === STATE.RUNNING;
	}

	// No disc reader in this page any more: a chosen disc cannot be read, so it is dropped (Boot then
	// opens the console's menus) and the slate says why.
	function discReaderGone(why) {
		disc.dead = true;
		disc.restarting = false;
		pageLog(why, 'error');
		const had = disc.current || disc.opening;
		disc.current = null;
		disc.opening = null;
		disc.urlPending = false;
		if (had) {
			page.autoBoot = false;
			if (disc.runIsDisc && vmLive())
				subtitle(DISC_COPY.readerFailed, DISC_ERROR_MS);
			discError(DISC_COPY.readerFailed);
		} else {
			render();
		}
	}

	// The request block's words, for the page's part in a worker restart.
	function discBlockView() {
		const buf = heapBuffer();
		if (!buf || !disc.block)
			return null;
		const i32 = new Int32Array(buf);
		const hdr = disc.block >> 2;
		return { i32, hdr, slots: Atomics.load(i32, hdr + DB_SLOTS), slotWords: Atomics.load(i32, hdr + DB_SLOT_BYTES) >> 2 };
	}

	// What a new worker needs to take over the disc in the block: the same source, size and generation,
	// so the emulator's open disc and the requests waiting in the block stay valid (DISC-DESIGN.md §5.6).
	// Null when the block does not hold the current disc.
	function discResumeFor(d) {
		const v = discBlockView();
		if (!v || !d || !d.opened)
			return null;
		if (Atomics.load(v.i32, v.hdr + DB_STATE) !== DB_STATE_READY)
			return null;
		const size = (Atomics.load(v.i32, v.hdr + DB_SIZE_LO) >>> 0) + (Atomics.load(v.i32, v.hdr + DB_SIZE_HI) >>> 0) * 2 ** 32;
		if (size !== d.size)
			return null;
		const generation = Atomics.load(v.i32, v.hdr + DB_GENERATION) >>> 0;
		return { ...(d.url ? { url: d.url } : { file: d.file }), name: d.name, generation, size };
	}

	function discLockName() {
		if (!navigator.locks || typeof navigator.locks.request !== 'function')
			return '';
		const id = typeof crypto.randomUUID === 'function' ? crypto.randomUUID() : `${Date.now()}-${Math.random()}`;
		return DISC_WORKER_LOCK + id;
	}

	// resume: from discResumeFor(), when this worker replaces one that died.
	function startDiscWorker(resume) {
		if (page.phase !== 'ready' || !api || disc.dead || disc.worker)
			return;
		const M = window.Module;
		if (typeof M._pcsx2web_disc_block !== 'function' || typeof M._pcsx2web_boot_disc !== 'function' || !wasmMemory) {
			discReaderGone('This build has no disc reader (pcsx2web_disc_block, pcsx2web_boot_disc or the wasm memory is missing).');
			return;
		}
		disc.block = M._pcsx2web_disc_block() >>> 0;
		disc.gamePtr = typeof M._pcsx2web_game === 'function' ? M._pcsx2web_game() >>> 0 : 0;
		let worker;
		try {
			worker = new Worker(DISC_WORKER_FILE);
		} catch (err) {
			discReaderGone(`Could not start the disc reader: ${errorText(err)}`);
			return;
		}
		const restarted = disc.restarting;
		const lock = discLockName();
		disc.worker = worker;
		// The worker takes this lock before it writes anything into the memory, so even before it answers,
		// being granted the lock means it is gone (or never got that far).
		disc.workerLock = lock;
		worker.onmessage = (e) => onDiscWorkerMessage(worker, e.data || {});
		worker.onerror = (e) => {
			e.preventDefault();
			onDiscWorkerDied(worker, e.message || 'worker error');
		};
		const attach = { type: 'attach', memory: wasmMemory, block: disc.block };
		if (lock)
			attach.lock = lock;
		if (resume)
			attach.resume = resume;
		discCall(attach).then((reply) => {
			if (worker !== disc.worker)
				return;
			if (!reply.ok) {
				onDiscWorkerDied(worker, reply.error ? reply.error.message : 'attach failed');
				return;
			}
			disc.attached = true;
			disc.restarting = false;
			// A worker that could not take its lock is timed instead when it dies (whenDiscWorkerGone()).
			if (!(reply.status && reply.status.lock))
				disc.workerLock = '';
			pageLog(`Disc reader ready (${reply.status && reply.status.loop} loop${lock && !disc.workerLock ? ', no lock' : ''}).`);
			const d = disc.current;
			if (restarted && d && d.opened) {
				const kept = !!(resume && reply.status && reply.status.disc && reply.status.disc.generation === resume.generation);
				if (kept) {
					pageLog(`The new disc reader took over ${d.name}${vmLive() ? '; the game goes on reading it' : ''}.`);
				} else {
					const why = reply.status && reply.status.error ? reply.status.error.message : 'the disc was not in the request block';
					pageLog(`The new disc reader could not take over ${d.name}: ${why}`, 'warn');
					d.opened = false; // opened again below, under a new generation
				}
			}
			reviewRunningDisc();
			// A disc chosen before the runtime was ready, or one a restarted worker has to open again.
			const next = disc.opening || (disc.current && !disc.current.opened ? disc.current : null);
			if (next) {
				disc.opening = next;
				openDiscInWorker(next, disc.op);
			} else {
				render();
				maybeAutoBoot();
			}
		});
	}

	function onDiscWorkerMessage(worker, m) {
		if (worker !== disc.worker)
			return;
		if (m.status)
			onDiscStatus(m.status);
		if (m.type === 'reply') {
			const resolve = disc.calls.get(m.id);
			disc.calls.delete(m.id);
			if (resolve)
				resolve(m);
		}
	}

	function onDiscStatus(status) {
		const before = disc.status ? disc.status.state : '';
		disc.status = status;
		if (!disc.attached)
			return; // a new worker's first words; its attach reply decides
		if (status.state === 'stopped' && before !== 'stopped') {
			// Its loop died: it handed back what it had taken, and a new worker takes over.
			onDiscWorkerDied(disc.worker, (status.error && status.error.message) || 'the disc reader stopped');
			return;
		}
		reviewRunningDisc();
	}

	// The disc as the running game sees it. The game reads the disc it booted with (that generation): a
	// read error, or a reader that dropped that disc or opened it again under a new generation, leaves the
	// game without it. Said once per loss, as a subtitle (the slate is hidden) and on the slate after Stop;
	// when reads work again, a subtitle says so.
	function reviewRunningDisc() {
		const status = disc.status;
		const d = disc.current;
		if (!status || !disc.runIsDisc || !vmLive() || !d) {
			if (status && status.state === 'ready')
				disc.errorShown = false;
			return;
		}
		const stranded = !status.disc || status.disc.generation !== disc.runGeneration;
		if (stranded || status.state === 'error') {
			if (disc.errorShown)
				return;
			disc.errorShown = true;
			const err = (!stranded && status.error) ||
				{ code: 'STALE', message: 'The disc reader no longer serves the disc the game booted with.' };
			pageLog(`Disc read error: ${err.message}`, 'warn');
			subtitle(discLostText(d, err), DISC_ERROR_MS);
			page.error = discFailureText(d, err); // the slate shows it after Stop
			page.errorIsDisc = true;
		} else if (status.state === 'ready' && disc.errorShown) {
			disc.errorShown = false;
			pageLog('The disc can be read again.');
			subtitle(DISC_COPY.readable);
			if (page.errorIsDisc)
				page.error = '';
		}
	}

	function discLostText(d, err) {
		if (d.kind !== 'url')
			return DISC_COPY.lost;
		return err && (err.code === 'TIMEOUT' || err.code === 'NETWORK') ? DISC_COPY.lostServer : DISC_COPY.lostUrl;
	}

	// Resolves once a terminated worker can no longer write into the emulator's memory: when the page is
	// granted the Web Lock the worker held for its whole life (released only when nothing of it runs, a
	// synchronous read into the memory included). Without the lock, after DISC_WORKER_GONE_MS.
	function whenDiscWorkerGone(lock) {
		const wait = () => new Promise((resolve) => setTimeout(resolve, DISC_WORKER_GONE_MS));
		if (!lock)
			return wait();
		const slow = setTimeout(() => pageLog('Still waiting for the stopped disc reader to end.', 'warn'), 10000);
		return navigator.locks.request(lock, () => {}).then(() => clearTimeout(slow), () => {
			clearTimeout(slow);
			return wait();
		});
	}

	// The worker crashed or its loop died. Once it is gone (whenDiscWorkerGone), what it had taken is
	// handed back (BUSY -> PENDING) and a new worker takes over the disc under the same generation; the
	// block stays ready meanwhile, so the emulator's reads wait rather than fail. After the last restart,
	// every waiting request fails instead.
	function onDiscWorkerDied(worker, why) {
		if (!worker || worker !== disc.worker)
			return;
		pageLog(`The disc reader stopped: ${why}`, 'error');
		const lock = disc.workerLock;
		disc.worker = null;
		disc.workerLock = '';
		disc.attached = false;
		disc.restarting = true;
		try {
			worker.terminate();
		} catch {
			// gone already
		}
		for (const resolve of disc.calls.values())
			resolve({ ok: false, error: { code: 'CANCELLED', message: why } });
		disc.calls.clear();
		const restart = disc.restarts++ < DISC_WORKER_RESTARTS;
		render();
		whenDiscWorkerGone(lock).then(() => {
			if (disc.dead)
				return;
			if (restart) {
				handBackDiscRequests();
				startDiscWorker(discResumeFor(disc.current));
			} else {
				failDiscRequests();
				discReaderGone('The disc reader kept failing; discs cannot be read until the page reloads.');
			}
		});
	}

	// Requests the dead worker had taken go back to pending, for the next worker.
	function handBackDiscRequests() {
		const v = discBlockView();
		if (!v)
			return;
		let handed = 0;
		for (let s = 0; s < v.slots; s++) {
			if (Atomics.compareExchange(v.i32, v.hdr + DB_HEADER_WORDS + s * v.slotWords, DS_BUSY, DS_PENDING) === DS_BUSY)
				handed++;
		}
		if (handed)
			pageLog(`${handed} disc read${handed > 1 ? 's' : ''} handed to the next disc reader.`);
	}

	// No worker any more: stop the block and finish its pending and busy requests with CANCELLED, so no
	// emulator thread waits for ever (DISC-DESIGN.md §7).
	function failDiscRequests() {
		const v = discBlockView();
		if (!v)
			return;
		const { i32, hdr } = v;
		Atomics.store(i32, hdr + DB_STATE, DB_STATE_STOPPED);
		for (let s = 0; s < v.slots; s++) {
			const base = hdr + DB_HEADER_WORDS + s * v.slotWords;
			if (Atomics.compareExchange(i32, base, DS_PENDING, DS_BUSY) === DS_PENDING || Atomics.load(i32, base) === DS_BUSY) {
				Atomics.store(i32, base + DS_RESULT, DE_CANCELLED);
				Atomics.store(i32, base, DS_DONE);
				Atomics.notify(i32, base);
			}
		}
		Atomics.add(i32, hdr + DB_DOORBELL, 1);
		Atomics.notify(i32, hdr + DB_DOORBELL);
	}

	// ---- boot and the running game

	// Before a disc boot the worker reads the start of the disc, so a disc that went away (a moved file, a
	// stopped server) is said on the slate instead of hanging the boot in PCSX2's disc detection.
	function discNeedsCheck() {
		const d = disc.current;
		return !!(d && d.opened && !disc.dead && disc.attached);
	}

	function checkDiscThenBoot() {
		const d = disc.current;
		const op = disc.op;
		disc.checking = true;
		render();
		discCall({ type: 'check' }).then((reply) => {
			disc.checking = false;
			if (op !== disc.op || disc.current !== d) {
				render();
				return;
			}
			if (!reply.ok && reply.error && reply.error.code === 'CANCELLED') {
				// The disc reader stopped during the check and is being restarted: nothing was learnt.
				pageLog(`The disc check was interrupted: ${reply.error.message}`, 'warn');
				render();
				return;
			}
			if (!reply.ok) {
				page.autoBoot = false;
				pageLog(`The disc can't be read: ${reply.error ? reply.error.message : 'unknown error'}`, 'warn');
				discError(discFailureText(d, reply.error));
				return;
			}
			startVm(true);
		});
	}

	// startVm(): a disc boot when a disc is open, otherwise the BIOS alone.
	function bootRequest() {
		const d = disc.current;
		disc.runIsDisc = !!(d && d.opened && !disc.dead);
		disc.runTitle = '';
		disc.runSerial = '';
		disc.errorShown = false;
		disc.runGeneration = disc.status && disc.status.disc ? disc.status.disc.generation : 0;
		if (!disc.runIsDisc)
			return api.boot(page.bios.name);
		const flags = params.get('fastboot') === '0' ? DISC_BOOT_FULL : 0;
		pageLog(`Disc boot: ${d.path} (${d.sourceName || d.name}), ${flags ? 'full boot' : 'fast boot'}.`);
		return window.Module.ccall('pcsx2web_boot_disc', 'number', ['string', 'string', 'number'], [page.bios.name, d.path, flags]);
	}

	const utf8Decoder = new TextDecoder();
	function cString(bytes) {
		const end = bytes.indexOf(0);
		return utf8Decoder.decode(end < 0 ? bytes : bytes.subarray(0, end));
	}

	// The game PCSX2 reports (Host::OnGameChanged), read through the seqlock in pcsx2web_game().
	function readGameDetails() {
		const buf = heapBuffer();
		if (!buf || !disc.gamePtr || disc.gamePtr + GAME_SERIAL_AT + GAME_SERIAL_BYTES + GAME_TITLE_BYTES > buf.byteLength)
			return;
		const words = new Uint32Array(buf, disc.gamePtr, 3);
		const seq = Atomics.load(words, 0);
		if (seq === disc.gameSeq || (seq & 1))
			return;
		const copy = new Uint8Array(buf, disc.gamePtr + GAME_SERIAL_AT, GAME_SERIAL_BYTES + GAME_TITLE_BYTES).slice();
		if (Atomics.load(words, 0) !== seq)
			return; // rewritten meanwhile: the next tick reads it again
		disc.gameSeq = seq;
		const serial = cString(copy.subarray(0, GAME_SERIAL_BYTES));
		const title = cString(copy.subarray(GAME_SERIAL_BYTES));
		const live = page.vmState === STATE.STARTING || page.vmState === STATE.RUNNING;
		if (!disc.runIsDisc || !live || !title || (title === disc.runTitle && serial === disc.runSerial))
			return;
		disc.runTitle = title;
		disc.runSerial = serial;
		pageLog(`Game: ${title}${serial ? ` (${serial})` : ''}`);
		const d = disc.current;
		if (d && d.title !== title) {
			d.title = title;
			if (d.remembered)
				rememberDisc(d);
		}
		render();
	}

	function discTick() {
		if (page.phase === 'ready') {
			readGameDetails();
			if (page.vmState !== disc.lastVm) {
				disc.lastVm = page.vmState;
				if (page.vmState === STATE.STOPPED || page.vmState === STATE.FAILED)
					checkMemoryCards(true); // a normal Stop closed the card: keep it now
			}
		}
		checkMemoryCards(false);
		const title = disc.runIsDisc && disc.runTitle && pageState() === 'running' ? `${disc.runTitle} · TabStationToo` : 'TabStationToo';
		if (document.title !== title)
			document.title = title;
	}

	// ---- the slate, the disc menu

	function memcardLine() {
		if (memcards.otherTab)
			return DISC_COPY.savesOtherTab;
		if (memcards.failed)
			return DISC_COPY.savesVisit;
		// Before the runtime mounts the cards, the line says what will happen.
		return memcards.persistent || (!memcards.mounted && typeof indexedDB !== 'undefined') ? DISC_COPY.savesKept : DISC_COPY.savesVisit;
	}

	function discLine(d) {
		return [{ name: d.name }, { sep: 1 }, ...discSizeParts(d.size), { sep: 1 }, discWhere(d)];
	}

	// slateModel() fills the model for the BIOS alone; this adds the disc (UI-SPEC.md §10).
	function applyDiscToSlate(state, m) {
		const d = disc.current;
		const b = page.bios;
		switch (state) {
		case 'nobios':
			if (d || disc.opening)
				m.fine.unshift([{ name: (d || disc.opening).name }, DISC_COPY.waiting]);
			break;
		case 'ready':
			if (!disc.dead)
				m.disc = d ? DISC_COPY.another : DISC_COPY.choose;
			if (d) {
				m.title = [discDisplayTitle(d)];
				m.body = discLine(d);
				m.alt = false; // the BIOS changes from its menu
				m.remove = true;
				m.fine = [[b ? DISC_COPY.withBios(biosTitle(b.name)) + ' ' : '', memcardLine()]];
			} else if (disc.remembered && !disc.dead) {
				m.fine = [[DISC_COPY.lastDisc, { name: disc.remembered.name }, { sep: 1 }, { discLink: DISC_COPY.openAgain }]];
			}
			break;
		case 'queued':
			if (d)
				m.body = discLine(d);
			break;
		case 'starting':
			if (disc.runIsDisc && d && page.vmState !== STATE.RUNNING)
				m.body = [{ name: d.name }, { sep: 1 }, { num: Math.max(0, Math.floor((performance.now() - page.bootAt) / 1000)), unit: 's' }];
			break;
		case 'stopped':
			if (disc.runIsDisc) {
				const title = disc.runTitle || (d ? discDisplayTitle(d) : '');
				m.body = page.ranMs ? [title, { sep: 1 }, 'ran ', { num: fmtDuration(page.ranMs) }] : [title];
			}
			if (!disc.dead)
				m.disc = d ? DISC_COPY.another : DISC_COPY.choose;
			break;
		case 'failed':
			if (disc.runIsDisc && d)
				m.body = ['PCSX2 couldn’t boot ', { name: d.name }, '. The end of the log:'];
			if (!disc.dead)
				m.disc = d ? DISC_COPY.another : DISC_COPY.choose;
			break;
		default:
			break;
		}
	}

	function discMenuOpen() {
		try {
			return discEls.menu.matches(':popover-open');
		} catch {
			return false;
		}
	}

	function hideDiscMenu() {
		if (discMenuOpen())
			discEls.menu.hidePopover();
	}

	function placeDiscMenu() {
		const r = discEls.btn.getBoundingClientRect();
		const width = Math.min(296, window.innerWidth - 32);
		const left = Math.max(16, Math.min(r.left, window.innerWidth - width - 16));
		discEls.menu.style.left = `${Math.round(left)}px`;
		discEls.menu.style.bottom = `${Math.round(Math.max(8, window.innerHeight - r.top + 8))}px`;
	}

	function renderDiscMenu() {
		const d = disc.current;
		setData('disc', d ? 'set' : 'none');
		if (!d || page.phase === 'dead' || page.phase === 'unsupported') {
			hideDiscMenu();
			return;
		}
		setText(discEls.btnName, discDisplayTitle(d));
		setText(discEls.menuName, d.name);
		rich(discEls.menuWhere, [...discSizeParts(d.size), { sep: 1 }, discWhere(d)]);
		const locked = !canChangeBios();
		for (const item of [discEls.menuChoose, discEls.menuRemove]) {
			if (locked) {
				item.setAttribute('aria-disabled', 'true');
				item.setAttribute('aria-describedby', 'disc-menu-reason');
			} else {
				item.removeAttribute('aria-disabled');
				item.removeAttribute('aria-describedby');
			}
		}
		discEls.menuReason.hidden = !locked;
	}

	// ---- remembering the disc (File System Access handles, Chromium)

	function discIdbOpen() {
		return new Promise((resolve, reject) => {
			if (!('indexedDB' in window)) {
				reject(new Error('IndexedDB is not available'));
				return;
			}
			const req = indexedDB.open(DISC_IDB_NAME, 1);
			req.onupgradeneeded = () => req.result.createObjectStore(DISC_IDB_STORE);
			req.onsuccess = () => resolve(req.result);
			req.onerror = () => reject(req.error);
			req.onblocked = () => reject(new Error('IndexedDB open blocked'));
		});
	}

	async function discIdb(mode, fn) {
		const db = await discIdbOpen();
		try {
			return await new Promise((resolve, reject) => {
				const tx = db.transaction(DISC_IDB_STORE, mode);
				const req = fn(tx.objectStore(DISC_IDB_STORE));
				let result;
				req.onsuccess = () => { result = req.result; };
				tx.oncomplete = () => resolve(result);
				tx.onerror = () => reject(tx.error || req.error);
				tx.onabort = () => reject(tx.error || new Error('IndexedDB transaction aborted'));
			});
		} finally {
			db.close();
		}
	}

	async function rememberDisc(d) {
		if (!d.handle)
			return;
		const rec = {
			handle: d.handle, name: d.sourceName || d.name, size: d.size,
			lastModified: (d.file && d.file.lastModified) || 0, title: d.title || '', savedAt: Date.now(),
		};
		try {
			await discIdb('readwrite', (s) => s.put(rec, DISC_IDB_KEY));
		} catch (err) {
			pageLog(`Could not remember the disc in this browser: ${err && err.message}`, 'warn');
			return;
		}
		if (disc.current !== d && disc.opening !== d) {
			// Removed or replaced while being saved: do not bring it back next visit.
			if (!disc.current || !disc.current.remembered)
				forgetRememberedDisc();
			return;
		}
		d.remembered = true;
		disc.remembered = rec;
		render();
	}

	async function forgetRememberedDisc() {
		const had = disc.remembered;
		disc.remembered = null;
		try {
			await discIdb('readwrite', (s) => s.delete(DISC_IDB_KEY));
		} catch (err) {
			if (had)
				pageLog(`Could not forget the disc: ${err && err.message}`, 'warn');
		}
	}

	async function loadRememberedDisc() {
		if (typeof window.showOpenFilePicker !== 'function')
			return; // without the File System Access API there is nothing to reopen
		let rec = null;
		try {
			rec = await discIdb('readonly', (s) => s.get(DISC_IDB_KEY));
		} catch (err) {
			pageLog(`Could not read the remembered disc: ${err && err.message}`, 'warn');
			return;
		}
		if (!rec || !rec.handle || typeof rec.handle.requestPermission !== 'function' || disc.current || disc.opening)
			return;
		disc.remembered = rec;
		pageLog(`Last disc: ${rec.name} (reopens with one press).`);
		render();
	}

	// ---- memory cards
	// Slot 1's card lives in /memcards, an IDBFS (IndexedDB) mount made before main() runs, so it outlives
	// a reload. One tab owns the cards: the one holding the MEMCARD_LOCK Web Lock, for as long as it is
	// open. Only the owner copies them to IndexedDB, so a second tab can never overwrite the first one's
	// saves with its older card; its own saves last for its visit (the slate says so).
	// PCSX2 keeps the card file open and writes it in place (unbuffered on wasm), so the copy is driven from
	// here: once the writes have been quiet for MEMCARD_QUIET_MS, at once when the PS2 stops, and when the
	// page is hidden. FS.syncfs copies only files whose time changed; for an 8 MB card that blocks this
	// thread for 30-50 ms. The copy is asynchronous and cannot finish during an unload, so from the first write
	// until the copy is done, leaving or reloading the page asks first (beforeunload).

	const MEMCARD_DIR = '/memcards';
	const MEMCARD_QUIET_MS = 1500;
	const MEMCARD_LOCK = 'tabstationtoo-memcards';
	const MEMCARD_LOCK_WAIT_MS = 2000; // on a reload, the page being replaced may hold the lock a moment longer
	const memcards = {
		mounted: false, persistent: false, failed: false, owner: false, otherTab: false,
		seen: '', changedAt: 0, saved: '', syncing: false, again: false, warned: false, guarded: false,
	};

	// Resolves true once this tab owns the cards, false when another tab keeps them.
	function claimMemoryCards() {
		const locks = navigator.locks;
		if (!locks || typeof locks.request !== 'function')
			return Promise.resolve(true); // no Web Locks: every tab copies its card, as without this
		return new Promise((resolve) => {
			const controller = new AbortController();
			const timer = setTimeout(() => controller.abort(), MEMCARD_LOCK_WAIT_MS);
			locks.request(MEMCARD_LOCK, { signal: controller.signal }, () => {
				clearTimeout(timer);
				resolve(true);
				return new Promise(() => {}); // held until the page goes away
			}).catch(() => {
				clearTimeout(timer);
				resolve(false);
			});
		});
	}

	// Card writes reach the file system on this thread (Emscripten runs a pthread's file calls here), so
	// they are seen as they happen: the unload guard is up from the first write of a save.
	function watchMemcardWrites(FS) {
		const write = FS.write;
		if (typeof write !== 'function' || write.memcardWatch)
			return;
		const watched = function (stream) {
			const result = write.apply(this, arguments);
			if (stream && typeof stream.path === 'string' && stream.path.startsWith(`${MEMCARD_DIR}/`))
				onMemcardWrite();
			return result;
		};
		watched.memcardWatch = true;
		FS.write = watched;
	}

	function onMemcardWrite() {
		memcards.changedAt = performance.now();
		if (memcards.owner && memcards.persistent)
			setMemcardGuard(true);
	}

	function onBeforeUnloadWithCard(e) {
		persistMemoryCards();
		e.preventDefault();
		e.returnValue = ''; // browsers before preventDefault() was honoured here
	}

	// Only while a write is not in IndexedDB yet: a beforeunload listener also keeps the page out of the
	// back/forward cache in some browsers.
	function setMemcardGuard(on) {
		if (on === memcards.guarded)
			return;
		memcards.guarded = on;
		if (on)
			window.addEventListener('beforeunload', onBeforeUnloadWithCard);
		else
			window.removeEventListener('beforeunload', onBeforeUnloadWithCard);
	}

	// Module.preRun: before main(), so PCSX2 finds the cards that were saved.
	function mountMemoryCards() {
		const M = window.Module;
		const FS = M.FS;
		try {
			FS.mkdir(MEMCARD_DIR);
		} catch {
			// exists
		}
		const IDBFS = FS.filesystems && FS.filesystems.IDBFS;
		if (!IDBFS || typeof indexedDB === 'undefined') {
			memcards.failed = true;
			pageLog('IndexedDB is not available: the memory card lasts for this visit only.', 'warn');
			return;
		}
		try {
			FS.mount(IDBFS, {}, MEMCARD_DIR);
		} catch (err) {
			memcards.failed = true;
			pageLog(`Could not keep memory cards in this browser: ${errorText(err)}`, 'warn');
			return;
		}
		memcards.mounted = true;
		watchMemcardWrites(FS);
		M.addRunDependency('memcards');
		claimMemoryCards().then((owner) => {
			memcards.owner = owner;
			memcards.otherTab = !owner;
			if (!owner)
				pageLog('TabStationToo is open in another tab, which keeps the memory card: saves in this tab last for this visit only.', 'warn');
			// Loaded after the lock: the page a reload replaced has made its last copy by then.
			FS.syncfs(true, (err) => {
				if (err) {
					memcards.failed = true;
					pageLog(`Could not load the memory cards from this browser: ${errorText(err)}`, 'warn');
				} else {
					memcards.persistent = true;
				}
				memcards.seen = memcards.saved = memcardSignature();
				if (memcards.saved)
					pageLog(`Memory cards from this browser: ${memcards.saved.split(';').filter(Boolean).map((e) => e.split(':')[0]).join(', ')}`);
				M.removeRunDependency('memcards');
			});
		});
	}

	function memcardSignature() {
		const FS = window.Module && window.Module.FS;
		if (!FS)
			return '';
		let sig = '';
		try {
			for (const name of FS.readdir(MEMCARD_DIR)) {
				if (name === '.' || name === '..')
					continue;
				const st = FS.stat(`${MEMCARD_DIR}/${name}`);
				sig += `${name}:${st.size}:${st.mtime.getTime()};`;
			}
		} catch {
			// no folder yet
		}
		return sig;
	}

	function checkMemoryCards(now) {
		if (!memcards.persistent || !memcards.owner)
			return;
		const sig = memcardSignature();
		const t = performance.now();
		if (sig !== memcards.seen) {
			memcards.seen = sig;
			memcards.changedAt = t;
		}
		const unsaved = sig !== memcards.saved;
		setMemcardGuard(unsaved || memcards.syncing);
		if (unsaved && (now || t - memcards.changedAt >= MEMCARD_QUIET_MS))
			persistMemoryCards();
	}

	function persistMemoryCards() {
		if (!memcards.persistent || !memcards.owner)
			return;
		if (memcards.syncing) {
			memcards.again = true;
			return;
		}
		const sig = memcardSignature();
		if (sig === memcards.saved) {
			setMemcardGuard(false);
			return;
		}
		memcards.syncing = true;
		setMemcardGuard(true);
		window.Module.FS.syncfs(false, (err) => {
			memcards.syncing = false;
			if (err) {
				pageLog(`Could not save the memory card in this browser: ${errorText(err)}`, 'warn');
				if (!memcards.warned) {
					memcards.warned = true;
					subtitle(DISC_COPY.memcardFailed);
				}
			} else {
				memcards.saved = sig;
				pageLog('Memory card saved in this browser.');
			}
			if (memcards.again) {
				memcards.again = false;
				persistMemoryCards();
			} else {
				// A write during the copy keeps the guard up until the next copy.
				setMemcardGuard(memcardSignature() !== memcards.saved);
			}
		});
	}

	// ---- wiring

	function wireDiscUi() {
		discEls.slateDisc.addEventListener('click', openDiscPicker);
		discEls.slateRemove.addEventListener('click', removeDisc);
		discEls.input.addEventListener('change', () => {
			const file = discEls.input.files && discEls.input.files[0];
			discEls.input.value = '';
			acceptDiscFile(file, null);
		});

		discEls.menu.addEventListener('beforetoggle', (e) => {
			if (e.newState === 'open') {
				renderDiscMenu();
				placeDiscMenu();
			}
		});
		discEls.menu.addEventListener('toggle', (e) => {
			const open = e.newState === 'open';
			discEls.btn.setAttribute('aria-expanded', open ? 'true' : 'false');
			if (open) {
				discEls.menuChoose.focus({ preventScroll: true });
			} else {
				const a = document.activeElement;
				if (!a || a === document.body || discEls.menu.contains(a))
					discEls.btn.focus({ preventScroll: true });
			}
			updateChrome();
		});
		discEls.menu.addEventListener('focusout', (e) => {
			const to = e.relatedTarget;
			if (to && !discEls.menu.contains(to) && to !== discEls.btn)
				hideDiscMenu();
		});
		discEls.menuChoose.addEventListener('click', () => {
			if (discEls.menuChoose.getAttribute('aria-disabled') !== 'true')
				openDiscPicker();
		});
		discEls.menuRemove.addEventListener('click', () => {
			if (discEls.menuRemove.getAttribute('aria-disabled') !== 'true')
				removeDisc();
		});
		window.addEventListener('resize', () => {
			if (discMenuOpen())
				placeDiscMenu();
		});

		// Keep the memory card when the page goes into the background, and start the copy when it goes
		// away (the unload guard has asked first while a write was not copied yet).
		window.addEventListener('pagehide', persistMemoryCards);
		document.addEventListener('visibilitychange', () => {
			if (document.visibilityState === 'hidden')
				persistMemoryCards();
		});
	}

	// Read-only hooks for automated tests, next to window.PCSX2Web.
	window.PCSX2WebDisc = Object.freeze({
		stats: () => ({
			disc: disc.current ? { name: disc.current.name, path: disc.current.path, size: disc.current.size,
				kind: disc.current.kind, opened: disc.current.opened, title: disc.current.title,
				remembered: disc.current.remembered } : null,
			opening: !!disc.opening,
			reader: disc.dead ? 'dead' : disc.restarting ? 'restarting' : disc.status ? disc.status.state : 'none',
			readerError: disc.status && disc.status.error ? { ...disc.status.error } : null,
			readerGeneration: disc.status && disc.status.disc ? disc.status.disc.generation : null,
			readerLock: !!disc.workerLock,
			restarts: disc.restarts,
			runGeneration: disc.runGeneration,
			readerStats: disc.status ? { ...disc.status.stats } : null,
			game: { title: disc.runTitle, serial: disc.runSerial, disc: disc.runIsDisc },
			lastDisc: disc.remembered ? disc.remembered.name : null,
			memcards: { persistent: memcards.persistent, owner: memcards.owner, otherTab: memcards.otherTab, failed: memcards.failed,
				saved: memcards.saved, syncing: memcards.syncing, unloadGuard: memcards.guarded },
		}),
	});

	// ------------------------------------------------------------------ keyboard

	function resolveKeyCodes() {
		const unknown = [];
		for (const name of new Set(Object.values(KEY_NAMES))) {
			const code = api.keyCode(name);
			if (typeof code === 'number' && code >= 0)
				keyCodes.set(name, code);
			else
				unknown.push(name);
		}
		if (unknown.length)
			pageLog(`Keys the emulator does not know (ignored): ${unknown.join(', ')}`, 'warn');
	}

	// Keys go to the PS2 while it shows a picture, the window has focus, and focus is on the
	// screen (or nowhere in particular). Focus on the sill, transport or a drawer releases it, in
	// the fullscreen theatre too.
	function captureActive() {
		if (page.phase !== 'ready' || page.vmState !== STATE.RUNNING || !page.frameSinceBoot)
			return false;
		if (!document.hasFocus())
			return false;
		const a = document.activeElement;
		return a === els.screen || a === document.body || a === null || a === document.documentElement;
	}

	function sendKey(code, pressed) {
		if (!api || page.phase !== 'ready')
			return;
		try {
			api.key(code, pressed);
		} catch (err) {
			fatal('crash', 'Calling pcsx2web_key failed', err);
		}
	}

	function markKey(name, down) {
		for (const k of document.querySelectorAll(`kbd[data-key="${name}"]`))
			k.classList.toggle('down', down);
	}

	// Single-key shortcuts only work while focus is in the theatre (WCAG 2.1.4).
	function inShortcutScope() {
		const a = document.activeElement;
		if (!a || a === document.body || a === document.documentElement)
			return captureActive();
		if (els.biosMenu.contains(a) || discEls.menu.contains(a) || els.about.contains(a))
			return false;
		return els.theatre.contains(a);
	}

	// Runs before the pad mapping. Returns true when the key was a page shortcut.
	function onShortcut(e) {
		if (swallowed.has(e.code)) {
			e.preventDefault(); // autorepeat of a shortcut key
			return true;
		}
		if (e.repeat || e.ctrlKey || e.metaKey)
			return false;
		const enter = e.code === 'Enter' || e.code === 'NumpadEnter';
		let action = null;
		if (e.altKey) {
			if (enter && !e.shiftKey)
				action = toggleFullscreen;
		} else if (enter && !e.shiftKey && document.activeElement === els.screen &&
			['ready', 'stopped', 'failed'].includes(pageState())) {
			action = boot;
		} else if (e.key === '?' && !PAD_CODES.has(e.code) && inShortcutScope()) {
			action = () => toggleDrawer('keys');
		} else if (e.code === 'Backquote' && !e.shiftKey && inShortcutScope()) {
			action = () => toggleDrawer('log');
		}
		if (!action)
			return false;
		e.preventDefault();
		swallowed.add(e.code);
		noteActivity();
		action();
		return true;
	}

	// Esc hides a transport tooltip (hovered or focused) until the pointer and focus have left
	// its button (WCAG 1.4.13).
	function dismissTips() {
		for (const b of document.querySelectorAll('.t-btn:hover, .t-btn:focus-visible'))
			b.classList.add('tip-off');
	}

	// Esc: native dialog/popover close first, then a drawer, then the keyboard.
	function onEscape(e) {
		if (els.about.open || menuOpen() || discMenuOpen())
			return;
		dismissTips();
		const a = document.activeElement;
		const drawer = drawerEl(page.drawer);
		if (drawer && drawer.contains(a)) {
			e.preventDefault();
			closeDrawer(true);
			return;
		}
		if (captureActive()) {
			e.preventDefault();
			releaseKeyboard(true);
		}
	}

	function onKeyDown(e) {
		// macOS (AppKit) drops keyup for keys released while Cmd is down, so anything held now may
		// never see its keyup. (Meta is Win/Super elsewhere; releasing there is harmless.)
		if ((e.key === 'Meta' || e.metaKey) && heldPhysical.size)
			releaseAllKeys(true);
		if (onShortcut(e))
			return;
		if (e.key === 'Escape' || e.code === 'Escape') {
			onEscape(e);
			return;
		}
		// Tab out of the captured screen releases the keyboard to "Press Enter to play", the next
		// stop in the tab order. It never reaches the PS2, and it never lands on Stop: Tab sits
		// next to Q and 1, and Enter is Start, so Tab then Enter would end the session.
		if (e.code === 'Tab' && !e.shiftKey && !e.altKey && !e.ctrlKey && !e.metaKey && captureActive()) {
			e.preventDefault();
			releaseKeyboard(true);
			return;
		}
		const name = KEY_NAMES[e.code];
		if (!name)
			return;
		const code = keyCodes.get(name);
		if (code === undefined || !captureActive())
			return;
		// Leave browser shortcuts (Ctrl+R, Ctrl+Shift+I, Alt+Left...) alone.
		if ((e.ctrlKey || e.metaKey || e.altKey) && !MODIFIER_NAMES.has(name))
			return;
		if (!KEEP_DEFAULT.has(name))
			e.preventDefault();
		if (e.repeat || heldPhysical.has(e.code))
			return;
		heldPhysical.set(e.code, code);
		const refs = (heldHostRefs.get(code) || 0) + 1;
		heldHostRefs.set(code, refs);
		if (refs === 1) {
			sendKey(code, true);
			markKey(name, true);
		}
	}

	function onKeyUp(e) {
		if (swallowed.delete(e.code)) {
			e.preventDefault();
			return;
		}
		const code = heldPhysical.get(e.code);
		if (code === undefined)
			return;
		heldPhysical.delete(e.code);
		const name = KEY_NAMES[e.code];
		if (!KEEP_DEFAULT.has(name))
			e.preventDefault();
		const refs = (heldHostRefs.get(code) || 1) - 1;
		if (refs > 0) {
			heldHostRefs.set(code, refs);
			return;
		}
		heldHostRefs.delete(code);
		sendKey(code, false);
		markKey(name, false);
	}

	function releaseAllKeys(send) {
		if (send !== false) {
			for (const code of heldHostRefs.keys())
				sendKey(code, false);
		}
		heldPhysical.clear();
		heldHostRefs.clear();
		swallowed.clear();
		for (const k of document.querySelectorAll('kbd.down'))
			k.classList.remove('down');
	}

	// Esc, or fullscreen ended by the browser: keys stop going to the PS2 and focus moves to the
	// "Click the screen to play" button.
	function releaseKeyboard(say) {
		releaseAllKeys(true);
		els.captureText.hidden = true;
		els.capture.hidden = false;
		els.capture.focus({ preventScroll: true });
		renderCapture();
		noteActivity();
		if (say)
			announce(COPY.announce.released);
	}

	function onFocusChange() {
		if (!captureActive())
			releaseAllKeys(true);
		renderCapture();
		updateChrome();
	}

	function onWindowFocusChange() {
		if (!captureActive())
			releaseAllKeys(true);
		renderCapture();
		updateChrome();
	}

	// After a mouse click on a page control, give the keyboard back to the PS2. Keyboard activation
	// (event.detail === 0) keeps focus where the user put it.
	function returnFocusAfterPointer(e) {
		if (e && e.detail > 0 && page.vmState === STATE.RUNNING && page.frameSinceBoot)
			els.screen.focus({ preventScroll: true });
	}

	// Letter labels for the keyboard layout in use (AZERTY shows Z Q S D for the left stick).
	// Digits keep their printed digit: on AZERTY navigator.keyboard reports & é " ' for them.
	let layoutMap = null;
	async function applyLayoutLabels() {
		const kb = navigator.keyboard;
		if (!kb || typeof kb.getLayoutMap !== 'function')
			return;
		try {
			layoutMap = await kb.getLayoutMap();
		} catch {
			return; // QWERTY labels stay
		}
		for (const k of document.querySelectorAll('kbd[data-code]'))
			setText(k, keyLabel(k.dataset.code, k.textContent));
	}

	function keyLabel(code, fallback) {
		const ch = layoutMap && layoutMap.get(code);
		return ch && ch.length === 1 && ch.trim() ? ch.toUpperCase() : fallback;
	}

	// ------------------------------------------------------------------ preferences

	function readPref(key) {
		try {
			return localStorage.getItem(key);
		} catch {
			return null;
		}
	}

	function writePref(key, value) {
		try {
			localStorage.setItem(key, value);
		} catch {
			// storage unavailable: preference lasts for this visit only
		}
	}

	function setCrisp(on) {
		setData('crisp', on ? 'on' : 'off');
		els.btnCrisp.setAttribute('aria-pressed', on ? 'true' : 'false');
	}

	// ------------------------------------------------------------------ fullscreen

	function fullscreenSupported() {
		return !!(document.fullscreenEnabled || document.webkitFullscreenEnabled);
	}

	function fullscreenElement() {
		return document.fullscreenElement || document.webkitFullscreenElement || null;
	}

	function toggleFullscreen() {
		if (!fullscreenSupported()) {
			subtitle(COPY.fullscreenUnavailable);
			return;
		}
		try {
			if (fullscreenElement()) {
				page.fsByPage = true;
				const exit = document.exitFullscreen || document.webkitExitFullscreen;
				const p = exit && exit.call(document);
				if (p && p.catch)
					p.catch(() => { page.fsByPage = false; });
			} else {
				const t = els.theatre;
				const req = t.requestFullscreen || t.webkitRequestFullscreen;
				const p = req && req.call(t, { navigationUI: 'hide' });
				if (p && p.catch)
					p.catch((err) => {
						pageLog(`Fullscreen was refused: ${err && err.message}`, 'warn');
						subtitle(COPY.fullscreenUnavailable);
					});
			}
		} catch (err) {
			pageLog(`Fullscreen is not available: ${err && err.message}`, 'warn');
			subtitle(COPY.fullscreenUnavailable);
		}
	}

	function onFullscreenChange() {
		const on = fullscreenElement() === els.theatre;
		setData('fs', on ? 'on' : 'off');
		const label = on ? COPY.exitFullscreen : COPY.fullscreen;
		setText(els.btnFsLabel, label);
		setText(els.btnFsTip, label);
		els.btnFsIcon.setAttribute('href', icon(on ? 'corners-in' : 'corners-out'));
		if (on) {
			// Whatever is outside the theatre is out of sight now.
			if (!els.theatre.contains(document.activeElement))
				els.screen.focus({ preventScroll: true });
		} else if (!page.fsByPage && pageState() === 'running') {
			// The browser ended it (usually Esc). Like Esc when keys were going to the PS2; a
			// control the user chose (a drawer, the transport) keeps focus, and the controls show.
			const a = document.activeElement;
			if (!a || a === els.screen || a === document.body || a === document.documentElement)
				releaseKeyboard(true);
			else
				noteActivity();
		}
		page.fsByPage = false;
		if (menuOpen())
			placeMenu();
		render();
	}

	// ------------------------------------------------------------------ subtitle, announcer

	let subtitleOpen = false;
	let subtitleTimer = 0;
	let subtitleToken = 0;
	let subtitleLeft = 0;
	let subtitleSince = 0;
	let subtitleHover = false;

	function reducedMotion() {
		return reducedMotionQuery.matches;
	}

	// Confirmations and hints only. The newest replaces the last; it pauses while hovered.
	// #subtitle is always rendered (a live region inside display:none is not announced), and the
	// text is set a tick after clearing it so a repeated message is announced again.
	function subtitle(text, ms = SUBTITLE_MS) {
		const token = ++subtitleToken;
		clearTimeout(subtitleTimer);
		els.subtitle.textContent = '';
		subtitleOpen = true;
		setData('subtitle', 'on');
		setTimeout(() => {
			if (token === subtitleToken)
				els.subtitle.textContent = text;
		}, 50);
		subtitleLeft = ms;
		startSubtitleTimer();
		updateChrome();
	}

	function startSubtitleTimer() {
		clearTimeout(subtitleTimer);
		if (subtitleHover || !subtitleOpen)
			return;
		subtitleSince = performance.now();
		subtitleTimer = setTimeout(endSubtitle, subtitleLeft);
	}

	function endSubtitle() {
		const token = subtitleToken;
		subtitleOpen = false;
		setData('subtitle', 'off');
		setTimeout(() => {
			if (token === subtitleToken && !subtitleOpen)
				els.subtitle.textContent = '';
		}, reducedMotion() ? 0 : 320);
		updateChrome();
	}

	// Messages from the same moment (a state change and an error, say) are read together.
	let announceToken = 0;
	let announceQueue = [];
	function announce(text) {
		if (!text)
			return;
		const token = ++announceToken;
		if (!announceQueue.includes(text))
			announceQueue.push(text);
		els.announce.textContent = '';
		setTimeout(() => {
			if (token !== announceToken)
				return;
			els.announce.textContent = announceQueue.join(' ');
			announceQueue = [];
		}, 50);
	}

	function flashLabel(el, text, restore) {
		setText(el, text);
		clearTimeout(el._flash);
		el._flash = setTimeout(() => setText(el, restore), COPY_FEEDBACK_MS);
	}

	// The button's own label says it for 2 s; screen readers do not re-read a focused button's
	// new name, so it is announced too.
	async function copyText(text, labelEl) {
		const restore = labelEl === els.slateCopyLabel ? COPY.copyDetails : COPY.copy;
		try {
			await navigator.clipboard.writeText(text);
			flashLabel(labelEl, COPY.copied, restore);
			announce(COPY.copied);
		} catch (err) {
			pageLog(`Could not copy: ${err && err.message}`, 'warn');
			flashLabel(labelEl, COPY.copyFailed, restore);
			announce(COPY.copyFailed);
		}
	}

	// ------------------------------------------------------------------ controls hiding

	let chromeHidden = false;
	let chromeTimer = 0;
	let lastActivity = 0;
	let pointerOverChrome = false;
	let lastPointerX = NaN;
	let lastPointerY = NaN;

	function chromeMayHide() {
		return pageState() === 'running' && captureActive() && document.hasFocus() &&
			!page.drawer && !menuOpen() && !discMenuOpen() && !els.about.open && !subtitleOpen && !page.drag &&
			!pointerOverChrome && !page.keepChrome && hoverQuery.matches;
	}

	function setChromeHidden(hidden) {
		if (chromeHidden === hidden)
			return;
		chromeHidden = hidden;
		setData('chrome', hidden ? 'hidden' : 'shown');
	}

	function scheduleChrome() {
		clearTimeout(chromeTimer);
		chromeTimer = 0;
		if (!chromeMayHide())
			return;
		const wait = Math.max(0, CHROME_IDLE_MS - (performance.now() - lastActivity));
		chromeTimer = setTimeout(() => {
			chromeTimer = 0;
			if (chromeMayHide())
				setChromeHidden(true);
		}, wait);
	}

	// Pointer activity, page shortcuts, Esc and state changes bring the controls back.
	function noteActivity() {
		lastActivity = performance.now();
		setChromeHidden(false);
		scheduleChrome();
	}

	// After anything that may change whether hiding is allowed.
	function updateChrome() {
		if (!chromeMayHide()) {
			clearTimeout(chromeTimer);
			chromeTimer = 0;
			setChromeHidden(false);
		} else if (!chromeHidden && !chromeTimer) {
			scheduleChrome();
		}
	}

	// ------------------------------------------------------------------ drawers, menu, dialog

	let drawerCloseTimer = 0;

	function drawerEl(name) {
		return name === 'keys' ? els.keysDrawer : name === 'log' ? els.logDrawer : null;
	}

	function drawerToggle(name) {
		return name === 'keys' ? els.btnKeys : els.btnLog;
	}

	// data-drawer (which widens the column under the screen) is cleared only here, once the
	// closing drawer is gone, so the column and the screen resize together, once.
	function finishDrawerClose() {
		clearTimeout(drawerCloseTimer);
		drawerCloseTimer = 0;
		for (const el of [els.keysDrawer, els.logDrawer]) {
			if (el.classList.contains('closing')) {
				el.classList.remove('closing');
				el.hidden = true;
			}
		}
		if (!page.drawer)
			setData('drawer', '');
		syncScreenWidth();
	}

	function openDrawer(name) {
		if (page.drawer === name)
			return;
		finishDrawerClose();
		if (page.drawer) {
			// Switching: hide the other one at once so the screen resizes only once.
			drawerEl(page.drawer).hidden = true;
			drawerToggle(page.drawer).setAttribute('aria-expanded', 'false');
		}
		page.drawer = name;
		const el = drawerEl(name);
		el.hidden = false;
		drawerToggle(name).setAttribute('aria-expanded', 'true');
		setData('drawer', name);
		if (name === 'log')
			renderFullLog();
		else
			applyLayoutLabels();
		syncScreenWidth();
		updateChrome();
	}

	function closeDrawer(focusToggle) {
		const name = page.drawer;
		if (!name)
			return;
		const el = drawerEl(name);
		const toggle = drawerToggle(name);
		const hadFocus = el.contains(document.activeElement);
		page.drawer = '';
		toggle.setAttribute('aria-expanded', 'false');
		if (focusToggle || hadFocus)
			toggle.focus({ preventScroll: true });
		finishDrawerClose(); // a drawer that was already closing
		if (reducedMotion()) {
			el.hidden = true;
			finishDrawerClose();
		} else {
			el.classList.add('closing');
			drawerCloseTimer = setTimeout(finishDrawerClose, 120);
		}
		updateChrome();
	}

	function toggleDrawer(name) {
		if (page.drawer === name)
			closeDrawer(false);
		else
			openDrawer(name);
	}

	function menuOpen() {
		try {
			return els.biosMenu.matches(':popover-open');
		} catch {
			return false;
		}
	}

	function hideMenu() {
		if (menuOpen())
			els.biosMenu.hidePopover();
	}

	// The menu opens upward from its trigger.
	function placeMenu() {
		const r = els.biosBtn.getBoundingClientRect();
		const width = Math.min(296, window.innerWidth - 32);
		const left = Math.max(16, Math.min(r.left, window.innerWidth - width - 16));
		els.biosMenu.style.left = `${Math.round(left)}px`;
		els.biosMenu.style.bottom = `${Math.round(Math.max(8, window.innerHeight - r.top + 8))}px`;
	}

	function showMenuItems() {
		els.menuConfirm.hidden = true;
		els.menuItems.hidden = false;
	}

	function openPicker() {
		if (!canChangeBios()) {
			subtitle(COPY.dropBlocked);
			return;
		}
		els.biosInput.click();
	}

	function openLogFromSlate() {
		openDrawer('log');
		els.log.focus({ preventScroll: true });
	}

	// Also done synchronously, because a hidden tab gets no ResizeObserver callbacks.
	function syncScreenWidth() {
		const w = els.screen.getBoundingClientRect().width;
		if (w)
			document.documentElement.style.setProperty('--screen-w', `${w}px`);
	}

	function focusPrimary() {
		if (!els.slatePrimary.hidden)
			els.slatePrimary.focus({ preventScroll: true });
	}

	// Focus the page may move: nothing chosen, the screen, or a control that just disappeared.
	function focusIsFree() {
		const a = document.activeElement;
		return !a || a === document.body || a === document.documentElement || a === els.screen ||
			!a.isConnected || a.getClientRects().length === 0;
	}

	// ------------------------------------------------------------------ rendering

	function setText(el, text) {
		if (el.textContent !== text)
			el.textContent = text;
	}

	function setData(key, value) {
		if (els.body.dataset[key] !== value)
			els.body.dataset[key] = value;
	}

	function span(cls, text) {
		const s = document.createElement('span');
		s.className = cls;
		s.textContent = text;
		return s;
	}

	function svgIcon(name, cls) {
		const svg = document.createElementNS(SVG_NS, 'svg');
		svg.setAttribute('class', cls);
		svg.setAttribute('aria-hidden', 'true');
		svg.setAttribute('focusable', 'false');
		const use = document.createElementNS(SVG_NS, 'use');
		use.setAttribute('href', icon(name));
		svg.appendChild(use);
		return svg;
	}

	function extLink(text, href) {
		const a = document.createElement('a');
		a.href = href;
		a.target = '_blank';
		a.rel = 'noopener noreferrer';
		a.textContent = text;
		a.appendChild(svgIcon('arrow-up-right', 'icon-ext'));
		a.appendChild(span('vh', ' (opens in a new tab)'));
		return a;
	}

	// Rich text: strings, { name } (mono file name), { num, unit }, { sep }, { br }, { line }
	// (a mono line of its own) and { link, href }. Rebuilt only when the parts change.
	const richKeys = new WeakMap();
	function rich(el, parts) {
		const key = JSON.stringify(parts);
		if (richKeys.get(el) === key)
			return;
		richKeys.set(el, key);
		el.textContent = '';
		for (const p of parts) {
			if (p === null || p === undefined || p === '')
				continue;
			if (typeof p === 'string' || typeof p === 'number') {
				el.appendChild(document.createTextNode(String(p)));
			} else if (p.name !== undefined) {
				el.appendChild(span('name', p.name));
			} else if (p.num !== undefined) {
				el.appendChild(span('num', String(p.num)));
				if (p.unit) {
					el.appendChild(document.createTextNode('\u00a0'));
					el.appendChild(span('unit', p.unit));
				}
			} else if (p.sep) {
				el.appendChild(span('sep', ' · '));
			} else if (p.br) {
				el.appendChild(document.createElement('br'));
			} else if (p.line !== undefined) {
				const s = span('name', p.line);
				s.style.display = 'block';
				s.style.marginBottom = '4px';
				el.appendChild(s);
			} else if (p.link !== undefined) {
				el.appendChild(extLink(p.link, p.href));
			} else if (p.logLink !== undefined) {
				const b = document.createElement('button');
				b.type = 'button';
				b.className = 'link-btn';
				b.textContent = p.logLink;
				b.addEventListener('click', openLogFromSlate);
				el.appendChild(b);
			} else if (p.discLink !== undefined) {
				const b = document.createElement('button');
				b.type = 'button';
				b.className = 'link-btn';
				b.textContent = p.discLink;
				b.addEventListener('click', reopenLastDisc);
				el.appendChild(b);
			}
		}
	}

	function runtimeState() {
		if (page.phase === 'ready')
			return 'ready';
		if (!page.runtimeRequested)
			return 'deferred';
		return page.wasmLoaded ? 'init' : 'loading';
	}

	// The page state, as body[data-state].
	function pageState() {
		if (page.phase === 'unsupported')
			return 'unsupported';
		if (page.phase === 'dead')
			return 'dead';
		if (page.phase === 'ready') {
			if (page.vmState === STATE.STARTING)
				return 'starting';
			if (page.vmState === STATE.RUNNING)
				return page.frameSinceBoot ? 'running' : 'starting';
		}
		// A ?bios= file still on its way is already the chosen BIOS: cancelling its queued boot shows the
		// title card it will land on, not the intake.
		if (!page.bios)
			return page.urlBiosName ? (page.autoBoot ? 'queued' : 'ready') : 'nobios';
		if (page.autoBoot)
			return 'queued';
		if (page.outcome === 'failed')
			return 'failed';
		if (page.outcome === 'stopped')
			return 'stopped';
		return 'ready';
	}

	function downloadTotals() {
		const w = progress.wasm;
		const d = progress.data;
		const total = w.total + (d.seen ? d.total : 0);
		const loaded = Math.min(w.loaded, w.total || w.loaded) + (d.seen ? Math.min(d.loaded, d.total || d.loaded) : 0);
		const known = w.total > 0 && (!d.seen || d.total > 0);
		const full = page.wasmLoaded && (!d.seen || d.done);
		return { loaded, total, known, full };
	}

	function loadFraction() {
		const t = downloadTotals();
		if (runtimeState() === 'init' || t.full)
			return 1;
		return t.known && t.total ? Math.min(1, t.loaded / t.total) : 0;
	}

	// The hairline under the screen and the sill's loading line: real bytes only.
	function renderProgress() {
		const runtime = runtimeState();
		if (runtime === 'ready' || page.phase === 'dead' || page.phase === 'unsupported') {
			if (els.hairline.isConnected)
				els.hairline.remove();
			rich(els.loadLine, []);
		} else if (runtime === 'deferred') {
			rich(els.loadLine, []);
		} else {
			const t = downloadTotals();
			const frac = loadFraction();
			els.hairlineFill.style.transform = `scaleX(${frac.toFixed(4)})`;
			els.hairline.setAttribute('aria-valuenow', String(Math.round(frac * 100)));
			const mb = (n) => (n / MB).toFixed(1);
			if (runtime === 'init' || t.full) {
				els.hairline.setAttribute('aria-valuetext', COPY.runtimeStarting);
				rich(els.loadLine, [COPY.runtimeStarting]);
			} else if (t.known) {
				els.hairline.setAttribute('aria-valuetext', `${mb(t.loaded)} of ${mb(t.total)} megabytes`);
				rich(els.loadLine, [COPY.preparing, '\u2002', { num: mb(t.loaded) }, ' / ', { num: mb(t.total), unit: 'MB' }]);
			} else {
				els.hairline.setAttribute('aria-valuetext', COPY.preparing);
				rich(els.loadLine, [COPY.preparing]);
			}
		}
		if (page.lastState === 'queued')
			renderSlate('queued');
	}

	function fmtDuration(ms) {
		const s = Math.max(0, Math.floor(ms / 1000));
		const h = Math.floor(s / 3600);
		const mm = String(Math.floor((s % 3600) / 60)).padStart(2, '0');
		const ss = String(s % 60).padStart(2, '0');
		return h ? `${h}:${mm}:${ss}` : `${mm}:${ss}`;
	}

	function biosLine(b) {
		return [{ name: b.name }, { sep: 1 }, ...sizeParts(b.size), { sep: 1 }, storedText(b)];
	}

	// The ?bios= file while it is being fetched: no size yet.
	function fetchingBiosLine() {
		return [{ name: page.urlBiosName || '' }, { sep: 1 }, COPY.stored.url];
	}

	// What the slate shows for a page state (copy from COPY). Null for running: the slate fades
	// out with the content it had.
	function slateModel(state) {
		const b = page.bios;
		const m = {
			key: state, title: [], body: null, meta: false, excerpt: '', primary: null, aside: '',
			alt: false, openLog: false, copy: false, error: page.error, dev: false, fine: [],
		};
		const bootPrimary = (label, iconName) => ({ label, icon: iconName, cap: true, action: boot });
		if (page.drag && (state !== 'dead' && state !== 'unsupported')) {
			m.key = 'drop-' + page.drag;
			m.title = [page.drag === 'ok' ? (page.bios ? DISC_COPY.drop : COPY.drop) : (page.bios ? DISC_COPY.blockedAny : COPY.dropBlocked)];
			m.error = '';
			return m;
		}
		switch (state) {
		case 'unsupported': {
			const env = page.env || { cause: '', checks: [] };
			m.title = [COPY.unsupported];
			m.body = [env.cause];
			m.dev = true;
			m.error = '';
			break;
		}
		case 'dead': {
			const f = page.fatal || {};
			m.key = 'dead-' + f.kind;
			m.error = '';
			if (f.kind === 'download') {
				m.title = [COPY.downloadFailed];
				m.body = [{ line: f.line }, COPY.downloadBody];
			} else if (f.kind === 'build') {
				m.title = [COPY.buildBroken];
				m.excerpt = f.detail;
			} else {
				m.title = [COPY.fatal];
				const saved = b && b.remembered ? ` ${COPY.fatalSaved}` : '';
				m.body = [f.kind === 'init' ? COPY.refused + saved : COPY.fatalBody + saved];
			}
			m.primary = { label: COPY.reload, icon: 'arrow-clockwise', cap: false, action: () => location.reload() };
			m.copy = true;
			break;
		}
		case 'nobios': {
			const touch = touchOnlyQuery.matches;
			m.title = [COPY.intakeTitle];
			m.body = [COPY.intakeBody];
			m.primary = { label: COPY.choose, icon: null, cap: false, action: openPicker };
			m.aside = touch ? '' : COPY.dropHere;
			m.fine = [[COPY.staysHere]];
			if (touch && !page.runtimeRequested)
				m.fine.push([COPY.touchDownload]);
			if (touch)
				m.fine.push([COPY.touchWarning]);
			m.fine.push([{ link: COPY.dumpLink, href: DUMP_URL }]);
			break;
		}
		case 'ready':
			m.title = [biosTitle(b ? b.name : page.urlBiosName)];
			m.body = b ? biosLine(b) : fetchingBiosLine();
			m.meta = true;
			m.primary = bootPrimary(COPY.boot, 'play');
			m.alt = true;
			if (!m.error && b && b.saveFailed)
				m.error = COPY.saveFailed;
			break;
		case 'queued': {
			const pct = Math.round(loadFraction() * 100);
			m.title = [COPY.queued, page.runtimeRequested ? ' · ' : '', page.runtimeRequested ? { num: `${pct}%` } : ''];
			m.body = b ? biosLine(b) : fetchingBiosLine();
			m.meta = true;
			m.primary = { label: COPY.boot, icon: 'play', cap: false, action: boot };
			m.aside = COPY.queuedCancel;
			break;
		}
		case 'starting': {
			const s = Math.max(0, Math.floor((performance.now() - page.bootAt) / 1000));
			m.title = [COPY.starting];
			m.body = page.vmState === STATE.RUNNING
				? [COPY.firstFrame, ' ', { num: s, unit: 's' }]
				: [{ name: b ? b.name : '' }, { sep: 1 }, { num: s, unit: 's' }];
			m.meta = true;
			if (s >= STARTING_LOG_S)
				m.fine = [[COPY.noAnswer, ' ', { logLink: COPY.openLog }]];
			else if (s >= STARTING_REASSURE_S) {
				m.fine = [[COPY.reassure]];
			}
			break;
		}
		case 'running':
			return null;
		case 'stopped':
			m.title = [COPY.stopped];
			m.body = page.ranMs ? [{ name: b.name }, { sep: 1 }, 'ran ', { num: fmtDuration(page.ranMs) }] : [{ name: b.name }];
			m.meta = true;
			m.primary = bootPrimary(COPY.bootAgain, 'play');
			break;
		case 'failed':
			m.title = [COPY.failed];
			m.body = ['PCSX2 couldn’t boot ', { name: b.name }, '. The end of the log:'];
			m.excerpt = emulatorTail(LOG_EXCERPT_LINES) || ' ';
			m.primary = bootPrimary(COPY.tryAgain, 'arrow-clockwise');
			m.alt = true;
			m.openLog = true;
			break;
		default:
			break;
		}
		applyDiscToSlate(state, m);
		return m;
	}

	let slateKey = '';
	let primaryAction = null;

	function renderSlate(state) {
		const m = slateModel(state);
		if (!m)
			return;
		if (m.key !== slateKey) {
			slateKey = m.key;
			els.slateInner.classList.remove('swap');
			void els.slateInner.offsetWidth; // restart the crossfade
			els.slateInner.classList.add('swap');
		}
		rich(els.slateTitle, m.title);

		els.slateBody.hidden = !m.body;
		if (m.body)
			rich(els.slateBody, m.body);
		els.slateBody.classList.toggle('meta', m.meta);

		// One row per log line (CSS cuts long ones); a build failure's list of what is missing wraps.
		els.slateExcerpt.hidden = !m.excerpt;
		els.slateExcerpt.classList.toggle('wrap', m.key === 'dead-build');
		if (m.excerpt)
			setText(els.slateExcerpt, m.excerpt);

		const p = m.primary;
		els.slatePrimary.hidden = !p;
		primaryAction = p ? p.action : null;
		if (p) {
			setText(els.slatePrimaryLabel, p.label);
			els.slatePrimaryIcon.toggleAttribute('hidden', !p.icon); // SVG elements have no .hidden property
			if (p.icon) {
				const use = els.slatePrimaryIcon.firstElementChild;
				if (use.getAttribute('href') !== icon(p.icon))
					use.setAttribute('href', icon(p.icon));
			}
			els.slatePrimaryCap.hidden = !p.cap;
		}
		// A queued boot is a pressed toggle ("Press again to cancel."), and an error line describes
		// the button that tries again.
		const queued = m.key === 'queued';
		if (queued)
			els.slatePrimary.setAttribute('aria-pressed', 'true');
		else
			els.slatePrimary.removeAttribute('aria-pressed');
		const described = [queued && m.aside ? 'slate-aside' : '', m.error ? 'slate-error' : ''].filter(Boolean).join(' ');
		if (described)
			els.slatePrimary.setAttribute('aria-describedby', described);
		else
			els.slatePrimary.removeAttribute('aria-describedby');
		setText(els.slateAside, m.aside);
		els.slateAside.hidden = !m.aside;
		els.slateAlt.hidden = !m.alt;
		els.slateOpenLog.hidden = !m.openLog;
		els.slateCopy.hidden = !m.copy;
		discEls.slateDisc.hidden = !m.disc;
		if (m.disc)
			setText(discEls.slateDisc, m.disc);
		discEls.slateRemove.hidden = !m.remove;
		els.slateActions.hidden = !(p || m.aside || m.alt || m.openLog || m.copy || m.disc || m.remove);

		setText(els.slateError, m.error || '');
		els.slateError.hidden = !m.error;

		els.slateDev.hidden = !m.dev;
		if (m.dev && page.env && !els.devChecks.childElementCount) {
			for (const c of page.env.checks) {
				const dt = document.createElement('dt');
				dt.textContent = c.label;
				const dd = document.createElement('dd');
				dd.textContent = c.pass ? COPY.pass : COPY.fail;
				dd.dataset.pass = String(c.pass);
				els.devChecks.append(dt, dd);
			}
		}

		const fineKey = JSON.stringify(m.fine);
		if (richKeys.get(els.slateFine) !== fineKey) {
			richKeys.set(els.slateFine, fineKey);
			els.slateFine.textContent = '';
			for (const line of m.fine) {
				const para = document.createElement('p');
				rich(para, line);
				els.slateFine.appendChild(para);
			}
		}
	}

	function renderCapture() {
		const playing = pageState() === 'running';
		const captured = playing && captureActive();
		setData('capture', playing ? (captured ? 'on' : 'off') : 'na');
		els.captureText.hidden = !captured;
		els.capture.hidden = !playing || captured;
		// "Press Enter to play" only for keyboard focus. A mouse or touch press focuses the button
		// too, and replacing the text under the pointer between press and release cancels the click.
		if (!els.capture.hidden)
			setText(els.capture, els.capture.matches(':focus-visible') ? COPY.releasedFocused : COPY.released);
		els.loadLine.hidden = playing;
	}

	function renderReadout(state) {
		const running = state === 'running';
		els.rDash.hidden = state !== 'starting';
		els.rSpeedWrap.hidden = !running;
		els.rRest.hidden = !running;
		if (!running) {
			setText(els.rFrame, '');
			return;
		}
		setText(els.rSpeed, `${Math.round(page.speed)}%`);
		setText(els.rVps, page.vps.toFixed(1));
		setText(els.rFps, page.displayFps.toFixed(0));
		setText(els.rFrame, frame.width ? `, frame ${frame.width} by ${frame.height}` : '');
	}

	function renderBiosMenu() {
		const b = page.bios;
		if (!b) {
			hideMenu();
			return;
		}
		setText(els.biosBtnName, biosTitle(b.name));
		setText(els.menuName, b.name);
		rich(els.menuWhere, [...sizeParts(b.size), { sep: 1 }, storedText(b)]);
		const locked = !canChangeBios();
		for (const item of [els.menuChoose, els.menuForget]) {
			if (locked) {
				item.setAttribute('aria-disabled', 'true');
				item.setAttribute('aria-describedby', 'menu-reason');
			} else {
				item.removeAttribute('aria-disabled');
				item.removeAttribute('aria-describedby');
			}
		}
		els.menuReason.hidden = !locked;
		if (locked && !els.menuConfirm.hidden) {
			showMenuItems();
			if (els.biosMenu.contains(document.activeElement) || focusIsFree())
				els.menuForget.focus({ preventScroll: true });
		}
	}

	function render() {
		const state = pageState();
		setData('state', state);
		setData('runtime', runtimeState());
		setData('lights', state === 'running' ? 'down' : 'up');
		setData('drag', page.drag);
		setData('bios', page.bios ? 'set' : 'none');
		const shown = page.frameSinceBoot && frame.presented > 0;
		setData('picture', state === 'running' ? 'live' : shown && ['stopped', 'failed', 'dead'].includes(state) ? 'dim' : 'none');
		renderSlate(state);
		renderProgress();
		renderCapture();
		renderReadout(state);
		renderBiosMenu();
		renderDiscMenu();
		els.btnFs.hidden = !fullscreenSupported();
		if (state === 'starting')
			announceStartingNotice();
		if (state !== page.lastState) {
			const prev = page.lastState;
			page.lastState = state;
			if (prev !== null)
				onPageStateChange(prev, state);
		}
		updateChrome();
	}

	// The slate is not a live region: its 10 s and 15 s lines are announced once each (only the
	// newer one when both are due at once, as after a hidden tab).
	function announceStartingNotice() {
		const s = (performance.now() - page.bootAt) / 1000;
		if (s >= STARTING_LOG_S && page.startNotice < 2) {
			page.startNotice = 2;
			announce(COPY.noAnswer);
		} else if (s >= STARTING_REASSURE_S && page.startNotice < 1) {
			page.startNotice = 1;
			announce(COPY.reassure);
		}
	}

	// Focus, announcements and timing that belong to entering a state.
	function onPageStateChange(prev, next) {
		const free = focusIsFree();
		const a = document.activeElement;
		const lost = !a || a === document.body || !a.isConnected || a.getClientRects().length === 0;
		switch (next) {
		case 'running':
			page.runStartedAt = performance.now();
			setData('tally', 'delayed');
			setTimeout(() => { delete els.body.dataset.tally; }, reducedMotion() ? 0 : 1100);
			if (lost)
				els.screen.focus({ preventScroll: true });
			renderCapture();
			announce(captureActive() ? COPY.announce.running : COPY.announce.runningReleased);
			maybeFirstBootHint();
			break;
		case 'stopped':
			page.ranMs = prev === 'running' && page.runStartedAt ? performance.now() - page.runStartedAt : 0;
			renderSlate(next);
			if (free)
				focusPrimary();
			announce(COPY.announce.stopped);
			break;
		case 'failed':
			if (free)
				focusPrimary();
			announce(COPY.announce.failed);
			break;
		case 'dead':
			if (free)
				els.slateTitle.focus({ preventScroll: true });
			announce(COPY.announce.dead);
			break;
		case 'starting':
			if (prev !== 'running')
				announce(COPY.announce.starting);
			break;
		case 'queued':
			announce(COPY.announce.queued);
			break;
		case 'ready':
			announce(COPY.announce.ready);
			break;
		default:
			break;
		}
		noteActivity();
	}

	function maybeFirstBootHint() {
		if (readPref(PREF_HINTED) === '1')
			return;
		writePref(PREF_HINTED, '1');
		subtitle(COPY.firstHint(keyLabel('KeyK', 'K'), keyLabel('KeyL', 'L')), HINT_MS);
	}

	// ------------------------------------------------------------------ wiring

	function hasFiles(e) {
		const types = e.dataTransfer && e.dataTransfer.types;
		return !!types && Array.prototype.indexOf.call(types, 'Files') >= 0;
	}

	function setDrag(value) {
		if (page.drag === value)
			return;
		page.drag = value;
		render();
	}

	function isEmptyRoom(t) {
		return t === document.documentElement || t === document.body || t === els.theatre ||
			t === els.stageBox || t === els.sill || t === els.transport || t === els.marquee ||
			t === els.credits || (t.classList && (t.classList.contains('t-left') || t.classList.contains('t-right') ||
			t.classList.contains('meter') || t.classList.contains('capture')));
	}

	function wireUi() {
		els.slatePrimary.addEventListener('click', (e) => {
			if (primaryAction)
				primaryAction(e);
		});
		els.slateAlt.addEventListener('click', openPicker);
		els.slateOpenLog.addEventListener('click', openLogFromSlate);
		els.slateCopy.addEventListener('click', () => copyText(fatalDetails(), els.slateCopyLabel));
		els.devCopy.addEventListener('click', () => copyText(els.devCmd.textContent, els.devCopyLabel));

		els.btnStop.addEventListener('click', (e) => {
			stop();
			returnFocusAfterPointer(e);
		});
		els.capture.addEventListener('click', () => els.screen.focus({ preventScroll: true }));

		els.biosInput.addEventListener('change', () => {
			const file = pickBiosCandidate(els.biosInput.files);
			els.biosInput.value = '';
			acceptFile(file, null); // a disc image picked here goes to the disc
		});

		els.btnCrisp.addEventListener('click', (e) => {
			const on = els.body.dataset.crisp !== 'on';
			setCrisp(on);
			writePref(PREF_CRISP, on ? '1' : '0');
			returnFocusAfterPointer(e);
		});
		els.btnKeys.addEventListener('click', (e) => {
			toggleDrawer('keys');
			returnFocusAfterPointer(e);
		});
		els.btnLog.addEventListener('click', (e) => {
			toggleDrawer('log');
			returnFocusAfterPointer(e);
		});
		els.keysClose.addEventListener('click', (e) => {
			closeDrawer(true);
			returnFocusAfterPointer(e);
		});
		els.logClose.addEventListener('click', (e) => {
			closeDrawer(true);
			returnFocusAfterPointer(e);
		});
		els.keepChrome.addEventListener('change', () => {
			page.keepChrome = els.keepChrome.checked;
			writePref(PREF_KEEP_CHROME, page.keepChrome ? '1' : '0');
			updateChrome();
		});

		els.btnFs.addEventListener('click', (e) => {
			toggleFullscreen();
			returnFocusAfterPointer(e);
		});
		els.screen.addEventListener('dblclick', () => {
			if (pageState() === 'running')
				toggleFullscreen();
		});
		document.addEventListener('fullscreenchange', onFullscreenChange);
		document.addEventListener('webkitfullscreenchange', onFullscreenChange);

		// BIOS menu (popover)
		els.biosMenu.addEventListener('beforetoggle', (e) => {
			if (e.newState === 'open') {
				renderBiosMenu();
				showMenuItems();
				placeMenu();
			}
		});
		els.biosMenu.addEventListener('toggle', (e) => {
			const open = e.newState === 'open';
			els.biosBtn.setAttribute('aria-expanded', open ? 'true' : 'false');
			if (open) {
				els.menuChoose.focus({ preventScroll: true });
			} else {
				const a = document.activeElement;
				if (!a || a === document.body || els.biosMenu.contains(a))
					els.biosBtn.focus({ preventScroll: true });
			}
			updateChrome();
		});
		window.addEventListener('resize', () => {
			syncScreenWidth();
			if (menuOpen())
				placeMenu();
		});
		els.menuChoose.addEventListener('click', () => {
			if (els.menuChoose.getAttribute('aria-disabled') === 'true')
				return;
			hideMenu();
			openPicker();
		});
		els.menuForget.addEventListener('click', () => {
			if (els.menuForget.getAttribute('aria-disabled') === 'true')
				return;
			els.menuItems.hidden = true;
			els.menuConfirm.hidden = false;
			els.menuKeep.focus({ preventScroll: true });
		});
		els.menuKeep.addEventListener('click', () => {
			showMenuItems();
			els.menuForget.focus({ preventScroll: true });
		});
		els.menuForgetYes.addEventListener('click', forgetBios);
		// A menu button's menu closes when Tab (or anything else) takes focus out of it.
		els.biosMenu.addEventListener('focusout', (e) => {
			const to = e.relatedTarget;
			if (to && !els.biosMenu.contains(to) && to !== els.biosBtn)
				hideMenu();
		});

		// A tooltip hidden with Esc stays hidden until both the pointer and focus have left its button.
		for (const b of document.querySelectorAll('.t-btn')) {
			const clearTipOff = () => setTimeout(() => {
				if (!b.matches(':hover') && document.activeElement !== b)
					b.classList.remove('tip-off');
			}, 0);
			b.addEventListener('pointerleave', clearTipOff);
			b.addEventListener('blur', clearTipOff);
		}

		// About
		els.aboutBtn.addEventListener('click', () => {
			els.about.showModal();
			updateChrome();
		});
		els.about.addEventListener('close', () => {
			els.aboutBtn.focus({ preventScroll: true });
			updateChrome();
		});
		els.about.addEventListener('click', (e) => {
			if (e.target === els.about) {
				const r = els.about.getBoundingClientRect();
				if (e.clientX < r.left || e.clientX > r.right || e.clientY < r.top || e.clientY > r.bottom)
					els.about.close(); // a click on the backdrop
			}
		});

		// Keyboard and focus
		window.addEventListener('keydown', onKeyDown);
		window.addEventListener('keyup', onKeyUp);
		window.addEventListener('blur', () => {
			releaseAllKeys(true);
			onWindowFocusChange();
		});
		window.addEventListener('focus', onWindowFocusChange);
		document.addEventListener('visibilitychange', () => {
			if (document.visibilityState !== 'visible')
				releaseAllKeys(true);
			onWindowFocusChange();
		});
		document.addEventListener('focusin', onFocusChange);
		document.addEventListener('focusout', () => setTimeout(onFocusChange, 0));

		// Controls hiding: pointer moves over 4 px, presses and wheel count as activity. This never
		// calls render().
		document.addEventListener('pointermove', (e) => {
			if (Math.hypot(e.clientX - lastPointerX, e.clientY - lastPointerY) > CHROME_MOVE_PX || Number.isNaN(lastPointerX)) {
				lastPointerX = e.clientX;
				lastPointerY = e.clientY;
				noteActivity();
			}
		}, { passive: true });
		document.addEventListener('pointerdown', noteActivity, { passive: true });
		document.addEventListener('wheel', noteActivity, { passive: true });
		document.addEventListener('pointerover', (e) => {
			const over = !!(e.target.closest && e.target.closest('.marquee, .sill, .transport, .credits, .drawer'));
			if (over !== pointerOverChrome) {
				pointerOverChrome = over;
				updateChrome();
			}
		});
		document.documentElement.addEventListener('pointerleave', () => {
			pointerOverChrome = false;
			updateChrome();
		});
		// While running, a press on the empty room gives the keyboard back to the PS2.
		document.addEventListener('mousedown', (e) => {
			if (e.button === 0 && pageState() === 'running' && isEmptyRoom(e.target)) {
				e.preventDefault();
				els.screen.focus({ preventScroll: true });
			}
		});

		els.subtitle.addEventListener('pointerenter', () => {
			subtitleHover = true;
			if (subtitleOpen) {
				clearTimeout(subtitleTimer);
				subtitleLeft = Math.max(800, subtitleLeft - (performance.now() - subtitleSince));
			}
		});
		els.subtitle.addEventListener('pointerleave', () => {
			subtitleHover = false;
			startSubtitleTimer();
		});

		// Drag and drop anywhere. Always swallow file drops so a stray drop cannot navigate
		// away from (and kill) a running emulator.
		let dragDepth = 0;
		let dragFocus = null; // focus when the drag began: the drop slate hides the slate's buttons
		const dragAllowed = () => page.phase === 'loading' || page.phase === 'ready';
		const endDrag = () => {
			setDrag('');
			const prev = dragFocus;
			dragFocus = null;
			if (!prev || !focusIsFree() || document.activeElement === prev)
				return;
			if (prev.isConnected && prev.getClientRects().length)
				prev.focus({ preventScroll: true });
			else if (prev !== document.body && prev !== document.documentElement)
				focusPrimary();
		};
		window.addEventListener('dragenter', (e) => {
			if (!hasFiles(e))
				return;
			e.preventDefault();
			if (!dragDepth)
				dragFocus = document.activeElement;
			dragDepth++;
			if (dragAllowed())
				setDrag(canChangeBios() ? 'ok' : 'blocked');
		});
		window.addEventListener('dragover', (e) => {
			if (!hasFiles(e))
				return;
			e.preventDefault();
			e.dataTransfer.dropEffect = dragAllowed() && canChangeBios() ? 'copy' : 'none';
		});
		window.addEventListener('dragleave', (e) => {
			if (!hasFiles(e))
				return;
			dragDepth = Math.max(0, dragDepth - 1);
			if (!dragDepth)
				endDrag();
		});
		window.addEventListener('drop', (e) => {
			if (!hasFiles(e))
				return;
			e.preventDefault();
			dragDepth = 0;
			const ok = dragAllowed() && canChangeBios();
			endDrag();
			if (ok)
				acceptDrop(e.dataTransfer);
		});

		// Log drawer
		els.log.addEventListener('scroll', () => {
			els.logLatest.hidden = logAtBottom();
		}, { passive: true });
		els.logLatest.addEventListener('click', () => {
			els.log.scrollTop = els.log.scrollHeight;
			els.logLatest.hidden = true;
			els.log.focus({ preventScroll: true });
		});
		els.logClear.addEventListener('click', () => {
			logLines.length = 0;
			logPending = [];
			els.log.textContent = '';
			els.logLatest.hidden = true;
			flushLog();
		});
		els.logCopy.addEventListener('click', () => copyText(logLines.map((l) => l.text).join('\n'), els.logCopyLabel));

		// The column under the screen (sill, transport, drawers, credits) follows the screen's
		// width. When the credits rewrap, the screen settles a frame later; the "ResizeObserver
		// loop" report for that is silenced in the window error handler.
		if (typeof ResizeObserver === 'function') {
			new ResizeObserver((entries) => {
				for (const entry of entries) {
					const box = entry.borderBoxSize && entry.borderBoxSize[0];
					const w = box ? box.inlineSize : entry.contentRect.width;
					document.documentElement.style.setProperty('--screen-w', `${w}px`);
				}
			}).observe(els.screen);
			// When the credits wrap (a narrow column), the separator left at the end of a line is
			// hidden. It stays in place, so hiding it never changes where the lines break.
			const creditsLine = els.credits.querySelector('p');
			new ResizeObserver(() => {
				for (const sep of creditsLine.querySelectorAll('.sep')) {
					const next = sep.parentElement.nextElementSibling; // the item after this one
					sep.classList.toggle('eol', !!next && next.getBoundingClientRect().top > sep.getBoundingClientRect().top + 4);
				}
			}).observe(creditsLine);
		}
	}

	// Read-only hooks for automated tests (for example a headless browser checking that frames arrive).
	window.PCSX2Web = Object.freeze({
		stats: () => ({
			phase: page.phase,
			state: STATE_NAMES[page.vmState] || String(page.vmState),
			view: pageState(),
			speed: page.speed,
			vps: page.vps,
			displayFps: page.displayFps,
			framesPresented: frame.presented,
			framesDropped: frame.dropped,
			lastSeq: frame.lastSeq,
			width: frame.width,
			height: frame.height,
			bios: page.bios ? page.bios.name : null,
			logLines: logTotal,
		}),
		logText: () => logLines.map((l) => l.text).join('\n'),
		// ?vu1jit= testing: the mode, the JIT's counters and the fuzzer's status.
		vu1jit: () => ({ mode: vu1jit.mode, stats: vu1jitStats(), fuzz: vu1jitFuzzStatus() }),
	});

	// ------------------------------------------------------------------ start

	function main() {
		wireUi();
		wireDiscUi();
		setCrisp(readPref(PREF_CRISP) === '1');
		page.keepChrome = readPref(PREF_KEEP_CHROME) === '1';
		els.keepChrome.checked = page.keepChrome;
		setData('fs', 'off');
		syncScreenWidth();
		for (const a of document.querySelectorAll('a.source-link'))
			a.href = SOURCE_URL;
		applyLayoutLabels();
		renderLogCount();

		const problem = environmentProblem();
		if (problem) {
			page.phase = 'unsupported';
			page.env = problem;
			pageLog(`This page can't run the emulator: ${problem.cause}`, 'warn');
			render();
			return; // do not load the wasm
		}

		const biosParam = params.get('bios');
		const discParam = params.get('disc');
		disc.urlPending = !!discParam; // a queued ?bios= boot waits for the ?disc= check
		// Touch-only devices: the download waits for a BIOS.
		if (!touchOnlyQuery.matches || biosParam)
			ensureRuntime();
		render();

		if (biosParam)
			loadBiosFromUrl(biosParam);
		else
			restoreRememberedBios();
		if (discParam)
			loadDiscFromUrl(discParam);
		else
			loadRememberedDisc();
	}

	main();
})();
