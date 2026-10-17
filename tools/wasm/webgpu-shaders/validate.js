// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Validates bin/resources/shaders/webgpu/*.wgsl against the browser's WebGPU implementation:
// every shader module (getCompilationInfo), every render pipeline the GS device will create from
// them (createRenderPipelineAsync with explicit layouts), a draw with each pipeline (draw-time
// validation: binding sizes, sample types, attachments), and a few pixel checks of the integer
// conversions. Run it with validate-shaders.mjs; the page prints one line per result and ends with
// "DONE ..." or "FAIL ...". ?mode=adapter only prints the adapter; ?allowFallback=1 accepts the
// software (SwiftShader) adapter instead of failing.

const logEl = document.getElementById("log");
function log(line) {
	console.log(line);
	logEl.textContent += line + "\n";
}

const params = new URLSearchParams(location.search);
const SHADER_DIR = params.get("shaders") ?? "shaders";
const SIZE = 64; // render target and source texture size
const UBO_OFFSET = 256; // dynamic offset used for every draw (minUniformBufferOffsetAlignment)
const UBO_BINDING_SIZE = 96; // CONVERT_PUSH_CONSTANTS_SIZE
const PRIMID_CONSTANTS = { PRIMID_MAX: (1 << 24) - 1, PRIMID_MIN: -1 }; // GSShaderEnums.h

const F = {
	Color: "rgba8unorm",
	DepthColor: "r32float",
	UInt16: "r16uint",
	UInt32: "r32uint",
	UNorm8: "r8unorm",
	ColorClip: "rgba16float", // Vulkan's RGBA16_UNORM needs the optional texture-formats-tier1
	PrimID: "r32float",
	DepthStencil: "depth32float-stencil8", // optional feature; depth32float without it
	Stencil: "depth32float-stencil8", // DATE setup; depth24plus-stencil8 without the feature
	Present: navigator.gpu ? navigator.gpu.getPreferredCanvasFormat() : "bgra8unorm",
};

const ALPHA_BLEND = {
	color: { srcFactor: "src-alpha", dstFactor: "one-minus-src-alpha", operation: "add" },
	alpha: { srcFactor: "one", dstFactor: "zero", operation: "add" },
};
const IMGUI_BLEND = {
	color: { srcFactor: "src-alpha", dstFactor: "one-minus-src-alpha", operation: "add" },
	alpha: { srcFactor: "one", dstFactor: "one-minus-src-alpha", operation: "add" },
};

const hasStencil = (format) => format.includes("stencil");

// The pipelines a WebGPU GS device creates from the utility shaders. input: the group(1) layout
// and the source texture of the test draw ("float": rgba8unorm; "depth": the depth aspect of a
// depth32float-stencil8 texture; "r32f": an r32float texture; both of the last two use the
// unfilterable layout).
function buildCases() {
	// Depth-stencil states.
	const DS_WRITE = { format: F.DepthStencil, depthWriteEnabled: true, depthCompare: "always" }; // depth_out
	const DS_NONE = { format: F.DepthStencil, depthWriteEnabled: false, depthCompare: "always" };
	const STENCIL_SET = { compare: "always", failOp: "keep", depthFailOp: "keep", passOp: "replace" };
	const DS_DATM = { format: F.Stencil, depthWriteEnabled: false, depthCompare: "always",
		stencilFront: STENCIL_SET, stencilBack: STENCIL_SET, stencilReadMask: 1, stencilWriteMask: 1 };

	const cases = [];
	const add = (c) => cases.push({ vs: "vs_main", input: "float", depth: null, constants: {}, vertex: "pt1", ...c });
	const conv = (fs, targets, extra = {}) => add({ file: "convert", fs, targets, ...extra, constants: { ...PRIMID_CONSTANTS, ...(extra.constants ?? {}) } });
	const color = (format, more = {}) => ({ format, ...more });

	// convert.wgsl
	for (const mask of [0xf, 0x7, 0x8, 0x0]) conv("ps_copy", [color(F.Color, { writeMask: mask })], { name: `ps_copy mask=${mask}` });
	for (const input of ["depth", "r32f"]) {
		conv("ps_depth_copy", [color(F.DepthColor)], { input });
		conv("ps_depth_copy_ds", [], { input, depth: DS_WRITE });
	}
	conv("ps_downsample_copy", [color(F.Color)]);
	conv("ps_filter_transparency", [color(F.Color)]);
	conv("ps_convert_rgb5a1_16bits", [color(F.UInt16)]);
	for (const fs of ["ps_datm1", "ps_datm0", "ps_datm1_rta_correction", "ps_datm0_rta_correction"]) conv(fs, [], { depth: DS_DATM });
	conv("ps_rta_correction", [color(F.Color)]);
	conv("ps_rta_correction", [color(F.Color, { writeMask: 0x8 })], { name: "ps_rta_correction mask=8" });
	conv("ps_rta_decorrection", [color(F.Color)]);
	conv("ps_colclip_init", [color(F.ColorClip)]);
	conv("ps_colclip_resolve", [color(F.Color)]);
	for (const input of ["depth", "r32f"]) {
		conv("ps_convert_depth32_32bits", [color(F.UInt32)], { input, name: `ps_convert_depth32_32bits (DEPTH32_TO_32_BITS, ${input})` });
		conv("ps_convert_depth32_32bits", [color(F.UInt16)], { input, name: `ps_convert_depth32_32bits (DEPTH32_TO_16_BITS, ${input})` });
		conv("ps_convert_depth32_rgba8", [color(F.Color)], { input });
		conv("ps_convert_depth32_rgba8", [color(F.Color, { writeMask: 0x7 })], { input, name: `ps_convert_depth32_rgba8 (DEPTH32_TO_RGB8, ${input})` });
		conv("ps_convert_depth16_rgb5a1", [color(F.Color)], { input });
		conv("ps_convert_depth32_depth24", [color(F.DepthColor)], { input });
		conv("ps_convert_depth32_depth24_ds", [], { input, depth: DS_WRITE });
	}
	conv("ps_convert_primid_rgba8", [color(F.Color)], { input: "r32f" });
	for (const base of ["ps_convert_rgba8_depth32", "ps_convert_rgba8_depth24", "ps_convert_rgba8_depth16", "ps_convert_rgb5a1_depth16"]) {
		for (const biln of [0, 1]) {
			conv(base, [color(F.DepthColor)], { constants: { HAS_BILN: biln }, name: `${base} biln=${biln}` });
			conv(`${base}_ds`, [], { depth: DS_WRITE, constants: { HAS_BILN: biln }, name: `${base}_ds biln=${biln}` });
		}
	}
	for (const fs of ["ps_convert_rgb5a1_8i", "ps_convert_rgba_8i"]) {
		conv(fs, [color(F.UNorm8)]);
		conv(fs, [color(F.Color)], { name: `${fs} (rgba8 target)` });
	}
	conv("ps_convert_clut_4", [color(F.Color)]);
	conv("ps_convert_clut_8", [color(F.Color)]);
	conv("ps_yuv", [color(F.Color)]);
	for (let i = 0; i < 4; i++) {
		for (const ds of [false, true]) {
			conv(`ps_primid_image_init_${i}`, [color(F.PrimID, { writeMask: GPUColorWrite.RED })],
				{ depth: ds ? DS_NONE : null, name: `ps_primid_image_init_${i} ds=${ds ? 1 : 0}` });
		}
	}

	// present.wgsl
	for (const fs of ["ps_copy", "ps_filter_scanlines", "ps_filter_diagonal", "ps_filter_triangular", "ps_filter_complex",
		"ps_filter_lottes", "ps_4x_rgss", "ps_automagical_supersampling"]) {
		add({ file: "present", fs, targets: [color(F.Present)] });
	}

	// merge.wgsl (blended onto the merge target, as GSDeviceVK's m_merge pipelines)
	for (const fs of ["ps_main0", "ps_main1"]) add({ file: "merge", fs, targets: [color(F.Color, { blend: ALPHA_BLEND })] });

	// interlace.wgsl
	for (let i = 0; i < 5; i++) add({ file: "interlace", fs: `ps_main${i}`, targets: [color(F.Color)] });

	// shadeboost.wgsl
	add({ file: "shadeboost", fs: "ps_main", targets: [color(F.Color)] });

	// imgui.wgsl
	add({ file: "imgui", fs: "ps_main", vertex: "imgui", targets: [color(F.Present, { blend: IMGUI_BLEND })] });

	for (const c of cases) c.name ??= c.input === "float" ? c.fs : `${c.fs} (${c.input})`;
	return cases;
}

const failures = [];
function fail(what, detail) {
	failures.push(what);
	log(`  FAIL ${what}: ${detail}`);
}

async function main() {
	if (!navigator.gpu) return log("FAIL navigator.gpu is missing");
	// Right after startup the GPU process may not be ready yet (Chrome logs a transient
	// CreateCommandBuffer failure and returns no adapter, or only the fallback adapter), so retry.
	let adapter = null;
	for (let attempt = 1; attempt <= 10; attempt++) {
		adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
		const desc = adapter ? `${adapter.info.vendor}/${adapter.info.architecture}${adapter.info.isFallbackAdapter ? " (fallback)" : ""}` : "none";
		log(`requestAdapter attempt ${attempt}: ${desc}`);
		if (adapter && !adapter.info.isFallbackAdapter) break;
		if (params.get("allowFallback") === "1" && adapter) break;
		await new Promise((r) => setTimeout(r, 1000));
	}
	if (!adapter) return log("FAIL requestAdapter returned null");
	if (adapter.info.isFallbackAdapter && params.get("allowFallback") !== "1") return log("FAIL only the fallback (software) adapter is available");
	const info = adapter.info ?? {};
	log(`ADAPTER vendor=${info.vendor} architecture=${info.architecture} device=${info.device} ` +
		`description="${info.description}" fallback=${info.isFallbackAdapter ?? false}`);
	log(`FEATURES ${[...adapter.features].sort().join(" ")}`);
	log(`USER AGENT ${navigator.userAgent}`);
	if (params.get("mode") === "adapter") return log("DONE adapter only");

	// No optional features beyond depth32float-stencil8 (the device's stencil_buffer format): the
	// utility shaders must work on the core baseline.
	const requiredFeatures = [];
	if (adapter.features.has("depth32float-stencil8")) requiredFeatures.push("depth32float-stencil8");
	else { F.DepthStencil = "depth32float"; F.Stencil = "depth24plus-stencil8"; }
	log(`DEPTH FORMATS depth_out=${F.DepthStencil} datm=${F.Stencil}`);
	const device = await adapter.requestDevice({ requiredFeatures });
	const uncaptured = [];
	device.addEventListener("uncapturederror", (ev) => uncaptured.push(ev.error.message));
	device.lost.then((l) => log(`DEVICE LOST ${l.reason}: ${l.message}`));

	// Shader modules: one per file, as the device will create them.
	const files = ["convert", "present", "merge", "interlace", "shadeboost", "imgui"];
	const modules = {};
	let warnings = 0;
	for (const file of files) {
		const code = await (await fetch(`${SHADER_DIR}/${file}.wgsl`, { cache: "no-store" })).text();
		device.pushErrorScope("validation");
		const module = device.createShaderModule({ label: file, code });
		const info = await module.getCompilationInfo();
		const err = await device.popErrorScope();
		const msgs = info.messages.map((m) => `${m.type} ${file}.wgsl:${m.lineNum}:${m.linePos} ${m.message}`);
		const errors = info.messages.filter((m) => m.type === "error").length;
		warnings += info.messages.filter((m) => m.type === "warning").length;
		log(`MODULE ${file}.wgsl: ${code.split("\n").length} lines, ${errors} errors, ${info.messages.length - errors} other messages`);
		for (const m of msgs) log(`  ${m}`);
		if (errors || err) fail(`module ${file}`, err ? err.message : msgs.join("; "));
		modules[file] = module;
	}

	// Layouts: group 0 = per-draw constants (dynamic offset), group 1 = source texture + sampler.
	const group0Layout = device.createBindGroupLayout({ label: "utility constants", entries: [
		{ binding: 0, visibility: GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT, buffer: { type: "uniform", hasDynamicOffset: true } },
	] });
	const texLayout = (sampleType, samplerType) => device.createBindGroupLayout({ label: `utility texture ${sampleType}`, entries: [
		{ binding: 0, visibility: GPUShaderStage.FRAGMENT, texture: { sampleType, viewDimension: "2d" } },
		{ binding: 1, visibility: GPUShaderStage.FRAGMENT, sampler: { type: samplerType } },
	] });
	const group1Float = texLayout("float", "filtering");
	const group1Unfilterable = texLayout("unfilterable-float", "non-filtering");
	const layouts = {
		float: device.createPipelineLayout({ label: "utility float", bindGroupLayouts: [group0Layout, group1Float] }),
		unfilterable: device.createPipelineLayout({ label: "utility unfilterable", bindGroupLayouts: [group0Layout, group1Unfilterable] }),
	};

	// Resources for the test draws.
	const usage = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC | GPUTextureUsage.COPY_DST;
	const texCache = new Map();
	const texture = (format, key = "") => {
		const k = `${format}/${key}`;
		if (!texCache.has(k)) texCache.set(k, device.createTexture({ label: k, size: [SIZE, SIZE], format, usage }));
		return texCache.get(k);
	};
	const pattern = new Uint8Array(SIZE * SIZE * 4);
	for (let i = 0, s = 12345; i < pattern.length; i++) { s = (s * 1103515245 + 12345) >>> 0; pattern[i] = s >>> 24; }
	const srcColor = texture(F.Color, "src");
	device.queue.writeTexture({ texture: srcColor }, pattern, { bytesPerRow: SIZE * 4 }, [SIZE, SIZE]);
	const srcR32f = texture("r32float", "src");
	device.queue.writeTexture({ texture: srcR32f }, new Float32Array(SIZE * SIZE).fill(0.25), { bytesPerRow: SIZE * 4 }, [SIZE, SIZE]);
	const srcDepth = texture(F.DepthStencil, "src");
	{
		// Fill the depth source by clearing it.
		const enc = device.createCommandEncoder();
		enc.beginRenderPass({ colorAttachments: [], depthStencilAttachment: { view: srcDepth.createView(),
			depthClearValue: 0.5, depthLoadOp: "clear", depthStoreOp: "store",
			...(hasStencil(F.DepthStencil) ? { stencilClearValue: 0, stencilLoadOp: "clear", stencilStoreOp: "store" } : {}) } }).end();
		device.queue.submit([enc.finish()]);
	}
	const linear = device.createSampler({ minFilter: "linear", magFilter: "linear" });
	const nearest = device.createSampler({});
	const sourceView = {
		float: srcColor.createView(),
		depth: srcDepth.createView({ aspect: "depth-only" }),
		r32f: srcR32f.createView(),
	};
	const group1 = {};
	for (const [kind, view] of Object.entries(sourceView)) {
		const float = kind === "float";
		group1[kind] = device.createBindGroup({ layout: float ? group1Float : group1Unfilterable,
			entries: [{ binding: 0, resource: view }, { binding: 1, resource: float ? linear : nearest }] });
	}
	const group1Nearest = device.createBindGroup({ layout: group1Float, entries: [{ binding: 0, resource: sourceView.float }, { binding: 1, resource: nearest }] });
	const ubo = device.createBuffer({ size: 1024, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
	const group0 = device.createBindGroup({ layout: group0Layout, entries: [{ binding: 0, resource: { buffer: ubo, size: UBO_BINDING_SIZE } }] });
	const setUniforms = (bytes) => {
		const data = new Uint8Array(UBO_BINDING_SIZE);
		data.set(new Uint8Array(bytes.buffer ?? bytes));
		device.queue.writeBuffer(ubo, UBO_OFFSET, data);
	};
	// Plausible constants for every utility shader at once: the first 32 bytes hold the
	// downsample/indexed/CLUT/YUV/merge/interlace values, the present fields follow.
	{
		const dv = new DataView(new ArrayBuffer(UBO_BINDING_SIZE));
		dv.setInt32(0, 0, true); dv.setInt32(4, 1, true); dv.setInt32(8, 2, true); dv.setFloat32(16, 4, true); dv.setFloat32(20, 1, true);
		for (let o = 32; o < 96; o += 4) dv.setFloat32(o, 1.0, true);
		setUniforms(new Uint8Array(dv.buffer));
	}

	// Full-target quad, triangle strip: D3D/WebGPU clip space, (0,0) texture origin at the top left.
	const quadPT1 = new Float32Array([
		-1, 1, 0, 1, 0, 0, 0, 0,
		1, 1, 0, 1, 1, 0, 0, 0,
		-1, -1, 0, 1, 0, 1, 0, 0,
		1, -1, 0, 1, 1, 1, 0, 0,
	]);
	const vbPT1 = device.createBuffer({ size: quadPT1.byteLength, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
	device.queue.writeBuffer(vbPT1, 0, quadPT1);
	const quadImGui = new ArrayBuffer(4 * 20);
	{
		const dv = new DataView(quadImGui);
		[[0, 0], [SIZE, 0], [0, SIZE], [SIZE, SIZE]].forEach(([x, y], i) => {
			dv.setFloat32(i * 20, x, true); dv.setFloat32(i * 20 + 4, y, true);
			dv.setFloat32(i * 20 + 8, x / SIZE, true); dv.setFloat32(i * 20 + 12, y / SIZE, true);
			dv.setUint32(i * 20 + 16, 0xffffffff, true);
		});
	}
	const vbImGui = device.createBuffer({ size: quadImGui.byteLength, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
	device.queue.writeBuffer(vbImGui, 0, quadImGui);

	const vertexLayouts = {
		pt1: [{ arrayStride: 32, attributes: [
			{ shaderLocation: 0, format: "float32x4", offset: 0 },
			{ shaderLocation: 1, format: "float32x2", offset: 16 }] }],
		imgui: [{ arrayStride: 20, attributes: [
			{ shaderLocation: 0, format: "float32x2", offset: 0 },
			{ shaderLocation: 1, format: "float32x2", offset: 8 },
			{ shaderLocation: 2, format: "unorm8x4", offset: 16 }] }],
	};

	function pipelineDescriptor(c) {
		return {
			label: c.name,
			layout: c.input === "float" ? layouts.float : layouts.unfilterable,
			vertex: { module: modules[c.file], entryPoint: c.vs, buffers: vertexLayouts[c.vertex] },
			fragment: { module: modules[c.file], entryPoint: c.fs, constants: c.constants,
				targets: c.targets.map((t) => ({ format: t.format, writeMask: t.writeMask ?? GPUColorWrite.ALL, blend: t.blend })) },
			primitive: { topology: "triangle-strip" },
			depthStencil: c.depth ?? undefined,
		};
	}

	function draw(c, pipeline, { targets, depthView, group1Override } = {}) {
		const enc = device.createCommandEncoder({ label: c.name });
		const pass = enc.beginRenderPass({
			colorAttachments: c.targets.map((t, i) => ({ view: (targets?.[i] ?? texture(t.format)).createView(),
				loadOp: "clear", storeOp: "store", clearValue: [0, 0, 0, 0] })),
			depthStencilAttachment: c.depth ? { view: depthView ?? texture(c.depth.format).createView(),
				depthClearValue: 0, depthLoadOp: "clear", depthStoreOp: "store",
				...(hasStencil(c.depth.format) ? { stencilClearValue: 0, stencilLoadOp: "clear", stencilStoreOp: "store" } : {}) } : undefined,
		});
		pass.setPipeline(pipeline);
		pass.setBindGroup(0, group0, [UBO_OFFSET]);
		pass.setBindGroup(1, group1Override ?? group1[c.input]);
		pass.setVertexBuffer(0, c.vertex === "imgui" ? vbImGui : vbPT1);
		if (c.depth?.stencilFront) pass.setStencilReference(1);
		pass.draw(4);
		pass.end();
		device.queue.submit([enc.finish()]);
	}

	async function readback(tex, bpp) {
		const bytesPerRow = Math.ceil((SIZE * bpp) / 256) * 256;
		const buf = device.createBuffer({ size: bytesPerRow * SIZE, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
		const enc = device.createCommandEncoder();
		enc.copyTextureToBuffer({ texture: tex }, { buffer: buf, bytesPerRow }, [SIZE, SIZE]);
		device.queue.submit([enc.finish()]);
		await buf.mapAsync(GPUMapMode.READ);
		const all = new Uint8Array(buf.getMappedRange()).slice();
		buf.unmap();
		buf.destroy();
		const out = new Uint8Array(SIZE * SIZE * bpp);
		for (let y = 0; y < SIZE; y++) out.set(all.subarray(y * bytesPerRow, y * bytesPerRow + SIZE * bpp), y * SIZE * bpp);
		return out;
	}

	// Pipelines and test draws.
	const cases = buildCases();
	const pipelines = new Map();
	let created = 0;
	for (const c of cases) {
		let pipeline;
		try {
			pipeline = await device.createRenderPipelineAsync(pipelineDescriptor(c));
		} catch (e) {
			fail(`pipeline ${c.file}:${c.name}`, `${e.reason ?? ""} ${e.message}`);
			continue;
		}
		created++;
		pipelines.set(c.name + "@" + c.file, { c, pipeline });
		device.pushErrorScope("validation");
		draw(c, pipeline);
		const err = await device.popErrorScope();
		if (err) fail(`draw ${c.file}:${c.name}`, err.message);
		else log(`  ok ${c.file}:${c.name}`);
	}
	log(`PIPELINES ${created}/${cases.length} created and drawn`);

	// Pixel checks.
	const get = (file, name) => pipelines.get(name + "@" + file);
	async function check(name, fn) {
		device.pushErrorScope("validation");
		let result;
		try {
			result = await fn();
		} catch (e) {
			result = `exception ${e.message}`;
		}
		const err = await device.popErrorScope();
		if (err) fail(`check ${name}`, err.message);
		else if (result !== true) fail(`check ${name}`, result);
		else log(`  ok check ${name}`);
	}
	const compare = (got, want, label, tolerance = 0) => {
		for (let i = 0; i < want.length; i++) {
			if (Math.abs(got[i] - want[i]) > tolerance) return `${label}: element ${i} is ${got[i]}, expected ${want[i]}`;
		}
		return true;
	};
	const rgbOnly = (px) => px.map((v, i) => (i % 4 === 3 ? 0 : v));

	await check("convert ps_copy is an exact copy, not flipped", async () => {
		const { c, pipeline } = get("convert", "ps_copy mask=15");
		const dst = texture(F.Color, "check");
		draw(c, pipeline, { targets: [dst], group1Override: group1Nearest });
		return compare(await readback(dst, 4), pattern, "rgba");
	});
	await check("convert rgba8 -> depth24 (r32float) -> rgb8 round trip", async () => {
		const toDepth = get("convert", "ps_convert_rgba8_depth24 biln=0");
		const mid = texture(F.DepthColor, "check");
		draw(toDepth.c, toDepth.pipeline, { targets: [mid], group1Override: group1Nearest });
		const toColor = get("convert", "ps_convert_depth32_rgba8 (DEPTH32_TO_RGB8, r32f)");
		const dst = texture(F.Color, "check");
		const g1 = device.createBindGroup({ layout: group1Unfilterable, entries: [{ binding: 0, resource: mid.createView() }, { binding: 1, resource: nearest }] });
		draw(toColor.c, toColor.pipeline, { targets: [dst], group1Override: g1 });
		return compare(await readback(dst, 4), rgbOnly(pattern), "rgb");
	});
	await check("convert rgba8 -> depth24 (frag_depth) -> rgb8 round trip", async () => {
		const toDepth = get("convert", "ps_convert_rgba8_depth24_ds biln=0");
		const ds = texture(F.DepthStencil, "check");
		draw(toDepth.c, toDepth.pipeline, { depthView: ds.createView(), group1Override: group1Nearest });
		const toColor = get("convert", "ps_convert_depth32_rgba8 (DEPTH32_TO_RGB8, depth)");
		const dst = texture(F.Color, "check");
		const g1 = device.createBindGroup({ layout: group1Unfilterable, entries: [{ binding: 0, resource: ds.createView({ aspect: "depth-only" }) }, { binding: 1, resource: nearest }] });
		draw(toColor.c, toColor.pipeline, { targets: [dst], group1Override: g1 });
		return compare(await readback(dst, 4), rgbOnly(pattern), "rgb");
	});
	await check("convert ps_convert_rgb5a1_16bits packs RGB5A1", async () => {
		const { c, pipeline } = get("convert", "ps_convert_rgb5a1_16bits");
		const dst = texture(F.UInt16, "check");
		draw(c, pipeline, { targets: [dst], group1Override: group1Nearest });
		const got = new Uint16Array((await readback(dst, 2)).buffer);
		const want = new Uint16Array(SIZE * SIZE);
		for (let i = 0; i < want.length; i++) {
			const [r, g, b, a] = pattern.subarray(i * 4, i * 4 + 4);
			want[i] = ((r & 0xf8) >> 3) | ((g & 0xf8) << 2) | ((b & 0xf8) << 7) | ((a & 0x80) << 8);
		}
		return compare(got, want, "rgb5a1");
	});
	await check("merge ps_main1 takes alpha from BGColor", async () => {
		const { c, pipeline } = get("merge", "ps_main1");
		const noBlend = await device.createRenderPipelineAsync({ ...pipelineDescriptor(c), fragment: { ...pipelineDescriptor(c).fragment, targets: [{ format: F.Color }] } });
		setUniforms(new Float32Array([0, 0, 0, 0.25]));
		const dst = texture(F.Color, "check");
		draw(c, noBlend, { targets: [dst], group1Override: group1Nearest });
		const want = pattern.map((v, i) => (i % 4 === 3 ? 64 : v));
		return compare(await readback(dst, 4), want, "rgba");
	});
	await check("convert ps_yuv EMODA=1 EMODC=1 gives luma", async () => {
		const { c, pipeline } = get("convert", "ps_yuv");
		setUniforms(new Int32Array([1, 1]));
		const dst = texture(F.Color, "check");
		draw(c, pipeline, { targets: [dst], group1Override: group1Nearest });
		const want = new Uint8Array(SIZE * SIZE * 4);
		for (let i = 0; i < SIZE * SIZE; i++) {
			const [r, g, b] = pattern.subarray(i * 4, i * 4 + 3);
			const y = (0xdb / 255) * (0.587 * g / 255 + 0.114 * b / 255 + 0.299 * r / 255) + 0x10 / 255;
			want.fill(Math.round(y * 255), i * 4, i * 4 + 4);
		}
		return compare(await readback(dst, 4), want, "luma", 1);
	});
	await check("imgui with uScale=(2/w,-2/h) uTranslate=(-1,1) is not flipped", async () => {
		const { c } = get("imgui", "ps_main");
		const desc = pipelineDescriptor(c);
		const noBlend = await device.createRenderPipelineAsync({ ...desc, fragment: { ...desc.fragment, targets: [{ format: F.Color }] } });
		setUniforms(new Float32Array([2 / SIZE, -2 / SIZE, -1, 1]));
		const dst = texture(F.Color, "check");
		draw({ ...c, targets: [{ format: F.Color }] }, noBlend, { targets: [dst], group1Override: group1Nearest });
		return compare(await readback(dst, 4), pattern, "rgba");
	});
	// GSDevice::ShadeBoost() passes the settings divided by 50: 50 everywhere is the identity.
	const shadeBoostReference = ([brt, con, sat, gam]) => {
		const out = new Uint8Array(SIZE * SIZE * 4);
		for (let i = 0; i < SIZE * SIZE; i++) {
			const c = [0, 1, 2].map((k) => (pattern[i * 4 + k] / 255) * brt);
			const lum = 0.2125 * c[0] + 0.7154 * c[1] + 0.0721 * c[2];
			for (let k = 0; k < 3; k++) {
				const s = lum + (c[k] - lum) * sat;
				const v = 0.5 + (s - 0.5) * con;
				out[i * 4 + k] = Math.round(Math.min(Math.max(Math.pow(v, 1 / gam), 0), 1) * 255);
			}
			out[i * 4 + 3] = pattern[i * 4 + 3];
		}
		return out;
	};
	for (const params of [[1, 1, 1, 1], [1.2, 0.9, 0.8, 1.1]]) {
		await check(`shadeboost ps_main with params ${params.join(", ")}`, async () => {
			const { c, pipeline } = get("shadeboost", "ps_main");
			setUniforms(new Float32Array(params));
			const dst = texture(F.Color, "check");
			draw(c, pipeline, { targets: [dst], group1Override: group1Nearest });
			return compare(await readback(dst, 4), shadeBoostReference(params), "rgba", 1);
		});
	}
	await check("interlace ps_main1 (bob) copies at LOD 0", async () => {
		const { c, pipeline } = get("interlace", "ps_main1");
		const dst = texture(F.Color, "check");
		draw(c, pipeline, { targets: [dst], group1Override: group1Nearest });
		return compare(await readback(dst, 4), pattern, "rgba");
	});

	await device.queue.onSubmittedWorkDone();
	for (const m of uncaptured) fail("uncaptured error", m);
	const summary = `${created}/${cases.length} pipelines, ${failures.length} failures, ${warnings} compiler warnings`;
	log(failures.length ? `FAIL ${summary}` : `DONE ${summary}`);
}

main().catch((e) => log(`FAIL exception ${e.stack ?? e}`));
