// Racing wheels on the PS2's two USB ports: the USB 1 and USB 2 sockets on the console's face and their menu, and a
// wheel under the controllers for each port that has one, cabled to its socket.
//
// - What the PS2 sees in a port: nothing, or one of PCSX2's emulated Logitech wheels (pcsx2web_set_wheel), next to
//   whatever plays on the two controller ports.
// - Its controls are mapped on the wheel's drawing, from anything the page can read: any gamepad or wheel the browser
//   sees (Gamepad API), or keys. They are fed to the emulated wheel with pcsx2web_wheel().
// - Its force feedback goes to one "FFB device", a real Logitech wheel connected with WebHID for that alone. The
//   game's commands (the classic Logitech 7-byte commands) reach the page through the FFB ring (pcsx2web_ffb_ring,
//   Main.cpp "Racing wheels"), tagged with their USB port, and go to that port's device as they are, in order. Only the
//   extended commands that change the wheel's mode or identity are held back (BLOCKED_EXT_COMMANDS): the wheel would
//   detach and come back as another device.
//
// Only FFB devices that understand the classic commands are accepted. G920/G923 for Xbox (HID++) and G29/G923 in PS4
// mode are refused. FFB needs WebHID (Chrome, Edge); the mapping works wherever gamepads do.
(() => {
	'use strict';

	const $ = (id) => document.getElementById(id);
	const els = {
		buttons: [$('usb-1'), $('usb-2')],
		menu: $('usb-menu'),
		title: $('usb-menu-title'),
		types: [...document.querySelectorAll('input[name="usb-type"]')],
		wheels: $('wheels'),
		row: $('wheels-row'),
		template: $('wheel-template'),
		cables: [[$('usb-cable-1-wall'), $('usb-cable-1')], [$('usb-cable-2-wall'), $('usb-cable-2')]],
	};
	if (!els.menu || els.buttons.some((b) => !b) || !els.template)
		return;

	const LOGITECH = 0x046d;
	const PREF_KEY = 'tabstation.usb'; // [{ type, pid, binds }] per port
	const POLL_MS = 2; // the browser clamps it to about 4 ms
	const RESEND_MS = 1000; // every input again, so a newly plugged emulated wheel gets the held state
	const LIVE_MS = 100;
	const WEB_STATE_STARTING = 1;
	const WEB_STATE_RUNNING = 2;
	const REPLUG_MS = 1000; // how long the emulated wheel stays out when it is replugged
	const AXIS_PICK = 0.5; // how far an axis must move from where it rested to be picked while mapping
	const STEER_DEG = 100; // how far the drawing's rim turns at full lock, as a 200° wheel

	// Extended commands (0xF8 xx) a wheel must not get: mode switches with or without detach, revert identity, the
	// rev lights, the exact range (no PS2 wheel has it). The 200° and 900° ones only go to wheels that have them.
	const BLOCKED_EXT_COMMANDS = new Set([0x01, 0x05, 0x06, 0x09, 0x0a, 0x10, 0x11, 0x12, 0x81]);
	const RANGE_COMMANDS = new Set([0x02, 0x03]);
	const STOP_ALL = [0xf3, 0, 0, 0, 0, 0, 0];
	const RANGE_200 = [0xf8, 0x02, 0, 0, 0, 0, 0];

	// How every PS2 wheel starts: no effects, its own centring spring on, and 200° on a wheel that can turn further.
	function defaultsFor(model) {
		const cmds = [STOP_ALL, [0xf4, 0, 0, 0, 0, 0, 0]];
		if (model && model.range)
			cmds.push(RANGE_200);
		return cmds;
	}

	const TYPE_NAMES = ['Driving Force', 'Driving Force Pro', 'Driving Force Pro (rev 11.02)', 'GT Force'];

	// The emulated wheel's inputs (usb_pad::ControlID) and where each is on the drawing (centre, width, height in its
	// 600 × 310 units), in the order "Map all controls" asks for them.
	const INPUTS = [
		{ id: 'SteerLeft', cid: 0, name: 'Steer left', label: 'Left', x: 30, y: 155, w: 44, h: 22, lead: true, analog: true },
		{ id: 'SteerRight', cid: 1, name: 'Steer right', label: 'Right', x: 380, y: 155, w: 44, h: 22, lead: true, analog: true },
		{ id: 'Gas', cid: 2, name: 'Gas', x: 545, y: 195, w: 54, h: 40, analog: true },
		{ id: 'Brake', cid: 3, name: 'Brake', x: 470, y: 196, w: 54, h: 40, analog: true },
		{ id: 'Up', cid: 4, name: 'D-pad up', x: 130, y: 134, w: 22, h: 22 },
		{ id: 'Down', cid: 5, name: 'D-pad down', x: 130, y: 176, w: 22, h: 22 },
		{ id: 'Left', cid: 6, name: 'D-pad left', x: 109, y: 155, w: 22, h: 22 },
		{ id: 'Right', cid: 7, name: 'D-pad right', x: 151, y: 155, w: 22, h: 22 },
		{ id: 'Cross', cid: 8, name: 'Cross', x: 270, y: 177, w: 22, h: 22, round: true },
		{ id: 'Square', cid: 9, name: 'Square', x: 248, y: 155, w: 22, h: 22, round: true },
		{ id: 'Circle', cid: 10, name: 'Circle', x: 292, y: 155, w: 22, h: 22, round: true },
		{ id: 'Triangle', cid: 11, name: 'Triangle', x: 270, y: 133, w: 22, h: 22, round: true },
		{ id: 'R1', cid: 12, name: 'R1 (right paddle, shift up)', label: 'R1', x: 360, y: 40, w: 32, h: 20, lead: true },
		{ id: 'L1', cid: 13, name: 'L1 (left paddle, shift down)', label: 'L1', x: 40, y: 40, w: 32, h: 20, lead: true },
		{ id: 'R2', cid: 14, name: 'R2', x: 218, y: 112, w: 30, h: 16 },
		{ id: 'L2', cid: 15, name: 'L2', x: 182, y: 112, w: 30, h: 16 },
		{ id: 'Select', cid: 16, name: 'Select', x: 182, y: 140, w: 30, h: 16 },
		{ id: 'Start', cid: 17, name: 'Start', x: 218, y: 140, w: 30, h: 16 },
		{ id: 'R3', cid: 18, name: 'R3', x: 218, y: 190, w: 22, h: 22, round: true },
		{ id: 'L3', cid: 19, name: 'L3', x: 182, y: 190, w: 22, h: 22, round: true },
		{ id: 'ShiftMinus', cid: 21, name: 'Shifter -', label: '-', x: 380, y: 228, w: 30, h: 18, lead: true },
		{ id: 'ShiftPlus', cid: 20, name: 'Shifter +', label: '+', x: 380, y: 282, w: 30, h: 18, lead: true },
	];
	const NUM_CIDS = 22;

	// What each emulated wheel has (PCSX2's bindings for it, usb-pad.cpp GetWheelBindings), where it differs from the
	// drawing: only the Driving Force Pro has the gear stick (its buttons 12 and 13, + and −); the Driving Force has no L3/R3; the GT Force only paddles and four buttons, in its own order (the
	// paddles first, as the real one reports them).
	const MODEL_INPUTS = {
		0: { hidden: new Set(['L3', 'R3', 'ShiftMinus', 'ShiftPlus']) },
		3: {
			hidden: new Set(['Up', 'Down', 'Left', 'Right', 'L2', 'R2', 'Select', 'Start', 'L3', 'R3', 'ShiftMinus', 'ShiftPlus']),
			cid: { L1: 8, R1: 9, Square: 10, Triangle: 11, Cross: 12, Circle: 13 },
			at: { Square: [130, 140], Cross: [130, 170], Triangle: [270, 140], Circle: [270, 170] },
			name: { L1: 'Left paddle', R1: 'Right paddle', Square: 'X', Triangle: 'Y', Cross: 'A', Circle: 'B' },
		},
	};
	const shown = (p, inp) => !MODEL_INPUTS[p.type]?.hidden.has(inp.id);
	const cidOf = (p, inp) => MODEL_INPUTS[p.type]?.cid?.[inp.id] ?? inp.cid;
	const nameOf = (p, inp) => MODEL_INPUTS[p.type]?.name?.[inp.id] ?? inp.name;
	const nextShown = (p, at) => {
		for (let i = at; i < INPUTS.length; i++)
			if (shown(p, INPUTS[i]))
				return i;
		return -1;
	};
	const CID_STEER_L = 0, CID_STEER_R = 1, CID_GAS = 2, CID_BRAKE = 3;

	// FFB devices: the shortest time between two commands, in ms. The wheel runs its effects in a loop (about 0.5 ms
	// on a DFGT, 2.2 ms on a G29) and can miss a command sent sooner. Needs proper testing per model.
	const MODELS = {
		0xc294: { name: 'Driving Force (or a wheel in compatibility mode)', gap: 2 },
		0xc298: { name: 'Driving Force Pro', gap: 2, range: true },
		0xc293: { name: 'GT Force', gap: 2 },
		0xc295: { name: 'MOMO Force', gap: 2 },
		0xca03: { name: 'MOMO Racing', gap: 2 },
		0xc29a: { name: 'Driving Force GT', gap: 1, range: true },
		0xc299: { name: 'G25', gap: 2, range: true },
		0xc29b: { name: 'G27', gap: 2, range: true },
		0xc24f: { name: 'G29', gap: 3, range: true },
		0xc266: { name: 'G923 for PlayStation', gap: 3, range: true },
	};
	const REFUSED = {
		0xc260: 'This G29 is in PS4 mode. Switch it to PS3 and choose it again.',
		0xc267: 'This G923 is in PS4 mode. Switch it to PS3 and choose it again.',
		0xc261: 'The G920 does not take the classic force feedback commands.',
		0xc262: 'The G920 does not take the classic force feedback commands.',
		0xc26d: 'The G923 for Xbox does not take the classic force feedback commands.',
		0xc26e: 'The G923 for Xbox does not take the classic force feedback commands.',
	};

	function newPort(index) {
		return {
			index, // 0 or 1
			type: -1, // the emulated wheel (core subtype), -1 nothing
			appliedType: null, // what the core was last told
			binds: {}, // input id -> { kind: 'key', code } | { kind: 'button', gp, index } | { kind: 'axis', gp, index, rest, full }
			savedPid: 0, // the FFB device to connect again by itself
			dev: null, // the FFB device
			model: null,
			outReportId: 0,
			outBytes: 7,
			sent: new Float32Array(NUM_CIDS).fill(-1), // what the core was last given
			lastResend: 0,
			queue: [], // commands waiting for the FFB device
			sending: false,
			lastSend: 0,
			forwarded: 0,
			blocked: 0,
			message: '', // the FFB device's last problem
			replugUntil: 0, // the emulated wheel is out of its port until then (replug)
			unit: null, // the wheel under the controllers
			note: '', // the wheel's status line
		};
	}

	const ports = [newPort(0), newPort(1)];
	const keysDown = new Set();
	let menuPort = 0; // the port the menu shows
	let listen = null; // { port, at, all, base, armed, since, rest: Map(gamepad id -> axes), pressed: Map(gamepad id -> buttons) }
	let lastState = 0;
	let lastLive = 0;
	let cableKey = '';

	const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
	const hex4 = (n) => n.toString(16).toUpperCase().padStart(4, '0');

	function savePrefs() {
		try {
			localStorage.setItem(PREF_KEY, JSON.stringify(ports.map((p) => ({ type: p.type, pid: p.savedPid, binds: p.binds }))));
		} catch {
			// not remembered
		}
	}

	function loadPrefs() {
		try {
			const saved = JSON.parse(localStorage.getItem(PREF_KEY) || '[]');
			for (const p of ports) {
				const s = saved[p.index] || {};
				if (Number.isInteger(s.type) && s.type >= -1 && s.type < TYPE_NAMES.length)
					p.type = s.type;
				if (Number.isInteger(s.pid) && MODELS[s.pid])
					p.savedPid = s.pid;
				if (s.binds && typeof s.binds === 'object')
					for (const inp of INPUTS)
						if (validBind(s.binds[inp.id]))
							p.binds[inp.id] = s.binds[inp.id];
			}
		} catch {
			// no storage: both ports start empty
		}
	}

	function validBind(b) {
		if (!b || typeof b !== 'object')
			return false;
		if (b.kind === 'key')
			return typeof b.code === 'string';
		if (b.kind === 'button')
			return typeof b.gp === 'string' && Number.isInteger(b.index);
		if (b.kind === 'axis') {
			if (!(typeof b.gp === 'string' && Number.isInteger(b.index) && Number.isFinite(b.rest) && (b.full === 1 || b.full === -1)))
				return false;
			if (b.rest > HAT_REST) // a hat mapped as an axis by an older version
				return false;
			b.rest = restPoint(b.rest);
			return b.rest !== b.full;
		}
		if (b.kind === 'hat')
			return typeof b.gp === 'string' && Number.isInteger(b.index) && Number.isInteger(b.step) && b.step >= 0 && b.step < 8;
		return false;
	}

	// ------------------------------------------------------------------ the emulator

	function module() {
		const M = window.Module;
		return M && typeof M._pcsx2web_set_wheel === 'function' && typeof M._pcsx2web_ffb_ring === 'function' ? M : null;
	}

	function memoryBuffer(M) {
		if (M.wasmMemory instanceof WebAssembly.Memory)
			return M.wasmMemory.buffer;
		if (globalThis.wasmMemory instanceof WebAssembly.Memory)
			return globalThis.wasmMemory.buffer;
		const heap = M.HEAPU8 || globalThis.HEAPU8;
		return heap ? heap.buffer : null;
	}

	// The core is told what is in each port. During a replug the port is empty until replugUntil.
	function applyTypes(M, now) {
		for (const p of ports) {
			const type = now < p.replugUntil ? -1 : p.type;
			if (p.appliedType === type)
				continue;
			M._pcsx2web_set_wheel(p.index, type);
			p.appliedType = type;
			p.sent.fill(-1);
		}
	}

	// Pulls the emulated wheel out of a running game and plugs it back in, so the game sets it up again and the
	// FFB device gets that too.
	function replug(p) {
		const M = module();
		if (M && p.type >= 0 && M._pcsx2web_state() === WEB_STATE_RUNNING)
			p.replugUntil = performance.now() + REPLUG_MS;
	}

	// Takes the game's commands from the FFB ring, each to its port's FFB device. Without one they are dropped.
	function drainRing(M) {
		const buffer = memoryBuffer(M);
		const ptr = M._pcsx2web_ffb_ring() >>> 0;
		if (!buffer || !ptr)
			return;
		const header = new Int32Array(buffer, ptr, 4);
		const size = Atomics.load(header, 3) >>> 0;
		if (!size)
			return;
		const head = Atomics.load(header, 0) >>> 0;
		let tail = Atomics.load(header, 1) >>> 0;
		if (head === tail)
			return;
		const bytes = new Uint8Array(buffer, ptr + 16, size * 8);
		for (; tail !== head; tail = (tail + 1) >>> 0) {
			const at = (tail % size) * 8;
			const p = ports[bytes[at + 7] & 1];
			if (p.dev)
				forward(p, Array.from(bytes.subarray(at, at + 7)));
		}
		Atomics.store(header, 1, head | 0);
	}

	function forward(p, cmd) {
		if (cmd[0] === 0xf8 && (BLOCKED_EXT_COMMANDS.has(cmd[1]) || (RANGE_COMMANDS.has(cmd[1]) && !p.model.range))) {
			p.blocked++;
			return;
		}
		p.forwarded++;
		send(p, cmd);
	}

	// ------------------------------------------------------------------ the FFB device

	function send(p, cmd) {
		p.queue.push(cmd);
		if (!p.sending)
			pump(p);
	}

	// One command at a time, in order, at least the model's gap apart.
	async function pump(p) {
		p.sending = true;
		try {
			while (p.queue.length && p.dev) {
				const cmd = p.queue.shift();
				const wait = p.lastSend + p.model.gap - performance.now();
				if (wait > 0)
					await sleep(wait);
				if (!p.dev)
					break;
				const buf = new Uint8Array(p.outBytes);
				buf.set(cmd.slice(0, p.outBytes));
				await p.dev.sendReport(p.outReportId, buf);
				p.lastSend = performance.now();
			}
		} catch (err) {
			p.queue.length = 0;
			p.message = `It stopped taking commands: ${err.message}`;
			render();
		} finally {
			p.sending = false;
		}
	}

	// Lets go of every force: the queue is dropped, then all four slots stop.
	function stopForces(p) {
		if (!p.dev)
			return;
		p.queue.length = 0;
		send(p, STOP_ALL);
	}

	// Puts the FFB device back as the game expects to find a wheel (defaultsFor). Whatever was still queued is dropped.
	function resetForces(p) {
		if (!p.dev)
			return;
		p.queue.length = 0;
		for (const cmd of defaultsFor(p.model))
			send(p, cmd);
	}

	// The first output report and its size, from the descriptor.
	function outputReport(dev) {
		const walk = (cols) => {
			for (const c of cols) {
				for (const r of c.outputReports || []) {
					const bits = r.items.reduce((s, it) => s + it.reportSize * it.reportCount, 0);
					return { id: r.reportId, bytes: Math.max(7, Math.ceil(bits / 8)) };
				}
				const found = walk(c.children || []);
				if (found)
					return found;
			}
			return null;
		};
		return walk(dev.collections || []) || { id: 0, bytes: 7 };
	}

	// Makes dev port p's FFB device. False (with p.message saying why) when it cannot take the classic commands.
	async function useDevice(p, dev) {
		if (REFUSED[dev.productId]) {
			p.message = REFUSED[dev.productId];
			return false;
		}
		const model = MODELS[dev.productId];
		if (!model) {
			p.message = `${dev.productName} (046D:${hex4(dev.productId)}) is not a wheel this page knows.`;
			return false;
		}
		const other = ports[1 - p.index];
		if (other.dev === dev) {
			p.message = `It already gets USB ${other.index + 1}'s force feedback.`;
			return false;
		}
		if (!dev.opened)
			await dev.open();
		if (p.dev && p.dev !== dev)
			disconnect(p);
		const out = outputReport(dev);
		Object.assign(p, { dev, model, outReportId: out.id, outBytes: out.bytes, forwarded: 0, blocked: 0, message: '',
			savedPid: dev.productId });
		savePrefs();
		resetForces(p); // queued before the emulated wheel is plugged back in (replug)
		replug(p);
		return true;
	}

	// Lets go of port p's FFB device, back as it was before the game (defaultsFor).
	function disconnect(p) {
		const dev = p.dev;
		if (!dev)
			return;
		const { outReportId, outBytes, model } = p;
		const cmds = defaultsFor(model);
		p.queue.length = 0;
		p.dev = null;
		p.model = null;
		if (ports.some((o) => o.dev === dev))
			return;
		(async () => {
			for (const cmd of cmds) {
				const buf = new Uint8Array(outBytes);
				buf.set(cmd);
				await dev.sendReport(outReportId, buf).catch(() => {});
				await sleep(model.gap + 2);
			}
			await dev.close().catch(() => {});
		})();
	}

	async function onConnectClick(p) {
		if (p.dev) {
			disconnect(p);
			p.savedPid = 0;
			savePrefs();
			render();
			return;
		}
		if (!('hid' in navigator)) {
			p.message = 'This browser has no WebHID. Use Chrome or Edge on a computer.';
			render();
			return;
		}
		try {
			const list = await navigator.hid.requestDevice({ filters: [{ vendorId: LOGITECH }] });
			if (list.length)
				await useDevice(p, list[0]);
		} catch (err) {
			p.message = `Could not open it: ${err.message}`;
		}
		render();
	}

	// FFB devices the user allowed before connect again by themselves, each to the port it was on.
	async function reconnect() {
		if (!('hid' in navigator))
			return;
		try {
			const list = await navigator.hid.getDevices();
			for (const p of ports) {
				if (p.dev || !p.savedPid)
					continue;
				const dev = list.find((d) => d.vendorId === LOGITECH && d.productId === p.savedPid &&
					!ports.some((o) => o.dev === d));
				if (dev)
					await useDevice(p, dev);
			}
		} catch {
			// nothing to do: the user can choose it from the menu
		}
		render();
	}

	// ------------------------------------------------------------------ the mapped controls

	function gamepadList() {
		try {
			return [...(navigator.getGamepads ? navigator.getGamepads() : [])].filter((g) => g && g.connected);
		} catch {
			return [];
		}
	}

	// A short, readable name for a gamepad ("Logitech G29 Driving Force Racing Wheel (Vendor: 046d ...)" -> "G29 Driving...").
	function gamepadName(id) {
		const name = id.replace(/\s*\(.*$/, '').replace(/^Logitech\s+/i, '').trim() || id;
		return name.length > 24 ? name.slice(0, 23) + '…' : name;
	}

	function keyLabel(code) {
		const arrows = { ArrowUp: '↑', ArrowDown: '↓', ArrowLeft: '←', ArrowRight: '→' };
		if (arrows[code])
			return arrows[code];
		return code.replace(/^Key/, '').replace(/^Digit/, '').replace(/^Numpad/, 'N').replace(/^(Shift|Control|Alt|Meta)(Left|Right)$/, '$1')
			.replace('Control', 'Ctrl')
			.replace('Backspace', 'Bksp').replace('Space', 'Spc').replace('Enter', 'Ent');
	}

	function bindLabel(b) {
		if (!b)
			return '—';
		if (b.kind === 'key')
			return keyLabel(b.code);
		if (b.kind === 'button')
			return `B${b.index}`;
		if (b.kind === 'hat')
			return '↑↗→↘↓↙←↖'[b.step];
		return `A${b.index}${b.full > b.rest ? '+' : '−'}`;
	}

	function bindTitle(b) {
		if (!b)
			return 'nothing';
		if (b.kind === 'key')
			return `the ${keyLabel(b.code)} key`;
		const what = b.kind === 'button' ? `button ${b.index}` : b.kind === 'hat' ? `D-pad ${['up', 'up-right', 'right', 'down-right', 'down', 'down-left', 'left', 'up-left'][b.step]}` : `axis ${b.index} ${b.full > b.rest ? 'up' : 'down'}`;
		return `${what} of ${gamepadName(b.gp)}`;
	}

	// Where an axis rests: the centre of a steering axis, or the end a pedal rests at. Taken from where it was when it
	// was mapped, which can be a little off while the control comes back.
	function restPoint(v) {
		return v <= -0.5 ? -1 : v >= 0.5 ? 1 : 0;
	}

	// A D-pad the browser shows as one axis (a hat, as Chrome does for Logitech wheels): -1 up, then in steps of 2/7
	// clockwise to 1 up-left, and about 1.29 when nothing is pressed. Its direction 0 to 7, or -1 for none.
	const HAT_REST = 1.1;
	function hatStep(v) {
		return v === undefined || Math.abs(v) > 1.05 ? -1 : ((Math.round((v + 1) * 3.5) % 8) + 8) % 8;
	}

	// How far the bound control is pressed, 0 to 1.
	function bindValue(b, pads) {
		if (!b)
			return 0;
		if (b.kind === 'key')
			return keysDown.has(b.code) ? 1 : 0;
		const g = pads.find((o) => o.id === b.gp);
		if (!g)
			return 0;
		if (b.kind === 'button') {
			const btn = g.buttons[b.index];
			return btn ? (btn.value > 0 ? btn.value : btn.pressed ? 1 : 0) : 0;
		}
		const v = g.axes[b.index];
		if (b.kind === 'hat') { // the direction, or a diagonal next to it
			const at = hatStep(v);
			return at >= 0 && Math.min((at - b.step + 8) % 8, (b.step - at + 8) % 8) <= 1 ? 1 : 0;
		}
		if (v === undefined || b.full === b.rest)
			return 0;
		return Math.max(0, Math.min(1, (v - b.rest) / (b.full - b.rest)));
	}

	function readInputs(p, pads) {
		const out = new Float32Array(NUM_CIDS);
		for (const inp of INPUTS) {
			if (!shown(p, inp))
				continue;
			const v = bindValue(p.binds[inp.id], pads);
			out[cidOf(p, inp)] = inp.analog ? v : v >= 0.5 ? 1 : 0;
		}
		// One side of the steering at a time: the emulated wheel only looks at the left one when both are pressed.
		const steer = out[CID_STEER_R] - out[CID_STEER_L];
		out[CID_STEER_L] = Math.max(0, -steer);
		out[CID_STEER_R] = Math.max(0, steer);
		return out;
	}

	// Hands the changed inputs (all of them every RESEND_MS) to the port's emulated wheel.
	function feedInputs(M, p, values, now) {
		const all = now - p.lastResend >= RESEND_MS;
		if (all)
			p.lastResend = now;
		for (let i = 0; i < NUM_CIDS; i++) {
			if (!all && values[i] === p.sent[i])
				continue;
			if (M._pcsx2web_wheel(p.index, i, values[i]) === 1)
				p.sent[i] = values[i];
		}
	}

	// ------------------------------------------------------------------ mapping

	// Mapping waits until every control is back where it rested (base: taken when mapping began) before it listens,
	// so the control just mapped, coming back, is not taken for the next one.
	const snapshot = (pads) => ({
		axes: new Map(pads.map((g) => [g.id, [...g.axes]])),
		pressed: new Map(pads.map((g) => [g.id, g.buttons.map((b) => b.pressed || b.value > 0.5)])),
	});

	function startListening(port, at, all) {
		const pads = gamepadList();
		const base = listen && listen.port === port && listen.all && all ? listen.base : snapshot(pads).axes;
		listen = { port, at, all, base, armed: false, since: performance.now(), rest: null, pressed: null };
		const p = ports[port];
		p.note = `Press a key or a button, or move a control, for ${nameOf(p, INPUTS[at])}. Esc stops.`;
		renderUnits();
	}

	function stopListening(note) {
		if (!listen)
			return;
		const p = ports[listen.port];
		listen = null;
		p.note = note || '';
		renderUnits();
	}

	function bindNow(b) {
		const p = ports[listen.port];
		const inp = INPUTS[listen.at];
		p.binds[inp.id] = b;
		savePrefs();
		const next = listen.all ? nextShown(p, listen.at + 1) : -1;
		if (next >= 0) {
			startListening(listen.port, next, true);
			return;
		}
		stopListening(listen.all ? 'All controls mapped.' : `${nameOf(p, inp)}: ${bindTitle(b)}.`);
	}

	const SETTLE = 0.15; // how close to its rest every axis must be before mapping listens
	const SETTLE_MS = 2000; // or this long, for a control that rested pressed when mapping began

	function settled(pads) {
		for (const g of pads) {
			if (g.buttons.some((b) => b.pressed || b.value > 0.5))
				return false;
			const base = listen.base.get(g.id);
			if (base && g.axes.some((v, i) => Math.abs(v - (base[i] ?? 0)) > SETTLE))
				return false;
		}
		return true;
	}

	// While mapping: the first button pressed, or axis moved far from where it rested, on any gamepad.
	function pollListening(pads) {
		if (!listen.armed) {
			if (!settled(pads) && performance.now() - listen.since < SETTLE_MS)
				return;
			const snap = snapshot(pads);
			Object.assign(listen, { armed: true, rest: snap.axes, pressed: snap.pressed });
			for (const [id, axes] of snap.axes)
				if (!listen.base.has(id))
					listen.base.set(id, axes);
			return;
		}
		for (const g of pads) {
			const rest = listen.rest.get(g.id);
			const pressed = listen.pressed.get(g.id);
			if (!rest || !pressed) { // connected while mapping: from now on
				listen.rest.set(g.id, [...g.axes]);
				listen.base.set(g.id, [...g.axes]);
				listen.pressed.set(g.id, g.buttons.map((b) => b.pressed || b.value > 0.5));
				continue;
			}
			for (let i = 0; i < g.buttons.length; i++) {
				const on = g.buttons[i].pressed || g.buttons[i].value > 0.5;
				if (on && !pressed[i]) {
					bindNow({ kind: 'button', gp: g.id, index: i });
					return;
				}
				pressed[i] = on;
			}
			for (let i = 0; i < g.axes.length; i++) {
				const r0 = rest[i] ?? 0;
				if (r0 > HAT_REST) { // a hat: the direction pressed
					const at = hatStep(g.axes[i]);
					if (at >= 0) {
						bindNow({ kind: 'hat', gp: g.id, index: i, step: at });
						return;
					}
					continue;
				}
				const d = g.axes[i] - r0;
				if (Math.abs(d) >= AXIS_PICK) {
					bindNow({ kind: 'axis', gp: g.id, index: i, rest: restPoint(r0), full: d > 0 ? 1 : -1 });
					return;
				}
			}
		}
	}

	function onKeyDown(e) {
		if (listen) {
			e.preventDefault();
			e.stopImmediatePropagation();
			if (e.code === 'Escape')
				stopListening('');
			else if (!e.repeat && e.code)
				bindNow({ kind: 'key', code: e.code });
			return;
		}
		keysDown.add(e.code);
	}

	// ------------------------------------------------------------------ the wheels under the controllers

	function buildUnit(p) {
		const el = els.template.content.firstElementChild.cloneNode(true);
		el.id = `wheel-${p.index + 1}`;
		const title = el.querySelector('.pad-title');
		title.id = `wheel-${p.index + 1}-title`;
		el.setAttribute('aria-labelledby', title.id);
		const u = { el, art: el.querySelector('.pad-art'), title, status: el.querySelector('.pad-status'),
			rotor: el.querySelector('.w-rotor'), spots: new Map(), ffbName: el.querySelector('.wheel-ffb-name'),
			ffbLive: el.querySelector('.wheel-ffb .wheel-ffb-live'), read: el.querySelector('.wheel-read'), ffbBtn: el.querySelector('.wheel-ffb-btn') };
		u.ffbBtn.addEventListener('click', () => onConnectClick(p));
		const box = el.querySelector('.pad-spots');
		INPUTS.forEach((inp, at) => {
			const btn = document.createElement('button');
			btn.type = 'button';
			btn.className = `pad-spot${inp.round ? ' round' : ''}${inp.lead ? ' lead' : ''}`;
			btn.style.cssText = `--x: ${inp.x}; --y: ${inp.y}; --w: ${inp.w}; --h: ${inp.h}`;
			const cap = document.createElement('kbd');
			btn.append(cap);
			btn.addEventListener('click', () => {
				if (listen && listen.port === p.index && listen.at === at)
					stopListening('');
				else {
					stopListening('');
					startListening(p.index, at, false);
				}
			});
			box.append(btn);
			u.spots.set(inp.id, { btn, cap });
		});
		el.querySelector('.wheel-map').addEventListener('click', () => {
			stopListening('');
			startListening(p.index, 0, true);
		});
		el.querySelector('.wheel-clear').addEventListener('click', () => {
			stopListening('');
			p.binds = {};
			savePrefs();
			p.note = 'Mapping cleared.';
			renderUnits();
		});
		return u;
	}

	function renderUnits() {
		let any = false;
		for (const p of ports) {
			const want = p.type >= 0;
			any ||= want;
			if (want && !p.unit) {
				p.unit = buildUnit(p);
				els.row.append(p.unit.el);
				// keep USB 1 first
				if (p.index === 0 && els.row.firstElementChild !== p.unit.el)
					els.row.prepend(p.unit.el);
			} else if (!want && p.unit) {
				p.unit.el.remove();
				p.unit = null;
				if (listen && listen.port === p.index)
					listen = null;
			}
			if (!p.unit)
				continue;
			const u = p.unit;
			u.title.textContent = `USB ${p.index + 1} · ${TYPE_NAMES[p.type]}`;
			u.el.dataset.type = String(p.type);
			const has = INPUTS.filter((inp) => shown(p, inp));
			const unmapped = has.filter((inp) => !p.binds[inp.id]).length;
			u.status.textContent = p.note || (unmapped === has.length
				? 'Nothing plays it yet: map its controls (click one, or Map all controls).'
				: unmapped ? `${has.length - unmapped} of ${has.length} controls mapped.` : 'All controls mapped.');
			u.ffbName.textContent = p.message || (p.dev ? `your ${p.model.name}.` : 'nothing.');
			u.ffbLive.hidden = !p.dev;
			u.ffbBtn.textContent = p.dev ? 'Stop sending force feedback' : 'Choose FFB passthrough device…';
			INPUTS.forEach((inp, at) => {
				const s = u.spots.get(inp.id);
				s.btn.hidden = !shown(p, inp);
				const [x, y] = MODEL_INPUTS[p.type]?.at?.[inp.id] ?? [inp.x, inp.y];
				s.btn.style.setProperty('--x', x);
				s.btn.style.setProperty('--y', y);
				const b = p.binds[inp.id];
				const name = nameOf(p, inp);
				const label = listen && listen.port === p.index && listen.at === at ? '…' : inp.lead && !b ? (inp.label || inp.name) : bindLabel(b);
				if (s.cap.textContent !== label)
					s.cap.textContent = label;
				s.btn.classList.toggle('unbound', !b);
				s.btn.classList.toggle('listening', !!listen && listen.port === p.index && listen.at === at);
				s.btn.setAttribute('aria-label', `${name}: ${bindTitle(b)}. Change`);
				s.btn.title = `${name}: ${bindTitle(b)}`;
			});
		}
		els.wheels.hidden = !any;
		scheduleCables();
	}

	// What the emulated wheel gets now: steering -1 (full left) to 1, the pedals 0 to 1.
	function readout(v) {
		if (!v)
			return '';
		const steer = v[CID_STEER_R] - v[CID_STEER_L];
		return `Steering ${steer < 0 ? '−' : steer > 0 ? '+' : ' '}${Math.abs(steer).toFixed(3)}   Gas ${v[CID_GAS].toFixed(2)}   Brake ${v[CID_BRAKE].toFixed(2)}`;
	}

	// Shows what is pressed now, and turns the rim with the steering.
	function renderPressed(p, values) {
		const u = p.unit;
		if (!u)
			return;
		for (const inp of INPUTS) {
			const s = u.spots.get(inp.id);
			const on = shown(p, inp) && values[cidOf(p, inp)] >= 0.5;
			if (s.btn.classList.contains('down') !== on)
				s.btn.classList.toggle('down', on);
		}
		const deg = Math.round((values[CID_STEER_R] - values[CID_STEER_L]) * STEER_DEG);
		const css = `${deg}deg`;
		if (u.rotor.style.getPropertyValue('--steer') !== css)
			u.rotor.style.setProperty('--steer', css);
	}

	// The USB cables: from the socket's centre straight down, between the controllers (or beside them when they are
	// one under the other), to the top of the wheel's rim. In page coordinates, behind the page like the controllers'.
	let cableFrame = 0;
	function scheduleCables() {
		if (!cableFrame)
			cableFrame = requestAnimationFrame(drawCables);
	}

	function drawCables() {
		cableFrame = 0;
		const sx = window.scrollX, sy = window.scrollY;
		const pads = [$('pad-1'), $('pad-2')].map((el) => el && el.querySelector('.pad-art')).filter(Boolean)
			.map((a) => a.getBoundingClientRect()).filter((r) => r.width);
		const vw = document.documentElement.clientWidth;
		const paths = ports.map((p) => {
			if (!p.unit)
				return '';
			const sr = els.buttons[p.index].getBoundingClientRect();
			const ar = p.unit.art.getBoundingClientRect();
			if (!sr.width || !ar.width)
				return '';
			const x0 = sr.left + sr.width / 2 + sx, y0 = sr.top + sr.height / 2 + sy;
			const x1 = ar.left + ar.width * 200 / 600 + sx, y1 = ar.top + ar.height * 15 / 310 + sy;
			const off = p.index === 0 ? -4 : 4;
			let lane; // the x the cable comes down at, past the controllers
			if (pads.length === 2 && pads[1].top < pads[0].bottom) // side by side: between them
				lane = (pads[0].right + pads[1].left) / 2 + off + sx;
			else if (pads.length) // one under the other: beside them
				lane = Math.min(vw - 8, Math.max(...pads.map((r) => r.right)) + 14) + off + sx;
			else
				lane = x0;
			const top = (pads.length ? Math.min(...pads.map((r) => r.top)) : y0 + 60) - 30 + sy;
			const bottom = (pads.length ? Math.max(...pads.map((r) => r.bottom)) : y1 - 60) + 24 + sy;
			const k1 = Math.max(20, (top - y0) * 0.6);
			const k2 = Math.max(20, (y1 - bottom) * 0.6);
			return `M${x0.toFixed(1)} ${y0.toFixed(1)} C${x0.toFixed(1)} ${(y0 + k1).toFixed(1)} ${lane.toFixed(1)} ${(top - k1).toFixed(1)} ${lane.toFixed(1)} ${top.toFixed(1)} ` +
				`L${lane.toFixed(1)} ${bottom.toFixed(1)} C${lane.toFixed(1)} ${(bottom + k2).toFixed(1)} ${x1.toFixed(1)} ${(y1 - k2).toFixed(1)} ${x1.toFixed(1)} ${y1.toFixed(1)}`;
		});
		const key = paths.join('|');
		if (key === cableKey)
			return;
		cableKey = key;
		paths.forEach((d, i) => {
			for (const path of els.cables[i]) {
				path.setAttribute('d', d);
				path.dataset.on = String(!!d);
			}
		});
	}

	// ------------------------------------------------------------------ the console's USB sockets and their menu

	function portLabel(p) {
		return p.type < 0 ? 'nothing plugged in' : TYPE_NAMES[p.type] + (p.dev ? `, force feedback to your ${p.model.name}` : '');
	}

	function renderMenu() {
		for (const p of ports) {
			const btn = els.buttons[p.index];
			btn.dataset.plugged = String(p.type >= 0);
			btn.dataset.wheel = String(p.type >= 0 && !!p.dev);
			btn.setAttribute('aria-label', `USB port ${p.index + 1}: ${portLabel(p)}`);
		}
		const p = ports[menuPort];
		els.title.textContent = `USB ${menuPort + 1}`;
		for (const r of els.types)
			r.checked = Number(r.value) === p.type;
	}

	function render() {
		renderMenu();
		renderUnits();
	}

	function menuOpen() {
		return els.menu.matches(':popover-open');
	}

	// Above its socket, as the console's other menus.
	function placeMenu(btn) {
		const r = btn.getBoundingClientRect();
		const width = Math.min(320, window.innerWidth - 32);
		const left = Math.max(16, Math.min(r.left, window.innerWidth - width - 16));
		els.menu.style.left = `${Math.round(left)}px`;
		els.menu.style.bottom = `${Math.round(Math.max(8, window.innerHeight - r.top + 8))}px`;
	}

	function openMenu(index) {
		const wasOpen = menuOpen();
		if (wasOpen && menuPort === index) {
			els.menu.hidePopover();
			return;
		}
		menuPort = index;
		renderMenu();
		placeMenu(els.buttons[index]);
		if (!wasOpen)
			els.menu.showPopover();
		for (const b of els.buttons)
			b.setAttribute('aria-expanded', String(b === els.buttons[index]));
		(els.types.find((r) => r.checked) || els.types[0]).focus({ preventScroll: true });
	}

	// ------------------------------------------------------------------ the loop

	function tick() {
		const M = module();
		const pads = gamepadList();
		if (listen)
			pollListening(pads);
		const now = performance.now();
		for (const p of ports) {
			if (p.type < 0)
				continue;
			const values = readInputs(p, pads);
			p.lastValues = values;
			renderPressed(p, values);
			if (M && !listen)
				feedInputs(M, p, values, now);
		}
		if (!M)
			return;
		applyTypes(M, now);
		drainRing(M);
		const state = M._pcsx2web_state();
		const booting = (s) => s === WEB_STATE_STARTING || s === WEB_STATE_RUNNING;
		if (booting(state) && !booting(lastState))
			ports.forEach(resetForces); // a game starts: the wheel as at power on, long before the game looks at USB
		if (lastState === WEB_STATE_RUNNING && state !== WEB_STATE_RUNNING)
			ports.forEach(resetForces); // the game stopped: nothing will lift what it left on, and the spring comes back
		lastState = state;
		if (now - lastLive >= LIVE_MS) {
			lastLive = now;
			for (const p of ports)
				if (p.unit)
					p.unit.read.textContent = readout(p.lastValues);
			for (const p of ports)
				if (p.unit && p.dev)
					p.unit.ffbLive.textContent = `${p.forwarded} commands sent, ${p.blocked} held back`;
		}
	}

	function wire() {
		loadPrefs();
		for (const btn of els.buttons)
			btn.addEventListener('click', () => openMenu(Number(btn.dataset.usb) - 1));
		els.menu.addEventListener('toggle', (e) => {
			if (e.newState === 'closed')
				for (const b of els.buttons)
					b.setAttribute('aria-expanded', 'false');
		});
		for (const r of els.types) {
			r.addEventListener('change', () => {
				if (!r.checked)
					return;
				const p = ports[menuPort];
				p.type = Number(r.value);
				if (p.type < 0) { // unplugged: the FFB device is let go, back as it was
					disconnect(p);
					p.savedPid = 0;
					p.message = '';
				} else
					stopForces(p); // the old wheel is unplugged from the game
				savePrefs();
				render();
			});
		}
		if ('hid' in navigator) {
			navigator.hid.addEventListener('connect', reconnect);
			navigator.hid.addEventListener('disconnect', (e) => {
				for (const p of ports) {
					if (p.dev === e.device) {
						disconnect(p);
						p.message = 'It was unplugged.';
					}
				}
				render();
			});
		}
		window.addEventListener('keydown', onKeyDown, true);
		window.addEventListener('keyup', (e) => keysDown.delete(e.code), true);
		window.addEventListener('blur', () => keysDown.clear());
		window.addEventListener('resize', () => {
			if (menuOpen())
				placeMenu(els.buttons[menuPort]);
			scheduleCables();
		});
		if (typeof ResizeObserver === 'function')
			new ResizeObserver(scheduleCables).observe(document.body);
		window.addEventListener('pagehide', () => ports.forEach(disconnect));
		render();
		requestAnimationFrame(() => requestAnimationFrame(() => $('usb-cables').classList.add('live')));
		setInterval(tick, POLL_MS);
		reconnect();
	}

	wire();
})();
