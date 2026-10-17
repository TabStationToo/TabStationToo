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
	const FD_VSYNC = 4;
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
	// ?gpu=0 keeps the GS on its in-memory device and this page on the 2D canvas (README).
	const GPU_ALLOWED = params.get('gpu') !== '0';
	// The WebGPU device needs JSPI (the GS thread suspends while it waits for the GPU). Without it the
	// module still starts (pcsx2-wasm/jspi-fallback.js) and draws through the 2D canvas.
	const HAS_JSPI = typeof WebAssembly === 'object' && typeof WebAssembly.Suspending === 'function' &&
		typeof WebAssembly.promising === 'function';
	// Test switches (README "Automated runs"): frames to keep a copy of (by vsync number), a fixed
	// surface size.
	const CAPTURE_FRAMES = (params.get('capture') || '').split(',').map(Number).filter((n) => n > 0);
	const TEST_SURFACE = /^(\d+)x(\d+)$/.exec(params.get('surface') || '');
	const FRAME_PORT_TIMEOUT_MS = 5000;
	const LATENCY_SAMPLES = 600;

	// ------------------------------------------------------------------ copy

	const COPY = {
		intakeTitle: 'Start with your BIOS.',
		intakeBody: 'TabStationToo needs the BIOS from a PS2 you own, usually a 4\u00a0MB .bin file. For now it runs the console’s own menus; games come later.',
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
		tooBig: (size) => `That file is ${size}. A BIOS is 4 to 8\u00a0MB, and game discs aren’t supported yet.`,
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
		gpuFallback: 'The GPU stopped working, so the picture is drawn without it now.',
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
	// The display canvas keeps the first kind of context it is given, so the context is made when
	// the first frame arrives: '2d' for GSDeviceNull's pixels, 'bitmaprenderer' for the WebGPU
	// device's ImageBitmaps. Switching (the GS gave up WebGPU) swaps in a fresh <canvas>.
	const display = { kind: null, ctx: null };
	function displayContext(kind) {
		if (display.kind === kind)
			return display.ctx;
		if (display.kind) {
			const fresh = els.canvas.cloneNode(false);
			els.canvas.replaceWith(fresh);
			els.canvas = fresh;
			observeCanvasSize();
		}
		display.ctx = kind === '2d' ? els.canvas.getContext('2d', { alpha: false }) : els.canvas.getContext('bitmaprenderer');
		display.kind = kind;
		pageLog(kind === '2d' ? 'Display: 2D canvas (no WebGPU).' : 'Display: WebGPU frames.');
		return display.ctx;
	}

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
		vsync: 0, // the vsync (counted from boot) whose picture is on screen (either path)
	};

	// The WebGPU device's frame port (see onFramePort).
	const gpu = {
		port: null,
		frames: 0, // bitmaps shown
		framesAtBoot: 0, // frames at the last boot request (frames - framesAtBoot came in this boot)
		lastPosted: 0, // the GS's post count at the last bitmap shown (posted - shown = dropped)
		latencies: [], // ms from the GS thread's post to transferFromImageBitmap, newest last
		fallback: '', // why the GS gave up WebGPU, once it has
		portWarned: false,
		captures: new Map(), // wanted vsync -> { vsync, width, height, rgba } of the first frame at or after it
		device: 0, // number of the GS device whose frames arrive (a recovered device has the next one)
		devices: [], // { device, at: performance.now() of its first frame, vsync }, newest last (at most 8)
	};
	const surfaceFeed = { width: 0, height: 0, scale: 0 };

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
	function showError(text) {
		page.error = text;
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
			surfaceSize: (w, h, scale) => M._pcsx2web_surface_size && M._pcsx2web_surface_size(w, h, scale),
			setCrisp: (on) => M._pcsx2web_set_crisp && M._pcsx2web_set_crisp(on ? 1 : 0),
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
		if (page.pendingBiosBytes && page.bios) {
			writeBiosToFs(page.bios.name, page.pendingBiosBytes);
			page.pendingBiosBytes = null;
		}
		renderProgress();
		feedSurfaceSize(0, 0, 0);
		api.setCrisp(els.body.dataset.crisp === 'on');
		frame.lastPollTime = performance.now();
		pollTimer = setInterval(poll, POLL_MS);
		rafId = requestAnimationFrame(drawLoop);
		poll();
		maybeAutoBoot();
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
		const args = [];
		if (!GPU_ALLOWED)
			args.push('-gpu', '0');
		for (const setting of params.getAll('set'))
			args.push('-set', setting);
		if (CAPTURE_FRAMES.length)
			args.push('-capture', CAPTURE_FRAMES.join(','));
		if (TEST_SURFACE)
			args.push('-surface', TEST_SURFACE[0]);
		if (params.get('gsidle') === 'block')
			args.push('-gs-idle', 'block');
		if (GPU_ALLOWED && navigator.gpu && !HAS_JSPI)
			pageLog('WebGPU needs JSPI, which this browser lacks: drawing through the 2D canvas.', 'warn');
		window.Module = {
			arguments: args,
			tstFramePort: onFramePort,
			tstGpuFallback: onGpuFallback,
			print: (text) => appendLog(text, 'stdout'),
			printErr: (text) => appendLog(text, 'stderr'),
			setStatus: onSetStatus,
			monitorRunDependencies: onRunDependencies,
			instantiateWasm,
			onRuntimeInitialized,
			onAbort: (what) => fatal('crash', 'The emulator aborted', what),
			onExit,
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
		const vsync = Atomics.load(desc, FD_VSYNC);
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

		const ctx = displayContext('2d');
		if (els.canvas.width !== width || els.canvas.height !== height) {
			els.canvas.width = width;
			els.canvas.height = height;
		}
		ctx.putImageData(frame.imageData, 0, 0);
		frame.presented++;
		frame.width = width;
		frame.height = height;
		frame.vsync = vsync;

		if (!page.frameSinceBoot && seq !== page.seqAtBoot) {
			page.frameSinceBoot = true;
			render();
		}
	}

	// ------------------------------------------------------------------ WebGPU frames

	// The GS thread hands over its frame port once per worker (Emscripten's "call a Module handler"
	// worker command), then posts every frame on it as an ImageBitmap, without returning to its
	// event loop (docs/web-port/WEBGPU-DESIGN.md §12.1).
	function onFramePort(port) {
		gpu.port = port;
		port.onmessage = (e) => {
			const m = e.data;
			if (!m)
				return;
			if (m.type === 'frame')
				showBitmapFrame(m);
			else if (m.type === 'fallback')
				onGpuFallback(m.reason, m.permanent);
		};
		pageLog('WebGPU frame port connected.');
	}

	function showBitmapFrame(m) {
		const bitmap = m.bitmap;
		if (page.phase !== 'ready' || !bitmap) {
			if (bitmap)
				bitmap.close();
			return;
		}
		const ctx = displayContext('bitmaprenderer');
		const wanted = CAPTURE_FRAMES.filter((n) => m.vsync >= n && !gpu.captures.has(n));
		if (wanted.length) {
			const probe = new OffscreenCanvas(bitmap.width, bitmap.height).getContext('2d', { willReadFrequently: true });
			probe.drawImage(bitmap, 0, 0);
			const copy = { vsync: m.vsync, width: bitmap.width, height: bitmap.height,
				rgba: probe.getImageData(0, 0, bitmap.width, bitmap.height).data };
			for (const n of wanted)
				gpu.captures.set(n, copy);
		}
		if (els.canvas.width !== bitmap.width || els.canvas.height !== bitmap.height) {
			els.canvas.width = bitmap.width;
			els.canvas.height = bitmap.height;
		}
		const width = bitmap.width, height = bitmap.height;
		ctx.transferFromImageBitmap(bitmap);
		gpu.latencies.push(performance.timeOrigin + performance.now() - m.postedAt);
		if (gpu.latencies.length > LATENCY_SAMPLES)
			gpu.latencies.shift();
		if (m.device !== gpu.device) {
			gpu.device = m.device;
			gpu.devices.push({ device: m.device, at: performance.now(), vsync: m.vsync });
			if (gpu.devices.length > 8)
				gpu.devices.shift();
			if (m.device > 1)
				pageLog(`WebGPU: frames from device ${m.device} (recovered or rebooted).`);
			// A boot that drew through the 2D path (no device then) does not keep a later one off WebGPU.
			gpu.fallback = '';
		}
		gpu.frames++;
		gpu.lastPosted = m.frame;
		frame.presented++;
		frame.width = width;
		frame.height = height;
		frame.vsync = m.vsync;
		if (!page.frameSinceBoot && (page.vmState === STATE.STARTING || page.vmState === STATE.RUNNING)) {
			page.frameSinceBoot = true;
			render();
		}
	}

	// The GS draws through the 2D path: for good once it gave WebGPU up after repeated device loss
	// (permanent), or for this boot when it could not create a device. drawLoop() swaps the canvas
	// when the first 2D frame arrives. The notice is only for a failure during play (UI-SPEC), the
	// permanent kind with this boot's picture coming through WebGPU: a boot that got no device just
	// uses the 2D canvas, like a browser without WebGPU, even after earlier boots ran on WebGPU.
	function onGpuFallback(reason, permanent) {
		gpu.fallback = reason || 'unknown';
		pageLog(permanent ? `WebGPU given up: ${gpu.fallback}. Drawing through the 2D canvas.` :
			`WebGPU unavailable for this boot: ${gpu.fallback}. Drawing through the 2D canvas.`, 'warn');
		if (permanent && gpu.frames > gpu.framesAtBoot)
			subtitle(COPY.gpuFallback);
	}

	function latencySummary() {
		const v = [...gpu.latencies].sort((a, b) => a - b);
		if (!v.length)
			return null;
		const at = (q) => v[Math.min(v.length - 1, Math.floor(q * v.length))];
		return { n: v.length, median: at(0.5), p99: at(0.99), max: v[v.length - 1] };
	}

	// The GS presents at the canvas's backing size in device pixels (WebGPU path; GSDeviceNull keeps
	// its own 640x480). ?surface=WxH pins it for tests.
	let canvasObserver = null;
	function observeCanvasSize() {
		if (typeof ResizeObserver !== 'function')
			return;
		if (!canvasObserver) {
			canvasObserver = new ResizeObserver((entries) => {
				for (const entry of entries) {
					const dp = entry.devicePixelContentBoxSize && entry.devicePixelContentBoxSize[0];
					const dpr = window.devicePixelRatio || 1;
					feedSurfaceSize(dp ? dp.inlineSize : Math.round(entry.contentRect.width * dpr),
						dp ? dp.blockSize : Math.round(entry.contentRect.height * dpr), dpr);
				}
			});
		}
		canvasObserver.disconnect();
		try {
			canvasObserver.observe(els.canvas, { box: 'device-pixel-content-box' });
		} catch {
			canvasObserver.observe(els.canvas);
		}
	}

	function feedSurfaceSize(width, height, scale) {
		if (TEST_SURFACE) {
			width = Number(TEST_SURFACE[1]);
			height = Number(TEST_SURFACE[2]);
			scale = 1;
		}
		if (width > 0 && height > 0) {
			surfaceFeed.width = width;
			surfaceFeed.height = height;
			surfaceFeed.scale = scale;
		}
		if (api && surfaceFeed.width)
			api.surfaceSize(surfaceFeed.width, surfaceFeed.height, surfaceFeed.scale);
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
		// With WebGPU on offer, the GS either hands over its frame port or reports that it gave up.
		if (GPU_ALLOWED && HAS_JSPI && navigator.gpu && !gpu.port && !gpu.fallback && !gpu.portWarned &&
			page.vmState === STATE.RUNNING && now - page.bootAt > FRAME_PORT_TIMEOUT_MS && display.kind !== '2d') {
			gpu.portWarned = true;
			pageLog(`No WebGPU frame port ${FRAME_PORT_TIMEOUT_MS / 1000} s after boot.`, 'error');
		}

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
		return page.phase === 'ready' && !!page.bios && !!page.writtenBiosName && !page.biosReading &&
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

	function startVm() {
		if (!canBoot())
			return;
		releaseAllKeys(true);
		let result;
		try {
			result = api.boot(page.bios.name);
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
		gpu.framesAtBoot = gpu.frames;
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
		if (els.biosMenu.contains(a) || els.about.contains(a))
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
		if (els.about.open || menuOpen())
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
		// The 2D path scales with CSS (image-rendering); the WebGPU device presents at the canvas's
		// own size, so it filters (or not) itself.
		if (api)
			api.setCrisp(on);
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
			!page.drawer && !menuOpen() && !els.about.open && !subtitleOpen && !page.drag &&
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
			m.title = [page.drag === 'ok' ? COPY.drop : COPY.dropBlocked];
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
		els.slateActions.hidden = !(p || m.aside || m.alt || m.openLog || m.copy);

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
			acceptBiosFile(file);
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
				acceptBiosFile(pickBiosCandidate(e.dataTransfer.files));
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
			display: display.kind === 'bitmaprenderer' ? 'webgpu' : display.kind,
			framePort: !!gpu.port,
			vsync: frame.vsync,
			gpuFrames: gpu.frames,
			gpuPosted: gpu.lastPosted,
			gpuDevices: gpu.devices.map((d) => ({ ...d })),
			gpuFallback: gpu.fallback,
			latency: latencySummary(),
			surface: surfaceFeed.width ? `${surfaceFeed.width}x${surfaceFeed.height}@${surfaceFeed.scale}` : null,
		}),
		logText: () => logLines.map((l) => l.text).join('\n'),
		// Tests: the copy asked for with ?capture= of the first frame presented at vsync n or later
		// (WebGPU: the bitmap the page received; 2D path: the pixels the GS published), as base64
		// RGBA with the vsync it shows (frame), or null.
		capture: (n) => {
			let c = gpu.captures.get(n);
			if (!c && api && window.Module._pcsx2web_test_capture) {
				const ptr = window.Module._pcsx2web_test_capture(n) >>> 0;
				const buf = heapBuffer();
				if (ptr && buf) {
					const h = new Uint32Array(buf, ptr, 4);
					c = { vsync: h[0], width: h[1], height: h[2], rgba: new Uint8Array(buf, h[3], h[1] * h[2] * 4).slice() };
					for (let i = 3; i < c.rgba.length; i += 4)
						c.rgba[i] = 255;
				}
			}
			if (!c)
				return null;
			let bin = '';
			for (let i = 0; i < c.rgba.length; i += 32768)
				bin += String.fromCharCode.apply(null, c.rgba.subarray(i, i + 32768));
			return { requested: n, frame: c.vsync, width: c.width, height: c.height, rgba: btoa(bin) };
		},
	});

	// ------------------------------------------------------------------ start

	function main() {
		wireUi();
		observeCanvasSize();
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
		// Touch-only devices: the download waits for a BIOS.
		if (!touchOnlyQuery.matches || biosParam)
			ensureRuntime();
		render();

		if (biosParam)
			loadBiosFromUrl(biosParam);
		else
			restoreRememberedBios();
	}

	main();
})();
