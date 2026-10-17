#!/usr/bin/env node
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Validates the WebGPU utility shaders (bin/resources/shaders/webgpu/*.wgsl) in headless Chrome on
// the machine's GPU: shader module compilation, every render pipeline the GS device creates from
// them, a test draw with each, and a few pixel checks. See validate.js for what is checked.
// No dependencies: Node 22+ (global WebSocket), Python 3 (tools/wasm/serve.py) and Chrome.
//
//   node tools/wasm/webgpu-shaders/validate-shaders.mjs [--shaders DIR] [--port 8797] [--chrome /usr/bin/google-chrome]
//        [--work DIR] [--timeout 60] [--query mode=adapter|allowFallback=1] [--no-default-flags]
//        [--chrome-log] [-- extra Chrome flags]
//
// --query is passed to the page: mode=adapter only reports the adapter; allowFallback=1 accepts
// the SwiftShader fallback adapter (for example with --no-default-flags -- --enable-unsafe-webgpu).
//
// It copies the page and the shaders into a fresh web root under --work (default: the system
// temporary directory), serves it with serve.py, runs Chrome with a throwaway profile there, prints
// the page's report and exits 0 when everything passed. Chrome, the server and the temporary
// files are removed at exit. It never touches a real Chrome profile.

import { spawn } from "node:child_process";
import { copyFileSync, mkdirSync, mkdtempSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "../../..");

// The smallest set of flags that gives headless Chrome 155 a hardware WebGPU adapter on Linux
// (Vulkan, AMD RADV): without --disable-vulkan-surface, Chrome's own vkCreateInstance fails in
// headless mode (VK_ERROR_EXTENSION_NOT_PRESENT, it asks for surface extensions) and WebGPU only
// offers the SwiftShader fallback adapter; without --enable-unsafe-webgpu, --enable-features=Vulkan
// or --use-angle=vulkan there is no hardware adapter either. Even then the first requestAdapter()
// can return null (a transient CreateCommandBuffer failure), so validate.js retries.
const WEBGPU_FLAGS = ["--enable-unsafe-webgpu", "--enable-features=Vulkan", "--use-angle=vulkan", "--disable-vulkan-surface"];

function parseArgs(argv) {
	const opts = { shaders: path.join(REPO, "bin/resources/shaders/webgpu"), port: 8797, chrome: "/usr/bin/google-chrome", work: tmpdir(), timeout: 60, query: "", defaultFlags: true, chromeLog: false, extra: [] };
	for (let i = 0; i < argv.length; i++) {
		const a = argv[i];
		if (a === "--") { opts.extra.push(...argv.slice(i + 1)); break; }
		else if (a === "--port") opts.port = Number(argv[++i]);
		else if (a === "--shaders") opts.shaders = path.resolve(argv[++i]);
		else if (a === "--chrome") opts.chrome = argv[++i];
		else if (a === "--work") opts.work = argv[++i];
		else if (a === "--timeout") opts.timeout = Number(argv[++i]);
		else if (a === "--query") opts.query = argv[++i];
		else if (a === "--no-default-flags") opts.defaultFlags = false;
		else if (a === "--chrome-log") opts.chromeLog = true;
		else throw new Error(`unknown argument ${a}`);
	}
	return opts;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function main() {
	const opts = parseArgs(process.argv.slice(2));
	const work = mkdtempSync(path.join(path.resolve(opts.work), "webgpu-shaders-"));
	const root = path.join(work, "www");
	const profile = path.join(work, "profile");
	mkdirSync(path.join(root, "shaders"), { recursive: true });
	for (const f of ["validate.html", "validate.js"]) copyFileSync(path.join(HERE, f), path.join(root, f));
	for (const f of readdirSync(opts.shaders).filter((f) => f.endsWith(".wgsl"))) copyFileSync(path.join(opts.shaders, f), path.join(root, "shaders", f));

	const children = [];
	const cleanup = () => {
		for (const c of children) if (c.exitCode === null) c.kill("SIGKILL");
		rmSync(work, { recursive: true, force: true });
	};
	process.on("SIGINT", () => { cleanup(); process.exit(130); });

	let status = 2;
	try {
		const server = spawn("python3", [path.join(REPO, "tools/wasm/serve.py"), "--port", String(opts.port), "--quiet", root],
			{ stdio: ["ignore", "ignore", "inherit"] });
		children.push(server);

		const flags = ["--headless=new", `--user-data-dir=${profile}`, "--remote-debugging-port=0", "--no-first-run",
			"--no-default-browser-check", "--disable-extensions", "--disable-background-networking", "--disable-sync",
			"--disable-component-update", "--disable-default-apps", "--password-store=basic",
			...(opts.defaultFlags ? WEBGPU_FLAGS : []), ...opts.extra, "about:blank"];
		console.log(`[driver] chrome flags: ${flags.filter((f) => !f.startsWith("--user-data-dir")).join(" ")}`);
		const chrome = spawn(opts.chrome, flags, { stdio: ["ignore", "ignore", "pipe"] });
		children.push(chrome);
		let stderr = "";
		const wsUrl = await new Promise((resolve, reject) => {
			const timer = setTimeout(() => reject(new Error("Chrome did not print its DevTools URL")), 15000);
			chrome.stderr.on("data", (d) => {
				stderr += d.toString();
				const m = stderr.match(/DevTools listening on (ws:\/\/\S+)/);
				if (m) { clearTimeout(timer); resolve(m[1]); }
			});
			if (opts.chromeLog) chrome.stderr.on("data", (d) => process.stderr.write(d));
			chrome.on("exit", (code) => reject(new Error(`Chrome exited early (${code}): ${stderr}`)));
		});

		const ws = new WebSocket(wsUrl);
		await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
		let nextId = 1;
		const pending = new Map();
		let finish;
		const finished = new Promise((r) => { finish = r; });
		ws.addEventListener("message", (ev) => {
			const msg = JSON.parse(ev.data);
			if (msg.id && pending.has(msg.id)) {
				const { resolve, reject } = pending.get(msg.id);
				pending.delete(msg.id);
				if (msg.error) reject(new Error(msg.error.message)); else resolve(msg.result);
			} else if (msg.method === "Runtime.consoleAPICalled") {
				const text = msg.params.args.map((a) => a.value ?? a.description ?? "").join(" ");
				for (const line of text.split("\n")) {
					console.log(line);
					if (/^DONE\b/.test(line)) finish(0);
					else if (/^FAIL\b/.test(line)) finish(1);
				}
			} else if (msg.method === "Runtime.exceptionThrown") {
				const d = msg.params.exceptionDetails;
				console.log(`[page exception] ${d.exception?.description ?? d.text}`);
				finish(1);
			}
		});
		const send = (method, params = {}, sessionId) => new Promise((resolve, reject) => {
			const id = nextId++;
			pending.set(id, { resolve, reject });
			ws.send(JSON.stringify({ id, method, params, sessionId }));
		});

		// Wait for the server, then open the page.
		const url = `http://127.0.0.1:${opts.port}/validate.html${opts.query ? `?${opts.query}` : ""}`;
		for (let i = 0; i < 50; i++) {
			try { if ((await fetch(url)).ok) break; } catch { /* not up yet */ }
			await sleep(100);
		}
		const { targetId } = await send("Target.createTarget", { url: "about:blank" });
		const { sessionId } = await send("Target.attachToTarget", { targetId, flatten: true });
		await send("Runtime.enable", {}, sessionId);
		await send("Page.navigate", { url }, sessionId);
		const timer = setTimeout(() => { console.log("[driver] TIMEOUT"); finish(3); }, opts.timeout * 1000);
		status = await finished;
		clearTimeout(timer);
		await send("Browser.close").catch(() => {});
		ws.close();
		await Promise.race([new Promise((r) => chrome.on("exit", r)), sleep(5000)]);
	} finally {
		cleanup();
	}
	process.exit(status);
}

main().catch((e) => { console.error(e); process.exit(2); });
