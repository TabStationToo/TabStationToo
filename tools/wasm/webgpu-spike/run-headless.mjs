#!/usr/bin/env node
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Drives headless Chrome on the real GPU through the DevTools protocol, for the WebGPU spike.
// No dependencies: Node 22+ (global WebSocket) and Chrome.
//
//   node run-headless.mjs --url http://127.0.0.1:8765/index.html?test=all --profile-root DIR
//        [--gpu core|compat] [--out DIR] [--timeout 60] [--chrome /usr/bin/google-chrome] [--gpu-info]
//        [--no-default-flags] [--chrome-log] [-- extra Chrome flags]
//
// Chrome runs with a throwaway profile (created under --profile-root, deleted at exit) and never
// touches a real profile. The console of the page and of its workers is printed with a timestamp.
// Lines the page prints act as commands:
//   SHOT <name>   300 ms later, screenshot the page (to <out>/<name>.png) and print the centre pixel
//                 of every canvas
//   GPUCRASH      crash the GPU process (Browser.crashGpuProcess)
//   DONE / FAIL   stop (exit status 0 / 1)

import { spawn } from "node:child_process";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import zlib from "node:zlib";

// Headless Chrome 155 on Linux (RADV, Mesa 23.2), see README.md. Without --disable-vulkan-surface
// Chrome's own Vulkan fails to start (vkCreateInstance -7) and WebGPU only offers SwiftShader.
//   core:   Dawn's Vulkan backend on the real GPU (core features); compositing falls back to
//           software, so screenshots do not show WebGPU canvases (the page reads them back).
//   compat: Dawn's OpenGL ES backend through ANGLE on the real GPU (compatibility mode); GPU
//           compositing works and screenshots show the canvas. Use with ?adapter=compat.
const WEBGPU_FLAGS = ["--enable-unsafe-webgpu", "--enable-features=Vulkan", "--use-angle=vulkan", "--ignore-gpu-blocklist"];
const GPU_FLAGS = { core: ["--disable-vulkan-surface"], compat: ["--use-webgpu-adapter=opengles"] };

function parseArgs(argv) {
	const opts = { timeout: 60, chrome: "/usr/bin/google-chrome", out: null, url: null, profileRoot: tmpdir(),
		extra: [], gpu: "core", noDefaultFlags: false, chromeLog: false, gpuInfo: false, width: 1024, height: 768 };
	for (let i = 0; i < argv.length; i++) {
		const a = argv[i];
		if (a === "--") { opts.extra.push(...argv.slice(i + 1)); break; }
		else if (a === "--url") opts.url = argv[++i];
		else if (a === "--out") opts.out = argv[++i];
		else if (a === "--timeout") opts.timeout = Number(argv[++i]);
		else if (a === "--chrome") opts.chrome = argv[++i];
		else if (a === "--gpu") opts.gpu = argv[++i];
		else if (a === "--profile-root") opts.profileRoot = argv[++i];
		else if (a === "--no-default-flags") opts.noDefaultFlags = true;
		else if (a === "--chrome-log") opts.chromeLog = true;
		else if (a === "--gpu-info") opts.gpuInfo = true;
		else throw new Error(`unknown argument ${a}`);
	}
	if (!opts.url) throw new Error("--url is required");
	if (!GPU_FLAGS[opts.gpu]) throw new Error("--gpu must be core or compat");
	return opts;
}

// Minimal PNG decoder (8-bit RGB/RGBA, not interlaced), enough for Chrome's screenshots.
function decodePng(buf) {
	let pos = 8, width = 0, height = 0, colorType = 0;
	const idat = [];
	while (pos < buf.length) {
		const len = buf.readUInt32BE(pos);
		const type = buf.toString("latin1", pos + 4, pos + 8);
		const data = buf.subarray(pos + 8, pos + 8 + len);
		if (type === "IHDR") {
			width = data.readUInt32BE(0); height = data.readUInt32BE(4);
			if (data[8] !== 8 || data[12] !== 0) throw new Error("unsupported PNG");
			colorType = data[9];
		} else if (type === "IDAT") idat.push(data);
		pos += 12 + len;
	}
	const bpp = colorType === 6 ? 4 : colorType === 2 ? 3 : 0;
	if (!bpp) throw new Error(`unsupported PNG colour type ${colorType}`);
	const raw = zlib.inflateSync(Buffer.concat(idat));
	const stride = width * bpp, out = Buffer.alloc(height * stride);
	for (let y = 0; y < height; y++) {
		const f = raw[y * (stride + 1)], src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
		const row = out.subarray(y * stride, (y + 1) * stride), prev = y ? out.subarray((y - 1) * stride, y * stride) : null;
		for (let x = 0; x < stride; x++) {
			const a = x >= bpp ? row[x - bpp] : 0, b = prev ? prev[x] : 0, c = prev && x >= bpp ? prev[x - bpp] : 0;
			let v = src[x];
			if (f === 1) v += a;
			else if (f === 2) v += b;
			else if (f === 3) v += (a + b) >> 1;
			else if (f === 4) { const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
			row[x] = v & 255;
		}
	}
	return { width, height, pixel: (x, y) => [...out.subarray((y * width + x) * bpp, (y * width + x) * bpp + 3)] };
}

class Cdp {
	constructor(ws) {
		this.ws = ws; this.nextId = 1; this.pending = new Map(); this.handlers = [];
		ws.addEventListener("message", (ev) => {
			const msg = JSON.parse(ev.data);
			if (msg.id && this.pending.has(msg.id)) {
				const { resolve, reject } = this.pending.get(msg.id);
				this.pending.delete(msg.id);
				if (msg.error) reject(new Error(`${msg.error.message} (${msg.error.code})`));
				else resolve(msg.result);
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

async function main() {
	const opts = parseArgs(process.argv.slice(2));
	const outDir = opts.out ? path.resolve(opts.out) : null;
	if (outDir) mkdirSync(outDir, { recursive: true });
	mkdirSync(opts.profileRoot, { recursive: true });
	const profile = mkdtempSync(path.join(opts.profileRoot, "webgpu-spike-chrome-"));
	const flags = [
		"--headless=new", `--user-data-dir=${profile}`, "--remote-debugging-port=0",
		"--no-first-run", "--no-default-browser-check", "--disable-extensions",
		"--disable-background-networking", "--disable-sync", "--disable-component-update",
		"--disable-default-apps", "--metrics-recording-only", "--password-store=basic",
		`--window-size=${opts.width},${opts.height}`,
		...(opts.noDefaultFlags ? [] : [...WEBGPU_FLAGS, ...GPU_FLAGS[opts.gpu]]), ...opts.extra, "about:blank",
	];
	const chrome = spawn(opts.chrome, flags, { stdio: ["ignore", "ignore", "pipe"] });
	const t0 = Date.now();
	const stamp = () => ((Date.now() - t0) / 1000).toFixed(3).padStart(8);
	const log = [];
	const print = (line) => { const l = `${stamp()} ${line}`; log.push(l); console.log(l); };
	print(`[driver] chrome flags: ${flags.filter((f) => !f.startsWith("--user-data-dir")).join(" ")}`);

	let stderrBuf = "";
	const wsUrl = await new Promise((resolve, reject) => {
		const timer = setTimeout(() => reject(new Error("Chrome did not print its DevTools URL")), 15000);
		chrome.stderr.on("data", (d) => {
			stderrBuf += d.toString();
			const m = stderrBuf.match(/DevTools listening on (ws:\/\/\S+)/);
			if (m) { clearTimeout(timer); resolve(m[1]); }
		});
		if (opts.chromeLog) chrome.stderr.on("data", (d) => { for (const l of d.toString().split("\n")) if (l.trim()) print(`[chrome] ${l}`); });
		chrome.on("exit", (code) => reject(new Error(`Chrome exited early (${code}): ${stderrBuf}`)));
	});

	const ws = new WebSocket(wsUrl);
	await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
	const cdp = new Cdp(ws);
	let finish;
	const finished = new Promise((resolve) => { finish = resolve; });
	const timer = setTimeout(() => { print("[driver] TIMEOUT"); finish(3); }, opts.timeout * 1000);

	const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
	const { sessionId: page } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
	const sessions = new Map([[page, "page"]]);
	let shots = Promise.resolve();

	async function screenshot(name) {
		const rects = await cdp.send("Runtime.evaluate", { returnByValue: true, expression:
			"[...document.querySelectorAll('canvas')].map((c) => { const r = c.getBoundingClientRect(); return [c.id || 'canvas', r.x, r.y, r.width, r.height]; })" }, page);
		const shot = await cdp.send("Page.captureScreenshot", { format: "png" }, page);
		const png = Buffer.from(shot.data, "base64");
		if (outDir) writeFileSync(path.join(outDir, `${name}.png`), png);
		const img = decodePng(png);
		const parts = (rects.result.value ?? []).map(([id, x, y, w, h]) => {
			const cx = Math.floor(x + w / 2), cy = Math.floor(y + h / 2);
			return `#${id} centre (${cx},${cy}) = rgb(${img.pixel(cx, cy).join(",")})`;
		});
		print(`[driver] SHOT ${name}: ${parts.join("; ") || "no canvas"}`);
	}

	function onLine(line, where) {
		print(`[${where}] ${line}`);
		const m = line.match(/^SHOT (\S+)/);
		// 300 ms later, as the page samples it: a worker's frame reaches the page after its task ends.
		if (m) shots = shots.then(() => new Promise((r) => setTimeout(r, 300))).then(() => screenshot(m[1]))
			.catch((e) => print(`[driver] screenshot failed: ${e.message}`));
		else if (/^GPUCRASH\b/.test(line)) {
			cdp.send("Browser.crashGpuProcess").then(() => print("[driver] crashed the GPU process"),
				(e) => print(`[driver] Browser.crashGpuProcess failed: ${e.message}`));
		} else if (/^DONE\b/.test(line)) shots.then(() => finish(0));
		else if (/^FAIL\b/.test(line)) shots.then(() => finish(1));
	}

	cdp.on((msg) => {
		const where = sessions.get(msg.sessionId) ?? "?";
		if (msg.method === "Runtime.consoleAPICalled") {
			const text = msg.params.args.map((a) => a.value ?? a.description ?? a.unserializableValue ?? "").join(" ");
			for (const line of text.split("\n")) onLine(line, msg.params.type === "log" ? where : `${where}:${msg.params.type}`);
		} else if (msg.method === "Runtime.exceptionThrown") {
			const d = msg.params.exceptionDetails;
			print(`[${where}:exception] ${d.exception?.description ?? d.text}`);
		} else if (msg.method === "Log.entryAdded") {
			print(`[${where}:log-${msg.params.entry.level}] ${msg.params.entry.text}`);
		} else if (msg.method === "Target.attachedToTarget") {
			// Workers (pthreads) attach here; follow their consoles too, and their own workers.
			const { sessionId, targetInfo } = msg.params;
			sessions.set(sessionId, targetInfo.type === "worker" ? `worker:${targetInfo.title || targetInfo.url}`.slice(0, 40) : targetInfo.type);
			cdp.send("Runtime.enable", {}, sessionId).catch(() => {});
			cdp.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, sessionId).catch(() => {});
			cdp.send("Runtime.runIfWaitingForDebugger", {}, sessionId).catch(() => {});
		} else if (msg.method === "Inspector.targetCrashed") {
			print(`[${where}] TARGET CRASHED`); finish(4);
		}
	});

	await cdp.send("Page.enable", {}, page);
	await cdp.send("Runtime.enable", {}, page);
	await cdp.send("Log.enable", {}, page);
	await cdp.send("Inspector.enable", {}, page).catch(() => {});
	await cdp.send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }, page);
	await cdp.send("Page.navigate", { url: opts.url }, page);

	if (opts.gpuInfo) {
		// chrome://gpu fills itself in asynchronously; print its text once it has settled.
		const { targetId: gpuTarget } = await cdp.send("Target.createTarget", { url: "chrome://gpu", background: true });
		const { sessionId: gpu } = await cdp.send("Target.attachToTarget", { targetId: gpuTarget, flatten: true });
		await new Promise((r) => setTimeout(r, 3000));
		const res = await cdp.send("Runtime.evaluate", { returnByValue: true, expression:
			`(() => { const walk = (n) => { let t = ""; if (n.shadowRoot) t += walk(n.shadowRoot);
				for (const c of n.childNodes) { if (c.nodeType === 3) t += c.textContent; else if (c.nodeType === 1 && c.tagName !== "STYLE" && c.tagName !== "SCRIPT") {
					t += walk(c); if (c.tagName === "TD") t += " "; if (/^(DIV|TR|LI|H[1-6]|P|BR)$/.test(c.tagName)) t += "\\n"; } } return t; };
				return walk(document.body); })()` }, gpu);
		for (const line of String(res.result.value ?? "").split("\n")) if (line.trim()) print(`[chrome://gpu] ${line}`);
		await cdp.send("Target.closeTarget", { targetId: gpuTarget });
	}

	const exitCode = await finished;
	clearTimeout(timer);
	await shots;
	if (outDir) writeFileSync(path.join(outDir, "console.log"), log.join("\n") + "\n");
	await cdp.send("Browser.close").catch(() => {});
	ws.close();
	await new Promise((resolve) => {
		if (chrome.exitCode !== null) resolve();
		else { chrome.on("exit", resolve); setTimeout(() => { chrome.kill("SIGKILL"); resolve(); }, 5000); }
	});
	rmSync(profile, { recursive: true, force: true });
	process.exit(exitCode);
}

main().catch((e) => { console.error(e); process.exit(2); });
