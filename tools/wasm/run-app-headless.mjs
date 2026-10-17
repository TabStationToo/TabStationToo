#!/usr/bin/env node
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Boots the TabStationToo page in headless Chrome on the machine's GPU and measures or checks it, for
// automated tests of the WebGPU GS device and the 2D fallback. No dependencies: Node 22+ (global
// WebSocket), Python 3 (tools/wasm/serve.py) and Chrome.
//
//   node tools/wasm/run-app-headless.mjs --root build-wasm/bin --bios-dir DIR --bios FILE [options]
//
// Options:
//   --port N            serve.py port on 127.0.0.1 (default 8120)
//   --query Q           extra query string for index.html (for example "gpu=0")
//   --frames N          stop once N frames were presented (default 600); 0 runs until --timeout
//   --duration S        stop normally S seconds after the page loaded (with --frames 0)
//   --capture N,M,...   read back the first frame the page received at vsync N or later (frames are
//                       identified by the vsync they show, counted from boot; the summary records it)
//   --snapshot N,M,...  ask PCSX2 for a screenshot (SaveSnapshotToMemory) at vsync N and fetch the PNG
//                       (recorded as failed when none was saved 300 vsyncs later)
//   --keys F:KEY,...    press KEY (KeyboardEvent.code: Arrow*, Enter, Backspace, Escape, KeyA..KeyZ) for
//                       100 ms once vsync F is reached, as trusted input to the focused screen
//   --click F:SEL,...   click the element document.querySelector(SEL) once vsync F is reached
//   --reboot-at N       at vsync N press Stop, wait until the VM stopped, then press Boot again
//   --destroy-at N,...  ask the GS device to destroy itself at each vsync N (WebGPU only)
//   --crash-at N,...    crash Chrome's GPU process once each vsync N is reached
//   --sample-every S    print a sample every S seconds: frames, VPS, the page's JS heap, wasm memory, and
//                       (Linux) the GPU process's resident memory and the GS worker's JS heap
//   --gc-at N,...       at each vsync N, run a full garbage collection in the page and every worker
//   --display-at N,...  once vsync N is reached, read back what the display canvas shows (its mean colour
//                       and alpha, in the summary's display), whichever path draws it
//   --profile V:S       from vsync V, record S seconds of CPU profile (100 us sampling) of every worker
//                       and the page, saved as <out>/profile-<name>.cpuprofile
//   --out DIR           where captures, snapshots and console.log go
//   --profile-root DIR  where the throwaway Chrome profile is created (deleted at exit)
//   --timeout S         give up after S seconds (default 300)
//   --gpu core|swiftshader|none   Chrome's WebGPU adapter (default core: Dawn Vulkan on the GPU)
//   --init-script JS    run JS in the page and in every worker before their own scripts (for example
//                       to hide navigator.gpu)
//   --cpus LIST         run Chrome under taskset -c LIST
//   --chrome PATH       default /usr/bin/google-chrome
//   --chrome-flag F     an extra Chrome switch (repeatable)
//   --quiet             do not echo the page's console
//
// The page and worker consoles go to <out>/console.log, the page's log (its last 2000 lines) to
// <out>/page-log.txt. At the end a JSON summary is printed on the
// last line: boot and frame timestamps (ms after navigation), the sampled metrics, the captures and
// whether the run ended normally with no error (ok; uncaptured WebGPU errors, exceptions and crashes
// make it false and are listed in errors). Chrome runs with a throwaway profile and never touches a real one;
// Chrome and the server are stopped at exit.

import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import zlib from "node:zlib";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "../..");

// Headless Chrome on Linux with a hardware WebGPU adapter (see tools/wasm/webgpu-spike/README.md).
const GPU_FLAGS = {
	core: ["--enable-unsafe-webgpu", "--enable-features=Vulkan", "--use-angle=vulkan", "--ignore-gpu-blocklist",
		"--disable-vulkan-surface"],
	swiftshader: ["--enable-unsafe-webgpu", "--use-webgpu-adapter=swiftshader"],
	none: ["--disable-features=WebGPU"],
};

function parseArgs(argv) {
	const o = { root: path.join(REPO, "build-wasm/bin"), biosDir: null, bios: null, port: 8120, query: "", frames: 600,
		capture: [], snapshot: [], keys: [], clicks: [], rebootAt: 0, destroyAt: [], crashAt: [], gcAt: [], displayAt: [], sampleEvery: 0, profile: null, out: null, profileRoot: tmpdir(),
		timeout: 300, duration: 0, gpu: "core", cpus: null, chrome: "/usr/bin/google-chrome", chromeFlags: [], initScript: "", quiet: false };
	const list = (s) => s.split(",").filter(Boolean).map(Number);
	for (let i = 0; i < argv.length; i++) {
		const a = argv[i], v = () => argv[++i];
		if (a === "--root") o.root = path.resolve(v());
		else if (a === "--bios-dir") o.biosDir = path.resolve(v());
		else if (a === "--bios") o.bios = v();
		else if (a === "--port") o.port = Number(v());
		else if (a === "--query") o.query = v();
		else if (a === "--frames") o.frames = Number(v());
		else if (a === "--capture") o.capture = list(v());
		else if (a === "--snapshot") o.snapshot = list(v());
		else if (a === "--keys") o.keys = v().split(",").filter(Boolean).map((s) => { const [f, k] = s.split(":"); return { frame: Number(f), key: k }; });
		else if (a === "--click") o.clicks = v().split(",").filter(Boolean).map((s) => { const i = s.indexOf(":"); return { frame: Number(s.slice(0, i)), sel: s.slice(i + 1) }; });
		else if (a === "--reboot-at") o.rebootAt = Number(v());
		else if (a === "--destroy-at") o.destroyAt = list(v());
		else if (a === "--crash-at") o.crashAt = list(v());
		else if (a === "--sample-every") o.sampleEvery = Number(v());
		else if (a === "--gc-at") o.gcAt = list(v());
		else if (a === "--display-at") o.displayAt = list(v());
		else if (a === "--profile") { const [pv, ps] = v().split(":").map(Number); o.profile = { vsync: pv, seconds: ps }; }
		else if (a === "--out") o.out = path.resolve(v());
		else if (a === "--profile-root") o.profileRoot = path.resolve(v());
		else if (a === "--timeout") o.timeout = Number(v());
		else if (a === "--duration") o.duration = Number(v());
		else if (a === "--gpu") o.gpu = v();
		else if (a === "--cpus") o.cpus = v();
		else if (a === "--chrome") o.chrome = v();
		else if (a === "--chrome-flag") o.chromeFlags.push(v());
		else if (a === "--init-script") o.initScript = v();
		else if (a === "--quiet") o.quiet = true;
		else throw new Error(`unknown argument ${a}`);
	}
	if (!o.biosDir || !o.bios) throw new Error("--bios-dir and --bios are required");
	if (!GPU_FLAGS[o.gpu]) throw new Error("--gpu must be core, swiftshader or none");
	return o;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// A worker blocked in Atomics.wait never answers; samples must not wait for it.
const within = (promise, ms) => Promise.race([promise, sleep(ms).then(() => { throw new Error("no answer"); })]);

// Resident memory (kB) of Chrome's GPU process, a descendant of the process the driver started
// (Linux /proc only).
function gpuProcessRssKb(rootPid) {
	try {
		const children = new Map();
		const gpu = [];
		for (const pid of readdirSync("/proc").filter((d) => /^\d+$/.test(d))) {
			try {
				const stat = readFileSync(`/proc/${pid}/stat`, "latin1");
				const ppid = Number(stat.slice(stat.lastIndexOf(")") + 2).split(" ")[1]);
				if (!children.has(ppid)) children.set(ppid, []);
				children.get(ppid).push(Number(pid));
				if (readFileSync(`/proc/${pid}/cmdline`, "latin1").includes("--type=gpu-process")) gpu.push(Number(pid));
			} catch { /* exited */ }
		}
		const mine = new Set();
		for (let todo = [rootPid]; todo.length;) {
			const pid = todo.pop();
			mine.add(pid);
			todo.push(...(children.get(pid) || []));
		}
		const pid = gpu.find((p) => mine.has(p));
		const m = pid && /VmRSS:\s+(\d+)/.exec(readFileSync(`/proc/${pid}/status`, "latin1"));
		return m ? Number(m[1]) : null;
	} catch { /* not Linux */ }
	return null;
}

// What the display canvas shows (either context type), summarised.
const DISPLAY_PROBE = `(() => {
	const c = document.getElementById('canvas');
	const o = new OffscreenCanvas(c.width, c.height).getContext('2d', { willReadFrequently: true });
	o.drawImage(c, 0, 0);
	const d = o.getImageData(0, 0, c.width, c.height).data;
	let rgb = 0, alpha = 0;
	for (let i = 0; i < d.length; i += 4) { rgb += d[i] + d[i + 1] + d[i + 2]; alpha += d[i + 3]; }
	const s = window.PCSX2Web.stats();
	return { width: c.width, height: c.height, mean: rgb / (d.length / 4 * 3), alpha: alpha / (d.length / 4), vsync: s.vsync,
		display: s.display, t: performance.now() };
})()`;

// Evaluated in each worker: the GS worker is the one that owns the WebGPU device's canvas.
const GS_WORKER_PROBE = `typeof tstGs === 'object' && typeof WebGPU === 'object' ? { canvas: !!tstGs.canvas,
	objects: Object.keys(WebGPU.Internals.jsObjects).length, futures: Object.keys(WebGPU.Internals.futures).length } : null`;

// Windows virtual-key codes of the non-letter keys --keys accepts (letters are KeyA..KeyZ).
const KEY_CODES = { ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Enter: 13, Backspace: 8, Escape: 27 };

// RGBA rows to a PNG (8-bit RGBA, filter 0).
function encodePng(width, height, rgba) {
	const crcTable = Array.from({ length: 256 }, (_, n) => {
		let c = n;
		for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
		return c >>> 0;
	});
	const crc = (buf) => { let c = 0xffffffff; for (const b of buf) c = crcTable[(c ^ b) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; };
	const chunk = (type, data) => {
		const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
		const td = Buffer.concat([Buffer.from(type, "latin1"), data]);
		const c = Buffer.alloc(4); c.writeUInt32BE(crc(td));
		return Buffer.concat([len, td, c]);
	};
	const ihdr = Buffer.alloc(13);
	ihdr.writeUInt32BE(width, 0); ihdr.writeUInt32BE(height, 4); ihdr[8] = 8; ihdr[9] = 6;
	const raw = Buffer.alloc((width * 4 + 1) * height);
	for (let y = 0; y < height; y++) rgba.copy(raw, y * (width * 4 + 1) + 1, y * width * 4, (y + 1) * width * 4);
	return Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]), chunk("IHDR", ihdr),
		chunk("IDAT", zlib.deflateSync(raw)), chunk("IEND", Buffer.alloc(0))]);
}

class Cdp {
	constructor(ws) {
		this.ws = ws; this.nextId = 1; this.pending = new Map(); this.handlers = [];
		ws.addEventListener("message", (ev) => {
			const msg = JSON.parse(ev.data);
			if (msg.id && this.pending.has(msg.id)) {
				const { resolve, reject } = this.pending.get(msg.id);
				this.pending.delete(msg.id);
				if (msg.error) reject(new Error(`${msg.error.message} (${msg.error.code})`)); else resolve(msg.result);
			} else if (msg.method) for (const h of this.handlers) h(msg);
		});
	}
	send(method, params = {}, sessionId = undefined) {
		const id = this.nextId++;
		this.ws.send(JSON.stringify({ id, method, params, sessionId }));
		return new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
	}
	on(h) { this.handlers.push(h); }
}

// Runs in the page. Counts frames from the page's own counters: the 2D path's frame descriptor
// (pcsx2web_frame, one seq per presented frame) or, in WebGPU mode, the bitmaps the frame port
// delivered (PCSX2Web.stats().gpuFrames). Polls every 2 ms so frame times are accurate to a few ms.
const PAGE_PROBE = `(() => {
	if (window.__probe) return true;
	const p = window.__probe = { t: {}, frameTimes: {}, want: [], vsyncWant: [], done: {} };
	const seq = () => {
		const M = window.Module;
		if (!M || !M._pcsx2web_frame || !M.wasmMemory) return 0;
		const ptr = M._pcsx2web_frame() >>> 0;
		return Atomics.load(new Uint32Array(M.wasmMemory.buffer, ptr, 5), 0);
	};
	const vsync = () => { const M = window.Module; return M && M._pcsx2web_vsync_count ? M._pcsx2web_vsync_count() >>> 0 : 0; };
	p.frames = () => {
		const s = window.PCSX2Web ? window.PCSX2Web.stats() : null;
		return Math.max(seq(), s && s.gpuFrames ? s.gpuFrames : 0);
	};
	p.vsync = vsync;
	const tick = () => {
		const now = performance.now();
		const s = window.PCSX2Web ? window.PCSX2Web.stats() : null;
		if (s) {
			if (!p.t.runtime && s.phase === 'ready') p.t.runtime = now;
			if (!p.t.boot && (s.state === 'starting' || s.state === 'running')) p.t.boot = now;
			if (!p.t.running && s.state === 'running') p.t.running = now;
		}
		const f = p.frames();
		if (f && !p.t.firstFrame) p.t.firstFrame = now;
		for (const n of p.want) if (f >= n && !p.frameTimes[n]) p.frameTimes[n] = now;
		p.lastFrames = f;
	};
	setInterval(tick, 2);
	return true;
})()`;

async function main() {
	const o = parseArgs(process.argv.slice(2));
	const outDir = o.out;
	if (outDir) mkdirSync(outDir, { recursive: true });
	mkdirSync(o.profileRoot, { recursive: true });
	const profile = mkdtempSync(path.join(o.profileRoot, "tst-chrome-"));
	const children = [];
	const t0 = Date.now();
	const log = [];
	const stamp = () => ((Date.now() - t0) / 1000).toFixed(3).padStart(8);
	const print = (line, echo = !o.quiet) => { const l = `${stamp()} ${line}`; log.push(l); if (echo) console.log(l); };
	const summary = { ok: false, frames: {}, samples: [], captures: {}, snapshots: {}, display: {}, events: [], errors: [] };
	let finish;
	const finished = new Promise((r) => { finish = r; });

	const cleanup = () => {
		for (const c of children) if (c.exitCode === null) c.kill("SIGKILL");
		rmSync(profile, { recursive: true, force: true });
	};
	process.on("SIGINT", () => { cleanup(); process.exit(130); });

	let status = 2;
	try {
		const server = spawn("python3", [path.join(REPO, "tools/wasm/serve.py"), "--port", String(o.port), "--quiet",
			"--bios-dir", o.biosDir, o.root], { stdio: ["ignore", "ignore", "inherit"] });
		children.push(server);

		const flags = ["--headless=new", `--user-data-dir=${profile}`, "--remote-debugging-port=0", "--no-first-run",
			"--no-default-browser-check", "--disable-extensions", "--disable-background-networking", "--disable-sync",
			"--disable-component-update", "--disable-default-apps", "--metrics-recording-only", "--password-store=basic",
			"--window-size=1280,900", "--enable-precise-memory-info", ...GPU_FLAGS[o.gpu], ...o.chromeFlags, "about:blank"];
		const cmd = o.cpus ? "taskset" : o.chrome;
		const args = o.cpus ? ["-c", o.cpus, o.chrome, ...flags] : flags;
		print(`[driver] ${o.cpus ? `taskset -c ${o.cpus} ` : ""}chrome ${flags.filter((f) => !f.startsWith("--user-data-dir")).join(" ")}`);
		const chrome = spawn(cmd, args, { stdio: ["ignore", "ignore", "pipe"] });
		children.push(chrome);
		let stderr = "";
		const wsUrl = await new Promise((resolve, reject) => {
			const timer = setTimeout(() => reject(new Error("Chrome did not print its DevTools URL")), 15000);
			chrome.stderr.on("data", (d) => {
				stderr += d.toString();
				const m = stderr.match(/DevTools listening on (ws:\/\/\S+)/);
				if (m) { clearTimeout(timer); resolve(m[1]); }
			});
			chrome.on("exit", (code) => reject(new Error(`Chrome exited early (${code}): ${stderr}`)));
		});
		const ws = new WebSocket(wsUrl);
		await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
		const cdp = new Cdp(ws);

		const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
		const { sessionId: page } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
		const sessions = new Map([[page, "page"]]);
		const evaluate = async (expression, awaitPromise = false) => {
			const r = await cdp.send("Runtime.evaluate", { expression, returnByValue: true, awaitPromise }, page);
			if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description ?? r.exceptionDetails.text);
			return r.result.value;
		};

		cdp.on((msg) => {
			const where = sessions.get(msg.sessionId) ?? "?";
			if (msg.method === "Runtime.consoleAPICalled") {
				const text = msg.params.args.map((a) => a.value ?? a.description ?? a.unserializableValue ?? "").join(" ");
				for (const line of text.split("\n")) {
					print(`[${where}${msg.params.type === "log" ? "" : ":" + msg.params.type}] ${line}`);
					if (/uncaptured WebGPU error|WebGPU: uncaptured/i.test(line)) summary.errors.push(line);
				}
			} else if (msg.method === "Runtime.exceptionThrown") {
				const d = msg.params.exceptionDetails;
				const text = d.exception?.description ?? d.text;
				print(`[${where}:exception] ${text}`);
				summary.errors.push(`exception: ${text.split("\n")[0]}`);
			} else if (msg.method === "Log.entryAdded") {
				const entry = msg.params.entry;
				print(`[${where}:log-${entry.level}:${entry.source}] ${entry.text}`);
				// Messages Chrome itself logs (WebGPU validation warnings among them); console messages,
				// which the page also receives from its workers, are handled above. After a GPU-process
				// crash (--crash-at), Chrome refusing WebGPU contexts is expected: the page falls back.
				const expected = o.crashAt.length > 0 && /Failed to create WebGPU Context Provider/.test(entry.text);
				if (!expected && entry.source !== "javascript" && entry.source !== "worker" && entry.source !== "console-api" &&
					(entry.level === "error" || /WebGPU|GPUValidationError|Invalid /i.test(entry.text)))
					summary.errors.push(`log: ${entry.text.split("\n")[0]}`);
			} else if (msg.method === "Target.attachedToTarget") {
				const { sessionId, targetInfo } = msg.params;
				sessions.set(sessionId, targetInfo.type === "worker" ? "worker" : targetInfo.type);
				cdp.send("Runtime.enable", {}, sessionId).catch(() => {});
				// Browser-side messages of the worker (WebGPU warnings, for example) come through Log.
				cdp.send("Log.enable", {}, sessionId).catch(() => {});
				cdp.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: !!o.initScript, flatten: true }, sessionId).catch(() => {});
				// With --init-script the worker waits (waitForDebuggerOnStart) until the script ran in it.
				(o.initScript ? cdp.send("Runtime.evaluate", { expression: o.initScript }, sessionId).catch(() => {}) : Promise.resolve())
					.then(() => cdp.send("Runtime.runIfWaitingForDebugger", {}, sessionId)).catch(() => {});
			} else if (msg.method === "Inspector.targetCrashed") {
				print(`[${where}] TARGET CRASHED`);
				summary.errors.push("target crashed");
				finish(4);
			}
		});

		await cdp.send("Page.enable", {}, page);
		await cdp.send("Runtime.enable", {}, page);
		await cdp.send("Log.enable", {}, page);
		await cdp.send("Inspector.enable", {}, page).catch(() => {});
		await cdp.send("Emulation.setFocusEmulationEnabled", { enabled: true }, page).catch(() => {});
		await cdp.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: !!o.initScript, flatten: true }, page);
		if (o.initScript)
			await cdp.send("Page.addScriptToEvaluateOnNewDocument", { source: o.initScript }, page);
		for (let i = 0; i < 50; i++) {
			try { if ((await fetch(`http://127.0.0.1:${o.port}/index.html`)).ok) break; } catch { /* not up yet */ }
			await sleep(100);
		}
		const query = new URLSearchParams(o.query);
		query.set("bios", `dev-bios/${o.bios}`);
		if (o.capture.length) query.set("capture", o.capture.join(","));
		const url = `http://127.0.0.1:${o.port}/index.html?${query}`;
		print(`[driver] open ${url}`);
		const navAt = Date.now();
		await cdp.send("Page.navigate", { url }, page);
		const timer = setTimeout(() => { print("[driver] TIMEOUT"); summary.errors.push("timeout"); finish(3); }, o.timeout * 1000);

		// The probe goes in once the page has its globals.
		for (;;) {
			try { if (await evaluate("typeof window.PCSX2Web === 'object'")) break; } catch { /* navigating */ }
			await sleep(50);
		}
		await evaluate(PAGE_PROBE);
		const wants = [...new Set([o.frames, 1, 60, 100, 150, 200, 300, 400, 450, 600, ...o.capture].filter((n) => n > 0))];
		await evaluate(`window.__probe.want = ${JSON.stringify(wants)}`);

		const pending = {
			keys: [...o.keys].sort((a, b) => a.frame - b.frame),
			snapshots: [...o.snapshot],
			captures: [...o.capture],
			destroy: [...o.destroyAt].sort((a, b) => a - b),
			crash: [...o.crashAt].sort((a, b) => a - b),
			gc: [...o.gcAt].sort((a, b) => a - b),
			display: [...o.displayAt].sort((a, b) => a - b),
			profile: o.profile,
			clicks: [...o.clicks].sort((a, b) => a.frame - b.frame),
			reboot: o.rebootAt,
		};
		let lastSubtitle = "";
		let lastSample = 0;
		let gsWorker = null;
		const sampleT0 = Date.now();
		const loop = (async () => {
			for (;;) {
				await sleep(20);
				let st;
				try {
					st = await evaluate(`(() => { const p = window.__probe, s = window.PCSX2Web.stats();
						const m = performance.memory || {};
						const sub = document.getElementById('subtitle');
						return { frames: p.lastFrames || 0, vsync: p.vsync(), t: p.t, frameTimes: p.frameTimes, s,
							subtitle: sub && document.body.dataset.subtitle === 'on' ? sub.textContent : '',
							heap: m.usedJSHeapSize || 0, wasm: window.Module && window.Module.wasmMemory ? window.Module.wasmMemory.buffer.byteLength : 0 }; })()`);
				} catch (e) {
					continue;
				}
				summary.t = st.t;
				summary.frames = st.frameTimes;
				summary.last = st.s;
				if (st.subtitle !== lastSubtitle) {
					lastSubtitle = st.subtitle;
					if (st.subtitle) {
						summary.events.push({ at: st.vsync || st.frames, subtitle: st.subtitle });
						print(`[driver] subtitle: ${st.subtitle}`);
					}
				}
				if (pending.profile && (st.vsync || st.frames) >= pending.profile.vsync) {
					const { seconds } = pending.profile;
					pending.profile = null;
					const targets = [...sessions].filter(([, type]) => type === "worker" || type === "page");
					await Promise.all(targets.map(([sid]) => cdp.send("Profiler.enable", {}, sid)
						.then(() => cdp.send("Profiler.setSamplingInterval", { interval: 100 }, sid))
						.then(() => cdp.send("Profiler.start", {}, sid)).catch(() => {})));
					const before = { t: Date.now(), vsync: st.vsync, frames: st.frames };
					print(`[driver] profiling ${targets.length} targets for ${seconds} s from vsync ${st.vsync}`);
					await sleep(seconds * 1000);
					const after = await evaluate("({ vsync: window.__probe.vsync(), frames: window.__probe.lastFrames || 0 })");
					let i = 0;
					for (const [sid, type] of targets) {
						try {
							const { profile } = await cdp.send("Profiler.stop", {}, sid);
							const name = `profile-${type}-${i++}.cpuprofile`;
							if (outDir) writeFileSync(path.join(outDir, name), JSON.stringify(profile));
						} catch { /* gone */ }
					}
					summary.profile = { seconds: (Date.now() - before.t) / 1000, vsyncs: after.vsync - before.vsync, frames: after.frames - before.frames };
					print(`[driver] profile done: ${JSON.stringify(summary.profile)}`);
				}
				if (st.s.state === "failed" || st.s.phase === "dead") {
					print(`[driver] page state ${st.s.phase}/${st.s.state}`);
					summary.errors.push(`state ${st.s.phase}/${st.s.state}`);
					return finish(1);
				}
				const v = st.vsync || st.frames;
				while (pending.keys.length && v >= pending.keys[0].frame) {
					const { key } = pending.keys.shift();
					summary.events.push({ at: v, key });
					// Trusted key events through the input pipeline, with the screen focused (the page only
					// sends keys to the PS2 while it has focus; focus emulation makes a headless page focused).
					await evaluate("document.getElementById('screen').focus(), true");
					const vk = KEY_CODES[key] ?? (/^Key([A-Z])$/.test(key) ? key.charCodeAt(3) : 0);
					const keyName = /^Key([A-Z])$/.test(key) ? key.slice(3).toLowerCase() : key;
					const ke = (type) => cdp.send("Input.dispatchKeyEvent", { type, code: key, key: keyName, windowsVirtualKeyCode: vk,
						nativeVirtualKeyCode: vk }, page);
					await ke("keyDown");
					setTimeout(() => ke("keyUp").catch(() => {}), 100);
					const focus = await evaluate("`${document.hasFocus()} ${document.activeElement && document.activeElement.id} ${document.body.dataset.capture}`");
					print(`[driver] key ${key} at vsync ${v} (focus, active element, capture: ${focus})`);
				}
				for (const n of [...pending.snapshots]) {
					if (v + 30 >= n && !summary.snapshots[n]) {
						summary.snapshots[n] = { requested: true };
						await evaluate(`window.Module._pcsx2web_test_snapshot(${n})`);
					}
				}
				for (const n of [...pending.snapshots]) {
					const r = await evaluate(`(() => { try { const d = window.Module.FS.readFile('/snapshots/frame-${n}.png'); let s = ''; for (let i = 0; i < d.length; i += 32768) s += String.fromCharCode.apply(null, d.subarray(i, i + 32768)); return btoa(s); } catch { return null; } })()`);
					// The PNG is written by a thread: take it once it is complete (ends with IEND).
					if (r && Buffer.from(r, "base64").subarray(-8, -4).toString("latin1") === "IEND") {
						const png = Buffer.from(r, "base64");
						const name = `snapshot-${n}.png`;
						if (outDir) writeFileSync(path.join(outDir, name), png);
						summary.snapshots[n] = { file: name, bytes: png.length, sha1: createHash("sha1").update(png).digest("hex") };
						pending.snapshots.splice(pending.snapshots.indexOf(n), 1);
						print(`[driver] snapshot at vsync ${n}: ${png.length} bytes`);
					} else if (v > n + 300) {
						// PCSX2 saves nothing when the screenshot fails (a timed-out readback, for one) and
						// logs why; the summary records it as failed.
						summary.snapshots[n] = { requested: true, failed: true };
						pending.snapshots.splice(pending.snapshots.indexOf(n), 1);
						print(`[driver] snapshot at vsync ${n}: none saved by vsync ${v}`);
					}
				}
				for (const n of [...pending.captures]) {
					const r = await evaluate(`window.PCSX2Web.capture ? window.PCSX2Web.capture(${n}) : null`, true);
					if (r) {
						const rgba = Buffer.from(r.rgba, "base64");
						const name = `capture-${n}.png`;
						if (outDir) writeFileSync(path.join(outDir, name), encodePng(r.width, r.height, rgba));
						summary.captures[n] = { file: name, width: r.width, height: r.height, frame: r.frame, sha1: createHash("sha1").update(rgba).digest("hex") };
						pending.captures.splice(pending.captures.indexOf(n), 1);
						print(`[driver] capture for vsync ${n}: vsync ${r.frame}, ${r.width}x${r.height}`);
					}
				}
				while (pending.clicks.length && v >= pending.clicks[0].frame) {
					const { sel } = pending.clicks.shift();
					const pageT = await evaluate(`(document.querySelector(${JSON.stringify(sel)}).click(), performance.now())`);
					summary.events.push({ at: v, click: sel, pageT });
					print(`[driver] clicked ${sel} at vsync ${v}`);
				}
				if (pending.reboot && v >= pending.reboot) {
					pending.reboot = 0;
					await evaluate("document.getElementById('btn-stop').click(), true");
					summary.events.push({ at: v, stop: true, t: Date.now() - navAt });
					print(`[driver] pressed Stop at vsync ${v}`);
					for (let i = 0; i < 500 && (await evaluate("window.PCSX2Web.stats().state")) !== "stopped"; i++)
						await sleep(20);
					const pageT = await evaluate("(document.getElementById('slate-primary').click(), performance.now())");
					summary.events.push({ boot: true, t: Date.now() - navAt, pageT });
					print("[driver] the VM stopped; pressed Boot again");
				}
				// Events carry the page's performance.now() (pageT), the clock of stats().gpuDevices.
				if (pending.destroy.length && v >= pending.destroy[0]) {
					pending.destroy.shift();
					const pageT = await evaluate("(window.Module._pcsx2web_test_lose_device(), performance.now())");
					summary.events.push({ at: v, destroy: true, t: Date.now() - navAt, pageT });
					print(`[driver] asked the GS device to destroy itself at vsync ${v}`);
				}
				if (pending.crash.length && v >= pending.crash[0]) {
					pending.crash.shift();
					const pageT = await evaluate("performance.now()");
					summary.events.push({ at: v, crash: true, t: Date.now() - navAt, pageT });
					cdp.send("Browser.crashGpuProcess").then(() => print(`[driver] crashed the GPU process at vsync ${v}`),
						(e) => print(`[driver] Browser.crashGpuProcess failed: ${e.message}`));
				}
				while (pending.display.length && v >= pending.display[0]) {
					const n = pending.display.shift();
					summary.display[n] = await evaluate(DISPLAY_PROBE);
					print(`[driver] display at vsync ${n}: ${JSON.stringify(summary.display[n])}`);
				}
				if (pending.gc.length && v >= pending.gc[0]) {
					pending.gc.shift();
					const targets = [...sessions].filter(([, type]) => type === "worker" || type === "page");
					await Promise.all(targets.map(([sid]) => within(cdp.send("HeapProfiler.collectGarbage", {}, sid), 5000).catch(() => {})));
					summary.events.push({ at: v, gc: true });
					print(`[driver] collected garbage in ${targets.length} targets at vsync ${v}`);
				}
				if (o.sampleEvery && Date.now() - lastSample >= o.sampleEvery * 1000) {
					lastSample = Date.now();
					// The GS worker: found once, by its WebGPU canvas.
					if (!gsWorker || !sessions.has(gsWorker)) {
						gsWorker = null;
						for (const [sid, type] of sessions) {
							if (type !== "worker") continue;
							const r = await within(cdp.send("Runtime.evaluate", { expression: GS_WORKER_PROBE, returnByValue: true }, sid), 500).catch(() => null);
							if (r && r.result && r.result.value && r.result.value.canvas) { gsWorker = sid; break; }
						}
					}
					let gs = null;
					if (gsWorker) {
						const [heapUse, tables] = await Promise.all([
							within(cdp.send("Runtime.getHeapUsage", {}, gsWorker), 1000).catch(() => null),
							within(cdp.send("Runtime.evaluate", { expression: GS_WORKER_PROBE, returnByValue: true }, gsWorker), 1000).catch(() => null)]);
						gs = { heap: heapUse ? heapUse.usedSize : null, ...(tables && tables.result ? tables.result.value : {}) };
					}
					const sample = { t: (Date.now() - sampleT0) / 1000, frames: st.frames, vsync: st.vsync, vps: st.s.vps, speed: st.s.speed,
						heap: st.heap, wasm: st.wasm, display: st.s.display, gpuFrames: st.s.gpuFrames, frameVsync: st.s.vsync,
						gpuRssKb: gpuProcessRssKb(chrome.pid), gs };
					summary.samples.push(sample);
					print(`[driver] sample ${JSON.stringify(sample)}`);
				}
				const doneFrames = (o.frames && st.frames >= o.frames) || (o.duration && Date.now() - navAt >= o.duration * 1000);
				if (doneFrames && !pending.snapshots.length && !pending.captures.length && !pending.keys.length && !pending.destroy.length && !pending.crash.length && !pending.gc.length && !pending.display.length && !pending.profile && !pending.clicks.length && !pending.reboot)
					return finish(0);
			}
		})();
		status = await finished;
		clearTimeout(timer);
		summary.ok = status === 0 && summary.errors.length === 0;
		summary.navAt = navAt;
		// A page that hangs must not hang the driver.
		try { summary.last = await within(evaluate("window.PCSX2Web.stats()"), 5000); } catch { /* gone */ }
		try {
			const pageLog = await within(evaluate("window.PCSX2Web.logText()"), 5000);
			summary.logTail = pageLog.split("\n").slice(-30);
			if (outDir) writeFileSync(path.join(outDir, "page-log.txt"), pageLog + "\n");
		} catch { /* gone */ }
		await within(cdp.send("Browser.close"), 5000).catch(() => {});
		ws.close();
		await Promise.race([new Promise((r) => chrome.on("exit", r)), sleep(5000)]);
		void loop;
	} catch (e) {
		summary.errors.push(String(e && e.stack || e));
	} finally {
		cleanup();
	}
	if (outDir) writeFileSync(path.join(outDir, "console.log"), log.join("\n") + "\n");
	console.log(JSON.stringify(summary));
	process.exit(status === 0 && summary.errors.length ? 5 : status);
}

main().catch((e) => { console.error(e); process.exit(2); });
