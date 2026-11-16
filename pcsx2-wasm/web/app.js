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

	// The deploy this page belongs to. tools/wasm/assemble-site.py loads app.js as app.js?v=<build id>
	// (a dev build has none), and every file loaded from here carries the same id. A browser may keep
	// files for a while (GitHub Pages allows 10 minutes): with the id, a page from an older deploy
	// keeps asking for that deploy's URLs instead of mixing its files with a newer emulator's.
	const BUILD_ID = (() => {
		try {
			return new URL(document.currentScript.src).searchParams.get('v') || '';
		} catch {
			return '';
		}
	})();
	const versioned = (file) => (BUILD_ID ? `${file}?v=${encodeURIComponent(BUILD_ID)}` : file);

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
	const PREF_CRISP = 'pcsx2web.crisp'; // before the picture modes: '1' was Crisp
	const PREF_PICTURE = 'pcsx2web.picture';
	// The Graphics menu's choice (UI-SPEC.md §12): 'auto', 'gpu' or 'sw'; ?gpu= and ?renderer= win over it.
	const PREF_RENDERER = 'pcsx2web.renderer';
	// The Graphics menu's resolution: '1' (native, as the PS2 draws) or '2' (twice the lines).
	const PREF_UPSCALE = 'pcsx2web.upscale';
	const PREF_KEEP_CHROME = 'pcsx2web.keepChrome';
	// Which PS2 port the keyboard plays as, and which one the first gamepad does (1 or 2).
	const PREF_PAD_KEYBOARD = 'pcsx2web.padKeyboard';
	const PREF_PAD_FIRST = 'pcsx2web.padFirst';
	const PREF_RUMBLE = 'pcsx2web.rumble.'; // + port: '0' turns that player's rumble off
	const PREF_HINTED = 'pcsx2web.firstBootHint';

	const SOURCE_URL = 'https://github.com/TabStationToo/TabStationToo';
	const DUMP_URL = 'https://pcsx2.net/docs/setup/bios/';
	const SVG_NS = 'http://www.w3.org/2000/svg';
	const icon = (name) => versioned('icons.svg') + '#i-' + name;

	const CHROME_IDLE_MS = 2500;
	const CHROME_MOVE_PX = 4;
	const SUBTITLE_MS = 4000;
	const HINT_MS = 8000;
	const COPY_FEEDBACK_MS = 2000;
	const SLOW_SPEED = 50;
	const SLOW_HINT_AFTER_MS = 5000;
	const STARTING_REASSURE_S = 10;
	const STARTING_LOG_S = 15;
	const STARTING_GZ_LOG_S = 180; // a .gz is indexed whole first (gzBoot())

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
	// The Graphics menu's stored choice, read once: the runtime takes it when it starts.
	const RENDERER_CHOICE = (() => {
		try {
			const v = localStorage.getItem(PREF_RENDERER);
			return v === 'gpu' || v === 'sw' ? v : 'auto';
		} catch {
			return 'auto';
		}
	})();
	const URL_GRAPHICS = params.has('gpu') || params.has('renderer');
	// ?gpu=0 keeps the GS on its in-memory device and this page on the 2D canvas (README), and so does
	// the Graphics menu's Software choice when the address has neither switch.
	// Auto tries WebGPU in every browser. (Firefox and Zen showed only the first WebGPU frame until the GS
	// configured the canvas again after each frame it hands over: see tst_post_frame in webgpu_host.js.)
	const IS_FIREFOX = /\bFirefox\//.test(navigator.userAgent);
	const GPU_ALLOWED = URL_GRAPHICS ? params.get('gpu') !== '0' : RENDERER_CHOICE !== 'sw';
	// The WebGPU device needs JSPI (the GS thread suspends while it waits for the GPU). Without it the
	// module still starts (pcsx2-wasm/jspi-fallback.js) and draws through the 2D canvas.
	const HAS_JSPI = typeof WebAssembly === 'object' && typeof WebAssembly.Suspending === 'function' &&
		typeof WebAssembly.promising === 'function';
	// Test switches (README "Automated runs"): frames to keep a copy of (by vsync number), a fixed
	// surface size.
	const CAPTURE_FRAMES = (params.get('capture') || '').split(',').map(Number).filter((n) => n > 0);
	const TEST_SURFACE = /^(\d+)x(\d+)$/.exec(params.get('surface') || '');
	// Developers: ?threads=1 adds how busy the EE, GS and VU1 threads are to the readout, so the limiting
	// thread can be read without the browser's developer tools (opening them slows WebAssembly down).
	const SHOW_THREADS = params.get('threads') === '1';
	const FRAME_PORT_TIMEOUT_MS = 5000;
	const LATENCY_SAMPLES = 600;
	const VU1JIT_PARAM = params.get('vu1jit');
	// PCSX2's hardware renderer on the WebGPU device is the default (?renderer=hw); ?renderer=sw picks the
	// software renderer. Without WebGPU the GS falls back to the software renderer either way.
	const RENDERER_PARAM = URL_GRAPHICS ? params.get('renderer') :
		RENDERER_CHOICE === 'gpu' ? 'hw' : RENDERER_CHOICE === 'sw' ? 'sw' : null;

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
		elfFetchFailed: (path, why) => `Couldn’t fetch the ELF from ${path} (${why}).`,
		elfOtherOrigin: '?elf= only accepts addresses on this server.',
		stateFetchFailed: (path, why) => `Couldn’t fetch the save state from ${path} (${why}).`,
		stateOtherOrigin: '?state= only accepts addresses on this server.',
		stored: { idb: 'Remembered in this browser', visit: 'This visit only', url: 'Loaded from ?bios=, not saved' },
		boot: 'Boot',
		another: 'Use another BIOS…',
		saveFailed: 'Couldn’t save the BIOS in this browser. It will last for this visit.',
		queued: 'Booting when ready',
		queuedCancel: 'Press again to cancel.',
		starting: 'Starting the PS2…',
		firstFrame: 'Waiting for the first frame…',
		reassure: 'Still starting. The first screen can take a few seconds here.',
		reassureGz: 'Still starting. A .gz image is read whole before the game starts: about a minute for a DVD.',
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
		causeWorker: 'This site needs a service worker to run the emulator, and this browser blocked it. Private windows often do.',
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
			gamepadConnected: (name, port) => `Gamepad connected: ${name}, Player ${port}.`,
			gamepadNeedsMap: (name, port) => `Gamepad connected: ${name}, Player ${port}. Its layout is not standard: map its controls on Player ${port}’s controller, under the console.`,
			gamepadMapped: (name) => `${name} is mapped. Press its controls to check them.`,
			gamepadMapCancelled: 'Mapping cancelled.',
			gamepadMapCleared: (name) => `The mapping of ${name} is cleared.`,
			gamepadUnused: (name) => `Gamepad connected: ${name}. Only two gamepads play, so it is not used.`,
			gamepadDisconnected: (name) => `Gamepad disconnected: ${name}.`,
			gamepadPlayer: (name, port) => (port ? `${name} plays as Player ${port}.` : `${name} is unplugged.`),
			keyboardPlayer: (port) => (port ? `The keyboard plays as Player ${port}.` : 'The keyboard is unplugged.'),
			gamepadsSwapped: (a, b) => `Players swapped. ${a.name} is Player ${a.port}. ${b.name} is Player ${b.port}.`,
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
		rThreads: $('r-threads'),
		rFrame: $('r-frame'),
		transport: $('transport'),
		btnStop: $('btn-stop'),
		biosBtn: $('bios-btn'),
		biosBtnName: $('bios-btn-name'),
		btnCrisp: $('btn-crisp'),
		ports: [$('port-1'), $('port-2')],
		portPick: $('port-pick'),
		portPickTitle: $('port-pick-title'),
		portPickList: $('port-pick-list'),
		btnLog: $('btn-log'),
		btnRec: $('btn-rec'),
		recDrawer: $('rec-drawer'),
		recStatus: $('rec-status'),
		recToggle: $('rec-toggle'),
		recToggleLabel: $('rec-toggle-label'),
		recCopy: $('rec-copy'),
		recCopyLabel: $('rec-copy-label'),
		recDownload: $('rec-download'),
		recClose: $('rec-close'),
		stateSave: $('state-save'),
		stateSaveLabel: $('state-save-label'),
		stateLoad: $('state-load'),
		stateLoadLabel: $('state-load-label'),
		stateReload: $('state-reload'),
		stateReloadLabel: $('state-reload-label'),
		stateInput: $('state-input'),
		recEmpty: $('rec-empty'),
		recChart: $('rec-chart'),
		recCrop: $('rec-crop'),
		recFrom: $('rec-from'),
		recTo: $('rec-to'),
		recSummary: $('rec-summary'),
		btnFs: $('btn-fs'),
		btnFsIcon: $('btn-fs-icon'),
		btnFsLabel: $('btn-fs-label'),
		btnFsTip: $('btn-fs-tip'),
		keysDrawer: $('keys-drawer'),
		keysClose: $('keys-close'),
		keepChrome: $('keep-chrome'),
		tabKeyboard: $('tab-keyboard'),
		tabGamepad: $('tab-gamepad'),
		panelKeyboard: $('panel-keyboard'),
		panelGamepad: $('panel-gamepad'),
		kbPlayers: [...document.querySelectorAll('input[name="kb-player"]')],
		gpEmpty: $('gp-empty'),
		gpTaps: $('gp-taps'),
		gpList: $('gp-list'),
		gpActions: $('gp-actions'),
		gpSwap: $('gp-swap'),
		gpTemplate: $('gp-template'),
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
		picture: 'smooth', // the remote's picture mode (PICTURE_MODES)
		upscale: 1, // the Graphics menu's resolution multiplier (1 or 2)
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
		litWait: false, // the disc library waits for the game's first picture (probeLit)
		bootHeld: false, // the library's boot waits for its disc to be cleaned (PCSX2WebLibrary.release)
		lit: false,
		seqAtBoot: 0,
		wasmLoaded: false,
		outcome: null, // null | 'stopped' | 'failed': what the slate shows after a run
		error: '', // the slate's coral line; stays until the next attempt
		errorIsDisc: false, // page.error is about the disc: a BIOS arriving does not clear it
		fatal: null, // { kind, what, detail, line }
		env: null, // environmentProblem() when unsupported
		drag: '', // '' | 'ok' | 'blocked'
		drawer: '', // '' | 'keys' | 'log' | 'rec'
		keysPort: 1, // the controller port the controls (the Keys drawer) were opened for
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
		mode: null, // { hw, webgpu, reason } as the GS opened its renderer (WebGPUHost::NoteRenderer)
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
			cause = isolationWorkerFailed() ? COPY.causeWorker : COPY.causeHeaders;
		else if (!threads)
			cause = `${COPY.causeThreads} ${COPY.tryBrowser}`;
		// isolation: the only thing missing is cross-origin isolation, which the service worker may
		// still bring (see isolationPending()).
		return cause ? { cause, checks, isolation: !isolated && cause === COPY.causeHeaders } : null;
	}

	// isolation.js and coi-serviceworker.js, loaded before this file, make the page cross-origin
	// isolated on hosts that cannot send the headers (GitHub Pages): a service worker adds them, and
	// the page reloads once.
	function isolationState() {
		return window.TabStationTooIsolation || { status: 'none', reason: '' };
	}

	function isolationWorkerFailed() {
		return isolationState().status === 'unavailable';
	}

	function isolationPending() {
		return isolationState().status === 'pending';
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
		const url = new URL(versioned(WASM_FILE), document.baseURI).href;
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
			'pcsx2web_key_code', 'pcsx2web_key', 'pcsx2web_pad_input', 'pcsx2web_pad', 'pcsx2web_frame'];
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
			threadUsage: (i) => (M._pcsx2web_thread_usage ? M._pcsx2web_thread_usage(i) : 0),
			perfRing: () => (M._pcsx2web_perf_ring ? M._pcsx2web_perf_ring() >>> 0 : 0),
			perfWritten: () => (M._pcsx2web_perf_written ? M._pcsx2web_perf_written() >>> 0 : 0),
			keyCode: (name) => M.ccall('pcsx2web_key_code', 'number', ['string'], [name]),
			key: (code, pressed) => M._pcsx2web_key(code, pressed ? 1 : 0),
			padInput: (name) => M.ccall('pcsx2web_pad_input', 'number', ['string'], [name]),
			// 0 when the emulator's queue is full: try again.
			pad: (port, input, value) => M._pcsx2web_pad(port, input, value) === 1,
			// Pointers are returned as signed i32; make them unsigned.
			frame: () => M._pcsx2web_frame() >>> 0,
			surfaceSize: (w, h, scale) => M._pcsx2web_surface_size && M._pcsx2web_surface_size(w, h, scale),
			setCrisp: (on) => M._pcsx2web_set_crisp && M._pcsx2web_set_crisp(on ? 1 : 0),
			setUpscale: (n) => M._pcsx2web_set_upscale && M._pcsx2web_set_upscale(n),
			setTvShader: (n) => M._pcsx2web_set_tv_shader && M._pcsx2web_set_tv_shader(n),
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
			checkPadInputs();
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
		feedSurfaceSize(0, 0, 0);
		api.setCrisp(els.body.dataset.crisp === 'on');
		api.setUpscale(page.upscale);
		api.setTvShader(PICTURE_MODES[page.picture].shader);
		audioStart(); // the context exists already when a boot was queued while the runtime loaded
		frame.lastPollTime = performance.now();
		pollTimer = setInterval(poll, POLL_MS);
		rafId = requestAnimationFrame(drawLoop);
		poll();
		maybeAutoBoot();
	}

	// ------------------------------------------------------------------ VU1 JIT testing

	// With no interface (pcsx2-wasm/README.md): the VU1 JIT is on by default; ?vu1jit=0 boots with the VU1
	// interpreter instead; ?vu1jit=1 also prints its counters to the browser console every 10 s; ?vu1jit=fuzz:SEED:COUNT[:timing|jit|both]
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
		if (VU1JIT_PARAM === '0') {
			vu1jit.mode = 'off';
			M._pcsx2web_vu1jit_enable(0);
			pageLog('VU1 JIT off in this session (?vu1jit=0): VU1 runs on the interpreter.');
		} else if (VU1JIT_PARAM === '1') {
			vu1jit.mode = 'on';
			M._pcsx2web_vu1jit_enable(1);
			pageLog('VU1 JIT testing: the JIT\'s counters go to the browser console every 10 s (?vu1jit=1).');
			setInterval(() => console.log('[vu1jit]', JSON.stringify(vu1jitStats())), 10000);
		} else if (VU1JIT_PARAM.startsWith('fuzz:')) {
			vu1jit.mode = 'fuzz';
			const spec = VU1JIT_PARAM.slice(5);
			// -2: the emulator thread is still setting up (the fuzzer must not race it); try again shortly.
			const rc = M.ccall('pcsx2web_vu1jit_fuzz', 'number', ['string'], [spec]);
			if (rc === -2) {
				setTimeout(startVu1JitTesting, 250);
				return;
			}
			if (rc !== 0) {
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
			pageLog(`Unknown ?vu1jit= value '${VU1JIT_PARAM}' (use 0, 1, or fuzz:SEED:COUNT[:timing|jit|both]).`, 'warn');
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
		const args = [];
		if (!GPU_ALLOWED)
			args.push('-gpu', '0');
		if (RENDERER_PARAM === 'hw' && GPU_ALLOWED)
			args.push('-set', 'EmuCore/GS/Renderer=32');
		else if (RENDERER_PARAM === 'sw')
			args.push('-set', 'EmuCore/GS/Renderer=13');
		else if (RENDERER_PARAM)
			pageLog(`Unknown ?renderer= value '${RENDERER_PARAM}' (use hw or sw).`, 'warn');
		for (const setting of params.getAll('set'))
			args.push('-set', setting);
		if (CAPTURE_FRAMES.length)
			args.push('-capture', CAPTURE_FRAMES.join(','));
		if (TEST_SURFACE)
			args.push('-surface', TEST_SURFACE[0]);
		if (params.get('gsidle') === 'block')
			args.push('-gs-idle', 'block');
		if (params.get('pipelines') === 'wait')
			args.push('-gs-pipelines', 'wait');
		if (params.get('unlimited') === '1')
			args.push('-unlimited');
		if (params.get('eejit') === '0')
			args.push('-eejit', '0');
		if (params.get('iopjit') === '0')
			args.push('-iopjit', '0');
		if (GPU_ALLOWED && navigator.gpu && !HAS_JSPI)
			pageLog('WebGPU needs JSPI, which this browser lacks: drawing through the 2D canvas.', 'warn');
		args.push(...audioModuleArguments());
		window.Module = {
			arguments: args,
			tstFramePort: onFramePort,
			tstGpuFallback: onGpuFallback,
			tstGpuMode: onGpuMode,
			print: (text) => appendLog(text, 'stdout'),
			printErr: (text) => appendLog(text, 'stderr'),
			setStatus: onSetStatus,
			monitorRunDependencies: onRunDependencies,
			instantiateWasm,
			// pcsx2-wasm.data, with this deploy's id. (The .wasm comes from instantiateWasm, and the
			// pthread workers load pcsx2-wasm.js from the script's own URL, id included.)
			locateFile: (path, dir) => dir + versioned(path),
			onRuntimeInitialized,
			onAbort: (what) => fatal('crash', 'The emulator aborted', what),
			onExit,
			preRun: [mountMemoryCards],
			postRun: [onRuntimeReadyForDiscs, onRuntimeReadyForCards],
		};

		const script = document.createElement('script');
		script.src = versioned(JS_FILE);
		script.async = true;
		script.addEventListener('error', async () => {
			let reason = 'not found';
			try {
				const r = await fetch(versioned(JS_FILE), { method: 'HEAD', cache: 'no-store' });
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
		// Not before the page is isolated (see isolationPending()): the runtime needs shared memory.
		if (page.runtimeRequested || page.phase !== 'loading' || !window.crossOriginIsolated)
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
		if (page.litWait && !page.lit)
			probeLit(frame.imageData);
		frame.presented++;
		frame.width = width;
		frame.height = height;
		frame.vsync = vsync;

		if (!page.frameSinceBoot && seq !== page.seqAtBoot) {
			page.frameSinceBoot = true;
			render();
		}
	}

	// The disc library keeps its room dark until the game shows a picture (UI-SPEC.md §15): while it waits
	// (page.litWait), a frame is looked at every LIT_PROBE_MS for anything brighter than near black. A
	// boot's first frames are usually black while the game loads.
	const LIT_PROBE_MS = 200;
	const LIT_LEVEL = 40; // a channel above this, out of 255, is light
	const LIT_SHARE = 0.005; // and this share of the sampled pixels being light makes a picture
	const litProbe = { at: 0, ctx: null };
	function probeLit(source) {
		const now = performance.now();
		if (now - litProbe.at < LIT_PROBE_MS)
			return;
		litProbe.at = now;
		let data;
		if (source instanceof ImageData) {
			data = source.data;
		} else {
			// A GPU frame: read back a small copy only (64 × 48).
			if (!litProbe.ctx)
				litProbe.ctx = new OffscreenCanvas(64, 48).getContext('2d', { willReadFrequently: true });
			litProbe.ctx.drawImage(source, 0, 0, 64, 48);
			data = litProbe.ctx.getImageData(0, 0, 64, 48).data;
		}
		const step = Math.max(1, Math.floor(data.length / 4 / 4096)) * 4;
		let light = 0, n = 0;
		for (let i = 0; i < data.length; i += step, n++) {
			if (data[i] > LIT_LEVEL || data[i + 1] > LIT_LEVEL || data[i + 2] > LIT_LEVEL)
				light++;
		}
		if (n && light / n >= LIT_SHARE) {
			page.lit = true;
			pageLog('Disc library: the game shows a picture.');
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
			else if (m.type === 'mode')
				onGpuMode(m);
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
		if (page.litWait && !page.lit)
			probeLit(bitmap);
		if (savestates.thumbWait)
			stateThumbFrom(bitmap);
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
		if (permanent)
			gpu.mode = { hw: false, webgpu: false, reason: gpu.fallback };
		renderGfx();
	}

	// The renderer the GS opened, and why it is not the GPU one when it is not.
	function onGpuMode(m) {
		gpu.mode = { hw: !!m.hw, webgpu: !!m.webgpu, reason: m.reason || '' };
		const name = gfxModeName(gpu.mode);
		const why = gfxWhy(gpu.mode);
		pageLog(`Graphics: ${name}${why ? ` (${why})` : ''}.`, gpu.mode.hw ? undefined : 'warn');
		renderGfx();
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

	// Where each presented pixel costs CPU time or a GPU readback, the GS presents at most this many
	// lines (about the PS2's own) and CSS scales the canvas up: the 2D path scales every frame on the
	// CPU, and Firefox's transferToImageBitmap() on a WebGPU canvas waits for the GPU and copies the
	// frame back (measured in Zen 1.22 on an M1 Max: 9.6 ms a frame at 1960x1470, 2.9 ms at 640x512;
	// Chrome: 0.02 ms at any size, so it keeps the full size).
	const SURFACE_CAP_LINES = 512;
	const SURFACE_CAPPED = IS_FIREFOX || !GPU_ALLOWED || !HAS_JSPI || !navigator.gpu;

	function feedSurfaceSize(width, height, scale) {
		if (TEST_SURFACE) {
			width = Number(TEST_SURFACE[1]);
			height = Number(TEST_SURFACE[2]);
			scale = 1;
		} else if (SURFACE_CAPPED && height > SURFACE_CAP_LINES) {
			const f = SURFACE_CAP_LINES / height;
			width = Math.max(1, Math.round(width * f));
			height = SURFACE_CAP_LINES;
			scale *= f;
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
		if (SHOW_THREADS)
			page.threads = [0, 1, 2].map((i) => api.threadUsage(i));
		if (rec.recording)
			recDrain();

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
		stateRender();
		renderEe();
		statesGameChanged();
		renderStates();
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
		return page.phase === 'ready' && !!page.bios && !!page.writtenBiosName && !page.biosReading && discBootable() && !devElf.pending && !devState.pending &&
			(page.vmState === STATE.WAITING || page.vmState === STATE.STOPPED || page.vmState === STATE.FAILED);
	}

	function canStop() {
		return page.phase === 'ready' && (page.vmState === STATE.STARTING || page.vmState === STATE.RUNNING);
	}

	// The Boot button and Enter on the screen. While the runtime is still loading this queues the
	// boot, and pressing again cancels it.
	function boot() {
		audioUnlock(); // inside the gesture: the browser lets sound start
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
		// The disc is still being opened or read (?disc=, a disc just chosen, a disc reader being restarted, the
		// check before a boot): the boot waits for it, queued like one pressed while the emulator downloads.
		if (page.phase === 'ready' && page.bios && !discBootable()) {
			if (disc.checking && !discCheckShown())
				return; // a second press of the boot that is reading the start of the disc right now
			if (reopenDisc()) {
				page.autoBoot = true; // a failed open says so and drops it (openDiscInWorker)
				render();
				return;
			}
			page.autoBoot = !page.autoBoot;
			render();
			return;
		}
		startVm();
	}

	// checked: the disc was just read (checkDiscThenBoot), or there is none.
	function startVm(checked = false) {
		if (!canBoot())
			return;
		audioUnlock(); // a boot without a gesture (?bios=) makes the context too; it waits for a key or click
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
		page.lit = false;
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
		audioFadeOut(); // before the PS2 stops and its sound ends
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
		// A BIOS the user chooses is a new attempt, so the error line goes. One that arrives by itself (?bios=,
		// or the one remembered here) keeps a disc error: with ?bios= and ?disc= the BIOS often arrives after
		// the disc failed, and the error is then the only sign of why the queued boot did not run.
		if (origin === 'file' || !page.errorIsDisc)
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
	const DISC_UNSUPPORTED_RE = /\.dump$/i;
	// Compressed images PCSX2's own readers open (through WebDisc::OpenCFile): the extension and the file's first
	// bytes must agree.
	const DISC_COMPRESSED = [
		{ re: /\.chd$/i, magic: [0x4d, 0x43, 0x6f, 0x6d, 0x70, 0x72, 0x48, 0x44] }, // "MComprHD"
		{ re: /\.cso$/i, magic: [0x43, 0x49, 0x53, 0x4f] }, // "CISO"
		{ re: /\.zso$/i, magic: [0x5a, 0x49, 0x53, 0x4f] }, // "ZISO"
		{ re: /\.gz$/i, magic: [0x1f, 0x8b] },
	];
	const DISC_BOOT_FULL = 1; // pcsx2web_boot_disc(): the BIOS intro instead of fast boot
	const DISC_POLL_MS = 500;
	const DISC_CHANGE_TIMEOUT_MS = 30000; // pcsx2web_change_disc(): a .gz insert indexes the whole image
	const DISC_ERROR_MS = 8000;
	// The check before a boot usually takes milliseconds; the slate says it is reading the disc only after this.
	const DISC_CHECK_SHOW_MS = 300;
	// ?disc= headers: as long as the disc worker gives a URL to open (URL_OPEN_TIMEOUT_MS).
	const DISC_URL_TIMEOUT_MS = 15000;
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
		savesVisit: 'Saves last for this visit only.',
		savesOtherTab: 'Saves last for this visit only: TabStationToo is open in another tab.',
		lastDisc: 'Last disc: ',
		openAgain: 'Open it again',
		waiting: ' is chosen. Add a BIOS to boot it.',
		blocked: 'Stop the PS2 first to change the disc.',
		blockedAny: 'Stop the PS2 first to change the disc or BIOS.',
		drop: 'Drop to use this disc or BIOS.',
		notDisc: 'That file isn’t a disc image TabStationToo can read. Use an .iso, .bin, .img, .chd or .cso of a PS2 disc.',
		compressed: 'Block dumps (.dump) aren’t supported. Use an .iso, .chd or .cso.',
		unreadable: 'Couldn’t read the disc file. It may have been moved, changed or deleted; choose it again.',
		denied: (name) => `The browser didn’t allow reading ${name}. Choose it again.`,
		fetchFailed: (path, why) => `Couldn’t open the disc at ${path} (${why}).`,
		otherOrigin: '?disc= only accepts addresses on this server.',
		lost: 'The disc can’t be read any more. Stop, then choose the file again.',
		lostServer: 'The disc server stopped answering, so the game can’t read its disc.',
		lostUrl: 'The game can’t read its disc from the server any more.',
		readable: 'The disc can be read again. If the game doesn’t go on, stop it and boot again.',
		checking: 'Reading the disc…',
		readerFailed: 'The disc reader stopped working. Reload the page to read discs again.',
		removed: 'Disc removed. Boot opens the console’s menus.',
		swapping: 'Changing the disc…',
		swapped: (name) => `Disc changed: ${name}.`,
		ejected: 'Disc removed. The game goes on with the tray empty.',
		swapFailed: 'Couldn’t change the disc. The game goes on with the one it had.',
		swapReverted: 'Couldn’t open that disc, so the old one went back in.',
		swapEmpty: 'Couldn’t open that disc. The tray is empty; choose a disc again.',
		insertFailed: 'PCSX2 couldn’t read that disc. The tray is empty; choose another.',
		menuReason: 'The disc can change once the PS2 runs.',
		libraryWhileRunning: 'Stop the PS2 to use the disc library. Choose another disc… changes it while it runs.',
		memcardFailed: 'Couldn’t keep the memory card in this browser. Saves last for this visit.',
		announceReady: 'Disc ready.',
		announceRemoved: 'Disc removed.',
		announceChecking: 'Reading the disc.',
		libraryNoBios: 'Add a BIOS first: the PS2 needs one to boot a disc.',
		libraryUnavailable: 'The disc reader isn’t working in this page. Reload it to boot discs.',
	};

	const discEls = {
		slateDisc: $('slate-disc'),
		slateLibrary: $('slate-library'),
		slateRemove: $('slate-remove'),
		input: $('disc-input'),
		btn: $('disc-btn'),
		btnName: $('disc-btn-name'),
		menu: $('disc-menu'),
		menuName: $('disc-menu-name'),
		menuWhere: $('disc-menu-where'),
		menuChoose: $('disc-menu-choose'),
		menuLibrary: $('disc-menu-library'),
		menuRemove: $('disc-menu-remove'),
		menuReason: $('disc-menu-reason'),
		tray: $('tray-library'),
	};

	const disc = {
		worker: null,
		workerLock: '', // the Web Lock the worker holds while it lives ('' if it cannot hold one)
		attached: false,
		dead: false, // no disc reader in this page (missing exports, or it kept failing)
		restarting: false, // a worker died; the next one is not attached yet
		restarts: 0,
		checking: false, // the disc is being read before a boot (checkDiscThenBoot)
		checkSlow: false, // that check has taken DISC_CHECK_SHOW_MS or more
		checkQueued: false, // the slate already showed that boot queued when the check began
		checkTimer: 0,
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
		runIsDisc: false, // the current or last boot booted a disc (false again once a running game's disc is ejected)
		trayEmpty: false, // a running game's disc was ejected (until a disc goes in, or the next boot)
		swapping: false, // swapDisc() or removeRunningDisc() is between its steps
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

	// The chosen disc is closed and nothing opens it: a restarted disc reader could not take it over and opening
	// it again failed too. A boot opens it again (DISC-DESIGN 5.6); false when there is nothing to open.
	function reopenDisc() {
		const d = disc.current;
		if (!d || d.opened || disc.opening || disc.urlPending || disc.checking || disc.restarting || !disc.attached)
			return false;
		disc.opening = d;
		pageLog(`Opening ${d.name} again for the boot.`);
		openDiscInWorker(d, disc.op);
		return true;
	}

	// Whether the slate shows the boot that waits for the disc check: once the check takes a while, or at once
	// when that boot was already shown queued (so the slate does not flip back to the title card meanwhile).
	function discCheckShown() {
		return disc.checkSlow || disc.checkQueued;
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

	// A disc image TabStationToo can read, from its name and first bytes: a flat image with an ISO 9660 volume
	// where PCSX2 looks, or a compressed image whose header matches its extension.
	function isDiscBytes(name, bytes, size) {
		const compressed = DISC_COMPRESSED.find((c) => c.re.test(name));
		if (compressed)
			return bytes.length >= compressed.magic.length && compressed.magic.every((b, i) => bytes[i] === b);
		return size >= DISC_MIN_SIZE && discLayout(bytes) !== null;
	}

	async function isDiscImage(file) {
		const bytes = new Uint8Array(await file.slice(0, Math.min(file.size, DISC_HEADER_BYTES)).arrayBuffer());
		return isDiscBytes(file.name, bytes, file.size);
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
		if (!canChangeDisc()) {
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
		} else if (!canChangeBios()) {
			subtitle(DISC_COPY.blockedAny);
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
		if (!canChangeDisc()) {
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

	function newDiscRecord(desc, handle) {
		const d = {
			name: sanitizeDiscName(desc.name), path: '', size: desc.size, kind: handle ? 'handle' : desc.kind,
			file: desc.file || null, url: desc.url || null, handle: handle || null, title: '', opened: false,
			remembered: false, sourceName: desc.name,
		};
		d.path = `${DISC_DIR}/${d.name}`;
		const r = disc.remembered;
		if (r && r.name === desc.name && r.size === desc.size && r.title)
			d.title = r.title;
		return d;
	}

	function chooseDisc(desc, handle) {
		if (!canChangeDisc()) {
			subtitle(DISC_COPY.blocked);
			return;
		}
		if (swapping()) {
			swapDisc(desc, handle);
			return;
		}
		const op = ++disc.op;
		const d = newDiscRecord(desc, handle);
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
			return false; // opened once the worker is attached
		const worker = disc.worker;
		const msg = d.url ? { type: 'open', url: d.url, name: d.name } : { type: 'open', file: d.file, name: d.name };
		const reply = await discCall(msg);
		if (op !== disc.op || disc.opening !== d)
			return false; // a newer choice or Remove won
		if (!reply.ok && worker !== disc.worker) {
			// The disc reader stopped during the open: nothing was learnt about the disc. It stays the one being
			// opened, so the restarted reader opens it (startDiscWorker), and a reader that cannot restart says so.
			pageLog(`The disc reader stopped while it opened ${d.name}; the next one opens it again.`, 'warn');
			return false;
		}
		disc.opening = null;
		if (!reply.ok) {
			// The worker keeps serving the disc it had, so the previous choice stands.
			pageLog(`Could not open the disc ${d.name}: ${reply.error ? reply.error.message : 'unknown error'}`, 'warn');
			page.autoBoot = false;
			discError(discFailureText(d, reply.error));
			return false;
		}
		const prev = disc.current;
		d.size = (reply.status && reply.status.disc && reply.status.disc.size) || d.size;
		d.generation = (reply.status && reply.status.disc && reply.status.disc.generation) || 0;
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
			return false;
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
			return true; // a swap inserts it (swapDisc); a restarted reader's reopen leaves the game without it (reviewRunningDisc)
		announce(DISC_COPY.announceReady);
		if (focusIsFree())
			focusPrimary();
		maybeAutoBoot();
		return true;
	}

	// ---- swapping the disc while the game runs (UI-SPEC.md §10)

	// The disc can change before a boot, and while a game or the console's menus run (not while it starts, not for
	// an ELF, not during another swap).
	function canChangeDisc() {
		if (canChangeBios())
			return true;
		return page.phase === 'ready' && page.vmState === STATE.RUNNING && !devElf.bytes && !disc.swapping;
	}

	const swapping = () => page.vmState === STATE.RUNNING && !canChangeBios();

	// pcsx2web_change_disc(path): '' ejects, a /disc/ path inserts. Resolves to true or false.
	function changeDiscInVm(path) {
		const M = window.Module;
		return new Promise((resolve) => {
			if (!M || typeof M._pcsx2web_disc_change_result !== 'function') {
				resolve(false);
				return;
			}
			const before = M._pcsx2web_disc_change_result() >>> 0;
			if (M.ccall('pcsx2web_change_disc', 'number', ['string'], [path]) !== 0) {
				resolve(false);
				return;
			}
			const started = performance.now();
			const check = () => {
				const result = M._pcsx2web_disc_change_result() >>> 0;
				if (result !== before)
					resolve((result & 1) === 1);
				else if (performance.now() - started > DISC_CHANGE_TIMEOUT_MS || page.vmState !== STATE.RUNNING)
					resolve(false);
				else
					setTimeout(check, 50);
			};
			setTimeout(check, 50);
		});
	}

	// The game's disc goes out: PCSX2 closes it and the game sees the tray open.
	async function ejectRunningDisc() {
		if (!disc.runIsDisc)
			return true;
		const ok = await changeDiscInVm('');
		if (ok) {
			disc.runIsDisc = false;
			disc.trayEmpty = true;
			disc.runTitle = '';
			disc.runSerial = '';
			disc.errorShown = false;
		}
		return ok;
	}

	// The current disc goes into the running PS2. Its title comes back through pcsx2web_game() as at a boot.
	async function insertRunningDisc(d) {
		const ok = await changeDiscInVm(d.path);
		if (ok) {
			disc.runIsDisc = true;
			disc.trayEmpty = false;
			disc.runGeneration = d.generation || (disc.status && disc.status.disc ? disc.status.disc.generation : 0);
			disc.runTitle = '';
			disc.runSerial = '';
			disc.errorShown = false;
		}
		return ok;
	}

	// Eject, open the new file in the worker (so nothing reads the old one any more), insert. If the new file
	// cannot be opened, the worker still serves the old disc, which goes back in.
	async function swapDisc(desc, handle) {
		disc.swapping = true;
		page.error = '';
		hideDiscMenu();
		subtitle(DISC_COPY.swapping);
		render();
		const prev = disc.current && disc.current.opened && disc.runIsDisc ? disc.current : null;
		let line;
		let swappedOk = false;
		try {
			if (!await ejectRunningDisc()) {
				line = DISC_COPY.swapFailed;
				return;
			}
			const op = ++disc.op;
			const d = newDiscRecord(desc, handle);
			disc.opening = d;
			pageLog(`Disc swap: ${desc.name} (${desc.size.toLocaleString('en-US')} bytes).`);
			if (!await openDiscInWorker(d, op)) {
				if (prev && disc.current === prev && await insertRunningDisc(prev)) {
					line = DISC_COPY.swapReverted;
				} else {
					line = DISC_COPY.swapEmpty;
				}
				return;
			}
			if (!await insertRunningDisc(d)) {
				line = DISC_COPY.insertFailed;
				return;
			}
			line = DISC_COPY.swapped(d.name);
			swappedOk = true;
		} finally {
			disc.swapping = false;
			if (line) {
				subtitle(line, swappedOk ? undefined : DISC_ERROR_MS);
				announce(line);
			}
			pageLog(line || 'Disc swap ended.');
			statesGameChanged();
			render();
		}
	}

	// Remove the disc while the game runs: the tray stays open, empty.
	async function removeRunningDisc() {
		disc.swapping = true;
		hideDiscMenu();
		render();
		let ok = false;
		try {
			ok = await ejectRunningDisc();
			if (ok) {
				disc.op++;
				const d = disc.current;
				disc.current = null;
				disc.opening = null;
				if (disc.attached)
					discCall({ type: 'close' });
				try {
					if (d)
						window.Module.FS.unlink(d.path);
				} catch {
					// already gone
				}
				forgetRememberedDisc();
			}
		} finally {
			disc.swapping = false;
			const line = ok ? DISC_COPY.ejected : DISC_COPY.swapFailed;
			subtitle(line);
			announce(line);
			pageLog(line);
			statesGameChanged();
			render();
		}
	}

	function removeDisc() {
		if (!canChangeDisc()) {
			subtitle(DISC_COPY.blocked);
			return;
		}
		if (swapping()) {
			removeRunningDisc();
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
		if (!canChangeDisc()) {
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
				types: [{ description: 'PS2 disc images', accept: { 'application/octet-stream': ['.iso', '.bin', '.img', '.chd', '.cso', '.zso', '.gz'] } }],
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
		if (!canChangeDisc()) {
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
		// Until this answers, a queued boot waits for the disc: a server that never answers must not hold it.
		const controller = new AbortController();
		let timedOut = false;
		const timer = setTimeout(() => {
			timedOut = true;
			controller.abort();
		}, DISC_URL_TIMEOUT_MS);
		try {
			const res = await fetch(url.href, {
				headers: { Range: `bytes=0-${DISC_HEADER_BYTES - 1}` }, cache: 'no-store', signal: controller.signal,
			});
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
			const why = timedOut ? 'no answer in time' : err && err.httpStatus ? `HTTP ${err.httpStatus}`
				: /byte ranges/.test(String(err && err.message)) ? 'no byte ranges' : 'network error';
			pageLog(`Could not fetch the disc from ${url.pathname}: ${timedOut ? `no answer in ${DISC_URL_TIMEOUT_MS / 1000} s` : errorText(err)}`, 'warn');
			discError(DISC_COPY.fetchFailed(displayPath(url.href), why));
			return;
		} finally {
			clearTimeout(timer);
		}
		disc.urlPending = false;
		if (!isDiscBytes(base, bytes, size)) {
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
	// opens the console's menus) and the slate says why. The request block is stopped, so what waits in it
	// fails now and later reads fail at once, instead of each waiting out its deadline for a reader that
	// never comes (no worker is left that could write into it).
	function discReaderGone(why) {
		disc.dead = true;
		disc.restarting = false;
		failDiscRequests();
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
			worker = new Worker(versioned(DISC_WORKER_FILE));
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
		if (disc.swapping)
			return; // the generation changes on purpose; insertRunningDisc() takes the new one
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
				startDiscWorker(discResumeFor(disc.current)); // if it cannot start, discReaderGone() stops the block
			} else {
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

	// Meanwhile the boot waits like a queued one (page.autoBoot; pressing Boot again cancels it). The slate
	// shows it only if the check takes a while: "Reading the disc…" (UI-SPEC.md §10).
	function checkDiscThenBoot() {
		const d = disc.current;
		const op = disc.op;
		disc.checking = true;
		disc.checkSlow = false;
		disc.checkQueued = page.lastState === 'queued';
		page.autoBoot = true;
		clearTimeout(disc.checkTimer);
		disc.checkTimer = setTimeout(() => {
			if (!disc.checking)
				return;
			const before = page.lastState;
			disc.checkSlow = true;
			render();
			if (before === 'queued' && page.lastState === 'queued')
				announce(DISC_COPY.announceChecking); // already queued: no state change says it
		}, DISC_CHECK_SHOW_MS);
		render();
		discCall({ type: 'check' }).then((reply) => {
			clearTimeout(disc.checkTimer);
			disc.checking = false;
			disc.checkSlow = false;
			disc.checkQueued = false;
			if (op !== disc.op || disc.current !== d) {
				page.autoBoot = false; // the disc was changed or removed meanwhile: this boot was for the old one
				render();
				return;
			}
			if (!reply.ok && reply.error && reply.error.code === 'CANCELLED' && (disc.restarting || disc.dead)) {
				// The disc reader stopped during the check. A restarted one runs the queued boot (and a new check)
				// once it has the disc; a reader that could not restart has said why (discReaderGone).
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
			if (!page.autoBoot) {
				render(); // cancelled meanwhile
				return;
			}
			page.autoBoot = false;
			startVm(true);
		});
	}

	// ?elf=<url> (developer runs, README "Automated runs"): a PS2 executable, fetched once and booted
	// instead of a disc or the BIOS menu, with the ?bios= BIOS. The queued ?bios= boot waits for it.
	const ELF_DIR = '/elf';
	const devElf = { pending: false, name: null, bytes: null };

	async function loadElfFromUrl(param) {
		const fail = (text, detail) => {
			devElf.pending = false;
			page.autoBoot = false;
			showError(text);
			if (detail)
				pageLog(detail, 'warn');
			render();
		};
		let url;
		try {
			url = new URL(param, location.href);
		} catch {
			fail(COPY.elfFetchFailed(param, 'not a valid address'));
			return;
		}
		if (url.origin !== location.origin) {
			fail(COPY.elfOtherOrigin, `Refusing to fetch an ELF from another origin: ${url.href}`);
			return;
		}
		let base = url.pathname.split('/').pop() || '';
		try {
			base = decodeURIComponent(base);
		} catch {
			// keep it encoded
		}
		devElf.pending = true;
		pageLog(`Fetching ELF from ${url.pathname}`);
		try {
			const response = await fetch(url.href, { credentials: 'same-origin', cache: 'no-store' });
			if (!response.ok)
				throw httpError(response.status, response.statusText, url.pathname);
			devElf.bytes = new Uint8Array(await response.arrayBuffer());
		} catch (err) {
			fail(COPY.elfFetchFailed(url.pathname, downloadReason(err)), `Could not fetch the ELF from ${url.pathname}: ${errorText(err)}`);
			return;
		}
		// Same rules as a BIOS name: a plain file name, written directly inside /elf.
		devElf.name = sanitizeBiosName(base || 'boot.elf');
		devElf.pending = false;
		pageLog(`ELF fetched: ${devElf.name} (${devElf.bytes.length.toLocaleString('en-US')} bytes).`);
		maybeAutoBoot();
	}

	// Writes the ELF into the module's filesystem (again: a Stop leaves it there) and boots it.
	function bootElfRequest() {
		const FS = window.Module.FS;
		const path = `${ELF_DIR}/${devElf.name}`;
		if (!FS.analyzePath(ELF_DIR).exists)
			FS.mkdir(ELF_DIR);
		FS.writeFile(path, devElf.bytes);
		pageLog(`ELF boot: ${path} with ${BIOS_DIR}/${page.bios.name}.`);
		return window.Module.ccall('pcsx2web_boot_elf', 'number', ['string', 'string'], [page.bios.name, path]);
	}

	// Developer save states (?threads=1, ?state=): snapshots of the whole emulated machine, not memory card saves.
	// Save state writes one at the CPU thread's next pause and downloads it; Load state… puts a .p2s file the user
	// picks into the running game; Reload state puts the last one saved or loaded back. ?state=<url>, a file on this
	// server (dev-bios/<name> with serve.py --bios-dir), is also loaded by the next boot once the game has started.
	const STATE_DIR = '/states';
	const STATE_TIMEOUT_MS = 60000;
	const devState = { pending: false, bootName: null, bootBytes: null, bytes: null, busy: null, picked: false };

	async function loadStateFromUrl(param) {
		const fail = (text, detail) => {
			devState.pending = false;
			page.autoBoot = false;
			showError(text);
			if (detail)
				pageLog(detail, 'warn');
			render();
		};
		let url;
		try {
			url = new URL(param, location.href);
		} catch {
			fail(COPY.stateFetchFailed(param, 'not a valid address'));
			return;
		}
		if (url.origin !== location.origin) {
			fail(COPY.stateOtherOrigin, `Refusing to fetch a save state from another origin: ${url.href}`);
			return;
		}
		let base = url.pathname.split('/').pop() || '';
		try {
			base = decodeURIComponent(base);
		} catch {
			// keep it encoded
		}
		pageLog(`Fetching the save state from ${url.pathname}`);
		try {
			const response = await fetch(url.href, { credentials: 'same-origin', cache: 'no-store' });
			if (!response.ok)
				throw httpError(response.status, response.statusText, url.pathname);
			devState.bootBytes = new Uint8Array(await response.arrayBuffer());
		} catch (err) {
			fail(COPY.stateFetchFailed(url.pathname, downloadReason(err)), `Could not fetch the save state from ${url.pathname}: ${errorText(err)}`);
			return;
		}
		devState.bootName = sanitizeBiosName(base || 'boot.p2s');
		devState.bytes = devState.bootBytes;
		devState.pending = false;
		pageLog(`Save state fetched: ${devState.bootName} (${devState.bootBytes.length.toLocaleString('en-US')} bytes); the next boot loads it.`);
		stateRender();
		maybeAutoBoot();
	}

	function writeStateFile(name, bytes) {
		const FS = window.Module.FS;
		if (!FS.analyzePath(STATE_DIR).exists)
			FS.mkdir(STATE_DIR);
		const path = `${STATE_DIR}/${name}`;
		FS.writeFile(path, bytes);
		return path;
	}

	// bootRequest(): the ?state= state for this boot, or none.
	function setBootState() {
		if (typeof window.Module._pcsx2web_set_boot_state !== 'function')
			return;
		const path = devState.bootBytes ? writeStateFile(devState.bootName, devState.bootBytes) : '';
		if (path)
			pageLog(`The boot loads the save state ${path}.`);
		window.Module.ccall('pcsx2web_set_boot_state', 'number', ['string'], [path]);
	}

	function stateOperation(kind) {
		const M = window.Module;
		if (devState.busy || savestates.busy || page.vmState !== STATE.RUNNING || !M || typeof M._pcsx2web_state_result !== 'function')
			return;
		if (kind === 'load' && !devState.bytes)
			return;
		const save = kind === 'save';
		const path = save ? `${STATE_DIR}/snapshot.p2s` : writeStateFile('restore.p2s', devState.bytes);
		if (save && !M.FS.analyzePath(STATE_DIR).exists)
			M.FS.mkdir(STATE_DIR);
		const before = M._pcsx2web_state_result() >>> 0;
		if (M.ccall(save ? 'pcsx2web_save_state' : 'pcsx2web_load_state', 'number', ['string'], [path]) !== 0) {
			pageLog(`${save ? 'Saving' : 'Loading'} the state was refused.`, 'warn');
			return;
		}
		devState.busy = kind;
		stateRender();
		const started = performance.now();
		const finish = (ok, why) => {
			devState.busy = null;
			devState.picked = false;
			if (ok && save) {
				const bytes = M.FS.readFile(path);
				devState.bytes = bytes;
				const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
				const a = document.createElement('a');
				const name = (disc.runSerial || disc.runTitle || 'bios').replace(/[^A-Za-z0-9._-]+/g, '-');
				a.href = url;
				a.download = `tabstationtoo-state-${name}-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-')}.p2s`;
				document.body.append(a);
				a.click();
				a.remove();
				setTimeout(() => URL.revokeObjectURL(url), 1000);
				pageLog(`State saved (${bytes.length.toLocaleString('en-US')} bytes) and downloaded as ${a.download}.`);
				announce('State saved');
			} else if (ok) {
				pageLog('State loaded.');
				announce('State loaded');
			} else {
				pageLog(`${save ? 'Saving' : 'Loading'} the state failed${why ? `: ${why}` : ' (see the log)'}.`, 'warn');
				announce(save ? 'Saving the state failed' : 'Loading the state failed');
			}
			stateRender();
		};
		const check = () => {
			const result = M._pcsx2web_state_result() >>> 0;
			if (result !== before) {
				finish((result & 1) === 1);
				return;
			}
			if (performance.now() - started > STATE_TIMEOUT_MS) {
				finish(false, 'no answer from the emulator');
				return;
			}
			setTimeout(check, 100);
		};
		setTimeout(check, 100);
	}

	// Load state…: the chosen file is loaded into the running game, and kept for Reload state.
	async function stateFileChosen() {
		const file = els.stateInput.files && els.stateInput.files[0];
		els.stateInput.value = '';
		if (!file || devState.busy || page.vmState !== STATE.RUNNING)
			return;
		let bytes;
		try {
			bytes = new Uint8Array(await file.arrayBuffer());
		} catch (err) {
			pageLog(`Could not read the save state ${file.name}: ${errorText(err)}`, 'warn');
			announce('Loading the state failed');
			return;
		}
		pageLog(`Save state chosen: ${file.name} (${bytes.length.toLocaleString('en-US')} bytes).`);
		devState.bytes = bytes;
		devState.picked = true;
		stateOperation('load');
		if (!devState.busy)
			devState.picked = false;
	}

	function stateRender() {
		if (!els.stateSave)
			return;
		const running = page.vmState === STATE.RUNNING;
		els.stateSave.disabled = !running || !!devState.busy;
		els.stateLoad.disabled = !running || !!devState.busy;
		els.stateReload.disabled = !running || !!devState.busy || !devState.bytes;
		setText(els.stateSaveLabel, devState.busy === 'save' ? 'Saving…' : 'Save state');
		const loading = devState.busy === 'load';
		setText(els.stateLoadLabel, loading && devState.picked ? 'Loading…' : 'Load state…');
		setText(els.stateReloadLabel, loading && !devState.picked ? 'Loading…' : 'Reload state');
	}

	// startVm(): an ELF boot with ?elf=, a disc boot when a disc is open, otherwise the BIOS alone.
	function bootRequest() {
		setBootState();
		eeBoot();
		if (devElf.bytes) {
			disc.runIsDisc = false;
			return bootElfRequest();
		}
		const d = disc.current;
		disc.runIsDisc = !!(d && d.opened && !disc.dead);
		disc.trayEmpty = false;
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
		if (!disc.runIsDisc || !live || (title === disc.runTitle && serial === disc.runSerial))
			return;
		// No title for a disc PCSX2's game database does not know (Host::OnGameChanged): the file name stays.
		const newSerial = serial !== disc.runSerial;
		disc.runTitle = title;
		disc.runSerial = serial;
		pageLog(`Game: ${title || 'not in the game database'}${serial ? ` (${serial})` : ''}`);
		if (newSerial) {
			pipeGameKnown(serial);
			codeGameKnown(serial);
			eeGameKnown(serial);
		}
		statesGameChanged();
		const d = disc.current;
		if (title && d && d.title !== title) {
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
				if (page.vmState === STATE.STOPPED || page.vmState === STATE.FAILED) {
					checkMemoryCards(true); // a normal Stop closed the card: keep it now
					pipeVmStopped();
					codeVmStopped();
				}
			}
			pipeTick();
			codeTick();
		}
		checkMemoryCards(false);
		const running = disc.runIsDisc && disc.current && pageState() === 'running';
		const game = running ? disc.runTitle || discDisplayTitle(disc.current) : '';
		const title = game ? `${game} · TabStationToo` : 'TabStationToo';
		if (document.title !== title)
			document.title = title;
	}

	// ------------------------------------------------------------------ pipeline lists
	// The WebGPU HW renderer logs the TFX pipeline keys its draws ask for (pcsx2web_tfx_log(), 36 bytes a
	// key). The page keeps them per game serial in IndexedDB (PIPE_IDB_NAME) and, when the same game boots
	// again, hands the list to the GS thread (pcsx2web_tfx_prewarm()), which creates those pipelines in the
	// background before the game asks for them (README "Pipeline lists"). Each list carries the key version
	// of the build that made it; the GS thread ignores a list whose keys meant something else.
	const PIPE_IDB_NAME = 'pcsx2-web-pipelines';
	const PIPE_IDB_STORE = 'tfx';
	const PIPE_KEY_BYTES = 36; // WebGPUHost::TFX_KEY_BYTES
	const PIPE_CAPACITY = 4096; // WebGPUHost::TFX_KEY_CAPACITY
	const PIPE_HEADER_BYTES = 16; // state, count, version lo, version hi
	const PIPE_SLOT_FREE = 0;
	const PIPE_SLOT_WRITING = 1;
	const PIPE_SLOT_READY = 2;
	const PIPE_READ_MS = 2000;
	const PIPE_SAVE_MS = 15000;
	const pipes = {
		serial: '', session: -1, staleSession: -1, read: 0, version: null, used: [], usedSet: new Set(),
		stored: null, handOver: null, dirty: false, lastRead: 0, lastSave: 0, writes: Promise.resolve(), warned: false,
	};

	function pipeIdbOpen() {
		return new Promise((resolve, reject) => {
			if (typeof indexedDB === 'undefined') {
				reject(new Error('IndexedDB is not available'));
				return;
			}
			const req = indexedDB.open(PIPE_IDB_NAME, 1);
			req.onupgradeneeded = () => req.result.createObjectStore(PIPE_IDB_STORE);
			req.onsuccess = () => resolve(req.result);
			req.onerror = () => reject(req.error);
			req.onblocked = () => reject(new Error('IndexedDB open blocked'));
		});
	}

	async function pipeIdb(mode, fn) {
		const db = await pipeIdbOpen();
		try {
			return await new Promise((resolve, reject) => {
				const tx = db.transaction(PIPE_IDB_STORE, mode);
				const req = fn(tx.objectStore(PIPE_IDB_STORE));
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

	function pipeWarn(what, err) {
		if (pipes.warned)
			return;
		pipes.warned = true;
		pageLog(`Pipeline list: could not ${what} (${err && err.message}); the game's first frames may stutter.`, 'warn');
	}

	const pipeKeyId = (bytes) => String.fromCharCode.apply(null, bytes);

	// The game's serial is known (readGameDetails()): start its list, and hand the stored one over.
	function pipeGameKnown(serial) {
		const M = window.Module;
		pipes.serial = serial;
		pipes.read = 0;
		pipes.used = [];
		pipes.usedSet = new Set();
		pipes.stored = null;
		pipes.handOver = null;
		pipes.dirty = false;
		if (!serial || !M || typeof M._pcsx2web_tfx_log !== 'function')
			return;
		pipeIdb('readonly', (s) => s.get(serial)).then((rec) => {
			if (pipes.serial !== serial || !rec || !(rec.keys instanceof ArrayBuffer) || !Array.isArray(rec.version))
				return;
			pipes.stored = rec;
			pipes.handOver = rec;
			pipeHandOver();
		}).catch((err) => pipeWarn('read the stored list', err));
	}

	// Writes the list into the prewarm slot, unless the GS thread is taking an earlier one (then the next
	// tick tries again). A list it has not taken yet is replaced.
	function pipeHandOver() {
		const rec = pipes.handOver;
		const buf = heapBuffer();
		if (!rec || !buf)
			return;
		const ptr = window.Module._pcsx2web_tfx_prewarm() >>> 0;
		const words = new Int32Array(buf, ptr, PIPE_HEADER_BYTES / 4);
		if (Atomics.compareExchange(words, 0, PIPE_SLOT_FREE, PIPE_SLOT_WRITING) !== PIPE_SLOT_FREE &&
			Atomics.compareExchange(words, 0, PIPE_SLOT_READY, PIPE_SLOT_WRITING) !== PIPE_SLOT_READY)
			return;
		pipes.handOver = null;
		const n = Math.min(Math.floor(rec.keys.byteLength / PIPE_KEY_BYTES), PIPE_CAPACITY);
		new Uint8Array(buf, ptr + PIPE_HEADER_BYTES, n * PIPE_KEY_BYTES).set(new Uint8Array(rec.keys, 0, n * PIPE_KEY_BYTES));
		words[2] = rec.version[0] | 0;
		words[3] = rec.version[1] | 0;
		Atomics.store(words, 1, n);
		Atomics.store(words, 0, PIPE_SLOT_READY);
		pageLog(`Pipeline list: ${n} pipelines this game used before, to create in the background.`);
	}

	// Copies the keys logged since the last read (a session is one GS device's log).
	function pipeRead() {
		const M = window.Module;
		const buf = heapBuffer();
		if (!pipes.serial || !buf || !M || typeof M._pcsx2web_tfx_log !== 'function')
			return;
		const ptr = M._pcsx2web_tfx_log() >>> 0;
		const words = new Uint32Array(buf, ptr, PIPE_HEADER_BYTES / 4);
		const session = Atomics.load(words, 0);
		if (session === pipes.staleSession)
			return; // the log of the VM that ran before this game's
		if (session !== pipes.session) {
			pipes.session = session;
			pipes.read = 0;
		}
		const count = Math.min(Atomics.load(words, 1), PIPE_CAPACITY);
		const version = [Atomics.load(words, 2) >>> 0, Atomics.load(words, 3) >>> 0];
		if (count <= pipes.read)
			return;
		const bytes = new Uint8Array(buf, ptr + PIPE_HEADER_BYTES + pipes.read * PIPE_KEY_BYTES,
			(count - pipes.read) * PIPE_KEY_BYTES).slice();
		if (Atomics.load(words, 0) !== session)
			return; // a new log started meanwhile: the next read starts it over
		pipes.version = version;
		for (let o = 0; o < bytes.length; o += PIPE_KEY_BYTES) {
			const key = bytes.subarray(o, o + PIPE_KEY_BYTES);
			const id = pipeKeyId(key);
			if (!pipes.usedSet.has(id)) {
				pipes.usedSet.add(id);
				pipes.used.push(key);
				pipes.dirty = true;
			}
		}
		pipes.read = count;
	}

	// Stores this session's keys, in the order draws first asked for them, then the stored keys of the same
	// version it has not used, up to PIPE_CAPACITY.
	function pipeSave() {
		pipeRead();
		if (!pipes.dirty || !pipes.serial || !pipes.version)
			return;
		const serial = pipes.serial;
		const version = pipes.version;
		const keys = pipes.used.slice(0, PIPE_CAPACITY);
		const old = pipes.stored;
		if (old && old.version[0] === version[0] && old.version[1] === version[1]) {
			const ob = new Uint8Array(old.keys);
			for (let o = 0; o + PIPE_KEY_BYTES <= ob.length && keys.length < PIPE_CAPACITY; o += PIPE_KEY_BYTES) {
				const key = ob.subarray(o, o + PIPE_KEY_BYTES);
				if (!pipes.usedSet.has(pipeKeyId(key)))
					keys.push(key);
			}
		}
		const out = new Uint8Array(keys.length * PIPE_KEY_BYTES);
		keys.forEach((k, i) => out.set(k, i * PIPE_KEY_BYTES));
		const rec = { version, keys: out.buffer, savedAt: Date.now() };
		pipes.dirty = false;
		pipes.stored = rec;
		pipes.lastSave = performance.now();
		// One write after another, so that an older list never lands after a newer one.
		pipes.writes = pipes.writes.then(() => pipeIdb('readwrite', (s) => s.put(rec, serial)))
			.catch((err) => pipeWarn('store the list', err));
	}

	function pipeTick() {
		if (pipes.handOver)
			pipeHandOver();
		const now = performance.now();
		if (!pipes.serial || now - pipes.lastRead < PIPE_READ_MS)
			return;
		pipes.lastRead = now;
		pipeRead();
		if (pipes.dirty && now - pipes.lastSave >= PIPE_SAVE_MS)
			pipeSave();
	}

	// The VM stopped: keep what it used, and ignore its log from now on (the next VM's device starts a new one).
	function pipeVmStopped() {
		const M = window.Module;
		const buf = heapBuffer();
		if (pipes.serial)
			pipeSave();
		if (buf && M && typeof M._pcsx2web_tfx_log === 'function')
			pipes.staleSession = Atomics.load(new Uint32Array(buf, M._pcsx2web_tfx_log() >>> 0, 1), 0);
		pipes.serial = '';
	}

	// ------------------------------------------------------------------ code lists
	// The EE JIT logs the blocks it compiles (pcsx2web_ee_code_log(): a PC and a hash of its code each). The page
	// keeps them per game serial in IndexedDB (CODE_IDB_NAME) and, when the same game boots again, hands the list
	// to the EE thread (pcsx2web_ee_code_list()), which compiles those blocks ahead when the frame limiter has time
	// left, before the game runs them (README "Code lists"). ?codelist=0 keeps the lists but does not hand them over.
	const CODE_IDB_NAME = 'pcsx2-web-code';
	const CODE_IDB_STORE = 'ee';
	const CODE_ENTRY_BYTES = 8; // PC, hash
	const CODE_CAPACITY = 32768; // EEJit::kCodeListCapacity
	const CODE_HEADER_BYTES = 16; // session or state, count, version lo, version hi
	const CODE_SLOT_FREE = 0;
	const CODE_SLOT_WRITING = 1;
	const CODE_SLOT_READY = 2;
	const CODE_READ_MS = 2000;
	const CODE_SAVE_MS = 15000;
	const codes = {
		serial: '', session: -1, staleSession: -1, read: 0, version: null, used: [], usedSet: new Set(),
		stored: null, handOver: null, dirty: false, lastRead: 0, lastSave: 0, writes: Promise.resolve(), warned: false,
	};

	function codeIdb(mode, fn) {
		return new Promise((resolve, reject) => {
			if (typeof indexedDB === 'undefined') {
				reject(new Error('IndexedDB is not available'));
				return;
			}
			const open = indexedDB.open(CODE_IDB_NAME, 1);
			open.onupgradeneeded = () => open.result.createObjectStore(CODE_IDB_STORE);
			open.onblocked = () => reject(new Error('IndexedDB open blocked'));
			open.onerror = () => reject(open.error);
			open.onsuccess = () => {
				const db = open.result;
				const tx = db.transaction(CODE_IDB_STORE, mode);
				const req = fn(tx.objectStore(CODE_IDB_STORE));
				let result;
				req.onsuccess = () => { result = req.result; };
				tx.oncomplete = () => { db.close(); resolve(result); };
				tx.onerror = () => { db.close(); reject(tx.error || req.error); };
				tx.onabort = () => { db.close(); reject(tx.error || new Error('IndexedDB transaction aborted')); };
			};
		});
	}

	function codeWarn(what, err) {
		if (codes.warned)
			return;
		codes.warned = true;
		pageLog(`Code list: could not ${what} (${err && err.message}); the game compiles its code as it runs it.`, 'warn');
	}

	// The game's serial is known (readGameDetails()): start its list, and hand the stored one over.
	function codeGameKnown(serial) {
		const M = window.Module;
		codes.serial = serial;
		codes.read = 0;
		codes.used = [];
		codes.usedSet = new Set();
		codes.stored = null;
		codes.handOver = null;
		codes.dirty = false;
		if (!serial || !M || typeof M._pcsx2web_ee_code_log !== 'function')
			return;
		codeIdb('readonly', (s) => s.get(serial)).then((rec) => {
			if (codes.serial !== serial || !rec || !(rec.entries instanceof ArrayBuffer) || !Array.isArray(rec.version))
				return;
			codes.stored = rec;
			if (params.get('codelist') === '0')
				return;
			codes.handOver = rec;
			codeHandOver();
		}).catch((err) => codeWarn('read the stored list', err));
	}

	// Writes the list into the EE thread's slot, unless it is taking an earlier one (then the next tick tries again).
	function codeHandOver() {
		const rec = codes.handOver;
		const buf = heapBuffer();
		if (!rec || !buf)
			return;
		const ptr = window.Module._pcsx2web_ee_code_list() >>> 0;
		const words = new Int32Array(buf, ptr, CODE_HEADER_BYTES / 4);
		if (Atomics.compareExchange(words, 0, CODE_SLOT_FREE, CODE_SLOT_WRITING) !== CODE_SLOT_FREE &&
			Atomics.compareExchange(words, 0, CODE_SLOT_READY, CODE_SLOT_WRITING) !== CODE_SLOT_READY)
			return;
		codes.handOver = null;
		const n = Math.min(Math.floor(rec.entries.byteLength / CODE_ENTRY_BYTES), CODE_CAPACITY);
		new Uint8Array(buf, ptr + CODE_HEADER_BYTES, n * CODE_ENTRY_BYTES).set(new Uint8Array(rec.entries, 0, n * CODE_ENTRY_BYTES));
		words[2] = rec.version[0] | 0;
		words[3] = rec.version[1] | 0;
		Atomics.store(words, 1, n);
		Atomics.store(words, 0, CODE_SLOT_READY);
		pageLog(`Code list: ${n} blocks of code this game ran before, to compile ahead.`);
	}

	// Copies the entries logged since the last read (a session is one VM's log).
	function codeRead() {
		const M = window.Module;
		const buf = heapBuffer();
		if (!codes.serial || !buf || !M || typeof M._pcsx2web_ee_code_log !== 'function')
			return;
		const ptr = M._pcsx2web_ee_code_log() >>> 0;
		const words = new Uint32Array(buf, ptr, CODE_HEADER_BYTES / 4);
		const session = Atomics.load(words, 0);
		if (session === codes.staleSession)
			return; // the log of the VM that ran before this game's
		if (session !== codes.session) {
			codes.session = session;
			codes.read = 0;
		}
		const count = Math.min(Atomics.load(words, 1), CODE_CAPACITY);
		const version = [Atomics.load(words, 2) >>> 0, Atomics.load(words, 3) >>> 0];
		if (count <= codes.read)
			return;
		const entries = new Uint32Array(buf, ptr + CODE_HEADER_BYTES + codes.read * CODE_ENTRY_BYTES, (count - codes.read) * 2).slice();
		if (Atomics.load(words, 0) !== session)
			return; // a new log started meanwhile: the next read starts it over
		codes.version = version;
		for (let i = 0; i < entries.length; i += 2) {
			const id = `${entries[i]}:${entries[i + 1]}`;
			if (!codes.usedSet.has(id)) {
				codes.usedSet.add(id);
				codes.used.push([entries[i], entries[i + 1]]);
				codes.dirty = true;
			}
		}
		codes.read = count;
	}

	// Stores this session's blocks, in the order they were compiled, then the stored ones of the same version it
	// did not compile, up to CODE_CAPACITY.
	function codeSave() {
		codeRead();
		if (!codes.dirty || !codes.serial || !codes.version)
			return;
		const serial = codes.serial;
		const version = codes.version;
		const list = codes.used.slice(0, CODE_CAPACITY);
		const old = codes.stored;
		if (old && old.version[0] === version[0] && old.version[1] === version[1]) {
			const ob = new Uint32Array(old.entries);
			for (let i = 0; i + 1 < ob.length && list.length < CODE_CAPACITY; i += 2) {
				if (!codes.usedSet.has(`${ob[i]}:${ob[i + 1]}`))
					list.push([ob[i], ob[i + 1]]);
			}
		}
		const out = new Uint32Array(list.length * 2);
		list.forEach(([pc, hash], i) => { out[2 * i] = pc; out[2 * i + 1] = hash; });
		const rec = { version, entries: out.buffer, savedAt: Date.now() };
		codes.dirty = false;
		codes.stored = rec;
		codes.lastSave = performance.now();
		// One write after another, so that an older list never lands after a newer one.
		codes.writes = codes.writes.then(() => codeIdb('readwrite', (s) => s.put(rec, serial)))
			.catch((err) => codeWarn('store the list', err));
	}

	function codeTick() {
		if (codes.handOver)
			codeHandOver();
		const now = performance.now();
		if (!codes.serial || now - codes.lastRead < CODE_READ_MS)
			return;
		codes.lastRead = now;
		codeRead();
		if (codes.dirty && now - codes.lastSave >= CODE_SAVE_MS)
			codeSave();
	}

	// The VM stopped: keep what it compiled, and ignore its log from now on (the next VM starts a new session).
	function codeVmStopped() {
		const M = window.Module;
		const buf = heapBuffer();
		if (codes.serial)
			codeSave();
		if (buf && M && typeof M._pcsx2web_ee_code_log === 'function')
			codes.staleSession = Atomics.load(new Uint32Array(buf, M._pcsx2web_ee_code_log() >>> 0, 1), 0);
		codes.serial = '';
	}

	// ---- the slate, the disc menu

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
				m.fine = [[b ? DISC_COPY.withBios(biosTitle(b.name)) + ' ' : '', ...memcardParts()]];
			} else if (disc.remembered && !disc.dead) {
				m.fine = [[DISC_COPY.lastDisc, { name: disc.remembered.name }, { sep: 1 }, { discLink: DISC_COPY.openAgain }]];
			}
			break;
		case 'queued':
			if (d)
				m.body = discLine(d);
			if (disc.checking && disc.checkSlow)
				m.title = [DISC_COPY.checking];
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
		// The empty tray opens the library, or in a running game the disc picker (insertWhileRunning).
		const tray = discEls.tray;
		const pick = !d && swapping();
		if (tray && tray.dataset.pick !== String(pick)) {
			tray.dataset.pick = String(pick);
			tray.setAttribute('aria-label', pick ? 'Insert a disc' : 'Disc library');
			if (pick)
				tray.removeAttribute('aria-haspopup');
			else
				tray.setAttribute('aria-haspopup', 'dialog');
		}
		if (!d || page.phase === 'dead' || page.phase === 'unsupported') {
			hideDiscMenu();
			return;
		}
		setText(discEls.btnName, discDisplayTitle(d));
		setText(discEls.menuName, d.name);
		rich(discEls.menuWhere, [...discSizeParts(d.size), { sep: 1 }, discWhere(d)]);
		// While a game runs the disc can change (swapDisc) but the library, which boots, waits for Stop.
		const locked = !canChangeDisc();
		const libraryLocked = !canChangeBios();
		for (const item of [discEls.menuChoose, discEls.menuLibrary, discEls.menuRemove]) {
			if (item === discEls.menuLibrary ? libraryLocked : locked) {
				item.setAttribute('aria-disabled', 'true');
				item.setAttribute('aria-describedby', 'disc-menu-reason');
			} else {
				item.removeAttribute('aria-disabled');
				item.removeAttribute('aria-describedby');
			}
		}
		setText(discEls.menuReason, locked ? DISC_COPY.menuReason : DISC_COPY.libraryWhileRunning);
		discEls.menuReason.hidden = !libraryLocked;
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
		// Earlier versions kept the title VMManager makes up for a disc its database does not know; the file
		// name reads better.
		if (typeof rec.title !== 'string' || rec.title === 'Unknown Game' || / \[\?\]$/.test(rec.title))
			rec.title = '';
		disc.remembered = rec;
		pageLog(`Last disc: ${rec.name} (reopens with one press).`);
		render();
	}

	// ---- memory cards (UI-SPEC.md §14)
	// A library of named cards in IndexedDB (CARD_IDB_NAME): 'cards' holds each card's record, 'bytes' its 8 MB
	// image, 'meta' which card is in which slot and which was last in each. A card in a slot is also a file in
	// /memcards (MEMFS), named after its id, which PCSX2 opens once pcsx2web_memcard() names it. Only those two are
	// loaded into the emulator. A card can be linked to a .ps2 file on disk (File System Access, Chromium): it is
	// read from the file when plugged in, and written back to it after each copy to IndexedDB.
	// One tab owns the cards: the one holding the MEMCARD_LOCK Web Lock, for as long as it is open. Only the owner
	// writes the library and the files, so a second tab can never overwrite the first one's saves with its older
	// cards; it loads the cards in the slots for its visit, and its card menus are disabled.
	// PCSX2 keeps a card's file open and writes it in place (unbuffered on wasm), so the copy is driven from here:
	// once the writes have been quiet for MEMCARD_QUIET_MS, at once when the PS2 stops, when a card is unplugged
	// and when the page is hidden. The copy is asynchronous and cannot finish during an unload, so from the first
	// write until the copy is done, leaving or reloading the page asks first (beforeunload).
	// A copy that fails is tried again later (MEMCARD_RETRY_MS; the copy on leaving does not wait, and is not
	// counted); after the last try the page gives up until the game writes to a card again, and the card line says
	// that saves last for this visit.

	const MEMCARD_DIR = '/memcards';
	const MEMCARD_QUIET_MS = 1500;
	const MEMCARD_RETRY_MS = [2000, 5000, 15000]; // after the 1st, 2nd and 3rd failed copy in a row
	const MEMCARD_LOCK = 'tabstationtoo-memcards';
	const MEMCARD_LOCK_WAIT_MS = 2000; // on a reload, the page being replaced may hold the lock a moment longer
	const CARD_SLOTS = 2;
	const CARD_BYTES = 8650752; // 8 MB of 512-byte pages, each with 16 bytes of ECC: PCSX2's .ps2 file
	const CARD_NAME_MAX = 32;
	const CARD_IDB_NAME = 'pcsx2-web-cards';
	const CARD_META_KEY = 'slots';
	const CARD_WRITE_DOT_MS = 600;
	const CARD_ACK_POLL_MS = 30;
	// Earlier versions kept slot 1's card in Emscripten's IDBFS: database '/memcards', store 'FILE_DATA', keyed
	// by path. It becomes "Memory card 1" once, then that database is deleted.
	const OLD_CARD_IDB = { name: '/memcards', store: 'FILE_DATA', key: '/memcards/Mcd001.ps2' };
	const MEMCARD_STATUS_BUSY = 1; // pcsx2web_memcard_status() flags
	const CARD_PICKER_TYPES = [{ description: 'PS2 memory card', accept: { 'application/octet-stream': ['.ps2'] } }];

	const CARD_COPY = {
		slot: (n) => `Card ${n}`,
		empty: (n) => `Card ${n} · empty`,
		saving: (n) => `Card ${n} · saving…`,
		btnName: (n, name) => name ? `Memory card ${n}: ${name}, plugged in` : `Memory card ${n}: empty`,
		emptyLine: (n) => `Card ${n} is empty.`,
		newTitle: (n) => `A new card, for Card ${n}.`,
		renameTitle: (name) => `Rename ${name}.`,
		inBrowser: 'In this browser',
		linked: 'Linked to ',
		onDisk: ' on disk',
		unlinked: 'Not linked any more',
		inOther: (n) => `In Card ${n}`,
		defaultName: (n) => `Memory card ${n}`,
		inserted: (name, n) => `${name} is in Card ${n}.`,
		unplugged: (n) => `Card ${n} unplugged.`,
		waitSave: (n) => `Card ${n} will unplug when the game finishes saving.`,
		waitReason: 'Unplugs when the game finishes saving.',
		deleted: (name) => `${name} deleted.`,
		removed: (name) => `${name} removed from the list. Its file stays on disk.`,
		renamed: (name) => `Renamed to ${name}.`,
		confirmDelete: (name) => `Delete ${name}? Its saves are only in this browser.`,
		nameEmpty: 'Give the card a name.',
		nameTaken: 'Another card is called that already.',
		notCard: 'That file isn’t an 8 MB PS2 memory card (.ps2).',
		unreadable: 'Couldn’t read that file. Try choosing it again.',
		denied: (file) => `The browser didn’t allow opening ${file}.`,
		noRoom: 'The browser has no room for another card. Delete one or save a copy to disk.',
		saveFailed: 'Couldn’t save the card here. Try again.',
		copyFailed: 'Couldn’t save the copy. Try again.',
		alreadyListed: (file, name) => `${file} is in the list already, as ${name}.`,
		inOtherSlot: (n) => `That card is in Card ${n}.`,
		unplugFirst: (n) => `Unplug it from Card ${n} first.`,
		writeFailed: (file, name) => `Couldn’t write ${file}. ${name} goes on in this browser.`,
		otherTab: 'TabStationToo is open in another tab.',
		noCard: 'No memory card in. Games won’t be able to save. ',
		insert: 'Insert a card',
		savesTo: (parts) => `Saves go to ${parts}.`,
		inSlot: (name, n) => `${name} in Card ${n}`,
	};

	const memcards = {
		mounted: false, persistent: false, failed: false, owner: false, otherTab: false,
		changedAt: 0, syncing: false, again: false, warned: false, guarded: false,
		// Writes are counted (FS.write on a card file): what the game wrote since the last copy is unsaved.
		writes: 0, savedWrites: 0, dirty: new Set(), loading: false,
		failures: 0, retryAt: 0, gaveUp: false, visitOnly: false,
	};

	// The library. slots[i] / last[i]: card ids or null. boot[i]: the bytes of the card a slot holds at start-up,
	// until the runtime's file system takes them. pending[i]: 'unplug' while an unplug waits for a save.
	const cards = {
		ready: null, loaded: false, list: new Map(), slots: [null, null], last: [null, null],
		boot: [null, null], fsReady: false, pending: [null, null], reqSeq: 0, statusPtr: 0,
		writeDot: [0, 0], dotTimer: [0, 0], busyOp: [false, false], persistAsked: false,
	};

	const cardFile = (id) => `${id}.ps2`;
	const cardPath = (id) => `${MEMCARD_DIR}/${cardFile(id)}`;
	const hasFileAccess = () => typeof window.showSaveFilePicker === 'function' && typeof window.showOpenFilePicker === 'function';
	const NEW_CARD_TO_FILE = false; // createCard(): link a new card to a file chosen in a save dialog

	function newCardId() {
		const bytes = new Uint8Array(6);
		crypto.getRandomValues(bytes);
		return 'c' + Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
	}

	function blankCard() {
		return new Uint8Array(CARD_BYTES).fill(0xff);
	}

	function slotOf(id) {
		const i = cards.slots.indexOf(id);
		return i < 0 ? -1 : i;
	}

	// The cards, the last one used in this slot first, then the most recently used.
	function cardsForPicker(slot) {
		const last = cards.last[slot];
		return [...cards.list.values()].sort((a, b) => (b.id === last) - (a.id === last) || (b.used || 0) - (a.used || 0) ||
			a.name.localeCompare(b.name));
	}

	function cardNameProblem(name, exceptId) {
		if (!name)
			return CARD_COPY.nameEmpty;
		const lower = name.toLocaleLowerCase();
		for (const c of cards.list.values()) {
			if (c.id !== exceptId && c.name.toLocaleLowerCase() === lower)
				return CARD_COPY.nameTaken;
		}
		return '';
	}

	function cleanCardName(name) {
		return String(name || '').replace(/\s+/g, ' ').trim().slice(0, CARD_NAME_MAX).trim();
	}

	function freeCardName(base) {
		base = cleanCardName(base) || CARD_COPY.defaultName(1);
		if (!cardNameProblem(base))
			return base;
		for (let n = 2; ; n++) {
			const suffix = ` (${n})`;
			const name = base.slice(0, CARD_NAME_MAX - suffix.length).trim() + suffix;
			if (!cardNameProblem(name))
				return name;
		}
	}

	function defaultCardName() {
		for (let n = 1; ; n++) {
			if (!cardNameProblem(CARD_COPY.defaultName(n)))
				return CARD_COPY.defaultName(n);
		}
	}

	// ---- the library in IndexedDB

	let cardDb = null;
	function cardIdbOpen() {
		if (cardDb)
			return cardDb;
		cardDb = new Promise((resolve, reject) => {
			if (typeof indexedDB === 'undefined') {
				reject(new Error('IndexedDB is not available'));
				return;
			}
			const req = indexedDB.open(CARD_IDB_NAME, 1);
			req.onupgradeneeded = () => {
				const db = req.result;
				db.createObjectStore('cards', { keyPath: 'id' });
				db.createObjectStore('bytes');
				db.createObjectStore('meta');
			};
			req.onsuccess = () => resolve(req.result);
			req.onerror = () => reject(req.error);
			req.onblocked = () => reject(new Error('IndexedDB is blocked by another tab'));
		});
		cardDb.catch(() => {
			cardDb = null;
		});
		return cardDb;
	}

	// fn(tx) queues requests; resolves with fn's result once the transaction is committed.
	async function cardTx(stores, mode, fn) {
		const db = await cardIdbOpen();
		return new Promise((resolve, reject) => {
			const tx = db.transaction(stores, mode);
			let result;
			try {
				result = fn(tx);
			} catch (err) {
				tx.abort();
				reject(err);
				return;
			}
			tx.oncomplete = () => resolve(result instanceof IDBRequest ? result.result : result);
			tx.onerror = () => reject(tx.error);
			tx.onabort = () => reject(tx.error || new Error('The IndexedDB transaction was aborted'));
		});
	}

	function cardRecordForDb(c) {
		return { id: c.id, name: c.name, created: c.created, used: c.used || 0, handle: c.handle || null,
			file: c.file || null, unlinked: !!c.unlinked };
	}

	function saveCardMeta() {
		if (!memcards.owner || !memcards.persistent)
			return Promise.resolve();
		const meta = { slots: cards.slots.slice(), last: cards.last.slice() };
		return cardTx('meta', 'readwrite', (tx) => tx.objectStore('meta').put(meta, CARD_META_KEY))
			.catch((err) => pageLog(`Could not remember which cards are plugged in: ${errorText(err)}`, 'warn'));
	}

	function saveCardRecord(c) {
		if (!memcards.owner || !memcards.persistent)
			return Promise.resolve();
		return cardTx('cards', 'readwrite', (tx) => tx.objectStore('cards').put(cardRecordForDb(c)))
			.catch((err) => pageLog(`Could not save the card ${c.name}: ${errorText(err)}`, 'warn'));
	}

	// A new card: its record and bytes in one transaction (a quota error leaves neither).
	async function addCardToLibrary(c, bytes) {
		if (!memcards.persistent)
			c.visitBytes = bytes; // no IndexedDB: the card lasts for this visit, in memory
		if (memcards.owner && memcards.persistent) {
			await cardTx(['cards', 'bytes'], 'readwrite', (tx) => {
				tx.objectStore('cards').put(cardRecordForDb(c));
				tx.objectStore('bytes').put(bytes, c.id);
			});
		}
		cards.list.set(c.id, c);
		askPersistentStorage();
	}

	async function readCardBytes(id) {
		if (!memcards.persistent) {
			const c = cards.list.get(id);
			return c && c.visitBytes ? c.visitBytes : null;
		}
		const bytes = await cardTx('bytes', 'readonly', (tx) => tx.objectStore('bytes').get(id));
		if (!bytes)
			return null;
		const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
		return view.byteLength === CARD_BYTES ? view : null;
	}

	function askPersistentStorage() {
		if (cards.persistAsked || !navigator.storage || typeof navigator.storage.persist !== 'function')
			return;
		cards.persistAsked = true;
		navigator.storage.persist().then((ok) => pageLog(ok ? 'The browser keeps this site’s storage (persistent).' :
			'The browser may clear this site’s storage when space runs low; save a copy of each card to disk to be safe.', ok ? undefined : 'warn'))
			.catch(() => {});
	}

	function isQuotaError(err) {
		return !!err && (err.name === 'QuotaExceededError' || err.name === 'NS_ERROR_DOM_QUOTA_REACHED' ||
			(err.inner && err.inner.name === 'QuotaExceededError'));
	}

	// The card an earlier version kept in IDBFS. Resolves to its bytes, or null.
	function readOldCard() {
		return new Promise((resolve) => {
			let req;
			try {
				req = indexedDB.open(OLD_CARD_IDB.name);
			} catch {
				resolve(null);
				return;
			}
			let created = false;
			req.onupgradeneeded = (e) => {
				if (e.oldVersion === 0) {
					created = true; // there was none: do not leave an empty one behind
					req.transaction.abort();
				}
			};
			req.onerror = () => {
				if (created)
					indexedDB.deleteDatabase(OLD_CARD_IDB.name);
				resolve(null);
			};
			req.onsuccess = () => {
				const db = req.result;
				if (!db.objectStoreNames.contains(OLD_CARD_IDB.store)) {
					db.close();
					resolve(null);
					return;
				}
				const get = db.transaction(OLD_CARD_IDB.store, 'readonly').objectStore(OLD_CARD_IDB.store).get(OLD_CARD_IDB.key);
				get.onsuccess = () => {
					db.close();
					const rec = get.result;
					const contents = rec && rec.contents;
					resolve(contents && contents.byteLength === CARD_BYTES ? new Uint8Array(contents) : null);
				};
				get.onerror = () => {
					db.close();
					resolve(null);
				};
			};
		});
	}

	// First visit of this version: the old card, if any, becomes "Memory card 1" in Card 1.
	async function migrateOldCard() {
		const bytes = await readOldCard();
		const meta = { slots: [null, null], last: [null, null] };
		if (bytes) {
			const c = { id: newCardId(), name: CARD_COPY.defaultName(1), created: Date.now(), used: Date.now(), handle: null, file: null };
			meta.slots[0] = meta.last[0] = c.id;
			await cardTx(['cards', 'bytes', 'meta'], 'readwrite', (tx) => {
				tx.objectStore('cards').put(cardRecordForDb(c));
				tx.objectStore('bytes').put(bytes, c.id);
				tx.objectStore('meta').put(meta, CARD_META_KEY);
			});
			pageLog('The memory card from an earlier visit is now “Memory card 1”, in Card 1.');
		} else {
			await cardTx('meta', 'readwrite', (tx) => tx.objectStore('meta').put(meta, CARD_META_KEY));
		}
		if (bytes) {
			try {
				indexedDB.deleteDatabase(OLD_CARD_IDB.name);
			} catch {
				// it stays; nothing reads it again
			}
		}
		return meta;
	}

	// Resolves true once this tab owns the cards, false when another tab keeps them.
	function claimMemoryCards() {
		const locks = navigator.locks;
		if (!locks || typeof locks.request !== 'function')
			return Promise.resolve(true); // no Web Locks: every tab keeps its cards, as without this
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

	// At page start: the lock, then the library (after the lock: the page a reload replaced has made its last
	// copy by then), then the bytes of the cards in the slots.
	function loadCardLibrary() {
		if (cards.ready)
			return cards.ready;
		cards.ready = (async () => {
			const owner = await claimMemoryCards();
			memcards.owner = owner;
			memcards.otherTab = !owner;
			if (!owner)
				pageLog('TabStationToo is open in another tab, which keeps the memory cards: saves in this tab last for this visit only.', 'warn');
			let meta = null;
			try {
				meta = await cardTx('meta', 'readonly', (tx) => tx.objectStore('meta').get(CARD_META_KEY));
				memcards.persistent = true;
				if (!meta && owner)
					meta = await migrateOldCard();
				const records = await cardTx('cards', 'readonly', (tx) => tx.objectStore('cards').getAll());
				for (const r of records || [])
					cards.list.set(r.id, { ...r });
			} catch (err) {
				memcards.failed = true;
				memcards.persistent = false;
				pageLog(`Could not load the memory cards from this browser: ${errorText(err)}`, 'warn');
			}
			if (meta && Array.isArray(meta.slots)) {
				for (let i = 0; i < CARD_SLOTS; i++) {
					cards.slots[i] = cards.list.has(meta.slots[i]) ? meta.slots[i] : null;
					cards.last[i] = cards.list.has(meta.last && meta.last[i]) ? meta.last[i] : null;
				}
			}
			let changed = false;
			for (let i = 0; i < CARD_SLOTS; i++) {
				const id = cards.slots[i];
				if (!id)
					continue;
				const bytes = await bootCardBytes(cards.list.get(id));
				if (bytes) {
					cards.boot[i] = bytes;
				} else {
					cards.slots[i] = null; // a linked file not allowed again yet: the picker offers it first
					cards.last[i] = id;
					changed = true;
				}
			}
			if (changed)
				saveCardMeta();
			if (cards.list.size)
				askPersistentStorage();
			cards.loaded = true;
			const names = cards.slots.map((id, i) => id ? `Card ${i + 1}: ${cards.list.get(id).name}` : '').filter(Boolean);
			if (names.length)
				pageLog(`Memory cards: ${names.join(', ')}.`);
			render();
		})();
		return cards.ready;
	}

	// A plugged card's bytes at start-up: from its file when it is linked and the browser still allows it,
	// otherwise the browser's copy (a linked card without permission comes back unplugged: null).
	async function bootCardBytes(c) {
		try {
			if (c.handle && !c.unlinked) {
				if (memcards.owner) {
					if (typeof c.handle.queryPermission !== 'function' ||
						(await c.handle.queryPermission({ mode: 'readwrite' })) !== 'granted')
						return null;
					const bytes = await readLinkedFile(c);
					if (bytes) {
						storeBrowserCopy(c, bytes);
						return bytes;
					}
					return null;
				}
			}
			return await readCardBytes(c.id);
		} catch (err) {
			pageLog(`Could not load the card ${c.name}: ${errorText(err)}`, 'warn');
			return null;
		}
	}

	// A linked card's file: its bytes, or null (logged) when it is not a card or cannot be read.
	async function readLinkedFile(c) {
		const file = await c.handle.getFile();
		if (file.size !== CARD_BYTES) {
			pageLog(`${file.name} is ${formatSize(file.size)}, not an 8 MB memory card.`, 'warn');
			return null;
		}
		return new Uint8Array(await file.arrayBuffer());
	}

	function storeBrowserCopy(c, bytes) {
		if (!memcards.owner || !memcards.persistent)
			return Promise.resolve(true);
		return cardTx('bytes', 'readwrite', (tx) => tx.objectStore('bytes').put(bytes, c.id)).then(() => true, (err) => {
			pageLog(`Could not keep a copy of ${c.name} in this browser: ${errorText(err)}`, 'warn');
			return false;
		});
	}

	// ---- the emulator's side

	// Card writes reach the file system on this thread (Emscripten runs a pthread's file calls here), so they are
	// seen as they happen: the unload guard is up from the first write of a save, and the card's dot lights.
	function watchMemcardWrites(FS) {
		const write = FS.write;
		if (typeof write !== 'function' || write.memcardWatch)
			return;
		const watched = function (stream) {
			const result = write.apply(this, arguments);
			if (!memcards.loading && stream && typeof stream.path === 'string' && stream.path.startsWith(`${MEMCARD_DIR}/`))
				onMemcardWrite(stream.path);
			return result;
		};
		watched.memcardWatch = true;
		FS.write = watched;
	}

	function onMemcardWrite(path) {
		memcards.changedAt = performance.now();
		memcards.writes++;
		memcards.dirty.add(path);
		memcards.gaveUp = false; // a new save: try to keep it again
		if (memcards.owner && memcards.persistent)
			setMemcardGuard(true);
		for (let i = 0; i < CARD_SLOTS; i++) {
			if (cards.slots[i] && cardPath(cards.slots[i]) === path)
				lightWriteDot(i);
		}
	}

	function lightWriteDot(slot) {
		const ui = cardUi[slot];
		if (!ui)
			return;
		if (!ui.btn.classList.contains('writing'))
			ui.btn.classList.add('writing');
		clearTimeout(cards.dotTimer[slot]);
		cards.dotTimer[slot] = setTimeout(() => ui.btn.classList.remove('writing'), CARD_WRITE_DOT_MS);
	}

	function onBeforeUnloadWithCard(e) {
		persistMemoryCards(true);
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

	// Module.preRun: before main(). The cards in the slots go into /memcards once the library is loaded.
	function mountMemoryCards() {
		const M = window.Module;
		const FS = M.FS;
		try {
			FS.mkdir(MEMCARD_DIR);
		} catch {
			// exists
		}
		memcards.mounted = true;
		watchMemcardWrites(FS);
		M.addRunDependency('memcards');
		loadCardLibrary().then(() => {
			for (let i = 0; i < CARD_SLOTS; i++) {
				if (cards.slots[i] && cards.boot[i])
					writeCardFile(cards.slots[i], cards.boot[i]);
				cards.boot[i] = null;
			}
			cards.fsReady = true;
			M.removeRunDependency('memcards');
		});
	}

	function writeCardFile(id, bytes) {
		const FS = window.Module.FS;
		memcards.loading = true; // not a write of the game's
		try {
			FS.writeFile(cardPath(id), bytes);
		} finally {
			memcards.loading = false;
		}
	}

	function removeCardFile(id) {
		const path = cardPath(id);
		memcards.dirty.delete(path);
		try {
			window.Module.FS.unlink(path);
		} catch {
			// not there
		}
	}

	// The bytes of a plugged card as the game left them.
	function readCardFile(id) {
		try {
			return window.Module.FS.readFile(cardPath(id));
		} catch {
			return null;
		}
	}

	// Module.postRun: the exports are bound; PCSX2 learns which cards are in the slots (it applies them at boot).
	function onRuntimeReadyForCards() {
		if (!api)
			return;
		cards.statusPtr = window.Module._pcsx2web_memcard_status ? window.Module._pcsx2web_memcard_status() >>> 0 : 0;
		for (let i = 0; i < CARD_SLOTS; i++) {
			if (cards.slots[i])
				coreSetCard(i, cardFile(cards.slots[i]));
		}
	}

	function cardStatus() {
		const buf = heapBuffer();
		if (!buf || !cards.statusPtr || cards.statusPtr + 12 > buf.byteLength)
			return null;
		const words = new Uint32Array(buf, cards.statusPtr, 1 + CARD_SLOTS);
		return { busy: (Atomics.load(words, 0) & MEMCARD_STATUS_BUSY) !== 0, applied: [Atomics.load(words, 1), Atomics.load(words, 2)] };
	}

	// Names the slot's file for PCSX2 ('' empties it). Resolves once PCSX2 has applied it, or at once when no VM
	// runs (it is applied at the next boot, and no card file is open meanwhile).
	function coreSetCard(slot, file) {
		const M = window.Module;
		if (!api || !M || typeof M._pcsx2web_memcard !== 'function')
			return Promise.resolve();
		const id = ++cards.reqSeq;
		return new Promise((resolve) => {
			const send = () => {
				const r = M.ccall('pcsx2web_memcard', 'number', ['number', 'string', 'number'], [slot, file, id]);
				if (r === 1) {
					setTimeout(send, CARD_ACK_POLL_MS); // the CPU thread is reading the last request
					return;
				}
				if (r !== 0) {
					pageLog(`The emulator refused the memory card ${file || '(none)'} for slot ${slot + 1}.`, 'warn');
					resolve();
					return;
				}
				const wait = () => {
					const st = cardStatus();
					if (!vmLive() || !st || st.applied[slot] >= id) {
						resolve();
						return;
					}
					setTimeout(wait, CARD_ACK_POLL_MS);
				};
				wait();
			};
			send();
		});
	}

	function cardBusy() {
		const st = vmLive() ? cardStatus() : null;
		return !!(st && st.busy);
	}

	// ---- plugging in and out

	// Plugs card id into the slot. gesture: called from a click (a linked file may ask for permission). Resolves
	// to '' or an error line for the menu.
	async function plugCard(slot, id) {
		const c = cards.list.get(id);
		if (!c || cards.slots[slot] || slotOf(id) >= 0)
			return '';
		let bytes = null;
		try {
			if (c.handle && memcards.owner) {
				const perm = typeof c.handle.requestPermission === 'function' ?
					await c.handle.requestPermission({ mode: 'readwrite' }) : 'granted';
				if (perm !== 'granted')
					return CARD_COPY.denied(c.file || c.name);
				bytes = await readLinkedFile(c);
				if (!bytes)
					return CARD_COPY.notCard;
				if (c.unlinked) {
					c.unlinked = false;
					saveCardRecord(c);
				}
				storeBrowserCopy(c, bytes);
			} else {
				bytes = await readCardBytes(id);
			}
		} catch (err) {
			pageLog(`Could not load the card ${c.name}: ${errorText(err)}`, 'warn');
			return c.handle ? CARD_COPY.unreadable : CARD_COPY.saveFailed;
		}
		if (!bytes)
			return CARD_COPY.unreadable;
		if (cards.slots[slot] || slotOf(id) >= 0)
			return ''; // plugged meanwhile (a second click)
		cards.slots[slot] = id;
		cards.last[slot] = id;
		c.used = Date.now();
		saveCardRecord(c);
		saveCardMeta();
		if (cards.fsReady) {
			writeCardFile(id, bytes);
			coreSetCard(slot, cardFile(id));
		} else {
			cards.boot[slot] = bytes; // the runtime's file system takes it before main()
		}
		pageLog(`Card ${slot + 1}: ${c.name}${c.handle ? ` (${c.file})` : ''}.`);
		subtitle(CARD_COPY.inserted(c.name, slot + 1));
		announce(CARD_COPY.inserted(c.name, slot + 1));
		render();
		return '';
	}

	// Unplug: at once, or once the game has finished writing (PCSX2's busy time) so a save is never cut short.
	function requestUnplug(slot) {
		if (!cards.slots[slot] || cards.pending[slot])
			return;
		if (cardBusy()) {
			cards.pending[slot] = 'unplug';
			subtitle(CARD_COPY.waitSave(slot + 1));
			announce(CARD_COPY.waitSave(slot + 1));
			render();
			return;
		}
		unplugCard(slot);
	}

	async function unplugCard(slot) {
		const id = cards.slots[slot];
		if (!id || cards.busyOp[slot])
			return;
		cards.busyOp[slot] = true;
		cards.pending[slot] = 'unplug';
		render();
		await coreSetCard(slot, ''); // PCSX2 has closed the file (or no VM has it open)
		const c = cards.list.get(id);
		const bytes = cards.fsReady ? readCardFile(id) : cards.boot[slot];
		if (bytes && c && !memcards.persistent)
			c.visitBytes = bytes;
		if (bytes && c && memcards.owner && memcards.persistent) {
			const path = cardPath(id);
			if (memcards.dirty.has(path) || memcards.writes !== memcards.savedWrites) {
				if (!(await storeBrowserCopy(c, bytes)))
					subtitle(DISC_COPY.memcardFailed);
				writeLinked(c, bytes);
			}
		}
		if (cards.fsReady)
			removeCardFile(id);
		cards.boot[slot] = null;
		cards.slots[slot] = null;
		cards.last[slot] = id;
		cards.pending[slot] = null;
		cards.busyOp[slot] = false;
		saveCardMeta();
		if (!memcards.dirty.size)
			memcards.savedWrites = memcards.writes; // nothing left unsaved
		cardUi[slot] && cardUi[slot].btn.classList.remove('writing');
		subtitle(CARD_COPY.unplugged(slot + 1));
		announce(CARD_COPY.unplugged(slot + 1));
		render();
		checkMemoryCards(false);
	}

	// The tick: an unplug that waits for a save goes on once PCSX2 is no longer busy with the cards.
	function checkPendingUnplugs() {
		for (let i = 0; i < CARD_SLOTS; i++) {
			if (cards.pending[i] === 'unplug' && !cards.busyOp[i] && !cardBusy())
				unplugCard(i);
		}
	}

	// ---- copying the cards to IndexedDB (and linked files)

	function memcardsUnsaved() {
		return memcards.writes !== memcards.savedWrites || memcards.dirty.size > 0;
	}

	function checkMemoryCards(now) {
		checkPendingUnplugs();
		if (!memcards.persistent || !memcards.owner)
			return;
		const t = performance.now();
		if (memcards.gaveUp) {
			setMemcardGuard(memcards.syncing);
			return;
		}
		const unsaved = memcardsUnsaved();
		setMemcardGuard(unsaved || memcards.syncing);
		if (unsaved && t >= memcards.retryAt && (now || t - memcards.changedAt >= MEMCARD_QUIET_MS))
			persistMemoryCards();
	}

	// leaving: the page is being hidden or left. That copy does not wait for the retry pause (it may be the
	// last chance to keep the card), and its failure does not count toward giving up: a few quick tab switches
	// say nothing new about IndexedDB, so only the timed copies decide that.
	function persistMemoryCards(leaving = false) {
		if (!memcards.persistent || !memcards.owner || memcards.gaveUp || !cards.fsReady)
			return;
		if (memcards.syncing) {
			memcards.again = true;
			return;
		}
		if (!memcardsUnsaved()) {
			setMemcardGuard(false);
			return;
		}
		// Writes from now on go to the next copy (they may also reach this one).
		const writes = memcards.writes;
		const dirty = memcards.dirty;
		memcards.dirty = new Set();
		const copies = [];
		for (let i = 0; i < CARD_SLOTS; i++) {
			const id = cards.slots[i];
			if (id && dirty.has(cardPath(id))) {
				const bytes = readCardFile(id);
				if (bytes)
					copies.push({ c: cards.list.get(id), bytes });
			}
		}
		memcards.syncing = true;
		setMemcardGuard(true);
		cardTx('bytes', 'readwrite', (tx) => {
			for (const { c, bytes } of copies)
				tx.objectStore('bytes').put(bytes, c.id);
		}).then(() => null, (err) => err || new Error('IndexedDB failed')).then((err) => {
			memcards.syncing = false;
			if (err) {
				for (const path of dirty)
					memcards.dirty.add(path);
				memcardCopyFailed(err, !leaving);
			} else {
				memcards.savedWrites = writes;
				memcards.failures = 0;
				memcards.retryAt = 0;
				if (copies.length)
					pageLog(`Saved in this browser: ${copies.map((x) => x.c.name).join(', ')}.`);
				for (const { c, bytes } of copies)
					writeLinked(c, bytes);
				if (memcards.visitOnly) {
					memcards.visitOnly = false; // kept again: the card line says so
					render();
				}
			}
			const again = memcards.again && !err;
			memcards.again = false;
			if (again)
				persistMemoryCards();
			else // a write during the copy keeps the guard up until the next copy
				setMemcardGuard(!memcards.gaveUp && memcardsUnsaved());
		});
	}

	// Tried again after a pause that grows; after the last try, no more copies and no "Leave site?" question until
	// the game writes to a card again, and the card line and one subtitle say that saves last for this visit.
	function memcardCopyFailed(err, counted) {
		pageLog(`Could not save the memory card in this browser: ${errorText(err)}${counted ? '' : ' (copy on leaving; not counted)'}`, 'warn');
		if (!counted)
			return; // the next timed copy tries again, on the current pause
		memcards.failures++;
		if (memcards.failures <= MEMCARD_RETRY_MS.length) {
			memcards.retryAt = performance.now() + MEMCARD_RETRY_MS[memcards.failures - 1];
			return;
		}
		memcards.gaveUp = true;
		memcards.visitOnly = true;
		memcards.failures = 0;
		memcards.retryAt = 0;
		setMemcardGuard(false);
		pageLog('Stopped saving the memory cards in this browser until the game writes to one again.', 'warn');
		if (!memcards.warned) {
			memcards.warned = true;
			subtitle(DISC_COPY.memcardFailed);
		}
		render();
	}

	// A linked card's file gets each copy, the latest only, one write at a time. A file that cannot be written
	// unlinks the card: it goes on from the browser's copy, and one subtitle says so.
	function writeLinked(c, bytes) {
		if (!c || !c.handle || c.unlinked || !memcards.owner)
			return;
		c.diskNext = bytes;
		if (c.diskBusy)
			return;
		c.diskBusy = true;
		(async () => {
			while (c.diskNext) {
				const next = c.diskNext;
				c.diskNext = null;
				try {
					if (typeof c.handle.queryPermission === 'function' &&
						(await c.handle.queryPermission({ mode: 'readwrite' })) !== 'granted')
						throw new Error('permission to write it was withdrawn');
					const w = await c.handle.createWritable();
					await w.write(next);
					await w.close();
				} catch (err) {
					pageLog(`Could not write ${c.file}: ${errorText(err)}`, 'warn');
					c.unlinked = true;
					c.diskNext = null;
					saveCardRecord(c);
					subtitle(CARD_COPY.writeFailed(c.file, c.name), HINT_MS);
					render();
				}
			}
			c.diskBusy = false;
		})();
	}

	// ---- making, opening, copying, renaming and deleting cards

	// New card: linked to a file the user picks where the browser can, otherwise kept in this browser. Resolves
	// to '' (done or cancelled) or an error line.
	async function createCard(slot, name) {
		const c = { id: newCardId(), name, created: Date.now(), used: Date.now(), handle: null, file: null };
		const bytes = blankCard();
		// A new card is kept in this browser, with no save dialog first: the dialog cannot show in some places
		// (an embedded browser, Brave) and creating then hung on it. "Save a copy to disk…" makes a file.
		if (NEW_CARD_TO_FILE && hasFileAccess()) {
			let handle;
			try {
				handle = await window.showSaveFilePicker({ suggestedName: `${name}.ps2`, types: CARD_PICKER_TYPES });
			} catch (err) {
				if (err && err.name === 'AbortError')
					return null; // cancelled: the name stays in the field
				pageLog(`The save dialog failed: ${errorText(err)}`, 'warn');
				return CARD_COPY.saveFailed;
			}
			const same = await findLinked(handle);
			if (same)
				return CARD_COPY.alreadyListed(handle.name, same.name);
			try {
				const w = await handle.createWritable();
				await w.write(bytes);
				await w.close();
			} catch (err) {
				pageLog(`Could not write ${handle.name}: ${errorText(err)}`, 'warn');
				return CARD_COPY.saveFailed;
			}
			c.handle = handle;
			c.file = handle.name;
		}
		try {
			await addCardToLibrary(c, bytes);
		} catch (err) {
			pageLog(`Could not keep the new card in this browser: ${errorText(err)}`, 'warn');
			return isQuotaError(err) ? CARD_COPY.noRoom : CARD_COPY.saveFailed;
		}
		return (await plugCard(slot, c.id)) || '';
	}

	async function findLinked(handle) {
		for (const c of cards.list.values()) {
			try {
				if (c.handle && typeof c.handle.isSameEntry === 'function' && await c.handle.isSameEntry(handle))
					return c;
			} catch {
				// a handle the browser cannot compare any more
			}
		}
		return null;
	}

	// Open a card file: with File System Access the card is linked to it, otherwise its bytes are kept in this
	// browser. pickedFile: from the file input (no File System Access).
	async function openCardFile(slot, pickedFile) {
		let handle = null;
		let file = pickedFile;
		if (!file) {
			try {
				[handle] = await window.showOpenFilePicker({ types: CARD_PICKER_TYPES, multiple: false });
			} catch (err) {
				if (err && err.name === 'AbortError')
					return null; // cancelled: the menu stays
				pageLog(`The file picker failed: ${errorText(err)}`, 'warn');
				return CARD_COPY.unreadable;
			}
			const same = await findLinked(handle);
			if (same) {
				const other = slotOf(same.id);
				if (other >= 0)
					return other === slot ? '' : CARD_COPY.inOtherSlot(other + 1);
				return plugCard(slot, same.id);
			}
			try {
				file = await handle.getFile();
			} catch (err) {
				pageLog(`Could not read ${handle.name}: ${errorText(err)}`, 'warn');
				return CARD_COPY.unreadable;
			}
		}
		if (file.size !== CARD_BYTES)
			return CARD_COPY.notCard;
		let bytes;
		try {
			bytes = new Uint8Array(await file.arrayBuffer());
		} catch (err) {
			pageLog(`Could not read ${file.name}: ${errorText(err)}`, 'warn');
			return CARD_COPY.unreadable;
		}
		if (handle && typeof handle.requestPermission === 'function') {
			let perm = 'denied';
			try {
				perm = await handle.requestPermission({ mode: 'readwrite' });
			} catch (err) {
				pageLog(`The browser did not let the page write ${handle.name}: ${errorText(err)}`, 'warn');
			}
			if (perm !== 'granted')
				return CARD_COPY.denied(handle.name);
		}
		const c = { id: newCardId(), name: freeCardName(file.name.replace(/\.ps2$/i, '')), created: Date.now(), used: Date.now(),
			handle, file: handle ? handle.name : null };
		try {
			await addCardToLibrary(c, bytes);
		} catch (err) {
			pageLog(`Could not keep ${file.name} in this browser: ${errorText(err)}`, 'warn');
			return isQuotaError(err) ? CARD_COPY.noRoom : CARD_COPY.saveFailed;
		}
		return (await plugCard(slot, c.id)) || '';
	}

	// Save a copy to disk: a save dialog where the browser has one, otherwise a download. Resolves to true when
	// saved, false when cancelled, or an error line.
	async function saveCardCopy(id) {
		const c = cards.list.get(id);
		if (!c)
			return false;
		let bytes = slotOf(id) >= 0 && cards.fsReady ? readCardFile(id) : null;
		try {
			bytes = bytes || (slotOf(id) >= 0 ? cards.boot[slotOf(id)] : null) || await readCardBytes(id);
		} catch (err) {
			pageLog(`Could not read the card ${c.name}: ${errorText(err)}`, 'warn');
		}
		if (!bytes)
			return CARD_COPY.copyFailed;
		const name = `${c.name.replace(/[\\/:*?"<>|]+/g, '_')}.ps2`;
		if (typeof window.showSaveFilePicker === 'function') {
			try {
				const handle = await window.showSaveFilePicker({ suggestedName: name, types: CARD_PICKER_TYPES });
				const w = await handle.createWritable();
				await w.write(bytes);
				await w.close();
				pageLog(`Saved a copy of ${c.name} as ${handle.name}.`);
				return true;
			} catch (err) {
				if (err && err.name === 'AbortError')
					return false;
				pageLog(`Could not save a copy of ${c.name}: ${errorText(err)}`, 'warn');
				return CARD_COPY.copyFailed;
			}
		}
		const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
		const a = document.createElement('a');
		a.href = url;
		a.download = name;
		document.body.appendChild(a);
		a.click();
		a.remove();
		setTimeout(() => URL.revokeObjectURL(url), 60000);
		pageLog(`Downloading a copy of ${c.name} (${name}).`);
		return true;
	}

	function renameCard(id, name) {
		const c = cards.list.get(id);
		if (!c)
			return '';
		const problem = cardNameProblem(name, id);
		if (problem)
			return problem;
		if (c.name !== name) {
			c.name = name;
			saveCardRecord(c);
			subtitle(CARD_COPY.renamed(name));
			announce(CARD_COPY.renamed(name));
			render();
		}
		return '';
	}

	async function deleteCard(id) {
		const c = cards.list.get(id);
		if (!c || slotOf(id) >= 0)
			return '';
		try {
			if (memcards.owner && memcards.persistent) {
				await cardTx(['cards', 'bytes'], 'readwrite', (tx) => {
					tx.objectStore('cards').delete(id);
					tx.objectStore('bytes').delete(id);
				});
			}
		} catch (err) {
			pageLog(`Could not delete the card ${c.name}: ${errorText(err)}`, 'warn');
			return CARD_COPY.saveFailed;
		}
		cards.list.delete(id);
		let changed = false;
		for (let i = 0; i < CARD_SLOTS; i++) {
			if (cards.last[i] === id) {
				cards.last[i] = null;
				changed = true;
			}
		}
		if (changed)
			saveCardMeta();
		pageLog(`${c.handle ? 'Removed from the list' : 'Deleted'}: ${c.name}.`);
		const said = c.handle ? CARD_COPY.removed(c.name) : CARD_COPY.deleted(c.name);
		subtitle(said);
		announce(said);
		render();
		return '';
	}

	// ---- the card buttons and menus
	// Each slot has a menu button in the transport and a popover, both made from #card-template. The popover shows
	// one view at a time: 'plugged' (the card in the slot), 'picker' (an empty slot: the library), 'more' (one card
	// of the picker), 'name' (a new card's name, or a new name) and 'confirm' (deleting a browser card).

	const cardUi = [];

	function cardWhere(c) {
		if (!c.handle)
			return [CARD_COPY.inBrowser];
		if (c.unlinked)
			return [CARD_COPY.unlinked];
		return [CARD_COPY.linked, { name: c.file }];
	}

	function cardMenuOpen() {
		return cardUi.some((ui) => {
			try {
				return ui.menu.matches(':popover-open');
			} catch {
				return false;
			}
		});
	}

	function cardMenuIsOpen(ui) {
		try {
			return ui.menu.matches(':popover-open');
		} catch {
			return false;
		}
	}

	function hideCardMenu(ui) {
		if (cardMenuIsOpen(ui))
			ui.menu.hidePopover();
	}

	function openCardMenu(slot) {
		const ui = cardUi[slot];
		if (ui && !cardMenuIsOpen(ui) && ui.btn.getClientRects().length)
			ui.menu.showPopover();
	}

	function placeCardMenu(ui) {
		const r = ui.btn.getBoundingClientRect();
		const width = Math.min(296, window.innerWidth - 32);
		const left = Math.max(16, Math.min(r.left, window.innerWidth - width - 16));
		ui.menu.style.left = `${Math.round(left)}px`;
		ui.menu.style.bottom = `${Math.round(Math.max(8, window.innerHeight - r.top + 8))}px`;
	}

	// Cards can change only in the tab that keeps them.
	const cardsLocked = () => !memcards.owner || !cards.loaded;

	function setItemDisabled(item, disabled, reasonId) {
		if (disabled) {
			item.setAttribute('aria-disabled', 'true');
			if (reasonId)
				item.setAttribute('aria-describedby', reasonId);
			else
				item.removeAttribute('aria-describedby');
		} else {
			item.removeAttribute('aria-disabled');
			item.removeAttribute('aria-describedby');
		}
	}

	const isDisabled = (el) => el.getAttribute('aria-disabled') === 'true';

	function makeCardUi(slot) {
		const tpl = $('card-template').content;
		const n = slot + 1;
		const btn = tpl.querySelector('.card-btn').cloneNode(true);
		const menu = tpl.querySelector('.card-menu').cloneNode(true);
		menu.id = `card-menu-${n}`;
		btn.id = `card-btn-${n}`;
		btn.querySelector('.mc-num').textContent = String(n);
		btn.setAttribute('popovertarget', menu.id);
		// The first card (UI-SPEC §16): one click on an empty slot, with no card yet, makes one and plugs it
		// in. Otherwise the slot opens its menu (the picker).
		btn.addEventListener('click', (e) => {
			if (cards.slots[slot] || cards.list.size || cardsLocked() || !cards.loaded)
				return;
			e.preventDefault(); // no menu
			cardAction(cardUi[slot], () => createCard(slot, defaultCardName()));
		});
		btn.setAttribute('aria-controls', menu.id);
		menu.setAttribute('aria-label', `Memory card ${n}`);
		const q = (sel) => menu.querySelector(sel);
		const ui = {
			slot, btn, menu, view: 'plugged', target: null, nameMode: 'new', back: 'picker', error: '', working: false,
			icon: btn.querySelector('.card-icon use'),
			pre: btn.querySelector('.card-pre'), num: btn.querySelector('.card-num'), rest: btn.querySelector('.card-rest'),
			tip: btn.querySelector('.tip'),
			views: {
				plugged: q('[data-view="plugged"]'), picker: q('[data-view="picker"]'), more: q('[data-view="more"]'),
				name: q('[data-view="name"]'), confirm: q('[data-view="confirm"]'),
			},
			plugName: q('[data-view="plugged"] .menu-name'), plugWhere: q('[data-view="plugged"] .menu-where'),
			unplug: q('[data-act="unplug"]'), rename: q('[data-act="rename"]'), copy: q('[data-act="copy"]'),
			plugReason: q('[data-view="plugged"] .menu-reason'),
			emptyLine: q('[data-view="picker"] .menu-where'), list: q('.card-list'),
			create: q('[data-act="new"]'), open: q('[data-act="open"]'), pickReason: q('[data-view="picker"] .menu-reason'),
			moreName: q('[data-view="more"] .menu-name'), moreWhere: q('[data-view="more"] .menu-where'),
			moreRename: q('[data-act="more-rename"]'), moreCopy: q('[data-act="more-copy"]'), moreDelete: q('[data-act="more-delete"]'),
			moreDeleteLabel: q('[data-act="more-delete"] .label'), moreDeleteIcon: q('[data-act="more-delete"] use'),
			moreBack: q('[data-act="more-back"]'), moreReason: q('[data-view="more"] .menu-reason'),
			form: q('[data-view="name"]'), formTitle: q('.card-form-title'), nameLabel: q('[data-view="name"] label'), nameInput: q('[data-view="name"] input'),
			nameOk: q('[data-act="name-ok"]'), nameCancel: q('[data-act="name-cancel"]'),
			confirmQ: q('[data-view="confirm"] p'), keep: q('[data-act="keep"]'), copyFirst: q('[data-act="copy-first"]'),
			deleteYes: q('[data-act="delete-yes"]'),
			errorLine: q('.card-error'),
			listKey: '',
		};
		// Ids for the labels and descriptions inside this slot's menu.
		for (const [el, id] of [[ui.plugReason, 'plug-reason'], [ui.pickReason, 'pick-reason'], [ui.moreReason, 'more-reason'],
			[ui.nameInput, 'name'], [ui.confirmQ, 'confirm-q'], [ui.errorLine, 'error']])
			el.id = `card-${n}-${id}`;
		ui.nameLabel.htmlFor = ui.nameInput.id;
		ui.views.confirm.setAttribute('aria-labelledby', ui.confirmQ.id);
		ui.nameInput.maxLength = CARD_NAME_MAX;
		ui.nameInput.setAttribute('aria-describedby', ui.errorLine.id);
		$(`bar-slot-${n}`).append(btn); // the card plugs into its slot on the console's front (UI-SPEC §16)
		discEls.menu.before(menu);
		wireCardUi(ui);
		return ui;
	}

	function setCardView(ui, view, focus = true) {
		ui.view = view;
		ui.error = '';
		renderCardMenu(ui);
		if (focus)
			focusCardView(ui);
	}

	function focusCardView(ui) {
		const v = ui.views[ui.view];
		let el = null;
		if (ui.view === 'name')
			el = ui.nameInput;
		else if (ui.view === 'confirm')
			el = ui.keep;
		else
			el = [...v.querySelectorAll('.menu-item')].find((b) => !isDisabled(b) && b.getClientRects().length) ||
				v.querySelector('.menu-item');
		if (el) {
			el.focus({ preventScroll: true });
			if (el === ui.nameInput)
				el.select();
		}
	}

	function showCardError(ui, text) {
		ui.error = text || '';
		renderCardMenu(ui);
		if (text)
			announce(text);
	}

	// Runs an action of the menu once at a time; a result that is a string is the menu's error line.
	async function cardAction(ui, fn, closeOnDone = true) {
		if (ui.working)
			return;
		ui.working = true;
		let result;
		try {
			result = await fn();
		} catch (err) {
			pageLog(`Memory card: ${errorText(err)}`, 'warn');
			result = CARD_COPY.saveFailed;
		}
		ui.working = false;
		if (typeof result === 'string' && result) {
			if (!cardMenuIsOpen(ui))
				openCardMenu(ui.slot);
			showCardError(ui, result);
			return result;
		}
		if (closeOnDone && result !== null && result !== false) {
			hideCardMenu(ui);
			ui.btn.focus({ preventScroll: true });
		} else {
			renderCardMenu(ui);
		}
		return result;
	}

	function renderCardButton(ui) {
		const n = ui.slot + 1;
		const id = cards.slots[ui.slot];
		const c = id ? cards.list.get(id) : null;
		const saving = !!c && cards.pending[ui.slot] === 'unplug';
		const href = icon(c ? 'sim-card-fill' : 'sim-card');
		if (ui.icon.getAttribute('href') !== href)
			ui.icon.setAttribute('href', href);
		ui.btn.classList.toggle('empty', !c);
		setText(ui.pre, c ? '' : 'Card ');
		setText(ui.num, String(n));
		setText(ui.rest, saving ? ' · saving…' : c ? ` · ${c.name}` : ' · empty');
		const label = saving ? CARD_COPY.saving(n) : c ? `${n} · ${c.name}` : CARD_COPY.empty(n);
		setText(ui.tip, saving ? CARD_COPY.saving(n) : c ? `Card ${n}: ${c.name}` : CARD_COPY.empty(n));
		const name = saving ? `${CARD_COPY.btnName(n, c.name)}, ${CARD_COPY.waitReason.toLowerCase()}` : CARD_COPY.btnName(n, c ? c.name : '');
		if (ui.btn.getAttribute('aria-label') !== name)
			ui.btn.setAttribute('aria-label', name);
		ui.btn.dataset.label = label;
		setText(ui.btn.querySelector('.mc-label'), c ? c.name : '');
	}

	function renderCardMenu(ui) {
		if (!cardMenuIsOpen(ui))
			return;
		const slot = ui.slot;
		const id = cards.slots[slot];
		const plugged = id ? cards.list.get(id) : null;
		const locked = cardsLocked();
		// The slot changed under a root view, or the card of a view went away: back to the slot's root view.
		if ((ui.view === 'plugged' && !plugged) || (ui.view === 'picker' && plugged) ||
			(['more', 'confirm'].includes(ui.view) && (!cards.list.has(ui.target) || slotOf(ui.target) >= 0 && ui.view === 'confirm')) ||
			(ui.view === 'name' && ui.nameMode === 'rename' && !cards.list.has(ui.target))) {
			const hadFocus = ui.menu.contains(document.activeElement);
			ui.view = plugged ? 'plugged' : 'picker';
			if (hadFocus)
				queueMicrotask(() => focusCardView(ui));
		}
		for (const [name, el] of Object.entries(ui.views))
			el.hidden = name !== ui.view;
		const lockedReason = locked ? CARD_COPY.otherTab : '';

		if (ui.view === 'plugged' && plugged) {
			setText(ui.plugName, plugged.name);
			rich(ui.plugWhere, cardWhere(plugged));
			const waiting = cards.pending[slot] === 'unplug';
			setItemDisabled(ui.unplug, locked || waiting, ui.plugReason.id);
			setItemDisabled(ui.rename, locked, ui.plugReason.id);
			setItemDisabled(ui.copy, false);
			setText(ui.plugReason, lockedReason || (waiting ? CARD_COPY.waitReason : ''));
			ui.plugReason.hidden = !ui.plugReason.textContent;
		} else if (ui.view === 'picker') {
			setText(ui.emptyLine, CARD_COPY.emptyLine(slot + 1));
			renderCardList(ui, locked);
			setItemDisabled(ui.create, locked, ui.pickReason.id);
			setItemDisabled(ui.open, locked, ui.pickReason.id);
			setText(ui.pickReason, lockedReason);
			ui.pickReason.hidden = !lockedReason;
		} else if (ui.view === 'more') {
			const c = cards.list.get(ui.target);
			const other = slotOf(c.id);
			setText(ui.moreName, c.name);
			rich(ui.moreWhere, other >= 0 ? [CARD_COPY.inOther(other + 1)] : cardWhere(c));
			setText(ui.moreDeleteLabel, c.handle ? 'Remove from list' : 'Delete');
			const href = icon(c.handle ? 'x' : 'trash');
			if (ui.moreDeleteIcon.getAttribute('href') !== href)
				ui.moreDeleteIcon.setAttribute('href', href);
			setItemDisabled(ui.moreRename, locked, ui.moreReason.id);
			setItemDisabled(ui.moreCopy, false);
			setItemDisabled(ui.moreDelete, locked || other >= 0, ui.moreReason.id);
			const reason = lockedReason || (other >= 0 ? CARD_COPY.unplugFirst(other + 1) : '');
			setText(ui.moreReason, reason);
			ui.moreReason.hidden = !reason;
		} else if (ui.view === 'name') {
			setText(ui.nameOk, ui.nameMode === 'new' ? 'Create' : 'Rename');
			const c = ui.nameMode === 'rename' ? cards.list.get(ui.target) : null;
			setText(ui.formTitle, c ? CARD_COPY.renameTitle(c.name) : CARD_COPY.newTitle(slot + 1));
			ui.nameInput.setAttribute('aria-invalid', ui.error ? 'true' : 'false');
		} else if (ui.view === 'confirm') {
			const c = cards.list.get(ui.target);
			setText(ui.confirmQ, CARD_COPY.confirmDelete(c.name));
		}
		setText(ui.errorLine, ui.error);
		ui.errorLine.hidden = !ui.error;
	}

	// The picker's rows, rebuilt only when what they show changes (focus stays on the same card).
	function renderCardList(ui, locked) {
		const rows = cardsForPicker(ui.slot).map((c) => {
			const other = slotOf(c.id);
			return { id: c.id, name: c.name, where: other >= 0 ? [CARD_COPY.inOther(other + 1)] : c.handle && !c.unlinked ? [{ name: c.file }, CARD_COPY.onDisk] : cardWhere(c),
				disabled: locked || other >= 0 };
		});
		const key = JSON.stringify(rows);
		if (key === ui.listKey)
			return;
		ui.listKey = key;
		const a = document.activeElement;
		const focusId = a && ui.list.contains(a) ? a.closest('.card-row').dataset.id : null;
		const focusMore = !!(a && a.classList.contains('card-more'));
		ui.list.textContent = '';
		ui.list.hidden = rows.length === 0;
		for (const r of rows) {
			const row = document.createElement('div');
			row.className = 'card-row';
			row.dataset.id = r.id;
			const pick = document.createElement('button');
			pick.type = 'button';
			pick.className = 'menu-item card-pick';
			pick.appendChild(svgIcon('sim-card', 'icon'));
			const text = span('card-pick-text', '');
			text.appendChild(span('card-pick-name', r.name));
			const where = span('card-pick-where', '');
			rich(where, r.where);
			text.appendChild(where);
			pick.appendChild(text);
			if (r.disabled)
				setItemDisabled(pick, true, locked ? ui.pickReason.id : null);
			pick.addEventListener('click', () => {
				if (isDisabled(pick))
					return;
				cardAction(ui, () => plugCard(ui.slot, r.id));
			});
			const more = document.createElement('button');
			more.type = 'button';
			more.className = 'btn-icon card-more';
			more.setAttribute('aria-label', `More for ${r.name}`);
			more.appendChild(svgIcon('dots-three', 'icon'));
			more.addEventListener('click', () => {
				ui.target = r.id;
				setCardView(ui, 'more');
			});
			row.append(pick, more);
			ui.list.appendChild(row);
			if (r.id === focusId)
				queueMicrotask(() => (focusMore ? more : pick).focus({ preventScroll: true }));
		}
	}

	function startNameForm(ui, mode, id, back) {
		ui.nameMode = mode;
		ui.target = id;
		ui.back = back;
		ui.nameInput.value = mode === 'new' ? defaultCardName() : cards.list.get(id).name;
		setCardView(ui, 'name');
	}

	async function submitNameForm(ui) {
		const name = cleanCardName(ui.nameInput.value);
		ui.nameInput.value = name;
		if (ui.nameMode === 'rename') {
			const problem = renameCard(ui.target, name);
			if (problem) {
				showCardError(ui, problem);
				ui.nameInput.focus({ preventScroll: true });
				return;
			}
			setCardView(ui, ui.back);
			return;
		}
		const problem = cardNameProblem(name);
		if (problem) {
			showCardError(ui, problem);
			ui.nameInput.focus({ preventScroll: true });
			return;
		}
		const result = await cardAction(ui, () => createCard(ui.slot, name));
		if (result === null && cardMenuIsOpen(ui))
			ui.nameInput.focus({ preventScroll: true }); // the save dialog was cancelled
	}

	function wireCardUi(ui) {
		const { menu, btn } = ui;
		menu.addEventListener('beforetoggle', (e) => {
			if (e.newState === 'open') {
				ui.view = cards.slots[ui.slot] ? 'plugged' : 'picker';
				ui.error = '';
				ui.listKey = '';
				placeCardMenu(ui);
			}
		});
		menu.addEventListener('toggle', (e) => {
			const open = e.newState === 'open';
			btn.setAttribute('aria-expanded', open ? 'true' : 'false');
			if (open) {
				renderCardMenu(ui);
				focusCardView(ui);
			} else {
				const a = document.activeElement;
				if (!a || a === document.body || menu.contains(a))
					btn.focus({ preventScroll: true });
			}
			updateChrome();
		});
		menu.addEventListener('focusout', (e) => {
			const to = e.relatedTarget;
			if (to && !menu.contains(to) && to !== btn)
				hideCardMenu(ui);
		});
		window.addEventListener('resize', () => {
			if (cardMenuIsOpen(ui))
				placeCardMenu(ui);
		});
		const on = (el, fn) => el.addEventListener('click', (e) => {
			if (!isDisabled(el))
				fn(e);
		});
		on(ui.unplug, () => {
			requestUnplug(ui.slot);
			if (cards.pending[ui.slot] === 'unplug' && !cards.busyOp[ui.slot]) {
				renderCardMenu(ui); // waits for the save: the menu says so
				focusCardView(ui);
				return;
			}
			hideCardMenu(ui);
			btn.focus({ preventScroll: true });
		});
		on(ui.rename, () => startNameForm(ui, 'rename', cards.slots[ui.slot], 'plugged'));
		on(ui.copy, () => cardAction(ui, () => saveCardCopy(cards.slots[ui.slot]), false));
		on(ui.create, () => startNameForm(ui, 'new', null, 'picker'));
		on(ui.open, () => {
			if (hasFileAccess()) {
				cardAction(ui, () => openCardFile(ui.slot, null));
				return;
			}
			const input = $('card-input');
			input.dataset.slot = String(ui.slot);
			input.click();
		});
		on(ui.moreRename, () => startNameForm(ui, 'rename', ui.target, 'more'));
		on(ui.moreCopy, () => cardAction(ui, () => saveCardCopy(ui.target), false));
		on(ui.moreDelete, () => {
			const c = cards.list.get(ui.target);
			if (!c)
				return;
			if (c.handle) {
				cardAction(ui, () => deleteCard(c.id), false).then(() => setCardView(ui, 'picker'));
				return;
			}
			setCardView(ui, 'confirm');
		});
		on(ui.moreBack, () => setCardView(ui, 'picker'));
		ui.form.addEventListener('submit', (e) => {
			e.preventDefault();
			submitNameForm(ui);
		});
		ui.nameCancel.addEventListener('click', () => setCardView(ui, ui.back));
		ui.keep.addEventListener('click', () => setCardView(ui, 'more'));
		ui.copyFirst.addEventListener('click', async () => {
			await cardAction(ui, () => saveCardCopy(ui.target), false);
			if (cardMenuIsOpen(ui) && ui.view === 'confirm')
				ui.keep.focus({ preventScroll: true });
		});
		ui.deleteYes.addEventListener('click', () => {
			const id = ui.target;
			cardAction(ui, () => deleteCard(id), false).then(() => {
				if (cardMenuIsOpen(ui))
					setCardView(ui, 'picker');
			});
		});
	}

	function renderCards() {
		setData('cards', cards.loaded ? 'ready' : 'loading');
		for (const ui of cardUi) {
			renderCardButton(ui);
			renderCardMenu(ui);
		}
	}

	// The memory-card line of the disc title card's fine print.
	function memcardParts() {
		if (memcards.otherTab)
			return [DISC_COPY.savesOtherTab];
		if (memcards.failed || memcards.visitOnly)
			return [DISC_COPY.savesVisit];
		if (!cards.loaded)
			return [];
		const inSlots = cards.slots.map((id, i) => id ? CARD_COPY.inSlot(cards.list.get(id).name, i + 1) : null).filter(Boolean);
		if (!inSlots.length)
			return [CARD_COPY.noCard, { cardLink: CARD_COPY.insert }];
		return [CARD_COPY.savesTo(inSlots.join(' and '))];
	}

	function wireCardsUi() {
		for (let i = 0; i < CARD_SLOTS; i++)
			cardUi.push(makeCardUi(i));
		const input = $('card-input');
		input.addEventListener('change', () => {
			const file = input.files && input.files[0];
			const slot = Number(input.dataset.slot) || 0;
			input.value = '';
			if (file)
				cardAction(cardUi[slot], () => openCardFile(slot, file));
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
				discEls.menuLibrary.focus({ preventScroll: true });
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
		window.addEventListener('pagehide', () => {
			persistMemoryCards(true);
			pipeSave();
		});
		document.addEventListener('visibilitychange', () => {
			if (document.visibilityState === 'hidden') {
				persistMemoryCards(true);
				pipeSave();
			}
		});
	}

	// The disc library (library/player-library.js, UI-SPEC.md §15) chooses a disc from a folder and boots
	// it while its insert plays over the page.
	// Set by the library (onPad): where a gamepad's presses go while its dialog is open (libraryPadFrame).
	let libraryPadHandler = null;

	window.PCSX2WebLibrary = Object.freeze({
		// The empty tray of a running game: a disc goes in without stopping it (swapDisc). True if handled.
		insertWhileRunning() {
			if (!swapping() || !canChangeDisc() || disc.current)
				return false;
			openDiscPicker();
			return true;
		},
		// Why a disc can't go in now, or '' when it can.
		blocked() {
			if (page.phase === 'dead' || page.phase === 'unsupported' || disc.dead)
				return DISC_COPY.libraryUnavailable;
			if (!page.bios && !page.urlBiosName)
				return DISC_COPY.libraryNoBios;
			if (!canChangeBios())
				return DISC_COPY.blocked;
			return '';
		},
		// Inside the user's press: sound may start, the disc opens, and its boot is queued, as a disc
		// chosen and Boot pressed while it opens. hold: the boot waits for release() (the library's disc is
		// being cleaned first: the game must not start under it).
		insert(file, handle, hold) {
			audioUnlock();
			hideDiscMenu();
			pageLog(`Disc library: ${file.name}`);
			page.autoBoot = !hold;
			page.bootHeld = !!hold;
			page.litWait = true;
			page.lit = false;
			chooseDisc({ kind: 'file', file, name: file.name, size: file.size }, handle || null);
			ensureRuntime();
		},
		// The held boot goes.
		release() {
			if (!page.bootHeld)
				return;
			page.bootHeld = false;
			page.autoBoot = true;
			maybeAutoBoot();
			render();
		},
		// What the library waits on: the page state, and the error the slate would show.
		status: () => ({
			view: pageState(),
			lit: !!page.lit,
			error: page.error || '',
			busy: !!(disc.opening || disc.checking || (page.autoBoot && page.phase !== 'ready')),
		}),
		// The dialog has gone: the picture (or the slate) takes the keyboard.
		focusScreen() {
			page.litWait = false;
			if (pageState() === 'running' || pageState() === 'starting')
				els.screen.focus({ preventScroll: true });
			else
				focusPrimary();
		},
		// Whether the library may be opened now (its buttons are aria-disabled otherwise). It is the page's home: it
		// opens without a BIOS too, and asks for one first.
		canOpen: () => canChangeBios(),
		// The BIOS, for the library's first step: whether there is one, one being read, and why the last failed.
		bios: () => ({ ok: !!page.bios || !!page.urlBiosName, name: page.bios ? page.bios.name : page.urlBiosName || '', reading: !!page.biosReading,
			error: pageState() === 'nobios' && !page.errorIsDisc ? page.error || '' : '' }),
		// Inside the user's press: the BIOS file picker.
		chooseBios: () => openPicker(),
		// A file dropped on the library's BIOS step (a disc image there goes to the disc, as on the slate).
		takeBios: (file) => acceptFile(file, null),
		// Eject while a game runs: the PS2 stops, and the library comes back once it has.
		stop: () => stop(),
		// A line in the subtitle, for a library button that can't open it now (Eject while the PS2 runs).
		say: (text) => subtitle(text),
		// The library's reader of gamepad presses (libraryPadFrame): { step: -1 | 1 } or { confirm: true }.
		onPad: (fn) => { libraryPadHandler = fn; },
	});

	// Read-only hooks for automated tests, next to window.PCSX2Web.
	window.PCSX2WebDisc = Object.freeze({
		// Automated runs: a same-origin disc URL, opened as ?disc= opens one (while a game runs, it is swapped in),
		// and Remove the disc. Both are what the disc menu does.
		load(url) {
			if (canChangeDisc())
				loadDiscFromUrl(url);
		},
		remove: () => removeDisc(),
		swapping: () => disc.swapping,
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
			game: { title: disc.runTitle, serial: disc.runSerial, disc: disc.runIsDisc, trayEmpty: disc.trayEmpty },
			lastDisc: disc.remembered ? disc.remembered.name : null,
			memcards: { persistent: memcards.persistent, owner: memcards.owner, otherTab: memcards.otherTab, failed: memcards.failed,
				loaded: cards.loaded, syncing: memcards.syncing, unloadGuard: memcards.guarded, writes: memcards.writes,
				savedWrites: memcards.savedWrites, failures: memcards.failures, gaveUp: memcards.gaveUp, visitOnly: memcards.visitOnly,
				slots: cards.slots.map((id) => id ? cards.list.get(id).name : null), pending: cards.pending.slice(),
				library: [...cards.list.values()].map((c) => ({ name: c.name, file: c.file, unlinked: !!c.unlinked })) },
		}),
	});

	// ------------------------------------------------------------------ audio (UI-SPEC.md §11)
	// The PS2's sound (docs/web-port/AUDIO-DESIGN.md). pcsx2web_audio_start() loads the emulator's module into the
	// AudioContext's worklet scope and makes one node for the page's lifetime. Its callback reads PCSX2's audio stream
	// on the browser's audio thread; the CPU thread attaches the stream while the context renders and drains it
	// otherwise. This thread owns only the context (made in a user gesture, at 48 kHz), the gain (volume and mute) and
	// what the Sound control shows: no sample passes through it.

	const AUDIO_RATE = 48000;
	const AUDIO_GAIN_TIME = 0.015; // s, time constant of gain changes, so they never click
	const AUDIO_STOP_FADE = 0.03; // s, the fade before the context is suspended after Stop
	const AUDIO_HELD_AFTER_MS = 300; // a resume() still pending this long means the browser holds the sound
	const PREF_VOLUME = 'pcsx2web.volume';
	const PREF_MUTED = 'pcsx2web.muted';
	const AUDIO_TAP_FILE = 'audio-tap.js';
	const AUDIO_TAP_NAME = 'tabstationtoo-tap';
	// pcsx2web_audio(): the audio block's words (AUDIO-DESIGN.md 4.5).
	const AUDIO_BLOCK = ['magic', 'version', 'worklet', 'streamRate', 'attached', 'heartbeat', 'frames', 'shortQuanta',
		'drained', 'contextRate', 'bufferMs', 'fill', 'attaches', 'steps', 'streams', 'reserved'];
	const AUDIO_BLOCK_MAGIC = 0x44554154; // "TAUD"
	const AUDIO_WORKLET_STATES = ['none', 'starting', 'running', 'failed'];

	const SOUND_COPY = {
		percent: (v) => `${v}%`,
		valueText: (v) => `${v} percent`,
		// The button's accessible name: its label, then the state.
		name: {
			on: (v) => `Sound, ${v} percent`,
			muted: 'Sound, muted',
			held: 'Sound, waiting for a key or click',
			unavailable: 'Sound, unavailable',
		},
		off: 'Sound off.',
		on: 'Sound on.',
		volume: (v) => `Volume ${v}%.`,
		heldHint: 'Press a key or click to turn the sound on.',
		heldNote: 'The browser holds the sound until you press a key or click.',
		unavailable: 'This browser can’t play the emulator’s sound.',
	};

	const soundEls = {
		btn: $('btn-sound'),
		icon: $('btn-sound-icon'),
		menu: $('sound-menu'),
		volume: $('sound-volume'),
		value: $('sound-value'),
		mute: $('sound-mute'),
		note: $('sound-note'),
	};

	// Test switches (UI-SPEC.md §11): ?mute=1, ?audio=null, ?audiotap=SECONDS, ?audiorate=HZ (the context's rate,
	// to exercise the emulator's resampling as PS1 mode would; 24000 to 384000, what it accepts for 48 kHz).
	const audioTapSeconds = (() => {
		const v = Number(params.get('audiotap'));
		return Number.isFinite(v) && v > 0 ? Math.min(v, 3600) : 0;
	})();
	const audioRate = (() => {
		const v = Number(params.get('audiorate'));
		return Number.isInteger(v) && v >= 24000 && v <= 384000 ? v : AUDIO_RATE;
	})();

	const audio = {
		off: params.get('audio') === 'null', // the emulator runs with PCSX2's Null backend: no sound at all
		testMute: params.get('mute') === '1',
		ctx: null,
		gain: null,
		node: null,
		tap: null,
		tapReady: null, // the tap's addModule() promise
		blockPtr: 0,
		volume: 100,
		muted: false,
		wanted: false, // queued, starting or running: sound should play
		started: false, // pcsx2web_audio_start() was called
		startAt: 0,
		failed: '', // why sound is unavailable
		resumeAt: 0, // performance.now() of the last resume() asked for
		resumeTried: false, // the one resume() without a gesture after the browser suspended the context
		suspendTimer: 0,
		renderTimer: 0,
		firstFrameSaid: false, // the held or unavailable subtitle, once per visit
		heldHintToken: 0, // the held subtitle's token while it may still show
		capture: { blocks: [], frames: 0, stored: 0, dropped: 0, next: -1, gaps: 0, firstAt: -1 },
	};

	function audioSupported() {
		return typeof AudioContext === 'function' && typeof AudioWorkletNode === 'function' && !!window.isSecureContext;
	}

	// Calls arriving from the emulator's callbacks (inside wasm) must never throw back into it.
	function audioGuard(fn) {
		return (...args) => {
			try {
				fn(...args);
			} catch (err) {
				pageLog(`Sound: ${errorText(err)}`, 'warn');
			}
		};
	}

	function audioGainTarget() {
		return audio.muted || audio.volume === 0 ? 0 : (audio.volume / 100) ** 2;
	}

	function applyAudioGain() {
		if (!audio.gain)
			return;
		const g = audio.gain.gain;
		const t = audio.ctx.currentTime;
		g.cancelScheduledValues(t);
		g.setTargetAtTime(audio.testMute ? 0 : audioGainTarget(), t, AUDIO_GAIN_TIME);
	}

	function audioFail(why) {
		if (audio.failed)
			return;
		audio.failed = why;
		pageLog(`Sound is unavailable: ${why}`, 'warn');
		if (audio.node) {
			try {
				audio.node.disconnect();
			} catch {
				// already disconnected
			}
		}
		renderSound();
		if (pageState() === 'running')
			audioFirstFrameHint();
	}

	// The test switch ?audio=null: main() keeps PCSX2's Null backend (Module.arguments).
	function audioModuleArguments() {
		return audio.off ? ['-audio-null'] : [];
	}

	// Inside a user gesture (Boot, a queued boot's press), or for a boot without one (?bios=): makes the context
	// once and asks it to run. A context made without a gesture stays suspended until the first key or click.
	function audioUnlock() {
		if (audio.off || audio.failed)
			return;
		if (!audio.ctx) {
			if (!audioSupported()) {
				audioFail('this browser has no AudioWorklet (or the page is not a secure context).');
				return;
			}
			try {
				audio.ctx = new AudioContext({ sampleRate: audioRate, latencyHint: 'interactive' });
			} catch (err) {
				pageLog(`Sound: no ${audioRate} Hz AudioContext (${errorText(err)}); using the device's rate.`, 'warn');
				try {
					audio.ctx = new AudioContext({ latencyHint: 'interactive' });
				} catch (err2) {
					audioFail(`the AudioContext could not be made (${errorText(err2)}).`);
					return;
				}
			}
			audio.gain = audio.ctx.createGain();
			audio.gain.gain.value = audio.testMute ? 0 : audioGainTarget();
			audio.gain.connect(audio.ctx.destination);
			audio.ctx.addEventListener('statechange', onAudioStateChange);
			if (audioTapSeconds) {
				audio.tapReady = audio.ctx.audioWorklet.addModule(AUDIO_TAP_FILE).then(() => true, (err) => {
					pageLog(`Sound: the test tap did not load (${errorText(err)}).`, 'warn');
					return false;
				});
			}
			pageLog(`Sound: AudioContext at ${audio.ctx.sampleRate} Hz, ${audio.ctx.state}.`);
			audioStart();
		}
		audioResume();
	}

	function audioResume() {
		const ctx = audio.ctx;
		if (!ctx || ctx.state === 'running' || ctx.state === 'closed')
			return;
		clearTimeout(audio.suspendTimer);
		audio.suspendTimer = 0;
		audio.resumeAt = performance.now();
		ctx.resume().catch(() => {}); // stays pending while the browser holds the sound
		clearTimeout(audio.renderTimer);
		audio.renderTimer = setTimeout(renderSound, AUDIO_HELD_AFTER_MS + 20);
	}

	// Once per page, once the runtime is ready and a context exists.
	function audioStart() {
		if (audio.started || audio.failed || !audio.ctx || !api)
			return;
		const M = window.Module;
		if (typeof M._pcsx2web_audio_start !== 'function' || typeof M._pcsx2web_audio !== 'function') {
			audioFail('this build of the emulator has no audio exports.');
			return;
		}
		audio.started = true;
		audio.startAt = performance.now();
		audio.blockPtr = M._pcsx2web_audio() >>> 0;
		M.pcsx2webAudioContext = audio.ctx;
		M.pcsx2webAudioReady = audioGuard(onAudioNode);
		let result;
		try {
			result = M._pcsx2web_audio_start();
		} catch (err) {
			audioFail(`pcsx2web_audio_start failed (${errorText(err)}).`);
			return;
		}
		if (result !== 0)
			audioFail(`pcsx2web_audio_start returned ${result}.`);
	}

	// From the emulator, once its worklet node exists (or null when it could not start).
	function onAudioNode(node) {
		if (!node) {
			audioFail('the audio worklet did not start.');
			return;
		}
		audio.node = node;
		node.addEventListener('processorerror', () => {
			markAudioWorkletFailed();
			audioFail('the audio worklet stopped with an error.');
		});
		pageLog(`Sound: worklet running ${Math.round(performance.now() - audio.startAt)} ms after its start.`);
		if (audio.tapReady)
			audio.tapReady.then(audioGuard(connectAudioTap));
		else
			node.connect(audio.gain);
		renderSound();
	}

	// ?audiotap: the worklet's output goes through a recording tap (audio-tap.js) before the gain.
	function connectAudioTap(loaded) {
		if (!loaded || audio.failed) {
			if (!audio.failed)
				audio.node.connect(audio.gain);
			return;
		}
		audio.tap = new AudioWorkletNode(audio.ctx, AUDIO_TAP_NAME, {
			numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], channelCount: 2, channelCountMode: 'explicit',
		});
		audio.tap.port.onmessage = (e) => onAudioTapBlock(e.data);
		audio.node.connect(audio.tap);
		audio.tap.connect(audio.gain);
		pageLog(`Sound: test tap recording up to ${audioTapSeconds} s.`);
	}

	function onAudioTapBlock(b) {
		const c = audio.capture;
		if (c.next >= 0 && b.at !== c.next)
			c.gaps++;
		c.next = b.at + b.l.length;
		c.frames += b.l.length;
		if (c.firstAt < 0)
			c.firstAt = b.at;
		if (c.stored + b.l.length > audioTapSeconds * audio.ctx.sampleRate) {
			c.dropped += b.l.length;
			return;
		}
		c.blocks.push(b);
		c.stored += b.l.length;
	}

	// The recorded frames since the last call, interleaved float32 little-endian, as base64.
	function takeAudioCapture() {
		const c = audio.capture;
		const frames = c.blocks.reduce((n, b) => n + b.l.length, 0);
		const out = new Float32Array(frames * 2);
		let i = 0;
		for (const b of c.blocks) {
			for (let k = 0; k < b.l.length; k++) {
				out[i++] = b.l[k];
				out[i++] = b.r[k];
			}
		}
		const firstAt = c.blocks.length ? c.blocks[0].at : -1;
		c.blocks = [];
		const bytes = new Uint8Array(out.buffer);
		let bin = '';
		for (let k = 0; k < bytes.length; k += 0x8000)
			bin += String.fromCharCode.apply(null, bytes.subarray(k, k + 0x8000));
		return { frames, firstAt, rate: audio.ctx ? audio.ctx.sampleRate : 0, data: btoa(bin),
			total: c.frames, dropped: c.dropped, gaps: c.gaps };
	}

	function readAudioBlock() {
		const p = audio.blockPtr;
		const buf = p ? heapBuffer() : null;
		if (!buf || p + AUDIO_BLOCK.length * 4 > buf.byteLength)
			return null;
		const view = new Uint32Array(buf, p, AUDIO_BLOCK.length);
		if (Atomics.load(view, 0) !== AUDIO_BLOCK_MAGIC)
			return null;
		const out = {};
		AUDIO_BLOCK.forEach((name, i) => { out[name] = Atomics.load(view, i); });
		out.worklet = AUDIO_WORKLET_STATES[out.worklet] || String(out.worklet);
		return out;
	}

	// A worklet that threw inside its callback never runs again, and may have left the emulator's "in the callback"
	// flag set: the failed state lets the CPU thread stop waiting for it (WebAudioStream::Detach()).
	function markAudioWorkletFailed() {
		const p = audio.blockPtr;
		const buf = p ? heapBuffer() : null;
		if (!buf || p + AUDIO_BLOCK.length * 4 > buf.byteLength)
			return;
		const view = new Uint32Array(buf, p, AUDIO_BLOCK.length);
		if (Atomics.load(view, 0) === AUDIO_BLOCK_MAGIC)
			Atomics.store(view, AUDIO_BLOCK.indexOf('worklet'), AUDIO_WORKLET_STATES.indexOf('failed'));
	}

	// Called from render(): sound plays while the PS2 is queued, starting or running. Otherwise the gain fades and the
	// context is suspended, so a stopped page costs no audio work; the next Boot resumes it.
	function audioSync(state) {
		const wanted = state === 'queued' || state === 'starting' || state === 'running';
		if (wanted === audio.wanted)
			return;
		audio.wanted = wanted;
		if (!audio.ctx)
			return;
		if (wanted) {
			applyAudioGain();
			audioResume();
			return;
		}
		audioFadeOut();
		clearTimeout(audio.suspendTimer);
		audio.suspendTimer = setTimeout(() => {
			audio.suspendTimer = 0;
			if (!audio.wanted && audio.ctx.state === 'running')
				audio.ctx.suspend().catch(() => {});
		}, AUDIO_STOP_FADE * 1000 + 20);
	}

	function audioFadeOut() {
		if (!audio.gain)
			return;
		const g = audio.gain.gain;
		const t = audio.ctx.currentTime;
		g.cancelScheduledValues(t);
		g.setValueAtTime(g.value, t);
		g.linearRampToValueAtTime(0, t + AUDIO_STOP_FADE);
	}

	function onAudioStateChange() {
		const s = audio.ctx.state;
		pageLog(`Sound: the AudioContext is ${s}.`);
		if (s === 'running') {
			audio.resumeTried = false;
			// The sound is on: the hint asking for a key or click has done its job.
			if (audio.heldHintToken && audio.heldHintToken === subtitleToken && subtitleOpen)
				endSubtitle();
			audio.heldHintToken = 0;
		} else if (audio.wanted && !audio.resumeTried && s !== 'closed') {
			// Suspended or interrupted by the browser: one try now; the next key press or click tries again.
			audio.resumeTried = true;
			audioResume();
		}
		renderSound();
	}

	// Capture phase, before anything else sees the event; it is never consumed.
	function onAudioGesture() {
		if (audio.wanted && audio.ctx && audio.ctx.state !== 'running')
			audioResume();
	}

	function soundView() {
		if (audio.off || audio.failed)
			return 'unavailable';
		if (audio.wanted && audio.ctx && audio.ctx.state !== 'running' &&
			performance.now() - audio.resumeAt >= AUDIO_HELD_AFTER_MS)
			return 'held';
		return audio.muted || audio.volume === 0 ? 'muted' : 'on';
	}

	function renderSound() {
		const view = soundView();
		const iconName = view === 'held' || view === 'unavailable' ? 'speaker-slash' : view === 'muted' ? 'speaker-x' :
			audio.volume >= 50 ? 'speaker-high' : 'speaker-low';
		if (soundEls.icon.getAttribute('href') !== icon(iconName))
			soundEls.icon.setAttribute('href', icon(iconName));
		const name = view === 'on' || (view === 'muted' && !audio.muted) ? SOUND_COPY.name.on(audio.volume) : SOUND_COPY.name[view];
		if (soundEls.btn.getAttribute('aria-label') !== name)
			soundEls.btn.setAttribute('aria-label', name);
		soundEls.mute.setAttribute('aria-pressed', audio.muted ? 'true' : 'false');
		$('remote-mute').setAttribute('aria-pressed', audio.muted ? 'true' : 'false');
		if (Number(soundEls.volume.value) !== audio.volume)
			soundEls.volume.value = String(audio.volume);
		soundEls.volume.setAttribute('aria-valuetext', SOUND_COPY.valueText(audio.volume));
		soundEls.volume.style.setProperty('--fill', `${audio.volume}%`);
		setText(soundEls.value, SOUND_COPY.percent(audio.volume));
		const note = view === 'held' ? SOUND_COPY.heldNote : view === 'unavailable' && !audio.off ? SOUND_COPY.unavailable : '';
		setText(soundEls.note, note);
		soundEls.note.hidden = !note;
		setData('sound', view);
	}

	// With the first frame: the browser holds the sound, or the page cannot play it. Returns true when it said so
	// (the first-boot hint then waits for the next boot).
	function audioFirstFrameHint() {
		if (audio.firstFrameSaid || audio.off)
			return false;
		const view = soundView();
		const held = audio.wanted && audio.ctx && audio.ctx.state !== 'running' && !audio.muted;
		if (view !== 'unavailable' && !held)
			return false;
		audio.firstFrameSaid = true;
		subtitle(view === 'unavailable' ? SOUND_COPY.unavailable : SOUND_COPY.heldHint, HINT_MS);
		audio.heldHintToken = view === 'unavailable' ? 0 : subtitleToken;
		return true;
	}

	function setVolume(v) {
		audio.volume = Math.max(0, Math.min(100, Math.round(v / 5) * 5));
		if (audio.muted) {
			audio.muted = false; // moving the slider unmutes
			writePref(PREF_MUTED, '0');
		}
		applyAudioGain();
		renderSound();
	}

	function setMuted(muted, say) {
		audio.muted = muted;
		writePref(PREF_MUTED, muted ? '1' : '0');
		applyAudioGain();
		renderSound();
		if (say)
			subtitle(muted ? SOUND_COPY.off : SOUND_COPY.on);
	}

	// M: the key labelled M on any layout, unless it is one of the pad's keys there.
	function isMuteKey(e) {
		return (e.key === 'm' || e.key === 'M') && !e.shiftKey && !e.altKey && !PAD_CODES.has(e.code);
	}

	function soundMenuOpen() {
		try {
			return soundEls.menu.matches(':popover-open');
		} catch {
			return false;
		}
	}

	function hideSoundMenu() {
		if (soundMenuOpen())
			soundEls.menu.hidePopover();
	}

	// Opens upward, right-aligned to its trigger.
	function placeSoundMenu() {
		const r = soundEls.btn.getBoundingClientRect();
		const width = Math.min(296, window.innerWidth - 32);
		const right = Math.max(16, Math.min(window.innerWidth - r.right, window.innerWidth - width - 16));
		soundEls.menu.style.right = `${Math.round(right)}px`;
		soundEls.menu.style.bottom = `${Math.round(Math.max(8, window.innerHeight - r.top + 8))}px`;
	}

	function loadSoundPrefs() {
		const v = Number(readPref(PREF_VOLUME));
		audio.volume = readPref(PREF_VOLUME) !== null && Number.isFinite(v) ? Math.max(0, Math.min(100, Math.round(v / 5) * 5)) : 100;
		audio.muted = readPref(PREF_MUTED) === '1';
	}

	function wireSoundUi() {
		loadSoundPrefs();
		// Touch grants the activation on release, so pointerup too.
		for (const type of ['keydown', 'pointerdown', 'pointerup'])
			window.addEventListener(type, onAudioGesture, true);

		soundEls.volume.addEventListener('input', () => setVolume(Number(soundEls.volume.value)));
		soundEls.volume.addEventListener('change', () => writePref(PREF_VOLUME, String(audio.volume)));
		soundEls.mute.addEventListener('click', () => setMuted(!audio.muted, false));
		// The remote under the console (UI-SPEC §16): volume in steps of 10, and mute.
		const stepVolume = (d) => {
			setVolume(audio.volume + d);
			writePref(PREF_VOLUME, String(audio.volume));
			subtitle(SOUND_COPY.volume(audio.volume));
		};
		$('remote-vol-down').addEventListener('click', () => stepVolume(-10));
		$('remote-vol-up').addEventListener('click', () => stepVolume(10));
		$('remote-mute').addEventListener('click', () => setMuted(!audio.muted, true));

		soundEls.menu.addEventListener('beforetoggle', (e) => {
			if (e.newState === 'open') {
				renderSound();
				placeSoundMenu();
			}
		});
		soundEls.menu.addEventListener('toggle', (e) => {
			const open = e.newState === 'open';
			soundEls.btn.setAttribute('aria-expanded', open ? 'true' : 'false');
			if (open) {
				soundEls.volume.focus({ preventScroll: true });
			} else {
				const a = document.activeElement;
				if (!a || a === document.body || soundEls.menu.contains(a))
					soundEls.btn.focus({ preventScroll: true });
			}
			updateChrome();
		});
		// Like the BIOS menu: Tab (or anything else) taking focus out of it closes it.
		soundEls.menu.addEventListener('focusout', (e) => {
			const to = e.relatedTarget;
			if (to && !soundEls.menu.contains(to) && to !== soundEls.btn)
				hideSoundMenu();
		});
		window.addEventListener('resize', () => {
			if (soundMenuOpen())
				placeSoundMenu();
		});
		document.addEventListener('fullscreenchange', () => {
			if (soundMenuOpen())
				placeSoundMenu();
		});
		renderSound();
	}

	// Hooks for automated tests, next to window.PCSX2Web: stats() only reads. With ?audiotap, takeCapture() hands over
	// (and so removes) what the tap recorded, and interrupt() suspends the context as the browser would.
	window.PCSX2WebAudio = Object.freeze({
		stats: () => ({
			supported: audioSupported(),
			off: audio.off,
			failed: audio.failed,
			view: soundView(),
			wanted: audio.wanted,
			volume: audio.volume,
			muted: audio.muted,
			testMute: audio.testMute,
			gainTarget: audioGainTarget(),
			gain: audio.gain ? audio.gain.gain.value : null,
			context: audio.ctx ? { state: audio.ctx.state, sampleRate: audio.ctx.sampleRate, currentTime: audio.ctx.currentTime,
				baseLatency: audio.ctx.baseLatency, outputLatency: audio.ctx.outputLatency } : null,
			node: !!audio.node,
			tap: audio.tap ? { frames: audio.capture.frames, stored: audio.capture.stored, dropped: audio.capture.dropped,
				gaps: audio.capture.gaps, firstAt: audio.capture.firstAt } : null,
			block: readAudioBlock(),
		}),
		takeCapture: () => (audio.tap ? takeAudioCapture() : null),
		// The browser suspending the context by itself (an interruption), for tests of the recovery.
		interrupt: () => (audioTapSeconds && audio.ctx ? audio.ctx.suspend().then(() => audio.ctx.state) : null),
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
		if (els.biosMenu.contains(a) || discEls.menu.contains(a) || soundEls.menu.contains(a) || els.about.contains(a) ||
			cardUi.some((ui) => ui.menu.contains(a)))
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
			action = () => {
				if (page.drawer !== 'keys') {
					page.keysPort = players.keyboard || 1;
					showPortControls();
				}
				toggleDrawer('keys');
			};
		} else if ((e.code === 'F1' || e.code === 'F3') && !e.shiftKey && inShortcutScope() && page.vmState === STATE.RUNNING) {
			const load = e.code === 'F3';
			action = () => stateShortcut(load);
		} else if (e.code === 'Backquote' && !e.shiftKey && inShortcutScope()) {
			action = () => toggleDrawer('log');
		} else if (isMuteKey(e) && (inShortcutScope() || soundEls.menu.contains(document.activeElement))) {
			action = () => setMuted(!audio.muted, true);
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
		for (const b of document.querySelectorAll(':is(.t-btn, .card-btn):is(:hover, :focus-visible)'))
			b.classList.add('tip-off');
	}

	// Esc: native dialog/popover close first, then a drawer, then the keyboard.
	function onEscape(e) {
		if (els.about.open || menuOpen() || discMenuOpen() || cardMenuOpen() || soundMenuOpen() || gfxMenuOpen() || eeMenuOpen() || stateMenuOpen())
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
			setKeyboardPad(name, true);
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
		setKeyboardPad(name, false);
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
		releaseKeyboardPad();
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

	// ------------------------------------------------------------------ players: keyboard and gamepads

	// The PS2 pad's inputs, in the order of the emulator's table (checkPadInputs verifies it).
	const PAD_INPUTS = ['Up', 'Right', 'Down', 'Left', 'Triangle', 'Circle', 'Cross', 'Square', 'Select', 'Start',
		'L1', 'L2', 'R1', 'R2', 'L3', 'R3', 'Analog', 'LUp', 'LRight', 'LDown', 'LLeft', 'RUp', 'RRight', 'RDown', 'RLeft'];
	const PAD_INDEX = Object.fromEntries(PAD_INPUTS.map((name, i) => [name, i]));
	// Host key name (KEY_NAMES) -> the pad input it plays, as it comes. The player changes it on the controllers
	// under the console (keyBinds, saved); KEY_PAD is what plays now. The Keys drawer shows the same layout.
	const DEFAULT_KEY_PAD = {
		Up: 'Up', Down: 'Down', Left: 'Left', Right: 'Right', Return: 'Start', Backspace: 'Select',
		Q: 'L1', E: 'R1', 1: 'L2', 3: 'R2', 2: 'L3', 4: 'R3',
		W: 'LUp', A: 'LLeft', S: 'LDown', D: 'LRight', T: 'RUp', F: 'RLeft', G: 'RDown', H: 'RRight',
		I: 'Triangle', J: 'Square', K: 'Cross', L: 'Circle',
	};
	const KEY_PAD = {};
	const PREF_KEY_BINDS = 'pcsx2web.keyBinds';
	const defaultKeyBinds = () => Object.fromEntries(Object.entries(DEFAULT_KEY_PAD).map(([key, input]) => [input, key]));
	// Pad input -> host key name, one key each. Saved whole, null for an input with no key.
	const keyBinds = (() => {
		const binds = defaultKeyBinds();
		try {
			const saved = JSON.parse(readPref(PREF_KEY_BINDS) || 'null');
			if (!saved || typeof saved !== 'object')
				return binds;
			const known = new Set(Object.values(KEY_NAMES));
			const taken = new Set();
			const out = {};
			for (const input of PAD_INPUTS) {
				const key = input in saved ? saved[input] : binds[input];
				if (typeof key === 'string' && known.has(key) && !taken.has(key)) {
					out[input] = key;
					taken.add(key);
				}
			}
			return out;
		} catch {
			return binds;
		}
	})();
	// Gamepad API "standard" mapping, by position: the bottom face button is Cross.
	const GAMEPAD_BUTTONS = { 0: 'Cross', 1: 'Circle', 2: 'Square', 3: 'Triangle', 4: 'L1', 5: 'R1', 8: 'Select',
		9: 'Start', 10: 'L3', 11: 'R3', 12: 'Up', 13: 'Down', 14: 'Left', 15: 'Right', 16: 'Analog' };
	const GAMEPAD_TRIGGERS = { 6: 'L2', 7: 'R2' };
	// The same layout as a mapping (readCustom's form), where a change to one control on a standard gamepad starts.
	const STANDARD_MAP = {
		buttons: Object.fromEntries(Object.entries({ ...GAMEPAD_BUTTONS, ...GAMEPAD_TRIGGERS }).map(([i, name]) => [name, { k: 'b', i: Number(i) }])),
		sticks: { L: { x: { i: 0, s: 1 }, y: { i: 1, s: 1 } }, R: { x: { i: 2, s: 1 }, y: { i: 3, s: 1 } } },
	};
	// Browsers pass sticks on as the hardware reports them, with the rest-position noise of worn
	// sticks; the PS2 pad has no dead zone of its own here. A round dead zone, rescaled so that the
	// full throw still reaches 1.
	const STICK_DEAD_ZONE = 0.1;
	const TRIGGER_DEAD_ZONE = 0.05;
	const STICK_STEPS = 127; // sticks change in steps of 1/127, so noise does not flood the queue
	const MAX_GAMEPADS = 2;
	// The Gamepad API has no button events: the page looks at the state. Once per animation frame can miss
	// a quick tap (and a busy page draws fewer frames than the display has), so it looks as often as the
	// browser runs a timer (an interval of 0: about every 4 ms, the floor browsers keep for repeating timers).
	// Every press and release seen goes to the PS2 as it is; the emulator holds each for at least two
	// vsyncs so the game sees it. Only the Gamepad tab's picture keeps a press for GAMEPAD_SHOW_MS, so a tap
	// shows there too: holding it for the PS2 would merge quick taps into one press when mashing.
	const GAMEPAD_POLL_MS = 0;
	const GAMEPAD_SHOW_MS = 50;
	// Firefox (and browsers built on it, such as Zen) hands a page gamepad changes late and in bursts while
	// the emulator runs: a quick tap's press and release, or a release and the next press when mashing,
	// can arrive together, so that no look sees them. With dom.gamepad.non_standard_events.enabled
	// (about:config; on in Nightly, off in release builds) it also fires gamepadbuttondown,
	// gamepadbuttonup and gamepadaxismove for every change, in order: the page plays each one as it comes,
	// so no tap is lost. Other browsers have neither the events nor the problem.
	const GAMEPAD_EVENTS = typeof window.GamepadButtonEvent === 'function';
	const PRESSED_BUTTON = Object.freeze({ pressed: true, touched: true, value: 1 });
	const RELEASED_BUTTON = Object.freeze({ pressed: false, touched: false, value: 0 });
	const PAD_BUTTON_COUNT = 17; // the PAD_INPUTS before the stick halves
	const VIEW_REFRESH_MS = 16;

	const players = {
		keyboard: ({ 0: 0, 2: 2 })[readPref(PREF_PAD_KEYBOARD)] ?? 1, // PS2 port 1 or 2, 0 while unplugged
		first: readPref(PREF_PAD_FIRST) === '2' ? 2 : 1, // the port a first gamepad starts on
	};
	// Per device (the keyboard, "pad:<Gamepad.index>"): the value of every pad input, 0 to 1. The
	// port plays the largest of its devices' values, so that a stick and a key never cancel each other.
	const padDevices = new Map();
	const padSent = [new Float64Array(PAD_INPUTS.length), new Float64Array(PAD_INPUTS.length)]; // doubles: a float would never equal the value sent
	let padRetryTimer = 0;
	const keyboardPad = new Float32Array(PAD_INPUTS.length);
	padDevices.set('keyboard', keyboardPad);
	// Gamepad.index -> { index, name, port (1, 2 or 0: not used), rank, connected, state, needsNeutral, view }
	const gamepads = new Map();
	let gamepadLoop = 0;

	function checkPadInputs() {
		for (let i = 0; i < PAD_INPUTS.length; i++) {
			if (api.padInput(PAD_INPUTS[i]) !== i) {
				pageLog(`The emulator numbers the pad input ${PAD_INPUTS[i]} differently; gamepads and the keyboard may not play.`, 'warn');
				return;
			}
		}
		syncPads();
	}

	function devicePort(id) {
		if (id === 'keyboard')
			return players.keyboard;
		const g = gamepads.get(Number(id.slice(4)));
		return g && g.connected ? g.port : 0;
	}

	// Sends what changed since the last call. A full queue (the emulator is far behind) keeps the
	// difference for the next try.
	function syncPads() {
		if (!api || page.phase !== 'ready')
			return;
		let retry = false;
		for (let port = 1; port <= 2; port++) {
			const sent = padSent[port - 1];
			for (let i = 0; i < PAD_INPUTS.length; i++) {
				let v = 0;
				for (const [id, state] of padDevices) {
					if (state[i] > v && devicePort(id) === port)
						v = state[i];
				}
				v = Math.round(v * 65535) / 65535;
				if (v === sent[i])
					continue;
				let ok = false;
				try {
					ok = api.pad(port - 1, i, v);
				} catch (err) {
					fatal('crash', 'Calling pcsx2web_pad failed', err);
					return;
				}
				if (ok)
					sent[i] = v;
				else
					retry = true;
			}
		}
		if (retry && !padRetryTimer)
			padRetryTimer = setTimeout(() => { padRetryTimer = 0; syncPads(); }, 20);
	}

	function setKeyboardPad(keyName, down) {
		const input = KEY_PAD[keyName];
		if (input === undefined)
			return;
		keyboardPad[PAD_INDEX[input]] = down ? 1 : 0;
		syncPads();
	}

	function releaseKeyboardPad() {
		if (!keyboardPad.some((v) => v > 0))
			return;
		keyboardPad.fill(0);
		syncPads();
	}

	// ---- gamepads

	function gamepadName(gp) {
		let name = String(gp.id || '').trim();
		name = name.replace(/\s*\((?:STANDARD GAMEPAD|Vendor:)[^)]*\)\s*$/i, '') // Chrome: "Name (STANDARD GAMEPAD Vendor: 045e Product: 028e)"
			.replace(/^[0-9a-f]{4}-[0-9a-f]{4}-/i, ''); // Firefox: "045e-028e-Name"
		return name || `Gamepad ${gp.index + 1}`;
	}

	// The PS2 only plays while it runs and the page has focus (a hidden or unfocused page gets no
	// gamepad state from the browser anyway).
	function gamepadsLive() {
		return page.phase === 'ready' && page.vmState === STATE.RUNNING && page.frameSinceBoot &&
			document.hasFocus() && document.visibilityState === 'visible' && !libraryDialog.open;
	}

	// The disc library plays by gamepad (UI-SPEC.md §15): the D-pad or the left stick steps along the shelf and
	// Cross puts the disc in front in, like the arrows and Enter. A held direction repeats. What was held when the
	// library opened is ignored until it is let go, and nothing in the library reaches the PS2.
	const LIBRARY_PAD_FIRST_MS = 400;
	const LIBRARY_PAD_REPEAT_MS = 110;
	const LIBRARY_PAD_PRESS = 0.5;
	const libraryDialog = document.getElementById('library');

	function libraryPadFrame(g, raw, now) {
		const st = g.library || (g.library = { armed: false, dir: 0, next: 0, cross: false });
		if (!libraryDialog.open || !libraryPadHandler || g.mapper) {
			st.armed = false;
			return;
		}
		const at = (name) => raw[PAD_INDEX[name]];
		const back = Math.max(at('Left'), at('Up'), at('LLeft')) >= LIBRARY_PAD_PRESS;
		const fwd = Math.max(at('Right'), at('Down'), at('LRight')) >= LIBRARY_PAD_PRESS;
		const dir = back === fwd ? 0 : fwd ? 1 : -1;
		const cross = at('Cross') >= LIBRARY_PAD_PRESS;
		if (!st.armed) {
			st.armed = !dir && !cross;
			return;
		}
		if (cross && !st.cross)
			libraryPadHandler({ confirm: true });
		st.cross = cross;
		if (dir !== st.dir) {
			st.dir = dir;
			st.next = now + LIBRARY_PAD_FIRST_MS;
			if (dir)
				libraryPadHandler({ step: dir });
		} else if (dir && now >= st.next) {
			st.next = now + LIBRARY_PAD_REPEAT_MS;
			libraryPadHandler({ step: dir });
		}
	}

	function stickValue(x, y) {
		const m = Math.hypot(x, y);
		if (!(m > STICK_DEAD_ZONE))
			return [0, 0];
		const k = Math.min(1, (m - STICK_DEAD_ZONE) / (1 - STICK_DEAD_ZONE)) / m;
		return [x * k, y * k];
	}

	function readGamepad(gp, out, g) {
		if (g && (g.custom || g.map)) {
			readCustom(gp, out, g);
			return;
		}
		out.fill(0);
		for (const [i, name] of Object.entries(GAMEPAD_BUTTONS)) {
			const b = gp.buttons[i];
			if (b && b.pressed)
				out[PAD_INDEX[name]] = 1;
		}
		for (const [i, name] of Object.entries(GAMEPAD_TRIGGERS)) {
			const b = gp.buttons[i];
			if (!b)
				continue;
			const v = b.pressed && !b.value ? 1 : b.value;
			if (v > TRIGGER_DEAD_ZONE)
				out[PAD_INDEX[name]] = Math.min(1, v);
		}
		for (const [x, y, up, down, left, right] of [[0, 1, 'LUp', 'LDown', 'LLeft', 'LRight'], [2, 3, 'RUp', 'RDown', 'RLeft', 'RRight']]) {
			const [sx, sy] = stickValue(gp.axes[x] || 0, gp.axes[y] || 0);
			const q = (v) => Math.round(Math.min(1, Math.abs(v)) * STICK_STEPS) / STICK_STEPS;
			out[PAD_INDEX[sy < 0 ? up : down]] = q(sy);
			out[PAD_INDEX[sx < 0 ? left : right]] = q(sx);
		}
	}

	function registerGamepad(gp) {
		let g = gamepads.get(gp.index);
		if (g) {
			g.name = gamepadName(gp);
			g.connected = true;
			g.needsNeutral = true;
		} else {
			const rank = [...gamepads.values()].filter((o) => o.connected).length;
			g = {
				index: gp.index,
				name: gamepadName(gp),
				port: rank === 0 ? players.first : rank === 1 ? 3 - players.first : 0,
				rank,
				connected: true,
				state: new Float32Array(PAD_INPUTS.length), // what the PS2 gets
				raw: new Float32Array(PAD_INPUTS.length), // what the gamepad holds
				shown: new Float32Array(PAD_INPUTS.length), // what the Gamepad tab shows
				needsNeutral: true,
				latch: new Float32Array(PAD_INPUTS.length), // the value a button was last seen at ...
				latchUntil: new Float64Array(PAD_INPUTS.length), // ... and until when the picture shows it
				viewAt: 0,
				view: null,
			};
			gamepads.set(gp.index, g);
		}
		// A gamepad that is not laid out in the standard way plays through the mapping its owner made; a standard
		// one too once its owner changed a control on its controller.
		g.custom = gp.mapping !== 'standard';
		g.key = padMapKey(gp);
		g.map = padMaps[g.key] || null;
		g.seen = [];
		g.mapper = null;
		padDevices.set('pad:' + g.index, g.state);
		renderGamepads();
		const needsMap = g.custom && !g.map;
		announce(!g.port ? COPY.announce.gamepadUnused(g.name) :
			needsMap ? COPY.announce.gamepadNeedsMap(g.name, g.port) : COPY.announce.gamepadConnected(g.name, g.port));
		pageLog(`Gamepad connected: ${g.name}${g.port ? ` (Player ${g.port})` : ' (not used)'}${needsMap ? '; its layout is not standard: map its controls on its controller, under the console' : ''}.`);
		startGamepadLoop();
	}

	function dropGamepad(g) {
		g.connected = false;
		g.state.fill(0);
		padDevices.delete('pad:' + g.index);
		syncPads();
		renderGamepads();
		announce(COPY.announce.gamepadDisconnected(g.name));
		pageLog(`Gamepad disconnected: ${g.name}.`);
	}

	function releaseGamepads() {
		for (const g of gamepads.values()) {
			if (g.connected) {
				g.state.fill(0);
				g.needsNeutral = true;
			}
		}
		syncPads();
	}

	function pollGamepads() {
		if (!navigator.getGamepads) {
			stopGamepadLoop();
			return;
		}
		const list = navigator.getGamepads();
		const live = gamepadsLive();
		for (const gp of list) {
			if (gp && gp.connected && gp.buttons.length && !(gamepads.get(gp.index) || {}).connected)
				registerGamepad(gp);
		}
		let any = false;
		let changed = false;
		for (const g of gamepads.values()) {
			if (!g.connected)
				continue;
			const gp = list[g.index];
			if (!gp || !gp.connected) {
				dropGamepad(g);
				continue;
			}
			any = true;
			playGamepad(g, gp, live);
			changed = true;
		}
		if (changed)
			syncPads();
		rumbleFrame(list, live);
		if (!any)
			stopGamepadLoop(); // the next gamepadconnected event starts it again
	}

	// ---- rumble (UI-SPEC.md §17): the game's motors on its players' gamepads

	// PCSX2 keeps each port's motors in two words (pcsx2web_pad_vibration(): large << 8 | small, 0-255). Each frame
	// a gamepad that can vibrate (Chromium's vibrationActuator) plays its port's: an effect of RUMBLE_EFFECT_MS,
	// renewed every RUMBLE_RENEW_MS while it runs, so that a hidden tab or a stopped game never leaves it buzzing.
	const RUMBLE_EFFECT_MS = 1000;
	const RUMBLE_RENEW_MS = 700;
	const rumble = {
		on: [readPref(PREF_RUMBLE + 1) !== '0', readPref(PREF_RUMBLE + 2) !== '0'],
		buf: null, words: null,
		playing: new Map(), // Gamepad.index -> { value, at }
	};

	// The Test button: the strong motor, then the weak one, on every gamepad that plays as this player, whether the
	// game's rumble is on or off (it tests the gamepad). The game's own motors take over at their next change.
	const RUMBLE_TEST_MS = 400;
	function testRumble(port) {
		const list = navigator.getGamepads ? navigator.getGamepads() : [];
		const acts = [];
		for (const g of connectedGamepads()) {
			const gp = g.port === port ? list[g.index] : null;
			if (gp && gp.vibrationActuator && typeof gp.vibrationActuator.playEffect === 'function')
				acts.push(gp.vibrationActuator);
		}
		const play = (strong, weak, delay) => {
			for (const act of acts) {
				try {
					const done = act.playEffect('dual-rumble', { startDelay: delay, duration: RUMBLE_TEST_MS, strongMagnitude: strong, weakMagnitude: weak });
					if (done && typeof done.catch === 'function')
						done.catch(() => {});
				} catch {
					// the gamepad cannot play it
				}
			}
		};
		play(1, 0, 0);
		setTimeout(() => play(0, 1, 0), RUMBLE_TEST_MS + 150);
		// Not left to the effect's duration, which some gamepads ignore in Chromium (stopAllRumble()).
		setTimeout(() => {
			for (const act of acts) {
				try {
					const done = typeof act.reset === 'function' ? act.reset() : null;
					if (done && typeof done.catch === 'function')
						done.catch(() => {});
				} catch {
					// gone
				}
			}
		}, 2 * RUMBLE_TEST_MS + 150);
		pads.note[port - 1] = acts.length ? 'Rumble test: the strong motor, then the weak one.' : 'No gamepad playing as this player can rumble.';
		pageLog(`Rumble test for Player ${port} on ${acts.length} gamepad${acts.length === 1 ? '' : 's'}.`);
		renderPads();
	}

	// Every gamepad's motors off now. Chromium does not always end an effect after its duration (a DualShock or
	// DualSense kept rumbling after its tab was closed, until the browser quit), so the page stops them itself when it
	// goes away, is hidden or loses focus: then rumbleFrame() no longer runs to send the zero.
	function stopAllRumble() {
		const list = navigator.getGamepads ? navigator.getGamepads() : [];
		for (const gp of list) {
			const act = gp && gp.vibrationActuator;
			if (!act || typeof act.reset !== 'function')
				continue;
			try {
				const done = act.reset();
				if (done && typeof done.catch === 'function')
					done.catch(() => {});
			} catch {
				// gone
			}
		}
		for (const p of rumble.playing.values())
			p.value = 0;
	}

	for (const [target, type] of [[window, 'pagehide'], [window, 'beforeunload'], [window, 'blur'], [document, 'freeze']])
		target.addEventListener(type, stopAllRumble);
	document.addEventListener('visibilitychange', () => {
		if (document.visibilityState === 'hidden')
			stopAllRumble();
	});

	function canRumble(g) {
		const gp = navigator.getGamepads && navigator.getGamepads()[g.index];
		return !!(gp && gp.vibrationActuator && typeof gp.vibrationActuator.playEffect === 'function');
	}

	function rumbleWords() {
		const M = window.Module;
		if (!M || typeof M._pcsx2web_pad_vibration !== 'function')
			return null;
		const buf = heapBuffer();
		if (rumble.buf !== buf) {
			rumble.buf = buf;
			rumble.words = new Uint32Array(buf, M._pcsx2web_pad_vibration() >>> 0, 2);
		}
		return rumble.words;
	}

	function rumbleFrame(list, live) {
		const words = live ? rumbleWords() : null;
		const now = performance.now();
		for (const g of gamepads.values()) {
			const gp = g.connected ? list[g.index] : null;
			const act = gp && gp.vibrationActuator;
			if (!act || typeof act.playEffect !== 'function')
				continue;
			const port = g.port;
			const value = words && port && rumble.on[port - 1] && !g.mapper ? Atomics.load(words, port - 1) : 0;
			const p = rumble.playing.get(g.index) || { value: 0, at: 0 };
			if (value === p.value && (value === 0 || now - p.at < RUMBLE_RENEW_MS))
				continue;
			p.value = value;
			p.at = now;
			rumble.playing.set(g.index, p);
			try {
				const done = value
					? act.playEffect('dual-rumble', { duration: RUMBLE_EFFECT_MS, strongMagnitude: (value >>> 8) / 255,
						weakMagnitude: (value & 255) / 255 })
					: typeof act.reset === 'function' ? act.reset() : null;
				if (done && typeof done.catch === 'function')
					done.catch(() => {}); // a newer effect replaced it, or the gamepad went away
			} catch {
				// the gamepad cannot play it
			}
		}
	}

	// Plays one state of a gamepad: what the PS2 gets (sent by the caller's syncPads), the Gamepad tab's
	// picture and a mapping run.
	function playGamepad(g, seen, live) {
		const raw = g.raw;
		readGamepad(seen, raw, g);
		const now = performance.now();
		g.shown.set(raw);
		for (let i = 0; i < PAD_BUTTON_COUNT; i++) {
			if (raw[i] > 0) {
				g.latch[i] = raw[i];
				g.latchUntil[i] = now + GAMEPAD_SHOW_MS;
			} else if (now < g.latchUntil[i]) {
				g.shown[i] = g.latch[i];
			}
		}
		if (g.mapper)
			mapperFrame(g, seen, now);
		if ((g.view || pads.seen) && now - g.viewAt >= VIEW_REFRESH_MS) {
			g.viewAt = now;
			if (g.view)
				updateGamepadView(g);
			updatePadLive(g);
		}
		libraryPadFrame(g, raw, now);
		const idle = !raw.some((v) => v > 0);
		if (idle)
			g.needsNeutral = false;
		else if (libraryDialog.open)
			g.needsNeutral = true; // what is held in the library is not for the game
		// After a change of player or a pause in play, the inputs held must be let go once.
		if (live && !g.needsNeutral && !g.mapper)
			g.state.set(raw);
		else
			g.state.fill(0);
	}

	// Firefox's gamepad events, in order: each one plays the gamepad as it is now with that change on top
	// (the gamepad can already show a later one), so every press and release reaches the PS2 as an edge,
	// even when a burst brings a whole tap, or a release and the next press, at once. The emulator keeps
	// each press long enough for the game.
	function onGamepadEvent(e) {
		const g = e.gamepad && gamepads.get(e.gamepad.index);
		const gp = g && g.connected && navigator.getGamepads()[g.index];
		if (!gp || !gp.connected)
			return;
		let buttons = gp.buttons;
		let axes = gp.axes;
		if (e.type === 'gamepadaxismove') {
			axes = Array.from(gp.axes);
			axes[e.axis] = e.value;
		} else {
			const down = e.type === 'gamepadbuttondown';
			const b = gp.buttons[e.button];
			if (b && b.pressed !== down) {
				buttons = Array.from(gp.buttons);
				buttons[e.button] = down ? PRESSED_BUTTON : RELEASED_BUTTON;
			}
		}
		playGamepad(g, { id: gp.id, index: gp.index, mapping: gp.mapping, connected: true, buttons, axes }, gamepadsLive());
		syncPads();
	}

	function startGamepadLoop() {
		if (!gamepadLoop && document.visibilityState === 'visible')
			gamepadLoop = setInterval(pollGamepads, GAMEPAD_POLL_MS);
	}

	function stopGamepadLoop() {
		if (gamepadLoop) {
			clearInterval(gamepadLoop);
			gamepadLoop = 0;
		}
	}

	function setGamepadPort(g, port) {
		if (g.port === port)
			return;
		g.port = port;
		g.state.fill(0);
		g.needsNeutral = true;
		if (g.rank === 0 && port) {
			players.first = port;
			writePref(PREF_PAD_FIRST, String(port));
		}
		syncPads();
		renderGamepads();
		announce(COPY.announce.gamepadPlayer(g.name, port));
	}

	function swapGamepads() {
		const two = [...gamepads.values()].filter((g) => g.connected && g.port).sort((a, b) => a.rank - b.rank);
		if (two.length < 2)
			return;
		const [a, b] = two;
		[a.port, b.port] = [b.port, a.port];
		for (const g of two) {
			g.state.fill(0);
			g.needsNeutral = true;
		}
		if (a.rank === 0) {
			players.first = a.port;
			writePref(PREF_PAD_FIRST, String(a.port));
		}
		syncPads();
		renderGamepads();
		announce(COPY.announce.gamepadsSwapped(a, b));
	}

	function setKeyboardPlayer(port) {
		if (players.keyboard === port)
			return;
		releaseAllKeys(true); // keys held now are let go; they play again once pressed anew
		players.keyboard = port;
		writePref(PREF_PAD_KEYBOARD, String(port));
		syncPads();
		announce(COPY.announce.keyboardPlayer(port));
		renderPorts();
	}

	// ---- the console's controller ports (UI-SPEC §16)

	// What plays on a port: the keyboard and the gamepads there.
	function portDevices(port) {
		const list = [];
		if (players.keyboard === port)
			list.push('Keyboard');
		for (const g of [...gamepads.values()].sort((a, b) => a.rank - b.rank))
			if (g.connected && g.port === port)
				list.push(g.name);
		return list;
	}

	function renderPorts() {
		for (const btn of els.ports) {
			const port = Number(btn.dataset.port);
			const who = portDevices(port);
			const text = who.length ? who.join(' and ') : 'nothing plugged in';
			btn.setAttribute('aria-label', `Controller port ${port}, Player ${port}: ${text}`);
			btn.setAttribute('aria-pressed', String(who.length > 0));
			setText(btn.querySelector('.tip'), who.length ? `Player ${port} · ${who.join(' + ')} · Unplug` : `Player ${port} · Plug in`);
		}
		if (page.drawer === 'keys')
			renderPortPick();
		renderPads();
	}

	// The drawer's "Player N plays with" row: the keyboard and each connected gamepad, the ones on this
	// port pressed. Choosing one moves it to this port and shows its keys.
	function renderPortPick() {
		const port = page.keysPort;
		setText(els.portPickTitle, `Player ${port} plays with`);
		const options = [{ id: 'keyboard', name: 'Keyboard', on: players.keyboard === port }];
		for (const g of [...gamepads.values()].sort((a, b) => a.rank - b.rank))
			if (g.connected)
				options.push({ id: g.index, name: g.name, on: g.port === port, g });
		const key = options.map((o) => `${o.id}:${o.on}`).join('|') + port;
		if (els.portPickList.dataset.key === key)
			return;
		els.portPickList.dataset.key = key;
		els.portPickList.replaceChildren(...options.map((o) => {
			const b = document.createElement('button');
			b.type = 'button';
			b.className = 'port-opt';
			b.setAttribute('aria-pressed', String(o.on));
			b.textContent = o.name;
			b.addEventListener('click', () => {
				if (o.g)
					setGamepadPort(o.g, port);
				else
					setKeyboardPlayer(port);
				for (const r of els.kbPlayers)
					r.checked = Number(r.value) === players.keyboard;
				selectControlsTab(o.g ? 'gamepad' : 'keyboard', false);
				renderPorts();
			});
			return b;
		}));
		if (options.length === 1) {
			const hint = document.createElement('span');
			hint.className = 'port-pick-hint';
			hint.textContent = 'or press a button on a gamepad to connect it.';
			els.portPickList.append(hint);
		}
	}

	// Opening the controls for page.keysPort: its row, and the tab of what plays there.
	function showPortControls() {
		renderPortPick();
		const port = page.keysPort;
		const gamepadThere = [...gamepads.values()].some((g) => g.connected && g.port === port);
		selectControlsTab(players.keyboard !== port && gamepadThere ? 'gamepad' : 'keyboard', false);
	}

	// ---- the controllers under the console (UI-SPEC §17)

	// Where each control is on a controller's drawing (pad-template, 600 × 310 units): its centre and size, in
	// tab order. The shoulder buttons sit behind one another, so theirs are at the ends of lines led out of them.
	const PAD_SPOTS = [
		{ id: 'L2', x: 70, y: 30, w: 40, h: 22, lead: true },
		{ id: 'L1', x: 70, y: 74, w: 40, h: 22, lead: true },
		{ id: 'Up', x: 184.8, y: 123.7, w: 22, h: 26 },
		{ id: 'Left', x: 165.9, y: 142.6, w: 26, h: 22 },
		{ id: 'Right', x: 203.7, y: 142.6, w: 26, h: 22 },
		{ id: 'Down', x: 184.8, y: 161.4, w: 22, h: 26 },
		{ id: 'Select', x: 264.2, y: 143.2, w: 26, h: 14 },
		{ id: 'Start', x: 333, y: 143.2, w: 26, h: 14 },
		{ id: 'Analog', x: 300, y: 179.4, w: 30, h: 14 },
		{ id: 'LUp', x: 241.4, y: 164.6, w: 16, h: 14 },
		{ id: 'LLeft', x: 206.9, y: 199.2, w: 14, h: 16 },
		{ id: 'L3', x: 241.4, y: 199.2, w: 24, h: 24, round: true },
		{ id: 'LRight', x: 276, y: 199.2, w: 14, h: 16 },
		{ id: 'LDown', x: 241.4, y: 233.8, w: 16, h: 14 },
		{ id: 'RUp', x: 358.6, y: 164.6, w: 16, h: 14 },
		{ id: 'RLeft', x: 324, y: 199.2, w: 14, h: 16 },
		{ id: 'R3', x: 358.6, y: 199.2, w: 24, h: 24, round: true },
		{ id: 'RRight', x: 393.1, y: 199.2, w: 14, h: 16 },
		{ id: 'RDown', x: 358.6, y: 233.8, w: 16, h: 14 },
		{ id: 'Triangle', x: 415.2, y: 112.2, w: 26, h: 26, round: true },
		{ id: 'Square', x: 384.5, y: 142.9, w: 26, h: 26, round: true },
		{ id: 'Circle', x: 445.9, y: 142.9, w: 26, h: 26, round: true },
		{ id: 'Cross', x: 415.2, y: 173.6, w: 26, h: 26, round: true },
		{ id: 'R1', x: 530, y: 74, w: 40, h: 22, lead: true },
		{ id: 'R2', x: 530, y: 30, w: 40, h: 22, lead: true },
	];
	const PAD_FRAME_H = 310;
	const PAD_CABLE_Y = 81.4; // where the cable comes out of the drawing, at its middle
	const PAD_LABELS = {
		Up: 'D-pad up', Down: 'D-pad down', Left: 'D-pad left', Right: 'D-pad right',
		Triangle: 'Triangle', Circle: 'Circle', Cross: 'Cross', Square: 'Square', Select: 'Select', Start: 'Start',
		L1: 'L1', L2: 'L2', R1: 'R1', R2: 'R2', L3: 'L3 (left stick pressed)', R3: 'R3 (right stick pressed)', Analog: 'Analog',
		LUp: 'Left stick up', LRight: 'Left stick right', LDown: 'Left stick down', LLeft: 'Left stick left',
		RUp: 'Right stick up', RRight: 'Right stick right', RDown: 'Right stick down', RLeft: 'Right stick left',
	};
	const STICK_INPUT = /^([LR])(Up|Down|Left|Right)$/;
	const KEY_TEXT = { Return: 'Enter', Backspace: 'Bksp', Control: 'Ctrl', PageUp: 'PgUp', PageDown: 'PgDn', Delete: 'Del', Insert: 'Ins' };
	const KEY_SPOKEN = { Up: 'Up arrow', Down: 'Down arrow', Left: 'Left arrow', Right: 'Right arrow', Return: 'Enter', Backspace: 'Backspace',
		Control: 'Control', PageUp: 'Page Up', PageDown: 'Page Down', Delete: 'Delete', Insert: 'Insert' };
	const KEY_ICONS = { Up: 'arrow-up', Down: 'arrow-down', Left: 'arrow-left', Right: 'arrow-right' };
	// A standard gamepad's buttons by number, as printed on the maker's pads that use the numbering.
	const SONY_BUTTONS = ['×', '○', '□', '△', 'L1', 'R1', 'L2', 'R2', 'Share', 'Options', 'L3', 'R3', '↑', '↓', '←', '→', 'Home'];
	const XBOX_BUTTONS = ['A', 'B', 'X', 'Y', 'LB', 'RB', 'LT', 'RT', 'View', 'Menu', 'LS', 'RS', '↑', '↓', '←', '→', 'Home'];
	const STICK_ARROWS = { Up: '↑', Down: '↓', Left: '←', Right: '→' };

	const pads = {
		units: [], // per port, built by buildPads
		shown: [null, null], // per port: what its controller shows, 'keyboard' or a Gamepad.index, as chosen on it
		listen: null, // { port, input, g }: a control waiting for its key (g null) or its gamepad control
		note: ['', ''], // per port: the last change, until the next
		swallowUp: '', // the code of the key just given to a control: its keyup must not press a button
		unplugged: [[], []], // per port: what played there when its plug was pulled, to plug back in
		seen: false, // the controllers are on screen
		cableFrame: 0,
		cablesLive: false,
		cableKey: '',
		cablePaths: null,
		cableAsked: 0,
		cableJob: null, // the layout waiting for the worker (the latest only)
		cableInFlight: null, // the key of the layout the worker is settling
		cableWorker: undefined,
		cableParams: null, // the cable tuner's settings (?tune-cables), else the worker's own
	};

	const letterCode = (name) => (/^[A-Z]$/.test(name) ? 'Key' + name : '');

	function keyText(name) {
		const code = letterCode(name);
		return code ? keyLabel(code, name) : KEY_TEXT[name] || name;
	}

	function keySpoken(name) {
		return KEY_SPOKEN[name] || keyText(name);
	}

	// A key's cap: an arrow's icon, or its name as the layout prints it ("Bksp", AZERTY's Q for A); empty for none.
	function fillKeyCap(el, name) {
		el.replaceChildren();
		el.dataset.key = name || '';
		const code = name ? letterCode(name) : '';
		if (code)
			el.dataset.code = code;
		else
			delete el.dataset.code;
		if (!name)
			return;
		if (KEY_ICONS[name])
			el.append(svgIcon(KEY_ICONS[name], 'icon'), span('vh', KEY_SPOKEN[name]));
		else
			el.textContent = keyText(name);
	}

	function applyKeyBinds() {
		for (const key of Object.keys(KEY_PAD))
			delete KEY_PAD[key];
		PAD_CODES.clear();
		for (const [input, key] of Object.entries(keyBinds)) {
			KEY_PAD[key] = input;
			for (const [code, name] of Object.entries(KEY_NAMES)) {
				if (name === key)
					PAD_CODES.add(code);
			}
		}
	}

	function renderKeyCaps() {
		for (const k of document.querySelectorAll('kbd[data-input]'))
			fillKeyCap(k, keyBinds[k.dataset.input] || '');
	}

	function saveKeyBinds() {
		applyKeyBinds();
		writePref(PREF_KEY_BINDS, JSON.stringify(Object.fromEntries(PAD_INPUTS.map((i) => [i, keyBinds[i] || null]))));
		renderKeyCaps();
	}

	// Gives a pad input a key. The input that had the key takes this one's old key, so that no key plays two
	// inputs and none goes missing. Returns that other input, if any.
	function setKeyBind(input, key) {
		const prev = keyBinds[input] || '';
		if (prev === key)
			return null;
		const other = PAD_INPUTS.find((i) => i !== input && keyBinds[i] === key) || null;
		releaseAllKeys(true);
		keyBinds[input] = key;
		if (other && prev)
			keyBinds[other] = prev;
		else if (other)
			delete keyBinds[other];
		saveKeyBinds();
		return other;
	}

	function keyBindsChanged() {
		const def = defaultKeyBinds();
		return PAD_INPUTS.some((i) => (keyBinds[i] || '') !== (def[i] || ''));
	}

	function resetKeyBinds() {
		releaseAllKeys(true);
		for (const input of Object.keys(keyBinds))
			delete keyBinds[input];
		Object.assign(keyBinds, defaultKeyBinds());
		saveKeyBinds();
	}

	const isSonyPad = (g) => /054c|dualshock|dualsense|wireless controller/i.test(g.key || '');
	const padMapOf = (g) => g.map || (g.custom ? null : STANDARD_MAP);

	// What plays a pad input on a gamepad, short enough for its button on the controller ("A", "B3", "A1−").
	function padControlText(g, input) {
		const map = padMapOf(g);
		if (!map)
			return '';
		const m = STICK_INPUT.exec(input);
		if (m) {
			const st = map.sticks && map.sticks[m[1]];
			const ax = st && (m[2] === 'Left' || m[2] === 'Right' ? st.x : st.y);
			if (!ax)
				return '';
			const std = STANDARD_MAP.sticks[m[1]];
			const stdAx = m[2] === 'Left' || m[2] === 'Right' ? std.x : std.y;
			if (!g.custom && ax.i === stdAx.i && ax.s === stdAx.s)
				return STICK_ARROWS[m[2]]; // the gamepad's own stick, as it comes
			// Right and down are the axis times its sign (readCustom).
			const plus = (m[2] === 'Right' || m[2] === 'Down') === (ax.s > 0);
			return `A${ax.i}${plus ? '+' : '−'}`;
		}
		const b = map.buttons && map.buttons[input];
		if (!b)
			return '';
		if (b.k === 'b') {
			const names = g.custom ? null : isSonyPad(g) ? SONY_BUTTONS : XBOX_BUTTONS;
			return (names && names[b.i]) || `B${b.i}`;
		}
		if (b.k === 'h')
			return `H${b.i}`;
		return `A${b.i}${b.act > b.rest ? '+' : '−'}`;
	}

	function padControlSpoken(g, input) {
		const map = padMapOf(g);
		const m = STICK_INPUT.exec(input);
		if (m) {
			const st = map && map.sticks && map.sticks[m[1]];
			const ax = st && (m[2] === 'Left' || m[2] === 'Right' ? st.x : st.y);
			return ax ? `axis ${ax.i}` : 'nothing';
		}
		const b = map && map.buttons && map.buttons[input];
		if (!b)
			return 'nothing';
		const text = padControlText(g, input);
		return b.k === 'b' && text !== `B${b.i}` ? `${text}, button ${b.i}` : describeBinding(b);
	}

	function connectedGamepads() {
		return [...gamepads.values()].filter((g) => g.connected).sort((a, b) => a.rank - b.rank);
	}

	function playsOn(id, port) {
		if (id === 'keyboard')
			return players.keyboard === port;
		const g = gamepads.get(id);
		return !!g && g.connected && g.port === port;
	}

	// What a port's controller shows: the device chosen on it while it plays there, else a gamepad there, else
	// the keyboard there; null when nothing plays as that player.
	function padShown(port) {
		const want = pads.shown[port - 1];
		if (want !== null && playsOn(want, port))
			return want;
		const g = connectedGamepads().find((o) => o.port === port);
		if (g)
			return g.index;
		return players.keyboard === port ? 'keyboard' : null;
	}

	function choosePadDevice(port, id) {
		stopListening(false);
		pads.shown[port - 1] = id;
		pads.note[port - 1] = '';
		if (id === 'keyboard') {
			setKeyboardPlayer(port);
			for (const r of els.kbPlayers)
				r.checked = Number(r.value) === players.keyboard;
		} else {
			const g = gamepads.get(id);
			if (g)
				setGamepadPort(g, port);
		}
		renderPads();
	}

	function buildPads() {
		for (const port of [1, 2]) {
			const el = $('pad-template').content.firstElementChild.cloneNode(true);
			el.id = `pad-${port}`;
			el.setAttribute('aria-labelledby', `pad-${port}-title`);
			const title = el.querySelector('.pad-title');
			title.id = `pad-${port}-title`;
			title.textContent = `Player ${port}`;
			el.querySelector('.pad-dev legend').textContent = `Player ${port} plays with`;
			const u = {
				port,
				el,
				art: el.querySelector('.pad-art'),
				spots: new Map(),
				sticks: {},
				devList: el.querySelector('.pad-dev-list'),
				devKey: '',
				status: el.querySelector('.pad-status'),
				also: el.querySelector('.pad-also'),
				map: el.querySelector('.pad-map'),
				reset: el.querySelector('.pad-reset'),
				rumble: el.querySelector('.pad-rumble'),
				rumbleInput: el.querySelector('.pad-rumble-input'),
				rumbleTest: el.querySelector('.pad-rumble-test'),
				cable: [$(`cable-${port}-wall`), $(`cable-${port}`), $(`cable-${port}-front-wall`), $(`cable-${port}-front`)],
				lit: false,
			};
			const box = el.querySelector('.pad-spots');
			for (const s of PAD_SPOTS) {
				const btn = document.createElement('button');
				btn.type = 'button';
				btn.className = `pad-spot${s.round ? ' round' : ''}${s.lead ? ' lead' : ''}`;
				btn.style.cssText = `--x: ${s.x}; --y: ${s.y}; --w: ${s.w}; --h: ${s.h}`;
				const cap = document.createElement('kbd');
				btn.append(cap);
				btn.addEventListener('click', () => listenFor(port, s.id));
				box.append(btn);
				u.spots.set(s.id, { btn, cap });
			}
			for (const c of el.querySelectorAll('.p-stick'))
				u.sticks[c.dataset.stick] = c;
			u.rumbleInput.checked = rumble.on[port - 1];
			u.rumbleInput.addEventListener('change', () => {
				rumble.on[port - 1] = u.rumbleInput.checked;
				writePref(PREF_RUMBLE + port, u.rumbleInput.checked ? '1' : '0');
				pageLog(`Rumble for Player ${port}: ${u.rumbleInput.checked ? 'on' : 'off'}.`);
			});
			u.rumbleTest.setAttribute('aria-label', `Test rumble for Player ${port}`);
			u.rumbleTest.addEventListener('click', () => testRumble(port));
			u.map.addEventListener('click', () => {
				const g = gamepads.get(padShown(port));
				if (!g || g.mapper)
					return;
				stopListening(false);
				pads.note[port - 1] = '';
				pads.listen = { port, input: null, g };
				startMapping(g);
			});
			u.reset.addEventListener('click', () => {
				stopListening(false);
				const id = padShown(port);
				if (id === 'keyboard') {
					resetKeyBinds();
					pads.note[port - 1] = 'The keys are back as they came.';
				} else if (gamepads.has(id)) {
					const g = gamepads.get(id);
					clearMapping(g);
					pads.note[port - 1] = g.custom ? 'Mapping cleared.' : 'The gamepad’s controls are back as they came.';
				}
				renderPads();
			});
			$('pads-row').append(el);
			pads.units.push(u);
		}
		// The keys drawer's caps follow the keys too.
		for (const k of els.panelKeyboard.querySelectorAll('kbd[data-key]')) {
			const input = DEFAULT_KEY_PAD[k.dataset.key];
			if (input)
				k.dataset.input = input;
		}
		applyKeyBinds();
		renderKeyCaps();
		// Before the page's own keys: while a control listens, the next key is its key.
		window.addEventListener('keydown', onPadsKeyDown, true);
		window.addEventListener('keyup', (e) => {
			if (pads.swallowUp && e.code === pads.swallowUp) {
				pads.swallowUp = '';
				e.preventDefault();
				e.stopImmediatePropagation();
			}
		}, true);
		document.addEventListener('pointerdown', (e) => {
			const l = pads.listen;
			if (l && !(l.input && pads.units[l.port - 1].spots.get(l.input).btn.contains(e.target)))
				stopListening(true);
		}, true);
		if (typeof IntersectionObserver === 'function') {
			new IntersectionObserver((entries) => {
				pads.seen = entries.some((en) => en.isIntersecting);
			}).observe($('pads'));
		}
		if (typeof ResizeObserver === 'function') {
			const ro = new ResizeObserver(scheduleCables);
			for (const el of [document.body, els.transport, $('pads')])
				ro.observe(el);
		}
		window.addEventListener('resize', scheduleCables);
		if (params.has('tune-cables'))
			buildCableTuner();
		renderPads();
	}

	function listenFor(port, input) {
		const was = pads.listen;
		stopListening(false);
		if (was && was.port === port && was.input === input)
			return; // a second click puts it down
		const id = padShown(port);
		if (id === null)
			return;
		pads.note[port - 1] = '';
		if (id === 'keyboard') {
			pads.listen = { port, input, g: null };
			renderPads();
			return;
		}
		const g = gamepads.get(id);
		if (!g || g.mapper)
			return;
		const stick = STICK_INPUT.exec(input);
		pads.listen = { port, input, g };
		startMapping(g, stick ? [stick[1] + 'X', stick[1] + 'Y'] : [input]); // a stick is mapped whole
		if (g.mapper)
			g.mapper.swap = true;
	}

	function stopListening(cancelled) {
		const l = pads.listen;
		if (!l)
			return;
		pads.listen = null;
		if (cancelled)
			pads.note[l.port - 1] = 'Unchanged.';
		if (l.g && l.g.mapper)
			mapperCancel(l.g);
		renderPads();
	}

	function onPadsKeyDown(e) {
		const l = pads.listen;
		if (!l)
			return;
		if (e.key === 'Tab') {
			stopListening(true); // and focus moves on
			return;
		}
		e.preventDefault();
		e.stopImmediatePropagation();
		if (e.repeat)
			return;
		if (e.key === 'Escape') {
			stopListening(true);
			return;
		}
		if (l.g)
			return; // the gamepad answers
		const key = KEY_NAMES[e.code];
		const label = PAD_LABELS[l.input];
		if (!key || KEEP_DEFAULT.has(key)) {
			pads.note[l.port - 1] = `That key stays with the browser. Press another for ${label}, or Esc.`;
			renderPads();
			return;
		}
		pads.swallowUp = e.code;
		const other = setKeyBind(l.input, key);
		pads.listen = null;
		const otherKey = other && keyBinds[other];
		pads.note[l.port - 1] = !other ? `${keySpoken(key)} plays ${label} now.` :
			otherKey ? `${keySpoken(key)} plays ${label} now, and ${keySpoken(otherKey)} ${PAD_LABELS[other]}.` :
				`${keySpoken(key)} plays ${label} now; ${PAD_LABELS[other]} has no key.`;
		renderPads();
	}

	function renderPads() {
		if (!pads.units.length)
			return;
		const l = pads.listen;
		if (l && l.g && !l.g.mapper) { // the gamepad answered
			pads.listen = null;
			const stick = l.input && STICK_INPUT.exec(l.input);
			pads.note[l.port - 1] = !l.input ? 'Mapped.' : stick ? `${stick[1] === 'L' ? 'Left' : 'Right'} stick mapped.` :
				`${PAD_LABELS[l.input]}: ${padControlText(l.g, l.input) || 'nothing'}.`;
		}
		for (const u of pads.units)
			renderPad(u);
		scheduleCables();
	}

	function renderPad(u) {
		const port = u.port;
		const id = padShown(port);
		const g = id !== null && id !== 'keyboard' ? gamepads.get(id) : null;
		u.el.dataset.plugged = String(id !== null);
		els.ports[port - 1].dataset.plugged = String(id !== null);
		renderPadDevices(u, id);
		const l = pads.listen && pads.listen.port === port ? pads.listen : null;
		const step = g && g.mapper ? g.mapper.steps[g.mapper.i] : null;
		for (const [input, { btn, cap }] of u.spots) {
			let spoken;
			if (id === 'keyboard') {
				cap.dataset.input = input;
				fillKeyCap(cap, keyBinds[input] || '');
				spoken = keyBinds[input] ? keySpoken(keyBinds[input]) : 'no key';
				btn.classList.toggle('unbound', !keyBinds[input]);
				btn.classList.toggle('listening', !!l && l.input === input);
			} else {
				delete cap.dataset.input;
				delete cap.dataset.key;
				delete cap.dataset.code;
				const text = g ? padControlText(g, input) : '';
				setText(cap, text);
				spoken = g ? padControlSpoken(g, input) : 'nothing';
				btn.classList.toggle('unbound', !text);
				const stick = STICK_INPUT.exec(input);
				btn.classList.toggle('listening', !!step && (step.id === input || (!!stick && step.side === stick[1])));
			}
			btn.setAttribute('aria-label', `${PAD_LABELS[input]}: ${spoken}`);
		}
		if (id === 'keyboard' || !g)
			resetPadLive(u);
		let status;
		if (id === null)
			status = `Nothing plays as Player ${port}. Choose the keyboard or a gamepad.`;
		else if (l && !l.g)
			status = `Press the key for ${PAD_LABELS[l.input]}. Esc cancels.`;
		else if (step)
			status = `${step.text} Esc cancels.`;
		else if (pads.note[port - 1])
			status = pads.note[port - 1];
		else if (g && g.custom && !g.map)
			status = 'This gamepad does not say how its controls are laid out: map them once, or click a control to map just that one.';
		else
			status = id === 'keyboard' ? 'Click a control, then press the key you want for it.' :
				'Click a control, then press the one you want for it on the gamepad.';
		setText(u.status, status);
		// Other devices playing as this player.
		const others = [];
		if (id !== 'keyboard' && players.keyboard === port)
			others.push('The keyboard');
		for (const o of connectedGamepads())
			if (o.port === port && o.index !== id)
				others.push(o.name);
		setText(u.also, others.length ? `${others.join(' and ')} also ${others.length > 1 ? 'play' : 'plays'} as Player ${port}.` : '');
		u.map.hidden = !g || !g.custom;
		u.rumble.hidden = !g || !canRumble(g);
		setText(u.map, g && g.map ? 'Map again' : 'Map all controls');
		u.reset.hidden = id === null || (id === 'keyboard' ? !keyBindsChanged() : !g || !g.map);
		setText(u.reset, id === 'keyboard' ? 'Reset the keys' : g && g.custom ? 'Clear the mapping' : 'Reset the controls');
	}

	// The device choice: the keyboard and each connected gamepad. Rebuilt only when the list changes, so that
	// focus stays on the choice just made.
	function renderPadDevices(u, id) {
		const options = [{ id: 'keyboard', name: 'Keyboard' }, ...connectedGamepads().map((g) => ({ id: g.index, name: g.name }))];
		const key = options.map((o) => `${o.id}:${o.name}`).join('|');
		if (u.devKey !== key) {
			u.devKey = key;
			u.devList.replaceChildren(...options.map((o) => {
				const label = document.createElement('label');
				label.className = 'check';
				const input = document.createElement('input');
				input.type = 'radio';
				input.name = `pad-dev-${u.port}`;
				input.value = String(o.id);
				input.addEventListener('change', () => {
					if (input.checked)
						choosePadDevice(u.port, o.id);
				});
				label.append(input, span('', o.name));
				return label;
			}));
			if (options.length === 1)
				u.devList.append(span('pad-dev-hint', 'Press a button on a gamepad to add it.'));
		}
		for (const r of u.devList.querySelectorAll('input'))
			r.checked = r.value === String(id);
	}

	// A gamepad's presses, and its sticks, on the controller that shows it.
	function updatePadLive(g) {
		for (const u of pads.units) {
			if (padShown(u.port) !== g.index)
				continue;
			const raw = g.shown;
			for (const [input, { btn }] of u.spots)
				btn.classList.toggle('down', raw[PAD_INDEX[input]] > 0);
			for (const side of ['L', 'R']) {
				const x = raw[PAD_INDEX[side + 'Right']] - raw[PAD_INDEX[side + 'Left']];
				const y = raw[PAD_INDEX[side + 'Down']] - raw[PAD_INDEX[side + 'Up']];
				u.sticks[side].style.translate = x || y ? `${(x * 14).toFixed(1)}px ${(y * 14).toFixed(1)}px` : '';
			}
			u.lit = true;
		}
	}

	function resetPadLive(u) {
		if (!u.lit)
			return;
		u.lit = false;
		for (const { btn } of u.spots.values())
			btn.classList.remove('down');
		for (const c of Object.values(u.sticks))
			c.style.translate = '';
	}

	// A port's button pulls its plug out (whatever plays as that player stops, and is remembered) or pushes it back
	// in (what was there, if it is free; else the keyboard or a gamepad that plays nowhere).
	function togglePort(port) {
		stopListening(false);
		const free = (id) => (id === 'keyboard' ? players.keyboard === 0 : !!gamepads.get(id) && gamepads.get(id).connected && gamepads.get(id).port === 0);
		const here = [];
		if (players.keyboard === port)
			here.push('keyboard');
		for (const g of connectedGamepads())
			if (g.port === port)
				here.push(g.index);
		if (here.length) {
			pads.unplugged[port - 1] = here;
			for (const id of here) {
				if (id === 'keyboard')
					setKeyboardPlayer(0);
				else
					setGamepadPort(gamepads.get(id), 0);
			}
			subtitle(`Player ${port} unplugged.`);
		} else {
			let ids = pads.unplugged[port - 1].filter(free);
			if (!ids.length) {
				const g = connectedGamepads().find((o) => o.port === 0);
				ids = free('keyboard') ? ['keyboard'] : g ? [g.index] : [];
			}
			if (!ids.length) {
				subtitle(`Nothing is free to plug in: choose what plays on Player ${port}\u2019s controller.`);
				return;
			}
			for (const id of ids) {
				if (id === 'keyboard')
					setKeyboardPlayer(port);
				else
					setGamepadPort(gamepads.get(id), port);
			}
			pads.shown[port - 1] = ids[0];
			subtitle(`Player ${port} plugged in: ${ids.map((id) => (id === 'keyboard' ? 'the keyboard' : gamepads.get(id).name)).join(' and ')}.`);
		}
		for (const r of els.kbPlayers)
			r.checked = Number(r.value) === players.keyboard;
		renderPads();
	}

	function scheduleCables() {
		if (!pads.cableFrame && pads.units.length)
			pads.cableFrame = requestAnimationFrame(drawCables);
	}

	// Cables, in page coordinates: they lie behind the page. Each is a rope that settles on the floor, both as long
	// and longer than either needs (cable-worker.js, which runs matter-js off the page's thread). The page sends the
	// straight runs from the plugs to the controllers and where the ropes may lie; until the answer comes, the
	// cables stay as they were.
	const CABLE_WORKER_FILE = 'cable-worker.js';
	const CABLE_GAP = 12; // px kept from the window's sides

	function cableWorker() {
		if (pads.cableWorker === undefined) {
			try {
				pads.cableWorker = new Worker(versioned(CABLE_WORKER_FILE));
				pads.cableWorker.addEventListener('message', (e) => {
					const fresh = pads.cableInFlight === pads.cableKey;
					pads.cableInFlight = null;
					if (fresh && e.data) { // the layout it was laid out for is still the page's
						pads.cablePaths = e.data.paths;
						setCablesStale(false);
						scheduleCables();
					}
					postCableJob();
				});
				pads.cableWorker.addEventListener('error', () => {
					pageLog('The cables could not be laid out (cable-worker.js failed).', 'warn');
					pads.cableWorker = null;
					pads.cableInFlight = null;
				});
			} catch {
				pads.cableWorker = null;
			}
		}
		return pads.cableWorker;
	}

	// One layout at a time in the worker: while it settles one, a newer layout replaces the waiting job, so that a
	// window being resized never leaves a queue of stale layouts to settle first.
	function postCableJob() {
		const worker = cableWorker();
		if (!worker || pads.cableInFlight || !pads.cableJob)
			return;
		pads.cableInFlight = pads.cableKey;
		worker.postMessage({ id: ++pads.cableAsked, ...pads.cableJob });
		pads.cableJob = null;
	}

	// Cables laid out for an older layout are hidden until the current one's come (they would lie where the ports
	// and controllers were).
	function setCablesStale(stale) {
		for (const id of ['cables', 'cables-front'])
			$(id).classList.toggle('stale', stale);
	}

	function drawCables() {
		pads.cableFrame = 0;
		const bar = $('ps2bar').getBoundingClientRect();
		const first = pads.units[0].art.getBoundingClientRect();
		const sx = window.scrollX, sy = window.scrollY;
		// Each cable's straight run, from the plug's centre (where it comes out of the boot) to the top of its controller.
		const runs = pads.units.map((u) => {
			const pr = els.ports[u.port - 1].getBoundingClientRect();
			const ar = u.art.getBoundingClientRect();
			if (!pr.width || !ar.width)
				return null;
			const x0 = pr.left + pr.width / 2 + sx;
			const y0 = pr.top + pr.height / 2 + sy;
			const x1 = ar.left + ar.width / 2 + sx;
			const y1 = ar.top + ar.height * PAD_CABLE_Y / PAD_FRAME_H + sy;
			if (u.port === 2 && ar.top > first.bottom) {
				// One controller under the other (narrow windows): the second cable goes down the margin beside the
				// first, not across it.
				const xs = Math.min(document.documentElement.clientWidth - 6, first.right + 10) + sx;
				const ya = first.top - 24 + sy;
				const yb = ar.top - 24 + sy;
				const k1 = Math.max(24, (ya - y0) * 0.5);
				const k2 = Math.max(16, (y1 - yb) * 0.6);
				return [[[x0, y0], [x0, y0 + k1], [xs, ya - k1], [xs, ya]],
					[[xs, ya], [xs, ya + (yb - ya) / 3], [xs, yb - (yb - ya) / 3], [xs, yb]],
					[[xs, yb], [xs, yb + k2], [x1, y1 - k2], [x1, y1]]];
			}
			const k = Math.max(24, (y1 - y0) * 0.55);
			return [[[x0, y0], [x0, y0 + k], [x1, y1 - k], [x1, y1]]];
		});
		const vw = document.documentElement.clientWidth;
		const arts = pads.units.map((u) => {
			const r = u.art.getBoundingClientRect();
			return { left: r.left + sx, right: r.right + sx, top: r.top + sy, bottom: r.bottom + sy };
		});
		const tr = document.createRange();
		tr.selectNodeContents($('pads-title'));
		const t = tr.getBoundingClientRect(); // the heading's words, not its whole row
		const avoid = t.width ? [{ left: t.left + sx - 8, right: t.right + sx + 8, top: t.top + sy - 8, bottom: t.bottom + sy + 8 }] : [];
		// The cables show in front of the page only over the console's face (cables-front).
		const clip = $('cables-front-rect');
		for (const [k, v] of [['x', bar.left + sx], ['y', bar.top + sy], ['width', bar.width], ['height', bar.height]])
			clip.setAttribute(k, v.toFixed(1));
		const job = { runs, arts, avoid, barBottom: bar.bottom + sy, box: { left: sx + CABLE_GAP, right: sx + vw - CABLE_GAP },
			params: pads.cableParams };
		const key = JSON.stringify(job);
		if (key !== pads.cableKey) {
			pads.cableKey = key;
			pads.cableJob = job;
			if (pads.cablePaths)
				setCablesStale(true);
			postCableJob();
		}
		pads.units.forEach((u, i) => {
			const d = (pads.cablePaths && pads.cablePaths[i]) || '';
			const on = String(u.el.dataset.plugged === 'true' || !!pads.cableParams); // the tuner shows both
			for (const p of u.cable) {
				if (p.getAttribute('d') !== d)
					p.setAttribute('d', d);
				if (p.dataset.on !== on)
					p.dataset.on = on;
			}
		});
		if (!pads.cablesLive) { // nothing slides in at load: only what changes later moves
			pads.cablesLive = true;
			requestAnimationFrame(() => requestAnimationFrame(() => {
				$('cables').classList.add('live');
				$('cables-front').classList.add('live');
				$('ps2bar').classList.add('live');
			}));
		}
	}

	// ---- the cable tuner (?tune-cables): sliders for how the cables lie, for the owner to try and copy out

	const PREF_CABLE_TUNE = 'pcsx2web.cableTune';
	// [setting, label, min, max, step]; the defaults are the worker's (cable-worker.js, CABLE_DEFAULTS).
	const CABLE_TUNE = [
		['seed1', 'Cable 1 shape (seed)', 0, 99999, 1],
		['seed2', 'Cable 2 shape (seed)', 0, 99999, 1],
		['slack', 'Slack (times the longer run)', 1, 1.8, 0.01],
		['extra', 'Extra length (px)', 0, 300, 5],
		['link', 'Link length (px)', 3, 10, 1],
		['bendMin', 'Bend stiffness, least', 0, 1, 0.01],
		['bendMax', 'Bend stiffness, most', 0, 1, 0.01],
		['bend4Min', 'Long bend stiffness, least', 0, 0.5, 0.01],
		['bend4Max', 'Long bend stiffness, most', 0, 0.5, 0.01],
		['memory', 'Coil memory', 0, 1.5, 0.05],
		['memoryWaves', 'Coil memory changes', 0, 8, 1],
		['dropWaves', 'Drop curves', 1, 9, 1],
		['dropDetail', 'Drop: small curves fade (higher, smoother)', 0.5, 3, 0.1],
		['friction', 'Drag', 0.02, 0.4, 0.01],
		['steps', 'Settling steps', 20, 800, 20],
		['smooth', 'Smoothing after settling', 0, 10, 1],
		['finalSmooth', 'Smoothing at the end', 0, 10, 1],
	];
	const CABLE_TUNE_DEFAULTS = {
		seed1: 94464, seed2: 70011, slack: 1.5, extra: 115, link: 9, bendMin: 0, bendMax: 0.34, bend4Min: 0.14,
		bend4Max: 0.09, memory: 0.5, memoryWaves: 5, dropWaves: 7, dropDetail: 1.4, friction: 0.12, steps: 380, smooth: 6,
		finalSmooth: 4,
	};

	function buildCableTuner() {
		let saved = {};
		try {
			saved = JSON.parse(readPref(PREF_CABLE_TUNE) || '{}') || {};
		} catch {
			saved = {};
		}
		const values = { ...CABLE_TUNE_DEFAULTS };
		for (const key of Object.keys(values))
			if (typeof saved[key] === 'number')
				values[key] = saved[key];
		pads.cableParams = { ...values };
		const panel = document.createElement('details');
		panel.className = 'cable-tuner';
		panel.open = true;
		const summary = document.createElement('summary');
		summary.textContent = 'Cable tuner';
		const note = document.createElement('p');
		note.className = 'cable-tuner-note';
		note.textContent = 'Move the sliders until the cables look right (try other window sizes too), then Copy settings and paste them in the chat.';
		const rows = document.createElement('div');
		rows.className = 'cable-tuner-rows';
		const out = document.createElement('textarea');
		out.className = 'cable-tuner-out';
		out.readOnly = true;
		out.rows = 4;
		out.setAttribute('aria-label', 'Settings to copy');
		let timer = 0;
		const apply = () => {
			writePref(PREF_CABLE_TUNE, JSON.stringify(values));
			out.value = JSON.stringify(values);
			clearTimeout(timer);
			timer = setTimeout(() => { // settle again once the slider rests a moment
				pads.cableParams = { ...values };
				scheduleCables();
			}, 150);
		};
		const inputs = new Map();
		for (const [key, label, min, max, step] of CABLE_TUNE) {
			const id = `cable-tune-${key}`;
			const row = document.createElement('div');
			row.className = 'cable-tuner-row';
			const lab = document.createElement('label');
			lab.htmlFor = id;
			lab.textContent = label;
			const range = document.createElement('input');
			range.type = key.startsWith('seed') ? 'number' : 'range';
			range.id = id;
			Object.assign(range, { min: String(min), max: String(max), step: String(step), value: String(values[key]) });
			const shown = document.createElement('output');
			shown.htmlFor = id;
			shown.textContent = String(values[key]);
			range.addEventListener('input', () => {
				const v = Number(range.value);
				if (!Number.isFinite(v))
					return;
				values[key] = v;
				shown.textContent = String(v);
				apply();
			});
			row.append(lab, range);
			if (key.startsWith('seed')) {
				const shuffle = document.createElement('button');
				shuffle.type = 'button';
				shuffle.className = 'btn-quiet';
				shuffle.textContent = 'Shuffle';
				shuffle.addEventListener('click', () => {
					range.value = String(Math.floor(Math.random() * 100000));
					range.dispatchEvent(new Event('input'));
				});
				row.append(shuffle);
			} else {
				row.append(shown);
			}
			rows.append(row);
			inputs.set(key, [range, shown]);
		}
		const actions = document.createElement('div');
		actions.className = 'cable-tuner-actions';
		const copy = document.createElement('button');
		copy.type = 'button';
		copy.className = 'btn-quiet';
		copy.textContent = 'Copy settings';
		copy.addEventListener('click', async () => {
			out.value = JSON.stringify(values);
			try {
				await navigator.clipboard.writeText(out.value);
				copy.textContent = 'Copied';
			} catch {
				out.select();
				copy.textContent = 'Select and copy below';
			}
			setTimeout(() => { copy.textContent = 'Copy settings'; }, 1600);
		});
		const reset = document.createElement('button');
		reset.type = 'button';
		reset.className = 'btn-quiet';
		reset.textContent = 'Reset';
		reset.addEventListener('click', () => {
			Object.assign(values, CABLE_TUNE_DEFAULTS);
			for (const [key, [range, shown]] of inputs) {
				range.value = String(values[key]);
				shown.textContent = String(values[key]);
			}
			apply();
		});
		actions.append(copy, reset);
		out.value = JSON.stringify(values);
		panel.append(summary, note, rows, actions, out);
		document.body.append(panel);
		scheduleCables();
	}

	// ---- gamepads the browser does not lay out in the standard way: the user maps them

	const PREF_PAD_MAPS = 'pcsx2web.padMaps';
	// A D-pad on one axis (a hat switch): eight positions clockwise from Up, 2/7 apart; at rest the axis
	// reads something above 1 (3.29 on an 8BitDo in D mode).
	const HAT_VALUES = [-1, -5 / 7, -3 / 7, -1 / 7, 1 / 7, 3 / 7, 5 / 7, 1];
	const HAT_DIRS = [['Up'], ['Up', 'Right'], ['Right'], ['Down', 'Right'], ['Down'], ['Down', 'Left'], ['Left'], ['Up', 'Left']];
	// A prompt listens at once: what is held when it appears counts only once let go and pressed again.
	const MAP_CAPTURE_MS = 300; // from the first sign of a press, what moves is collected until it is let go
	const MAP_RELEASE_MS = 2500; // longest wait for the control to be let go
	const MAP_REST = 0.25; // how close to where it started an axis must come back to be let go
	// The prompts, in order. kind: button, trigger (an axis or a button), dpad (buttons, or a hat for all four),
	// stickx and sticky (an axis; Y is asked pushed up).
	const MAP_PROMPTS = [
		{ id: 'Cross', kind: 'button', text: 'Press the bottom face button (Cross).' },
		{ id: 'Circle', kind: 'button', text: 'Press the right face button (Circle).' },
		{ id: 'Square', kind: 'button', text: 'Press the left face button (Square).' },
		{ id: 'Triangle', kind: 'button', text: 'Press the top face button (Triangle).' },
		{ id: 'L1', kind: 'button', text: 'Press the left bumper (L1).' },
		{ id: 'R1', kind: 'button', text: 'Press the right bumper (R1).' },
		{ id: 'L2', kind: 'trigger', text: 'Press the left trigger (L2) all the way.' },
		{ id: 'R2', kind: 'trigger', text: 'Press the right trigger (R2) all the way.' },
		{ id: 'Select', kind: 'button', text: 'Press Select (Back, View or Share).' },
		{ id: 'Start', kind: 'button', text: 'Press Start (Menu or Options).' },
		{ id: 'Up', kind: 'dpad', text: 'Press D-pad Up.' },
		{ id: 'Right', kind: 'dpad', text: 'Press D-pad Right.' },
		{ id: 'Down', kind: 'dpad', text: 'Press D-pad Down.' },
		{ id: 'Left', kind: 'dpad', text: 'Press D-pad Left.' },
		{ id: 'L3', kind: 'button', text: 'Click the left stick in (L3).' },
		{ id: 'R3', kind: 'button', text: 'Click the right stick in (R3).' },
		{ id: 'LX', kind: 'stickx', side: 'L', text: 'Push the left stick all the way right.' },
		{ id: 'LY', kind: 'sticky', side: 'L', text: 'Push the left stick all the way up.' },
		{ id: 'RX', kind: 'stickx', side: 'R', text: 'Push the right stick all the way right.' },
		{ id: 'RY', kind: 'sticky', side: 'R', text: 'Push the right stick all the way up.' },
		{ id: 'Analog', kind: 'button', text: 'Press the center button (Analog). Skip it if the gamepad has none.' },
	];

	const padMaps = (() => {
		try {
			const m = JSON.parse(readPref(PREF_PAD_MAPS) || '{}');
			return m && typeof m === 'object' ? m : {};
		} catch {
			return {};
		}
	})();
	// The numbering of buttons and axes depends on the browser as well as the gamepad: the counts tell them apart.
	const padMapKey = (gp) => `${gp.id}|${gp.buttons.length}|${gp.axes.length}`;
	const savePadMaps = () => writePref(PREF_PAD_MAPS, JSON.stringify(padMaps));

	function hatDirs(v) {
		for (let k = 0; k < 8; k++) {
			if (Math.abs(v - HAT_VALUES[k]) < 0.05)
				return HAT_DIRS[k];
		}
		return null;
	}

	// Reads a gamepad through the mapping the user made (g.map).
	function readCustom(gp, out, g) {
		out.fill(0);
		const m = g.map;
		if (!m)
			return;
		const put = (name, v) => {
			const i = PAD_INDEX[name];
			if (v > out[i])
				out[i] = v;
		};
		// How far an axis is from its rest towards its pressed value, 0 to 1.
		const reach = (b) => {
			const a = gp.axes[b.i];
			if (a === undefined)
				return 0;
			if (!g.seen[b.i]) { // some browsers report 0 until the axis first changes: wait for the rest value
				if (Math.abs(a - b.rest) < 0.15)
					g.seen[b.i] = true;
				else
					return 0;
			}
			const t = (a - b.rest) / (b.act - b.rest);
			return t > 0 ? Math.min(1, t) : 0;
		};
		for (const [name, b] of Object.entries(m.buttons || {})) {
			const analog = name === 'L2' || name === 'R2';
			if (b.k === 'b') {
				const x = gp.buttons[b.i];
				if (x)
					put(name, analog ? (x.value > TRIGGER_DEAD_ZONE ? x.value : x.pressed ? 1 : 0) : (x.pressed ? 1 : 0));
			} else if (b.k === 'a') {
				const t = reach(b);
				put(name, analog ? (t > TRIGGER_DEAD_ZONE ? t : 0) : (t > 0.5 ? 1 : 0));
			} else if (b.k === 'h') {
				const dirs = hatDirs(gp.axes[b.i]);
				if (dirs && dirs.includes(name))
					put(name, 1);
			}
		}
		const q = (v) => Math.round(Math.min(1, Math.abs(v)) * STICK_STEPS) / STICK_STEPS;
		for (const [side, up, down, left, right] of [['L', 'LUp', 'LDown', 'LLeft', 'LRight'], ['R', 'RUp', 'RDown', 'RLeft', 'RRight']]) {
			const st = m.sticks && m.sticks[side];
			if (!st)
				continue;
			const [sx, sy] = stickValue(st.x ? (gp.axes[st.x.i] || 0) * st.x.s : 0, st.y ? (gp.axes[st.y.i] || 0) * st.y.s : 0);
			put(sy < 0 ? up : down, q(sy));
			put(sx < 0 ? left : right, q(sx));
		}
	}

	// What is pressed or moved now that was not at the start of the prompt.
	function mapActivity(kind, gp, snap) {
		const found = [];
		if (kind !== 'stickx' && kind !== 'sticky') {
			gp.buttons.forEach((b, i) => {
				if ((b.pressed || b.value > 0.5) && !snap.buttons[i])
					found.push({ k: 'b', i });
			});
		}
		const threshold = kind === 'dpad' ? 0.3 : 0.5;
		gp.axes.forEach((a, i) => {
			if (Math.abs(a) > 1.05) // a hat's resting value
				return;
			if (Math.abs(a - snap.axes[i]) > threshold || (kind === 'dpad' && Math.abs(snap.axes[i]) > 1.05 && hatDirs(a)))
				found.push({ k: 'a', i, v: a });
		});
		return found;
	}

	function bindingFrom(step, cap, snap) {
		const axes = [...cap.axes.entries()].sort((x, y) => Math.abs(y[1] - snap.axes[y[0]]) - Math.abs(x[1] - snap.axes[x[0]]));
		const button = cap.buttons.size ? [...cap.buttons.keys()][0] : null;
		const axis = axes.length ? { i: axes[0][0], rest: snap.axes[axes[0][0]], act: axes[0][1] } : null;
		switch (step.kind) {
		case 'trigger':
			return axis ? { k: 'a', ...axis } : button !== null ? { k: 'b', i: button } : null;
		case 'dpad': {
			if (button !== null)
				return { k: 'b', i: button };
			if (!axis)
				return null;
			const hat = Math.abs(axis.rest) > 1.05 || [1, 2, 3, 4, 5, 6].some((k) => Math.abs(axis.act - HAT_VALUES[k]) < 0.05);
			return hat ? { k: 'h', i: axis.i } : { k: 'a', ...axis };
		}
		case 'stickx':
		case 'sticky':
			if (!axis)
				return null;
			return { i: axis.i, s: Math.sign(axis.act - axis.rest) * (step.kind === 'sticky' ? -1 : 1) || 1 };
		default:
			return button !== null ? { k: 'b', i: button } : axis ? { k: 'a', ...axis } : null;
		}
	}

	function describeBinding(b) {
		if (!b)
			return 'nothing seen';
		if (b.k === 'b')
			return `button ${b.i}`;
		if (b.k === 'h')
			return `hat on axis ${b.i}`;
		if (b.k === 'a')
			return `axis ${b.i}`;
		return `axis ${b.i}`;
	}

	function startMapping(g, ids) {
		const steps = MAP_PROMPTS.filter((p) => !ids || ids.includes(p.id));
		if (!steps.length || g.mapper)
			return;
		const draft = JSON.parse(JSON.stringify(g.map || (g.custom ? { buttons: {}, sticks: {} } : STANDARD_MAP)));
		draft.buttons = draft.buttons || {};
		draft.sticks = draft.sticks || {};
		g.mapper = { steps, i: 0, draft, phase: 'start', until: 0, snap: null, cap: null, skip: new Set(), last: '' };
		g.state.fill(0);
		syncPads();
		renderMapUi(g);
	}

	function mapperAdvance(g, to) {
		const m = g.mapper;
		m.i = to;
		while (m.i < m.steps.length && m.skip.has(m.steps[m.i].id))
			m.i++;
		m.phase = 'start';
		m.cap = null;
		if (m.i >= m.steps.length) {
			g.map = m.draft;
			padMaps[g.key] = g.map;
			savePadMaps();
			g.mapper = null;
			g.seen = [];
			g.needsNeutral = true;
			announce(COPY.announce.gamepadMapped(g.name));
			pageLog(`Gamepad mapped: ${g.name}.`);
		}
		renderMapUi(g);
	}

	function mapperStore(g, step, binding) {
		const d = g.mapper.draft;
		if (step.kind === 'stickx' || step.kind === 'sticky') {
			d.sticks[step.side] = d.sticks[step.side] || {};
			if (binding)
				d.sticks[step.side][step.kind === 'stickx' ? 'x' : 'y'] = binding;
			else
				delete d.sticks[step.side][step.kind === 'stickx' ? 'x' : 'y'];
		} else if (binding) {
			// Changing one control on its controller: the control that had this binding takes this one's old one.
			const prev = d.buttons[step.id];
			const same = (b) => b.k === binding.k && b.i === binding.i && (b.k !== 'a' || (b.act > b.rest) === (binding.act > binding.rest));
			if (g.mapper.swap && binding.k !== 'h') {
				for (const [name, b] of Object.entries(d.buttons)) {
					if (name !== step.id && same(b)) {
						if (prev && prev.k !== 'h')
							d.buttons[name] = prev;
						else
							delete d.buttons[name];
					}
				}
			}
			d.buttons[step.id] = binding;
			if (binding.k === 'h') { // a hat is all four directions
				for (const dir of ['Up', 'Right', 'Down', 'Left']) {
					d.buttons[dir] = binding;
					g.mapper.skip.add(dir);
				}
			}
		} else {
			delete d.buttons[step.id];
		}
	}

	// One animation frame of a gamepad's mapping run.
	function mapperFrame(g, gp, now) {
		const m = g.mapper;
		const step = m.steps[m.i];
		const held = (b) => b.pressed || b.value > 0.5;
		if (m.phase === 'start') {
			m.snap = { buttons: gp.buttons.map(held), axes: [...gp.axes] };
			m.phase = 'wait';
		}
		if (m.phase === 'wait') {
			gp.buttons.forEach((b, i) => {
				if (m.snap.buttons[i] && !held(b))
					m.snap.buttons[i] = false; // let go: its next press counts
			});
		}
		const found = mapActivity(step.kind, gp, m.snap);
		if (m.phase === 'wait') {
			if (!found.length)
				return;
			m.phase = 'capture';
			m.until = now + MAP_CAPTURE_MS;
			m.cap = { buttons: new Map(), axes: new Map() };
		}
		if (m.phase === 'capture') {
			for (const f of found) {
				if (f.k === 'b') {
					m.cap.buttons.set(f.i, true);
				} else {
					const prev = m.cap.axes.get(f.i);
					if (prev === undefined || Math.abs(f.v - m.snap.axes[f.i]) > Math.abs(prev - m.snap.axes[f.i]))
						m.cap.axes.set(f.i, f.v);
				}
			}
			if (found.length && now < m.until)
				return;
			const binding = bindingFrom(step, m.cap, m.snap);
			mapperStore(g, step, binding);
			m.last = `${step.id}: ${describeBinding(binding)}`;
			m.phase = 'release';
			m.until = now + MAP_RELEASE_MS;
			renderMapUi(g);
			return;
		}
		// The next prompt starts once the control is let go, axes back near where they started, so that a
		// stick or trigger on its way back does not count for it.
		const back = () => [...m.cap.axes.keys()].every((i) => Math.abs(gp.axes[i] - m.snap.axes[i]) < MAP_REST);
		if (m.phase === 'release' && ((!found.length && back()) || now >= m.until))
			mapperAdvance(g, m.i + 1);
	}

	function mapperSkip(g) {
		const m = g.mapper;
		if (!m)
			return;
		mapperStore(g, m.steps[m.i], null);
		mapperAdvance(g, m.i + 1);
	}

	function mapperBack(g) {
		const m = g.mapper;
		if (!m)
			return;
		m.skip.clear();
		m.last = '';
		mapperAdvance(g, Math.max(0, m.i - 1));
	}

	function mapperCancel(g) {
		if (!g.mapper)
			return;
		g.mapper = null;
		g.needsNeutral = true;
		renderMapUi(g);
		announce(COPY.announce.gamepadMapCancelled);
	}

	function clearMapping(g) {
		delete padMaps[g.key];
		savePadMaps();
		g.map = null;
		g.seen = [];
		g.state.fill(0);
		renderMapUi(g);
		syncPads();
		announce(COPY.announce.gamepadMapCleared(g.name));
	}

	function renderMapUi(g) {
		renderPads();
		const v = g.view;
		if (!v)
			return;
		const ui = v.map;
		ui.el.hidden = !g.custom;
		v.el.classList.toggle('gp-mappable', g.custom && !g.mapper);
		const m = g.mapper;
		const step = m ? m.steps[m.i] : null;
		for (const [i, cap] of v.caps)
			cap.classList.toggle('target', !!step && step.id === PAD_INPUTS[i]);
		for (const s of v.sticks)
			s.el.classList.toggle('target', !!step && step.side === (s.left ? 'L' : 'R'));
		if (!g.custom)
			return;
		ui.idle.hidden = !!m;
		ui.run.hidden = !m;
		ui.note.hidden = !!m;
		ui.note.textContent = g.map ?
			'You mapped this gamepad. Press its controls to check that the right ones light up, or click one in the picture to map it again.' :
			'This gamepad does not report a standard layout, so the page cannot tell which control is which. Map it once: press each control when asked. It is remembered for this gamepad in this browser.';
		ui.start.textContent = g.map ? 'Map again' : 'Map controls';
		ui.clear.hidden = !g.map;
		if (m) {
			const done = m.i >= m.steps.length;
			ui.step.textContent = done ? '' : `Step ${m.i + 1} of ${m.steps.length}${m.last ? ` · Last: ${m.last}` : ''}`;
			ui.prompt.textContent = done ? '' : step.text;
			ui.back.disabled = m.i === 0;
		}
	}

	// ---- Keys drawer: the Gamepad tab

	function renderGamepads() {
		renderPorts();
		const connected = [...gamepads.values()].filter((g) => g.connected).sort((a, b) => a.rank - b.rank);
		els.gpEmpty.hidden = connected.length > 0;
		els.gpActions.hidden = connected.filter((g) => g.port).length < 2;
		for (const g of gamepads.values()) {
			if (!g.connected && g.view) {
				g.view.el.remove();
				g.view = null;
			}
		}
		for (const g of connected) {
			if (!g.view)
				g.view = buildGamepadView(g);
			g.view.name.textContent = g.name;
			g.view.legend.textContent = `Player for ${g.name}`;
			g.view.players.hidden = !g.port;
			g.view.unused.hidden = !!g.port;
			for (const r of g.view.radios)
				r.checked = Number(r.value) === g.port;
			renderMapUi(g);
			els.gpList.append(g.view.el); // in order
		}
	}

	function buildGamepadView(g) {
		const el = els.gpTemplate.content.firstElementChild.cloneNode(true);
		const radios = [...el.querySelectorAll('input[type="radio"]')];
		for (const r of radios) {
			r.name = `gp-player-${g.index}`;
			r.addEventListener('change', () => {
				if (r.checked)
					setGamepadPort(g, Number(r.value));
			});
		}
		const caps = new Map();
		for (const c of el.querySelectorAll('.cap[data-input]'))
			caps.set(PAD_INDEX[c.dataset.input], c);
		const view = {
			el,
			name: el.querySelector('.gp-name'),
			legend: el.querySelector('legend'),
			players: el.querySelector('.players'),
			unused: el.querySelector('.gp-unused'),
			radios,
			caps,
			sticks: [...el.querySelectorAll('.stick')].map((s) => ({ el: s, left: s.dataset.stick === 'L' })),
			map: {
				el: el.querySelector('.gp-map'),
				note: el.querySelector('.gp-map-note'),
				idle: el.querySelector('.gp-map-idle'),
				run: el.querySelector('.gp-map-run'),
				step: el.querySelector('.gp-map-step'),
				prompt: el.querySelector('.gp-map-prompt'),
				start: el.querySelector('.gp-map-start'),
				clear: el.querySelector('.gp-map-clear'),
				back: el.querySelector('.gp-map-back'),
			},
		};
		view.map.start.addEventListener('click', () => startMapping(g));
		view.map.clear.addEventListener('click', () => clearMapping(g));
		el.querySelector('.gp-map-skip').addEventListener('click', () => mapperSkip(g));
		el.querySelector('.gp-map-back').addEventListener('click', () => mapperBack(g));
		el.querySelector('.gp-map-cancel').addEventListener('click', () => mapperCancel(g));
		// With a mapping run not going, a click on a control in the picture maps just that one (a D-pad
		// button maps the whole D-pad, a stick both its axes).
		for (const cap of el.querySelectorAll('.cap[data-input]')) {
			cap.addEventListener('click', () => {
				if (!g.custom || g.mapper)
					return;
				const name = cap.dataset.input;
				startMapping(g, ['Up', 'Right', 'Down', 'Left'].includes(name) ? ['Up', 'Right', 'Down', 'Left'] : [name]);
			});
		}
		for (const s of el.querySelectorAll('.stick')) {
			s.addEventListener('click', () => {
				if (g.custom && !g.mapper)
					startMapping(g, [s.dataset.stick + 'X', s.dataset.stick + 'Y']);
			});
		}
		return view;
	}

	function updateGamepadView(g) {
		if (page.drawer !== 'keys' || els.panelGamepad.hidden)
			return;
		const v = g.view;
		const raw = g.shown;
		for (const [i, cap] of v.caps)
			cap.classList.toggle('down', raw[i] > 0);
		for (const s of v.sticks) {
			const [up, down, left, right] = s.left ? ['LUp', 'LDown', 'LLeft', 'LRight'] : ['RUp', 'RDown', 'RLeft', 'RRight'];
			const x = raw[PAD_INDEX[right]] - raw[PAD_INDEX[left]];
			const y = raw[PAD_INDEX[down]] - raw[PAD_INDEX[up]];
			s.el.style.setProperty('--sx', x.toFixed(3));
			s.el.style.setProperty('--sy', y.toFixed(3));
			s.el.classList.toggle('down', x !== 0 || y !== 0);
		}
	}

	function selectControlsTab(name, focus) {
		const gamepad = name === 'gamepad';
		els.tabKeyboard.setAttribute('aria-selected', String(!gamepad));
		els.tabGamepad.setAttribute('aria-selected', String(gamepad));
		els.tabKeyboard.tabIndex = gamepad ? -1 : 0;
		els.tabGamepad.tabIndex = gamepad ? 0 : -1;
		els.panelKeyboard.hidden = gamepad;
		els.panelGamepad.hidden = !gamepad;
		if (focus)
			(gamepad ? els.tabGamepad : els.tabKeyboard).focus();
		syncScreenWidth();
	}

	function wirePlayersUi() {
		for (const r of els.kbPlayers) {
			r.checked = Number(r.value) === players.keyboard;
			r.addEventListener('change', () => {
				if (r.checked)
					setKeyboardPlayer(Number(r.value));
			});
		}
		els.tabKeyboard.addEventListener('click', () => selectControlsTab('keyboard', false));
		els.tabGamepad.addEventListener('click', () => selectControlsTab('gamepad', false));
		for (const tab of [els.tabKeyboard, els.tabGamepad]) {
			tab.addEventListener('keydown', (e) => {
				if (e.altKey || e.ctrlKey || e.metaKey || e.shiftKey)
					return;
				let to = null;
				if (e.key === 'ArrowLeft' || e.key === 'ArrowRight')
					to = tab === els.tabKeyboard ? 'gamepad' : 'keyboard';
				else if (e.key === 'Home')
					to = 'keyboard';
				else if (e.key === 'End')
					to = 'gamepad';
				if (to) {
					e.preventDefault();
					selectControlsTab(to, true);
				}
			});
		}
		els.gpSwap.addEventListener('click', swapGamepads);
		buildPads();
		renderPorts();
		window.addEventListener('gamepadconnected', startGamepadLoop);
		if (GAMEPAD_EVENTS) {
			window.addEventListener('gamepadbuttondown', onGamepadEvent);
			window.addEventListener('gamepadbuttonup', onGamepadEvent);
			window.addEventListener('gamepadaxismove', onGamepadEvent);
		}
		els.gpTaps.hidden = !IS_FIREFOX || GAMEPAD_EVENTS;
		window.addEventListener('gamepaddisconnected', startGamepadLoop);
		// A gamepad that was connected before the page opened shows up after its first button press;
		// look once in case the browser already lists it.
		startGamepadLoop();
	}

	// ------------------------------------------------------------------ graphics (UI-SPEC.md §12)

	const gfxEls = {
		btn: $('btn-gfx'),
		menu: $('gfx-menu'),
		mode: $('gfx-mode'),
		why: $('gfx-why'),
		applies: $('gfx-applies'),
		check: $('gfx-check'),
		result: $('gfx-result'),
		radios: [...document.querySelectorAll('input[name="gfx-choice"]')],
		scaleRadios: [...document.querySelectorAll('input[name="gfx-scale"]')],
		scaleNote: $('gfx-scale-note'),
		pictureRadios: [...document.querySelectorAll('input[name="gfx-picture"]')],
		pictureNote: $('gfx-picture-note'),
	};
	// The HW renderer's one hard requirement, then the features it uses when they are there
	// (GSDeviceWebGPU::CreateInstanceAndDevice()).
	const GFX_REQUIRED = ['dual-source-blending'];
	const GFX_OPTIONAL = ['depth32float-stencil8', 'texture-compression-bc', 'float32-filterable', 'float32-blendable',
		'primitive-index', 'texture-formats-tier1'];
	let gfxChecking = false;

	function gfxMenuOpen() {
		try {
			return gfxEls.menu.matches(':popover-open');
		} catch {
			return false;
		}
	}

	function hideGfxMenu() {
		if (gfxMenuOpen())
			gfxEls.menu.hidePopover();
	}

	function placeGfxMenu() {
		const r = gfxEls.btn.getBoundingClientRect();
		const width = Math.min(320, window.innerWidth - 32);
		const right = Math.max(16, Math.min(window.innerWidth - r.right, window.innerWidth - width - 16));
		gfxEls.menu.style.right = `${Math.round(right)}px`;
		gfxEls.menu.style.bottom = `${Math.round(Math.max(8, window.innerHeight - r.top + 8))}px`;
	}

	function gfxModeName(mode) {
		if (!mode)
			return 'Not started yet';
		if (mode.hw)
			return 'GPU renderer';
		return mode.webgpu ? 'GPU display, software drawing' : 'Software on a 2D canvas';
	}

	// Why WebGPU (or its renderer) is not used, in plain words; empty when the GPU renderer runs.
	function gfxWhy(mode) {
		if (!mode || mode.hw)
			return '';
		if (mode.reason)
			return mode.reason;
		if (!GPU_ALLOWED)
			return URL_GRAPHICS ? 'the address has ?gpu=0' : 'you chose Software';
		if (!navigator.gpu)
			return 'this browser has no WebGPU';
		if (!HAS_JSPI)
			return 'WebGPU needs JSPI, which this browser lacks';
		if (RENDERER_PARAM === 'sw')
			return 'the address has ?renderer=sw';
		return gpu.fallback || '';
	}

	function currentChoice() {
		const r = gfxEls.radios.find((x) => x.checked);
		return r ? r.value : 'auto';
	}

	function renderGfx() {
		const name = gfxModeName(gpu.mode);
		const why = gfxWhy(gpu.mode);
		gfxEls.mode.textContent = name;
		// No WebGPU although it was allowed: the flags hint says how to turn it on (Linux Chromium).
		const hint = gpu.mode && !gpu.mode.webgpu && GPU_ALLOWED ? gfxFlagsHint() : '';
		gfxEls.why.hidden = !why;
		gfxEls.why.textContent = why ? `Because ${why}.${hint ? ` ${hint}` : ''}` : '';
		gfxEls.btn.setAttribute('aria-label', gpu.mode ? `Graphics, ${name}` : 'Graphics');
		const changed = currentChoice() !== RENDERER_CHOICE;
		gfxEls.applies.textContent = URL_GRAPHICS ? 'The address’s ?gpu= or ?renderer= decides for this visit.' :
			changed ? 'Applies when the page is reloaded.' : 'Remembered in this browser. A change applies after a reload.';
		for (const r of gfxEls.scaleRadios)
			r.checked = Number(r.value) === page.upscale;
		// Only the GPU renderer draws above native; the software renderer ignores it.
		gfxEls.scaleNote.textContent = gpu.mode && !gpu.mode.hw ?
			'Only the GPU renderer draws at 2×; this picture is drawn in software. Remembered in this browser.' :
			'Remembered in this browser. Applies at once.';
		// The CRT pictures are shaders of the WebGPU device; the 2D canvas has none.
		const noShaders = display.kind === '2d';
		for (const r of gfxEls.pictureRadios) {
			r.checked = r.value === page.picture;
			r.disabled = noShaders && PICTURE_MODES[r.value].shader !== 0;
		}
		gfxEls.pictureNote.textContent = noShaders ?
			'CRT needs WebGPU graphics; this picture is on a 2D canvas. Remembered in this browser.' :
			'Remembered in this browser. Applies at once.';
	}

	// Linux desktop Chromium ships WebGPU off for most GPUs; the flags page turns it on.
	function gfxFlagsHint() {
		const ua = navigator.userAgent;
		if (!/Linux/.test(ua) || /Android/.test(ua) || !/Chrom(e|ium)\//.test(ua))
			return '';
		const flags = navigator.brave ? 'brave://flags' : /Edg\//.test(ua) ? 'edge://flags' : /OPR\//.test(ua) ? 'opera://flags' : 'chrome://flags';
		return `On Linux this browser keeps WebGPU off for most GPUs. Open ${flags}, enable “Unsafe WebGPU Support” and “Vulkan”, then relaunch the browser.`;
	}

	function showGfxResult(lines) {
		gfxEls.result.replaceChildren(...lines.filter(Boolean).map((t) => {
			const p = document.createElement('p');
			p.textContent = t;
			return p;
		}));
		pageLog(`Check WebGPU: ${lines.filter(Boolean).join(' ')}`);
	}

	// Asks this page (not the emulator's worker) for an adapter and a device with the features the GPU
	// renderer uses, and says what it got.
	async function checkWebGPU() {
		if (gfxChecking)
			return;
		gfxChecking = true;
		gfxEls.check.setAttribute('aria-disabled', 'true');
		gfxEls.result.replaceChildren(Object.assign(document.createElement('p'), { textContent: 'Checking…' }));
		const hint = gfxFlagsHint();
		try {
			if (!navigator.gpu) {
				showGfxResult(['No WebGPU: this browser doesn’t offer it here.', hint]);
				return;
			}
			let adapter = null;
			for (const options of [{ powerPreference: 'high-performance' }, {}]) {
				adapter = await navigator.gpu.requestAdapter(options).catch(() => null);
				if (adapter)
					break;
			}
			if (!adapter) {
				showGfxResult(['WebGPU is there, but the browser gave no adapter: it has WebGPU turned off for this GPU, or the GPU is blocklisted.',
					hint || 'Check that hardware acceleration is on in the browser’s settings.']);
				return;
			}
			const info = adapter.info || {};
			const gpuName = [info.vendor, info.architecture, info.description].filter(Boolean).join(' ') || 'unknown GPU';
			const missingRequired = GFX_REQUIRED.filter((f) => !adapter.features.has(f));
			const missingOptional = GFX_OPTIONAL.filter((f) => !adapter.features.has(f));
			let device = null, refused = '';
			try {
				device = await adapter.requestDevice({ requiredFeatures: [...GFX_REQUIRED, ...GFX_OPTIONAL].filter((f) => adapter.features.has(f)) });
			} catch (e) {
				refused = String(e && e.message || e);
			}
			if (device)
				device.destroy();
			const lines = [];
			if (!device)
				lines.push(`The browser gave an adapter (${gpuName}) but refused a device: ${refused || 'no reason given'}.`);
			else if (missingRequired.length)
				lines.push(`WebGPU works on ${gpuName}, but it lacks ${missingRequired.join(', ')}, which the GPU renderer needs: the picture is drawn in software and shown through WebGPU.`);
			else
				lines.push(`WebGPU works on ${gpuName}${info.isFallbackAdapter || adapter.isFallbackAdapter ? ' (a software fallback adapter, slow)' : ''}. The GPU renderer can run.`);
			if (device && missingOptional.length)
				lines.push(`Missing optional features: ${missingOptional.join(', ')}.`);
			if (device && !HAS_JSPI)
				lines.push('The emulator can’t use it in this browser, though: it needs JSPI (WebAssembly stack switching).');
			showGfxResult(lines);
		} catch (e) {
			showGfxResult([`The check failed: ${e && e.message || e}.`, hint]);
		} finally {
			gfxChecking = false;
			gfxEls.check.removeAttribute('aria-disabled');
		}
	}

	function wireGfxUi() {
		for (const r of gfxEls.radios) {
			r.checked = r.value === RENDERER_CHOICE;
			r.addEventListener('change', () => {
				if (!r.checked)
					return;
				writePref(PREF_RENDERER, r.value);
				pageLog(`Graphics choice: ${r.value} (applies after a reload).`);
				renderGfx();
			});
		}
		page.upscale = readPref(PREF_UPSCALE) === '2' ? 2 : 1;
		for (const r of gfxEls.scaleRadios) {
			r.addEventListener('change', () => {
				if (!r.checked)
					return;
				page.upscale = Number(r.value) === 2 ? 2 : 1;
				writePref(PREF_UPSCALE, String(page.upscale));
				if (api)
					api.setUpscale(page.upscale);
				pageLog(`Graphics resolution: ${page.upscale === 2 ? '2×' : 'native'}.`);
				renderGfx();
			});
		}
		for (const r of gfxEls.pictureRadios) {
			r.addEventListener('change', () => {
				if (!r.checked)
					return;
				setPicture(r.value);
				writePref(PREF_PICTURE, r.value);
				subtitle(`Picture: ${PICTURE_MODES[r.value].name}.`);
			});
		}
		gfxEls.check.addEventListener('click', checkWebGPU);
		gfxEls.menu.addEventListener('beforetoggle', (e) => {
			if (e.newState === 'open') {
				renderGfx();
				placeGfxMenu();
			}
		});
		gfxEls.menu.addEventListener('toggle', (e) => {
			const open = e.newState === 'open';
			gfxEls.btn.setAttribute('aria-expanded', open ? 'true' : 'false');
			if (open) {
				(gfxEls.radios.find((x) => x.checked) || gfxEls.radios[0]).focus({ preventScroll: true });
			} else {
				const a = document.activeElement;
				if (!a || a === document.body || gfxEls.menu.contains(a))
					gfxEls.btn.focus({ preventScroll: true });
			}
			updateChrome();
		});
		gfxEls.menu.addEventListener('focusout', (e) => {
			const to = e.relatedTarget;
			if (to && !gfxEls.menu.contains(to) && to !== gfxEls.btn)
				hideGfxMenu();
		});
		for (const type of ['resize', 'fullscreenchange'])
			(type === 'resize' ? window : document).addEventListener(type, () => {
				if (gfxMenuOpen())
					placeGfxMenu();
			});
		renderGfx();
	}

	// ------------------------------------------------------------------ underclock (UI-SPEC.md §12a)

	// PCSX2's EE Cycle Rate speedhack, chosen per game (by serial) and remembered in this browser: the PS2's main
	// processor runs fewer instructions per frame, which lightens heavy scenes; some games then run in slow motion or
	// glitch, so it is off unless chosen.
	const PREF_EE_RATE = 'pcsx2web.eeRate.';
	const EE_RATE_NAMES = { 0: 'off', '-1': 'light', '-2': 'medium', '-3': 'strong' };
	const eeEls = {
		btn: $('btn-ee'),
		menu: $('ee-menu'),
		game: $('ee-game'),
		reason: $('ee-reason'),
		radios: [...document.querySelectorAll('input[name="ee-rate"]')],
	};
	const ee = { serial: '', rate: 0 };

	function eeMenuOpen() {
		try {
			return eeEls.menu.matches(':popover-open');
		} catch {
			return false;
		}
	}

	function placeEeMenu() {
		const r = eeEls.btn.getBoundingClientRect();
		const width = Math.min(320, window.innerWidth - 32);
		const right = Math.max(16, Math.min(window.innerWidth - r.right, window.innerWidth - width - 16));
		eeEls.menu.style.right = `${Math.round(right)}px`;
		eeEls.menu.style.bottom = `${Math.round(Math.max(8, window.innerHeight - r.top + 8))}px`;
	}

	function eeStoredRate(serial) {
		const v = Number(readPref(PREF_EE_RATE + serial));
		return v === -1 || v === -2 || v === -3 ? v : 0;
	}

	function eeApply(rate) {
		ee.rate = rate;
		const M = window.Module;
		if (M && typeof M._pcsx2web_set_ee_cycle_rate === 'function')
			M._pcsx2web_set_ee_cycle_rate(rate);
	}

	// bootRequest(): every boot starts at full speed, until the game is known.
	function eeBoot() {
		ee.serial = '';
		eeApply(0);
		renderEe();
	}

	// The running game's serial is known: its own choice.
	function eeGameKnown(serial) {
		ee.serial = serial || '';
		const rate = ee.serial ? eeStoredRate(ee.serial) : 0;
		eeApply(rate);
		if (rate)
			pageLog(`Underclock for ${ee.serial}: ${EE_RATE_NAMES[rate]} (EE cycle rate ${rate}).`);
		renderEe();
	}

	function renderEe() {
		const running = page.vmState === STATE.RUNNING || page.vmState === STATE.STARTING;
		const known = running && !!ee.serial;
		for (const r of eeEls.radios) {
			r.checked = Number(r.value) === (known ? ee.rate : 0);
			r.disabled = !known;
		}
		eeEls.game.textContent = known ? (disc.runTitle || ee.serial) : running ? 'Not a game with a serial' : 'No game running';
		eeEls.reason.textContent = known ? 'Remembered for this game. Some games run in slow motion or glitch underclocked.' :
			'Start a game to choose. Each game keeps its own choice.';
		eeEls.btn.setAttribute('aria-label', `Underclock, ${EE_RATE_NAMES[known ? ee.rate : 0]}`);
		eeEls.btn.classList.toggle('on', known && ee.rate !== 0);
		const tip = eeEls.btn.querySelector('.tip');
		if (tip)
			tip.textContent = known && ee.rate ? `Underclock: ${EE_RATE_NAMES[ee.rate]}` : 'Underclock';
	}

	function wireEeUi() {
		for (const r of eeEls.radios) {
			r.addEventListener('change', () => {
				if (!r.checked || !ee.serial)
					return;
				const rate = Number(r.value);
				writePref(PREF_EE_RATE + ee.serial, String(rate));
				eeApply(rate);
				pageLog(`Underclock for ${ee.serial}: ${EE_RATE_NAMES[rate]} (EE cycle rate ${rate}).`);
				renderEe();
			});
		}
		eeEls.menu.addEventListener('beforetoggle', (e) => {
			if (e.newState === 'open') {
				renderEe();
				placeEeMenu();
			}
		});
		eeEls.menu.addEventListener('toggle', (e) => {
			const open = e.newState === 'open';
			eeEls.btn.setAttribute('aria-expanded', open ? 'true' : 'false');
			if (open) {
				const first = eeEls.radios.find((x) => x.checked && !x.disabled) || eeEls.radios.find((x) => !x.disabled);
				(first || eeEls.menu).focus({ preventScroll: true });
			} else {
				const a = document.activeElement;
				if (!a || a === document.body || eeEls.menu.contains(a))
					eeEls.btn.focus({ preventScroll: true });
			}
			updateChrome();
		});
		eeEls.menu.addEventListener('focusout', (e) => {
			const to = e.relatedTarget;
			if (to && !eeEls.menu.contains(to) && to !== eeEls.btn)
				eeEls.menu.hidePopover();
		});
		for (const type of ['resize', 'fullscreenchange'])
			(type === 'resize' ? window : document).addEventListener(type, () => {
				if (eeMenuOpen())
					placeEeMenu();
			});
		renderEe();
	}

	// ------------------------------------------------------------------ save states (UI-SPEC.md §19)

	// Snapshots of the whole PS2 (PCSX2's .p2s), kept per game serial in IndexedDB, at most STATES_PER_GAME each. The
	// emulator writes and reads them in /states through pcsx2web_save_state() and pcsx2web_load_state(), the same
	// calls as the Recorder's developer states (devState), which share their busy lock.
	const STATES_IDB_NAME = 'pcsx2-web-states';
	const STATES_STORE = 'states';
	const STATES_PER_GAME = 10;
	const STATE_BIOS_KEY = '(console)';
	const STATE_THUMB_W = 128, STATE_THUMB_H = 96;
	const STATE_THUMB_WAIT_MS = 400;
	const P2S_MAGIC = [0x50, 0x4b, 0x03, 0x04]; // a .p2s is a zip archive
	const STATE_COPY = {
		noGame: 'No game running',
		consoleMenus: 'The console’s menus',
		startFirst: 'Start a game to save its state.',
		unknownYet: 'Saving waits until the game is known.',
		kept: 'Kept in this browser.',
		full: `${STATES_PER_GAME} states: saving replaces the oldest.`,
		versions: 'States only load into the game they were saved from, in this version of TabStationToo or one close to it.',
		saving: 'Saving…',
		loading: 'Loading…',
		newest: 'Newest · F3',
		btnName: (n) => n ? `Save states, ${n} for this game` : 'Save states',
		moreFor: (when) => `More for the state of ${when}`,
		saved: 'State saved.',
		loaded: (when) => `State loaded: ${when}.`,
		deleted: 'State deleted.',
		noneYet: 'No state for this game yet. F1 saves one.',
		saveFailed: 'Couldn’t save the state. The log may say why.',
		loadFailed: 'That state didn’t load. It may be from another version of TabStationToo.',
		cardBusy: 'The game is saving to a memory card. Try again in a few seconds.',
		noRoom: 'The browser has no room for another state. Delete one or save a copy to disk.',
		unreadable: 'Couldn’t read that file. Try choosing it again.',
		notState: 'That file isn’t a PS2 save state (.p2s).',
	};
	const stEls = {
		btn: $('btn-state'),
		menu: $('state-menu'),
		game: $('state-game'),
		save: $('state-new'),
		saveLabel: $('state-new-label'),
		list: $('state-list'),
		none: $('state-none'),
		open: $('state-open'),
		reason: $('state-reason'),
		views: [...document.querySelectorAll('#state-menu .state-view')],
		moreName: $('state-more-name'),
		moreWhere: $('state-more-where'),
		copy: $('state-copy'),
		del: $('state-delete'),
		back: $('state-back'),
		error: $('state-error'),
		input: $('player-state-input'),
	};
	// key: the running game's states key (its serial, STATE_BIOS_KEY, or '' while unknown or stopped); list: its
	// records without their bytes, newest first; busy: 'save', or the id of the state being loaded ('file' for one
	// opened from disk); thumbWait: resolves with the next WebGPU frame's thumbnail.
	const savestates = { db: null, key: '', list: [], busy: null, view: 'list', target: null, thumbWait: null,
		listKey: '', loadSeq: 0 };

	function stateMenuOpen() {
		try {
			return stEls.menu.matches(':popover-open');
		} catch {
			return false;
		}
	}

	function placeStateMenu() {
		const r = stEls.btn.getBoundingClientRect();
		const width = Math.min(320, window.innerWidth - 32);
		const right = Math.max(16, Math.min(window.innerWidth - r.right, window.innerWidth - width - 16));
		stEls.menu.style.right = `${Math.round(right)}px`;
		stEls.menu.style.bottom = `${Math.round(Math.max(8, window.innerHeight - r.top + 8))}px`;
	}

	// ---- storage

	function statesDb() {
		if (!savestates.db) {
			savestates.db = new Promise((resolve, reject) => {
				if (typeof indexedDB === 'undefined') {
					reject(new Error('no IndexedDB'));
					return;
				}
				const req = indexedDB.open(STATES_IDB_NAME, 1);
				req.onupgradeneeded = () => {
					const store = req.result.createObjectStore(STATES_STORE, { keyPath: 'id' });
					store.createIndex('key', 'key');
				};
				req.onsuccess = () => resolve(req.result);
				req.onerror = () => reject(req.error);
			});
			savestates.db.catch(() => {
				savestates.db = null;
			});
		}
		return savestates.db;
	}

	async function statesTx(mode, fn) {
		const db = await statesDb();
		return new Promise((resolve, reject) => {
			const tx = db.transaction(STATES_STORE, mode);
			let result;
			Promise.resolve(fn(tx.objectStore(STATES_STORE))).then((r) => (result = r));
			tx.oncomplete = () => resolve(result);
			tx.onerror = () => reject(tx.error);
			tx.onabort = () => reject(tx.error || new Error('aborted'));
		});
	}

	const reqValue = (req) => new Promise((resolve, reject) => {
		req.onsuccess = () => resolve(req.result);
		req.onerror = () => reject(req.error);
	});

	// The states of one game, newest first, without their bytes.
	async function readStates(key) {
		const all = await statesTx('readonly', (store) => reqValue(store.index('key').getAll(key)));
		return (all || []).map(({ bytes, ...meta }) => meta).sort((a, b) => b.at - a.at);
	}

	async function readStateBytes(id) {
		const rec = await statesTx('readonly', (store) => reqValue(store.get(id)));
		return rec ? new Uint8Array(await rec.bytes.arrayBuffer()) : null;
	}

	// Adds a state and drops the oldest beyond STATES_PER_GAME, in one transaction.
	async function putState(rec) {
		await statesTx('readwrite', async (store) => {
			store.put(rec);
			const keys = await reqValue(store.index('key').getAll(rec.key));
			keys.sort((a, b) => b.at - a.at);
			for (const old of keys.slice(STATES_PER_GAME))
				store.delete(old.id);
		});
	}

	async function refreshStates() {
		const key = savestates.key;
		if (!key) {
			savestates.list = [];
			renderStates();
			return;
		}
		try {
			const list = await readStates(key);
			if (key === savestates.key)
				savestates.list = list;
		} catch (err) {
			pageLog(`Could not read the save states: ${errorText(err)}`, 'warn');
			savestates.list = [];
		}
		renderStates();
	}

	// The running game's key: its serial once known; the console's menus have their own.
	function stateKeyNow() {
		if (page.vmState !== STATE.RUNNING && page.vmState !== STATE.STARTING)
			return '';
		if (devElf.bytes || disc.trayEmpty || disc.swapping)
			return '';
		if (!disc.runIsDisc)
			return STATE_BIOS_KEY;
		return disc.runSerial || '';
	}

	function statesGameChanged() {
		const key = stateKeyNow();
		if (key === savestates.key)
			return;
		savestates.key = key;
		savestates.list = [];
		if (savestates.view === 'more')
			setStateView('list');
		refreshStates();
	}

	// ---- thumbnails: the picture at the moment Save state was pressed

	function stateThumbFrom(source) {
		const resolve = savestates.thumbWait;
		savestates.thumbWait = null;
		if (!resolve)
			return;
		try {
			const c = new OffscreenCanvas(STATE_THUMB_W, STATE_THUMB_H);
			c.getContext('2d', { alpha: false }).drawImage(source, 0, 0, STATE_THUMB_W, STATE_THUMB_H);
			c.convertToBlob({ type: 'image/jpeg', quality: 0.82 }).then(resolve, () => resolve(null));
		} catch {
			resolve(null);
		}
	}

	function grabStateThumb() {
		return new Promise((resolve) => {
			if (display.kind === '2d' || typeof OffscreenCanvas === 'undefined') {
				savestates.thumbWait = resolve;
				stateThumbFrom(els.canvas);
				return;
			}
			savestates.thumbWait = resolve;
			setTimeout(() => {
				if (savestates.thumbWait === resolve)
					stateThumbFrom(els.canvas); // no frame came (a paused picture): whatever the canvas shows
			}, STATE_THUMB_WAIT_MS);
		});
	}

	// ---- the emulator's side

	// Runs pcsx2web_save_state() or pcsx2web_load_state() on path; resolves to true or false.
	function runStateOp(save, path) {
		const M = window.Module;
		return new Promise((resolve) => {
			if (!M || typeof M._pcsx2web_state_result !== 'function') {
				resolve(false);
				return;
			}
			const before = M._pcsx2web_state_result() >>> 0;
			if (M.ccall(save ? 'pcsx2web_save_state' : 'pcsx2web_load_state', 'number', ['string'], [path]) !== 0) {
				resolve(false);
				return;
			}
			const started = performance.now();
			const check = () => {
				const result = M._pcsx2web_state_result() >>> 0;
				if (result !== before)
					resolve((result & 1) === 1);
				else if (performance.now() - started > STATE_TIMEOUT_MS)
					resolve(false);
				else
					setTimeout(check, 50);
			};
			setTimeout(check, 50);
		});
	}

	const stateBusy = () => !!savestates.busy || !!devState.busy;

	function unlinkStateFile(path) {
		try {
			window.Module.FS.unlink(path);
		} catch {
			// not there
		}
	}

	function stateError(text) {
		stEls.error.textContent = text || '';
		stEls.error.hidden = !text;
		if (text && stateMenuOpen())
			announce(text);
	}

	// Saves a state of the running game and keeps it first in its list. Resolves to '' or an error line.
	async function savePlayerState() {
		const key = savestates.key;
		if (stateBusy() || page.vmState !== STATE.RUNNING || !key)
			return '';
		savestates.busy = 'save';
		stateError('');
		renderStates();
		const thumb = grabStateThumb();
		const path = `${STATE_DIR}/player.p2s`;
		const FS = window.Module.FS;
		if (!FS.analyzePath(STATE_DIR).exists)
			FS.mkdir(STATE_DIR);
		const ok = await runStateOp(true, path);
		let problem = '';
		if (!ok) {
			pageLog('Saving the state failed (see the log above).', 'warn');
			problem = STATE_COPY.saveFailed;
		} else {
			const bytes = FS.readFile(path);
			unlinkStateFile(path);
			const rec = { id: newStateId(), key, title: key === STATE_BIOS_KEY ? '' : (disc.runTitle || ''), at: Date.now(),
				size: bytes.length, thumb: await thumb, bytes: new Blob([bytes], { type: 'application/octet-stream' }) };
			try {
				askPersistentStorage();
				await putState(rec);
				pageLog(`State saved for ${key} (${bytes.length.toLocaleString('en-US')} bytes).`);
				announce(STATE_COPY.saved);
				subtitle(STATE_COPY.saved);
			} catch (err) {
				pageLog(`Could not keep the state: ${errorText(err)}`, 'warn');
				problem = err && err.name === 'QuotaExceededError' ? STATE_COPY.noRoom : STATE_COPY.saveFailed;
			}
		}
		savestates.thumbWait = null;
		savestates.busy = null;
		await refreshStates();
		stateRender();
		return problem;
	}

	// Loads bytes into the running game. id: the state's id (its row says Loading…) or 'file'.
	async function loadPlayerState(id, bytes, when) {
		if (stateBusy() || page.vmState !== STATE.RUNNING)
			return '';
		if (cardBusy())
			return STATE_COPY.cardBusy;
		savestates.busy = id;
		stateError('');
		renderStates();
		const path = writeStateFile('player-load.p2s', bytes);
		const ok = await runStateOp(false, path);
		unlinkStateFile(path);
		savestates.busy = null;
		renderStates();
		stateRender();
		if (!ok) {
			pageLog('Loading the state failed (see the log above).', 'warn');
			return STATE_COPY.loadFailed;
		}
		pageLog(`State loaded${when ? `: ${when}` : ''}.`);
		const line = STATE_COPY.loaded(when || 'from the file');
		announce(line);
		subtitle(line);
		return '';
	}

	async function loadStoredState(rec) {
		let bytes;
		try {
			bytes = await readStateBytes(rec.id);
		} catch (err) {
			pageLog(`Could not read the state: ${errorText(err)}`, 'warn');
		}
		if (!bytes)
			return STATE_COPY.loadFailed;
		return loadPlayerState(rec.id, bytes, stateWhen(rec.at));
	}

	// F1 and F3 (UI-SPEC.md §19).
	async function stateShortcut(load) {
		if (page.vmState !== STATE.RUNNING || !savestates.key || stateBusy())
			return;
		let problem;
		if (!load) {
			problem = await savePlayerState();
		} else if (!savestates.list.length) {
			subtitle(STATE_COPY.noneYet);
			announce(STATE_COPY.noneYet);
			return;
		} else {
			problem = await loadStoredState(savestates.list[0]);
		}
		if (problem) {
			subtitle(problem);
			announce(problem);
		}
	}

	async function stateFileChosenPlayer() {
		const file = stEls.input.files && stEls.input.files[0];
		stEls.input.value = '';
		if (!file || stateBusy() || page.vmState !== STATE.RUNNING || !savestates.key)
			return;
		let bytes;
		try {
			bytes = new Uint8Array(await file.arrayBuffer());
		} catch (err) {
			pageLog(`Could not read the save state ${file.name}: ${errorText(err)}`, 'warn');
			stateError(STATE_COPY.unreadable);
			return;
		}
		if (bytes.length < 4 || P2S_MAGIC.some((b, i) => bytes[i] !== b)) {
			stateError(STATE_COPY.notState);
			return;
		}
		pageLog(`Save state chosen: ${file.name} (${bytes.length.toLocaleString('en-US')} bytes).`);
		const key = savestates.key;
		const problem = await loadPlayerState('file', bytes, '');
		if (problem) {
			stateError(problem);
			return;
		}
		// Kept with this game's states, at the time it was opened, with today's picture.
		const rec = { id: newStateId(), key, title: disc.runTitle || '', at: Date.now(), size: bytes.length,
			thumb: await grabStateThumb(), bytes: new Blob([bytes], { type: 'application/octet-stream' }) };
		try {
			await putState(rec);
		} catch (err) {
			pageLog(`Could not keep the opened state: ${errorText(err)}`, 'warn');
		}
		refreshStates();
	}

	async function copyStateToDisk(rec) {
		let bytes;
		try {
			bytes = await readStateBytes(rec.id);
		} catch {
			bytes = null;
		}
		if (!bytes) {
			stateError(STATE_COPY.unreadable);
			return;
		}
		const d = new Date(rec.at);
		const two = (n) => String(n).padStart(2, '0');
		const stamp = `${d.getFullYear()}-${two(d.getMonth() + 1)}-${two(d.getDate())} ${two(d.getHours())}-${two(d.getMinutes())}`;
		const name = `${rec.key === STATE_BIOS_KEY ? 'BIOS' : rec.key} ${stamp}.p2s`.replace(/[\\/:*?"<>|]+/g, '-');
		const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
		const a = document.createElement('a');
		a.href = url;
		a.download = name;
		document.body.append(a);
		a.click();
		a.remove();
		setTimeout(() => URL.revokeObjectURL(url), 1000);
		pageLog(`State saved to disk as ${name}.`);
	}

	async function deleteState(rec) {
		try {
			await statesTx('readwrite', (store) => store.delete(rec.id));
		} catch (err) {
			pageLog(`Could not delete the state: ${errorText(err)}`, 'warn');
			stateError(STATE_COPY.saveFailed);
			return;
		}
		announce(STATE_COPY.deleted);
		subtitle(STATE_COPY.deleted);
		setStateView('list');
		await refreshStates();
	}

	function newStateId() {
		const bytes = new Uint8Array(6);
		crypto.getRandomValues(bytes);
		return 's' + Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
	}

	// "Today 19:42", "Yesterday 08:10", "12 Oct 21:03" (and the year when it is not this one).
	function stateWhen(at) {
		const d = new Date(at);
		const now = new Date();
		const time = d.toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' });
		const day = (x) => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime();
		const days = Math.round((day(now) - day(d)) / 86400000);
		if (days === 0)
			return `today ${time}`;
		if (days === 1)
			return `yesterday ${time}`;
		const opts = { day: 'numeric', month: 'short' };
		if (d.getFullYear() !== now.getFullYear())
			opts.year = 'numeric';
		return `${d.toLocaleDateString('en-GB', opts)} ${time}`;
	}

	const capFirst = (t) => t.charAt(0).toUpperCase() + t.slice(1);

	function stateSize(bytes) {
		return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
	}

	// ---- the menu

	function setStateView(view) {
		savestates.view = view;
		for (const v of stEls.views)
			v.hidden = v.dataset.view !== view;
		stateError('');
		renderStates();
		if (stateMenuOpen())
			queueMicrotask(() => focusStateMenu());
	}

	function focusStateMenu() {
		if (savestates.view === 'more') {
			stEls.copy.focus({ preventScroll: true });
			return;
		}
		const first = [stEls.save, ...stEls.list.querySelectorAll('.state-pick'), stEls.open].find((b) => !isDisabled(b));
		(first || stEls.menu).focus({ preventScroll: true });
	}

	function stateAction(fn) {
		Promise.resolve(fn()).then((problem) => {
			if (problem)
				stateError(problem);
		});
	}

	function renderStates() {
		const running = page.vmState === STATE.RUNNING;
		const key = savestates.key;
		const live = running && !!key;
		const busy = stateBusy();
		const n = key ? savestates.list.length : 0;
		setText(stEls.game, !key ? (running || page.vmState === STATE.STARTING ? (disc.runTitle || 'Starting…') : STATE_COPY.noGame) :
			key === STATE_BIOS_KEY ? STATE_COPY.consoleMenus : (disc.runTitle || key));
		setText(stEls.saveLabel, savestates.busy === 'save' ? STATE_COPY.saving : 'Save state');
		setItemDisabled(stEls.save, !live || busy, live ? null : stEls.reason.id);
		setItemDisabled(stEls.open, !live || busy, live ? null : stEls.reason.id);
		stEls.none.hidden = !live || n > 0;
		setText(stEls.reason, !running ? STATE_COPY.startFirst : !key ? STATE_COPY.unknownYet :
			`${STATE_COPY.kept} ${n >= STATES_PER_GAME ? STATE_COPY.full + ' ' : ''}${STATE_COPY.versions}`);
		stEls.btn.setAttribute('aria-label', STATE_COPY.btnName(n));
		renderStateRows(live, busy);
		if (savestates.view === 'more') {
			const rec = savestates.list.find((r) => r.id === savestates.target);
			if (!rec) {
				savestates.view = 'list';
				for (const v of stEls.views)
					v.hidden = v.dataset.view !== 'list';
			} else {
				setText(stEls.moreName, capFirst(stateWhen(rec.at)));
				setText(stEls.moreWhere, `${stateSize(rec.size)} · ${STATE_COPY.kept}`);
			}
		}
	}

	function renderStateRows(live, busy) {
		const rows = live ? savestates.list : [];
		const key = JSON.stringify([rows.map((r) => [r.id, r.at]), savestates.busy, busy, new Date().toDateString()]);
		if (key === savestates.listKey)
			return;
		savestates.listKey = key;
		const a = document.activeElement;
		const focusRow = a && stEls.list.contains(a) ? a.closest('.state-row') : null;
		const focusId = focusRow ? focusRow.dataset.id : null;
		const focusMore = !!(a && a.classList.contains('card-more'));
		for (const img of stEls.list.querySelectorAll('img'))
			URL.revokeObjectURL(img.src);
		stEls.list.textContent = '';
		rows.forEach((r, i) => {
			const when = stateWhen(r.at);
			const row = document.createElement('div');
			row.className = 'state-row';
			row.dataset.id = r.id;
			const pick = document.createElement('button');
			pick.type = 'button';
			pick.className = 'menu-item state-pick';
			if (r.thumb) {
				const img = document.createElement('img');
				img.className = 'state-thumb';
				img.alt = '';
				img.src = URL.createObjectURL(r.thumb);
				pick.appendChild(img);
			} else {
				pick.appendChild(span('state-thumb', ''));
			}
			const text = span('card-pick-text state-pick-text', '');
			text.appendChild(span('card-pick-name', savestates.busy === r.id ? STATE_COPY.loading : capFirst(when)));
			text.appendChild(span('card-pick-where', i === 0 ? STATE_COPY.newest : stateSize(r.size)));
			pick.appendChild(text);
			if (i === 0)
				pick.setAttribute('aria-keyshortcuts', 'F3');
			setItemDisabled(pick, busy, null);
			pick.addEventListener('click', () => {
				if (isDisabled(pick))
					return;
				stateAction(() => loadStoredState(r));
			});
			const more = document.createElement('button');
			more.type = 'button';
			more.className = 'btn-icon card-more';
			more.setAttribute('aria-label', STATE_COPY.moreFor(when));
			more.appendChild(svgIcon('dots-three', 'icon'));
			more.addEventListener('click', () => {
				savestates.target = r.id;
				setStateView('more');
			});
			row.append(pick, more);
			stEls.list.appendChild(row);
			if (r.id === focusId)
				queueMicrotask(() => (focusMore ? more : pick).focus({ preventScroll: true }));
		});
	}

	function wireStateUi() {
		stEls.save.addEventListener('click', () => {
			if (!isDisabled(stEls.save))
				stateAction(savePlayerState);
		});
		stEls.open.addEventListener('click', () => {
			if (!isDisabled(stEls.open))
				stEls.input.click();
		});
		stEls.input.addEventListener('change', stateFileChosenPlayer);
		const target = () => savestates.list.find((r) => r.id === savestates.target);
		stEls.copy.addEventListener('click', () => {
			const rec = target();
			if (rec)
				copyStateToDisk(rec);
		});
		stEls.del.addEventListener('click', () => {
			const rec = target();
			if (rec)
				deleteState(rec);
		});
		stEls.back.addEventListener('click', () => setStateView('list'));
		stEls.menu.addEventListener('beforetoggle', (e) => {
			if (e.newState === 'open') {
				savestates.view = 'list';
				for (const v of stEls.views)
					v.hidden = v.dataset.view !== 'list';
				stateError('');
				statesGameChanged();
				renderStates();
				placeStateMenu();
			}
		});
		stEls.menu.addEventListener('toggle', (e) => {
			const open = e.newState === 'open';
			stEls.btn.setAttribute('aria-expanded', open ? 'true' : 'false');
			if (open) {
				focusStateMenu();
			} else {
				const a = document.activeElement;
				if (!a || a === document.body || stEls.menu.contains(a))
					stEls.btn.focus({ preventScroll: true });
			}
			updateChrome();
		});
		stEls.menu.addEventListener('focusout', (e) => {
			const to = e.relatedTarget;
			if (to && !stEls.menu.contains(to) && to !== stEls.btn)
				stEls.menu.hidePopover();
		});
		stEls.menu.addEventListener('keydown', (e) => {
			if (e.key === 'Escape' && savestates.view === 'more') {
				e.preventDefault();
				e.stopPropagation();
				setStateView('list');
			}
		});
		for (const type of ['resize', 'fullscreenchange'])
			(type === 'resize' ? window : document).addEventListener(type, () => {
				if (stateMenuOpen())
					placeStateMenu();
			});
		renderStates();
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
		// The 2D path scales with CSS (image-rendering); the WebGPU device presents at the canvas's
		// own size, so it filters (or not) itself.
		if (api)
			api.setCrisp(on);
	}

	// The remote's PICTURE button cycles the picture modes. CRT is the CRT-Geom shader as in ShaderGlass (TV shader 8: a
	// spherical tube, a beam per scanline, an aperture mask) and CRT 2 the same with glow, short trails and darker corners
	// (TV shader 9), both applied when the WebGPU device presents; the 2D path's device has no TV shaders, so there they
	// are left out.
	const PICTURE_MODES = {
		smooth: { name: 'Smooth', crisp: false, shader: 0, next: 'crisp' },
		crisp: { name: 'Crisp', crisp: true, shader: 0, next: 'crt' },
		crt: { name: 'CRT', crisp: false, shader: 8, next: 'crt2' },
		crt2: { name: 'CRT 2', crisp: false, shader: 9, next: 'smooth' },
	};

	function readPicturePref() {
		const v = readPref(PREF_PICTURE);
		if (v in PICTURE_MODES)
			return v;
		return readPref(PREF_CRISP) === '1' ? 'crisp' : 'smooth';
	}

	function setPicture(mode) {
		page.picture = mode;
		const m = PICTURE_MODES[mode];
		setCrisp(m.crisp);
		setData('picmode', mode);
		els.btnCrisp.setAttribute('aria-label', `Picture: ${m.name}`);
		els.btnCrisp.classList.toggle('on', mode !== 'smooth');
		if (api)
			api.setTvShader(m.shader);
		for (const r of gfxEls.pictureRadios)
			r.checked = r.value === mode;
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
			!page.drawer && !menuOpen() && !discMenuOpen() && !cardMenuOpen() && !soundMenuOpen() && !gfxMenuOpen() && !eeMenuOpen() && !stateMenuOpen() && !els.about.open && !subtitleOpen && !page.drag &&
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

	// ------------------------------------------------------------------ recorder (?threads=1)
	// The CPU thread writes one record per vsync into a ring (pcsx2web_perf_ring(), PERF_FIELDS
	// doubles each: vsync, wall ms, cumulative busy ms of the EE, GS and VU1 threads, cumulative JIT
	// compile ms, cumulative GPU pipeline ms and count, vsync rate in Hz, cumulative ms the EE JIT's
	// code generation thread built modules, cumulative EE modules installed and those built or
	// compiled ahead from the game's code list). While recording, poll()
	// drains it into rows, one per vsync: its speed (the PS2's frame time over the wall time it
	// took), each thread's busy share and what was compiled during it. A drop shorter than the
	// readout's averaging window shows here. Up to 10 minutes are kept.
	const PERF_RECORDS = 8192;
	const PERF_FIELDS = 12;
	const REC_MAX_ROWS = 30000;
	// With the frame limiter on, single vsyncs jitter around their target (the limiter sleeps in whole
	// milliseconds), so the chart and the summary lead with the speed over the last REC_WINDOW vsyncs.
	const REC_WINDOW = 10;
	const rec = { recording: false, rows: [], read: 0, prev: null, t0: 0, from: 0, to: 0, startedAt: 0, info: null, drag: null };

	function recStart() {
		rec.rows = [];
		rec.prev = null;
		rec.t0 = 0;
		rec.read = api ? api.perfWritten() : 0;
		rec.from = rec.to = 0;
		rec.startedAt = performance.now();
		rec.recording = true;
		const mode = gpu.mode;
		rec.info = {
			game: disc.runTitle || '(BIOS)', serial: disc.runSerial || '',
			renderer: mode ? `${mode.hw ? 'hardware' : 'software'} renderer${mode.webgpu ? ' on WebGPU' : ' on a 2D canvas'}` : 'unknown renderer',
			browser: navigator.userAgent, date: new Date().toISOString(),
		};
		recRender();
	}

	function recStop() {
		recDrain();
		rec.recording = false;
		rec.from = 0;
		rec.to = Math.max(0, rec.rows.length - 1);
		recRender();
		if (rec.rows.length)
			els.recFrom.focus({ preventScroll: true });
	}

	function recDrain() {
		if (!api)
			return;
		const ptr = api.perfRing(), buf = heapBuffer();
		if (!ptr || !buf)
			return;
		const ring = new Float64Array(buf, ptr, PERF_RECORDS * PERF_FIELDS);
		const written = api.perfWritten();
		// Records more than a ring behind were overwritten; keep a margin for the one being written.
		let i = Math.max(rec.read, written - PERF_RECORDS + 16);
		for (; i < written; i++) {
			const o = (i % PERF_RECORDS) * PERF_FIELDS;
			const r = { v: ring[o], t: ring[o + 1], ee: ring[o + 2], gs: ring[o + 3], vu: ring[o + 4], jit: ring[o + 5],
				gpu: ring[o + 6], pipes: ring[o + 7], hz: ring[o + 8], bg: ring[o + 9], mods: ring[o + 10], ahead: ring[o + 11] };
			const p = rec.prev;
			rec.prev = r;
			if (!rec.t0)
				rec.t0 = r.t; // row times count from the first record
			if (!p || r.v !== p.v + 1 || !(r.t > p.t))
				continue; // the first record, or a gap (a reboot, or records lost): no row
			const ms = r.t - p.t;
			const pct = (a, b) => Math.max(0, Math.min(100, ((a - b) / ms) * 100));
			rec.rows.push({
				v: r.v, t: (p.t - rec.t0) / 1000, ms, speed: r.hz > 0 ? (1000 / r.hz / ms) * 100 : 0,
				ee: pct(r.ee, p.ee), gs: pct(r.gs, p.gs), vu: pct(r.vu, p.vu),
				jit: Math.max(0, r.jit - p.jit), gpu: Math.max(0, r.gpu - p.gpu), pipes: Math.max(0, r.pipes - p.pipes), hz: r.hz,
				bg: Math.max(0, r.bg - p.bg), mods: Math.max(0, r.mods - p.mods), ahead: Math.max(0, r.ahead - p.ahead),
			});
			const row = rec.rows[rec.rows.length - 1];
			const win = rec.rows.slice(-REC_WINDOW);
			row.speed10 = r.hz > 0 ? ((win.length * 1000) / r.hz / win.reduce((a, w) => a + w.ms, 0)) * 100 : 0;
			if (rec.rows.length >= REC_MAX_ROWS) {
				rec.read = written;
				recStop();
				return;
			}
		}
		rec.read = written;
		if (page.drawer === 'rec')
			recRender();
	}

	function recSelection() {
		return rec.rows.slice(rec.from, rec.to + 1);
	}

	function recStats(rows) {
		const n = rows.length;
		if (!n)
			return null;
		const mean = (k, rs = rows) => rs.reduce((a, r) => a + r[k], 0) / rs.length;
		const bySpeed = [...rows].sort((a, b) => a.speed10 - b.speed10);
		const worst = bySpeed.slice(0, Math.max(1, Math.round(n * 0.05)));
		const jitRows = rows.filter((r) => r.jit > 0.05), gpuRows = rows.filter((r) => r.pipes > 0);
		return {
			n, seconds: rows.reduce((a, r) => a + r.ms, 0) / 1000,
			min: bySpeed[0].speed10, minRaw: Math.min(...rows.map((r) => r.speed)),
			mean: (n * 100) / rows.reduce((a, r) => a + 100 / Math.max(r.speed, 0.001), 0),
			under90: rows.filter((r) => r.speed10 < 90).length, under75: rows.filter((r) => r.speed10 < 75).length,
			slowest: bySpeed[0], ee: mean('ee'), gs: mean('gs'), vu: mean('vu'),
			worstEe: mean('ee', worst), worstGs: mean('gs', worst), worstVu: mean('vu', worst), worstN: worst.length,
			jitMs: rows.reduce((a, r) => a + r.jit, 0), jitVsyncs: jitRows.length,
			gpuMs: rows.reduce((a, r) => a + r.gpu, 0), pipes: rows.reduce((a, r) => a + r.pipes, 0), gpuVsyncs: gpuRows.length,
			bgMs: rows.reduce((a, r) => a + r.bg, 0), mods: rows.reduce((a, r) => a + r.mods, 0), ahead: rows.reduce((a, r) => a + r.ahead, 0),
			quietSlow: rows.filter((r) => r.speed10 < 95 && r.jit <= 0.05 && r.bg <= 0.05 && r.pipes === 0).length,
		};
	}

	const fmt0 = (x) => x.toFixed(0), fmt1 = (x) => x.toFixed(1), fmt2 = (x) => x.toFixed(2);

	function recSummaryText(st, rows) {
		if (!st)
			return '';
		const t0 = rows[0].t, t1 = rows[rows.length - 1].t + rows[rows.length - 1].ms / 1000;
		return `${fmt2(t0)} s to ${fmt2(t1)} s: ${st.n} vsyncs\n` +
			`speed over ${REC_WINDOW} vsyncs: overall ${fmt0(st.mean)}%, lowest ${fmt0(st.min)}% (ending at vsync ${st.slowest.v}), ` +
			`${st.under90} vsyncs under 90%, ${st.under75} under 75%; slowest single vsync ${fmt0(st.minRaw)}%\n` +
			`busy, mean: EE ${fmt0(st.ee)}%  GS ${fmt0(st.gs)}%  VU ${fmt0(st.vu)}%; ` +
			`in the slowest ${st.worstN}: EE ${fmt0(st.worstEe)}%  GS ${fmt0(st.worstGs)}%  VU ${fmt0(st.worstVu)}%\n` +
			`compiling: JITs ${fmt1(st.jitMs)} ms over ${st.jitVsyncs} vsyncs; GPU pipelines ${st.pipes} (${fmt1(st.gpuMs)} ms) over ${st.gpuVsyncs} vsyncs\n` +
			`EE code: ${st.mods} modules (${st.ahead} of them compiled ahead), ${fmt1(st.bgMs)} ms built on the helper thread; ` +
			`${st.quietSlow} vsyncs under 95% with nothing compiling`;
	}

	const REC_COLUMNS = 'vsync,time_s,ms,speed_pct,speed10_pct,ee_pct,gs_pct,vu_pct,jit_ms,gpu_pipeline_ms,gpu_pipelines,' +
		'jit_helper_ms,ee_modules,ee_modules_ahead';
	const recRowCsv = (r) => [r.v, fmt2(r.t), fmt2(r.ms), fmt1(r.speed), fmt1(r.speed10), fmt0(r.ee), fmt0(r.gs), fmt0(r.vu), fmt1(r.jit), fmt1(r.gpu),
		r.pipes, fmt1(r.bg), r.mods, r.ahead].join(',');

	// For pasting into a message: the context, the summary and the cropped vsyncs (averaged in groups
	// when there are more than 1500, so the text stays short).
	function recText() {
		const rows = recSelection();
		const st = recStats(rows);
		if (!st)
			return '';
		const i = rec.info;
		const lines = ['TabStationToo recording', `game: ${i.game}${i.serial ? ` (${i.serial})` : ''}; ${i.renderer}; ${i.date}`,
			`browser: ${i.browser}`, recSummaryText(st, rows)];
		const group = Math.ceil(rows.length / 1500);
		lines.push(group > 1 ? `per ${group} vsyncs (averaged; the lowest speed of each group in min_speed_pct):` : 'per vsync:');
		lines.push(group > 1 ? `${REC_COLUMNS},min_speed_pct` : REC_COLUMNS);
		for (let k = 0; k < rows.length; k += group) {
			const g = rows.slice(k, k + group);
			if (group === 1) {
				lines.push(recRowCsv(g[0]));
				continue;
			}
			const sum = (key) => g.reduce((a, r) => a + r[key], 0);
			const ms = sum('ms');
			const speed = (g.length * 100) / g.reduce((a, r) => a + 100 / Math.max(r.speed, 0.001), 0);
			lines.push([g[0].v, fmt2(g[0].t), fmt2(ms / g.length), fmt1(speed), fmt1(speed),
				fmt0(sum('ee') / g.length), fmt0(sum('gs') / g.length), fmt0(sum('vu') / g.length), fmt1(sum('jit')), fmt1(sum('gpu')),
				sum('pipes'), fmt1(sum('bg')), sum('mods'), sum('ahead'), fmt1(Math.min(...g.map((r) => r.speed)))].join(','));
		}
		return lines.join('\n');
	}

	function recDownload() {
		const rows = recSelection();
		if (!rows.length)
			return;
		const text = [REC_COLUMNS, ...rows.map(recRowCsv)].join('\n') + '\n';
		const url = URL.createObjectURL(new Blob([text], { type: 'text/csv' }));
		const a = document.createElement('a');
		const name = (rec.info.serial || rec.info.game || 'bios').replace(/[^A-Za-z0-9._-]+/g, '-');
		a.href = url;
		a.download = `tabstationtoo-recording-${name}-${rec.info.date.slice(0, 19).replace(/[:T]/g, '-')}.csv`;
		document.body.append(a);
		a.click();
		a.remove();
		setTimeout(() => URL.revokeObjectURL(url), 1000);
	}

	function recRender() {
		const has = rec.rows.length > 0;
		const done = has && !rec.recording;
		setText(els.recToggleLabel, rec.recording ? 'Stop' : 'Record');
		els.recCopy.disabled = !done;
		els.recDownload.disabled = !done;
		if (rec.recording)
			setText(els.recStatus, `recording, ${fmt0((performance.now() - rec.startedAt) / 1000)} s, ${rec.rows.length} vsyncs`);
		else
			setText(els.recStatus, has ? `${fmt1(rec.rows.reduce((a, r) => a + r.ms, 0) / 1000)} s, ${rec.rows.length} vsyncs` : '');
		els.recEmpty.hidden = has;
		els.recChart.hidden = !has;
		els.recCrop.hidden = !done;
		if (done) {
			for (const input of [els.recFrom, els.recTo])
				input.max = String(rec.rows.length - 1);
			els.recFrom.value = String(rec.from);
			els.recTo.value = String(rec.to);
			els.recFrom.setAttribute('aria-valuetext', `vsync ${rec.rows[rec.from].v}, ${fmt2(rec.rows[rec.from].t)} s`);
			els.recTo.setAttribute('aria-valuetext', `vsync ${rec.rows[rec.to].v}, ${fmt2(rec.rows[rec.to].t)} s`);
		}
		const rows = done ? recSelection() : rec.rows;
		const st = recStats(rows);
		setText(els.recSummary, done ? `${recSummaryText(st, rows)}\nmarks under the chart, top to bottom: compiling on the EE or VU thread (coral); ` +
			'EE code built on the helper thread (grey, amber where code compiled ahead went in); GPU pipelines (sand)' : '');
		els.recChart.setAttribute('aria-label', st ? `Speed over ${REC_WINDOW} vsyncs, lowest ${fmt0(st.min)}%, ${st.under90} vsyncs under 90%` : 'No recording yet');
		if (has && page.drawer === 'rec')
			recDrawChart();
	}

	function recDrawChart() {
		const canvas = els.recChart;
		const dpr = window.devicePixelRatio || 1;
		const w = Math.max(1, Math.round(canvas.clientWidth * dpr)), h = Math.max(1, Math.round(canvas.clientHeight * dpr));
		if (canvas.width !== w || canvas.height !== h) {
			canvas.width = w;
			canvas.height = h;
		}
		const ctx = canvas.getContext('2d');
		const css = getComputedStyle(document.documentElement);
		const color = (name) => css.getPropertyValue(name).trim();
		ctx.clearRect(0, 0, w, h);
		const rows = rec.rows, n = rows.length;
		const x = (i) => (n > 1 ? (i / (n - 1)) * (w - 1) : 0);
		const top = 4 * dpr, bottom = h - 12 * dpr; // the bottom strip holds the compile marks, three rows
		const y = (pct) => bottom - (Math.min(Math.max(pct, 0), 150) / 150) * (bottom - top);
		if (!rec.recording) {
			ctx.fillStyle = 'rgb(242 181 68 / 0.12)';
			ctx.fillRect(x(rec.from), 0, Math.max(1, x(rec.to) - x(rec.from)), h);
		}
		ctx.fillStyle = color('--line-2');
		ctx.fillRect(0, Math.round(y(100)), w, Math.max(1, dpr));
		const line = (key, stroke, width) => {
			ctx.beginPath();
			for (let i = 0; i < n; i++) {
				const px = x(i), py = y(rows[i][key]);
				if (i === 0)
					ctx.moveTo(px, py);
				else
					ctx.lineTo(px, py);
			}
			ctx.strokeStyle = stroke;
			ctx.lineWidth = width * dpr;
			ctx.stroke();
		};
		ctx.globalAlpha = 0.7;
		line('ee', color('--coral'), 1);
		line('gs', color('--sand'), 1);
		line('vu', color('--ink-2'), 1);
		ctx.globalAlpha = 0.35;
		line('speed', color('--amber'), 1);
		ctx.globalAlpha = 1;
		line('speed10', color('--amber'), 1.5);
		for (let i = 0; i < n; i++) {
			const r = rows[i];
			if (r.jit > 0.05) {
				ctx.fillStyle = color('--coral');
				ctx.fillRect(x(i), h - 11 * dpr, Math.max(1, dpr), 3 * dpr);
			}
			if (r.bg > 0.05 || r.ahead > 0) {
				// Built on the helper thread; amber where modules compiled ahead were installed.
				ctx.fillStyle = r.ahead > 0 ? color('--amber') : color('--ink-2');
				ctx.fillRect(x(i), h - 7 * dpr, Math.max(1, dpr), 3 * dpr);
			}
			if (r.pipes > 0) {
				ctx.fillStyle = color('--sand');
				ctx.fillRect(x(i), h - 3 * dpr, Math.max(1, dpr), 3 * dpr);
			}
		}
	}

	// Dragging across the chart selects that part, like the two sliders.
	function recChartPointer() {
		const index = (e) => {
			const box = els.recChart.getBoundingClientRect();
			const f = Math.min(Math.max((e.clientX - box.left) / Math.max(box.width, 1), 0), 1);
			return Math.round(f * Math.max(rec.rows.length - 1, 0));
		};
		els.recChart.addEventListener('pointerdown', (e) => {
			if (rec.recording || !rec.rows.length)
				return;
			rec.drag = index(e);
			rec.from = rec.to = rec.drag;
			els.recChart.setPointerCapture(e.pointerId);
			recRender();
		});
		els.recChart.addEventListener('pointermove', (e) => {
			if (rec.drag === null)
				return;
			const i = index(e);
			rec.from = Math.min(rec.drag, i);
			rec.to = Math.max(rec.drag, i);
			recRender();
		});
		const end = () => {
			rec.drag = null;
		};
		els.recChart.addEventListener('pointerup', end);
		els.recChart.addEventListener('pointercancel', end);
	}

	// ------------------------------------------------------------------ drawers, menu, dialog

	let drawerCloseTimer = 0;

	function drawerEl(name) {
		return name === 'keys' ? els.keysDrawer : name === 'log' ? els.logDrawer : name === 'rec' ? els.recDrawer : null;
	}

	function drawerToggle(name) {
		return name === 'keys' ? els.ports[page.keysPort - 1] : name === 'rec' ? els.btnRec : els.btnLog;
	}

	// data-drawer (which widens the column under the screen) is cleared only here, once the
	// closing drawer is gone, so the column and the screen resize together, once.
	function finishDrawerClose() {
		clearTimeout(drawerCloseTimer);
		drawerCloseTimer = 0;
		for (const el of [els.keysDrawer, els.logDrawer, els.recDrawer]) {
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
		else if (name === 'rec')
			recRender();
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
	// (a mono line of its own), { link, href }, and buttons that look like links ({ logLink }, { discLink }, { cardLink }). Rebuilt only when the parts change.
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
			} else if (p.cardLink !== undefined) {
				const b = document.createElement('button');
				b.type = 'button';
				b.className = 'link-btn';
				b.textContent = p.cardLink;
				b.addEventListener('click', () => openCardMenu(0));
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
		// A boot waiting for the disc check shows as queued only once the check takes a while (discCheckShown).
		if (page.autoBoot && (!disc.checking || discCheckShown()))
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
		// The queued title's percentage. Not when the boot has just left the queue: render() calls this before it
		// updates page.lastState, and the slate would show the queue again until the next render.
		if (page.lastState === 'queued' && pageState() === 'queued')
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
			// The percentage is the emulator's download; once it runs, the boot waits for the disc or ?bios=.
			const loading = page.phase === 'loading' && page.runtimeRequested;
			const pct = Math.round(loadFraction() * 100);
			m.title = [COPY.queued, loading ? ' · ' : '', loading ? { num: `${pct}%` } : ''];
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
			if (s >= startingLogSeconds())
				m.fine = [[COPY.noAnswer, ' ', { logLink: COPY.openLog }]];
			else if (s >= STARTING_REASSURE_S) {
				m.fine = [[gzBoot() ? COPY.reassureGz : COPY.reassure]];
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
		discEls.slateLibrary.hidden = !m.disc; // wherever a disc can be chosen, the library can choose it
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
		els.rThreads.hidden = !SHOW_THREADS;
		if (SHOW_THREADS && page.threads)
			setText(els.rThreads, `  EE ${page.threads[0].toFixed(0)}%  GS ${page.threads[1].toFixed(0)}%  VU ${page.threads[2].toFixed(0)}% busy`);
		setText(els.rFrame, frame.width ? `, frame ${frame.width} by ${frame.height}` : '');
	}

	function renderBiosMenu() {
		const b = page.bios;
		if (!b) {
			hideMenu();
			return;
		}
		// On the console's model plate (UI-SPEC §16): the model when the name has one (SCPH-70004), else the title.
		const model = /(SCPH|DTL)[-_ ]?H?\d{4,5}(?!\d)/i.exec(b.name);
		setText(els.biosBtnName, model ? model[0].toUpperCase().replace(/[_ ]/, '-') : biosTitle(b.name));
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
		// Dark while a game runs or starts, and while the library's boot waits for the runtime: as dark as the
		// library it comes from, so that nothing changes colour as the library fades away.
		setData('lights', state === 'running' || state === 'starting' || (state === 'queued' && page.litWait) ? 'down' : 'up');
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
		renderCards();
		audioSync(state);
		renderSound();
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
		if (s >= startingLogSeconds() && page.startNotice < 2) {
			page.startNotice = 2;
			announce(COPY.noAnswer);
		} else if (s >= STARTING_REASSURE_S && page.startNotice < 1) {
			page.startNotice = 1;
			announce(gzBoot() ? COPY.reassureGz : COPY.reassure);
		}
	}

	// A .gz disc: PCSX2 reads the whole image to index it before the game starts (DISC-DESIGN.md 5.4).
	function gzBoot() {
		return !!(disc.runIsDisc && disc.current && /\.gz$/i.test(disc.current.name));
	}

	function startingLogSeconds() {
		return gzBoot() ? STARTING_GZ_LOG_S : STARTING_LOG_S;
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
			if (!audioFirstFrameHint())
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
			announce(disc.checking && disc.checkSlow ? DISC_COPY.announceChecking : COPY.announce.queued);
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
			let next = PICTURE_MODES[page.picture].next;
			const noCrt = PICTURE_MODES[next].shader !== 0 && display.kind === '2d';
			while (PICTURE_MODES[next].shader !== 0 && display.kind === '2d')
				next = PICTURE_MODES[next].next;
			setPicture(next);
			writePref(PREF_PICTURE, next);
			subtitle(`Picture: ${PICTURE_MODES[next].name}.${noCrt ? ' CRT needs WebGPU graphics (Menu).' : ''}`);
			returnFocusAfterPointer(e);
		});
		// The controller ports (UI-SPEC §16): the controls for that player. Another port while they are
		// open switches to it; the same one closes them.
		for (const btn of els.ports) {
			btn.addEventListener('click', (e) => {
				togglePort(Number(btn.dataset.port));
				returnFocusAfterPointer(e);
			});
		}
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
		for (const b of document.querySelectorAll('.t-btn, .card-btn')) {
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
			releaseGamepads();
			onWindowFocusChange();
		});
		window.addEventListener('focus', onWindowFocusChange);
		document.addEventListener('visibilitychange', () => {
			if (document.visibilityState !== 'visible') {
				releaseAllKeys(true);
				releaseGamepads();
				stopGamepadLoop();
			} else {
				startGamepadLoop();
			}
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

		// Recorder (developer switch ?threads=1).
		els.btnRec.hidden = !SHOW_THREADS;
		els.btnRec.addEventListener('click', () => toggleDrawer('rec'));
		els.recClose.addEventListener('click', () => closeDrawer(true));
		els.recToggle.addEventListener('click', () => (rec.recording ? recStop() : recStart()));
		els.recCopy.addEventListener('click', () => copyText(recText(), els.recCopyLabel));
		els.recDownload.addEventListener('click', recDownload);
		els.stateSave.addEventListener('click', () => stateOperation('save'));
		els.stateLoad.addEventListener('click', () => els.stateInput.click());
		els.stateInput.addEventListener('change', stateFileChosen);
		els.stateReload.addEventListener('click', () => stateOperation('load'));
		for (const input of [els.recFrom, els.recTo]) {
			input.addEventListener('input', () => {
				const a = Number(els.recFrom.value), b = Number(els.recTo.value);
				rec.from = Math.min(a, b);
				rec.to = Math.max(a, b);
				recRender();
			});
		}
		recChartPointer();
		if (typeof ResizeObserver === 'function')
			new ResizeObserver(() => { if (page.drawer === 'rec') recRender(); }).observe(els.recChart);

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
			gpuMode: gpu.mode,
			latency: latencySummary(),
			surface: surfaceFeed.width ? `${surfaceFeed.width}x${surfaceFeed.height}@${surfaceFeed.scale}` : null,
			rumble: page.phase === 'ready' && rumbleWords() ? [0, 1].map((i) => Atomics.load(rumble.words, i)) : null,
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
		// ?vu1jit= testing: the mode, the JIT's counters and the fuzzer's status.
		vu1jit: () => ({ mode: vu1jit.mode, stats: vu1jitStats(), fuzz: vu1jitFuzzStatus() }),
	});

	// ------------------------------------------------------------------ start

	function showUnsupported(problem) {
		page.phase = 'unsupported';
		page.env = problem;
		const iso = isolationState();
		if (iso.status === 'unavailable')
			pageLog(`The cross-origin isolation service worker could not help: ${iso.reason}.`, 'warn');
		pageLog(`This page can't run the emulator: ${problem.cause}`, 'warn');
		render();
	}

	function main() {
		wireUi();
		wireDiscUi();
		wireCardsUi();
		observeCanvasSize();
		wireSoundUi();
		wireGfxUi();
		wireEeUi();
		wireStateUi();
		wirePlayersUi();
		setPicture(readPicturePref());
		page.keepChrome = readPref(PREF_KEEP_CHROME) === '1';
		els.keepChrome.checked = page.keepChrome;
		setData('fs', 'off');
		syncScreenWidth();
		for (const a of document.querySelectorAll('a.source-link'))
			a.href = SOURCE_URL;
		applyLayoutLabels();
		renderLogCount();

		if (BUILD_ID)
			pageLog(`Build ${BUILD_ID}.`);
		const problem = environmentProblem();
		if (problem && problem.isolation && isolationPending()) {
			// The service worker is being installed and the page reloads by itself, usually within a
			// second. Until then the intake shows as on any first visit, and nothing downloads.
			window.addEventListener('tabstationtoo-isolation', () => showUnsupported(environmentProblem()), { once: true });
			render();
			return;
		}
		if (problem) {
			showUnsupported(problem);
			return; // do not load the wasm
		}
		if (navigator.serviceWorker && navigator.serviceWorker.controller)
			pageLog('A service worker (coi-serviceworker.js) serves this page and adds the cross-origin isolation headers.');
		loadCardLibrary();

		const biosParam = params.get('bios');
		const discParam = params.get('disc');
		const elfParam = params.get('elf');
		const stateParam = params.get('state');
		disc.urlPending = !!discParam; // a queued ?bios= boot waits for the ?disc= check
		devElf.pending = !!elfParam; // and for the ?elf= download
		devState.pending = !!stateParam; // and for the ?state= download
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
		if (elfParam)
			loadElfFromUrl(elfParam);
		if (stateParam)
			loadStateFromUrl(stateParam);
	}

	main();
})();
