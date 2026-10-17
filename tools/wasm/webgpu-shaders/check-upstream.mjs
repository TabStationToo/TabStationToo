#!/usr/bin/env node
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Lists the upstream changes that the WebGPU shaders have not picked up yet.
//
//   node tools/wasm/webgpu-shaders/check-upstream.mjs
//
// Each bin/resources/shaders/webgpu/*.wgsl names the Vulkan GLSL file it was ported from and the
// commit it was last synced with ("WebGPU port of vulkan/<file>.glsl (last synced with commit <hash>)").
// For each one this prints the later commits that touched the GLSL source, so they can be ported by
// hand (`git show <commit> -- <file>`), after which the hash in the header is updated. Exits 1 when
// a shader is behind. Run it after merging a newer upstream master.

import { execFileSync } from "node:child_process";
import { readdirSync, readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const REPO = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../../..");
const DIR = path.join(REPO, "bin/resources/shaders/webgpu");
const HEADER = /WebGPU port of vulkan\/([\w.-]+\.glsl) \(last synced with commit ([0-9a-f]{7,40})\)/;

let behind = 0;
for (const file of readdirSync(DIR).filter((f) => f.endsWith(".wgsl")).sort()) {
	const m = readFileSync(path.join(DIR, file), "utf8").match(HEADER);
	if (!m) {
		console.log(`${file}: no "WebGPU port of vulkan/<file>.glsl (last synced with commit <hash>)" header`);
		behind++;
		continue;
	}
	const [, source, commit] = m;
	const sourcePath = `bin/resources/shaders/vulkan/${source}`;
	const log = execFileSync("git", ["-C", REPO, "log", "--format=%h %ad %s", "--date=short", `${commit}..HEAD`, "--", sourcePath],
		{ encoding: "utf8" }).trim();
	if (!log) {
		console.log(`${file}: up to date with ${sourcePath} (${commit})`);
		continue;
	}
	behind++;
	console.log(`${file}: ${sourcePath} changed since ${commit}:`);
	for (const line of log.split("\n")) console.log(`  ${line}`);
}
process.exit(behind ? 1 : 0);
