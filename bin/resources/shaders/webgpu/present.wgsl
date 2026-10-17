// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// WebGPU port of vulkan/present.glsl (last synced with commit 95b50e7f81). Keep its order and names
// so upstream changes can be carried over: `git log 95b50e7f81.. -- bin/resources/shaders/vulkan/present.glsl`.
//
// Bindings (the utility pipeline layout shared with convert.wgsl, merge.wgsl and interlace.wgsl):
//   @group(0) @binding(0)  uniform buffer, dynamic offset: PresentUniforms, the 96-byte
//                          DisplayConstantBuffer (GSDevice.h) that Vulkan passes as push constants.
//   @group(1) @binding(0)  samp0_tex: texture_2d<f32>, sampleType "float".
//   @group(1) @binding(1)  samp0: sampler, type "filtering" (linear or point, chosen by the device).
//
// Vertex input (GSVertexPT1, arrayStride 32): @location(0) float32x4 at offset 0 (position),
// @location(1) float32x2 at offset 16 (texture coordinate). D3D/WebGPU clip space: no Y flip.
//
// Entry points (ShaderEntryPoint(PresentShader)), each writing vec4 to the swap chain format:
//   ps_copy, ps_filter_scanlines, ps_filter_diagonal, ps_filter_triangular, ps_filter_complex,
//   ps_filter_lottes, ps_4x_rgss, ps_automagical_supersampling

struct VSOutput
{
	@builtin(position) p: vec4<f32>,
	@location(0) t: vec2<f32>,
};

@vertex
fn vs_main(@location(0) a_pos: vec4<f32>, @location(1) a_tex: vec2<f32>) -> VSOutput
{
	var output: VSOutput;
	output.p = a_pos;
	output.t = a_tex;
	return output;
}

struct PresentUniforms
{
	u_source_rect: vec4<f32>,
	u_target_rect: vec4<f32>,
	u_source_size: vec2<f32>,
	u_target_size: vec2<f32>,
	u_target_resolution: vec2<f32>,
	u_rcp_target_resolution: vec2<f32>, // 1 / u_target_resolution
	u_source_resolution: vec2<f32>,
	u_rcp_source_resolution: vec2<f32>, // 1 / u_source_resolution
	u_time: f32,
	// Padded to 96 bytes (DisplayConstantBuffer::TimeAndPad).
};
@group(0) @binding(0) var<uniform> cb: PresentUniforms;

@group(1) @binding(0) var samp0_tex: texture_2d<f32>;
@group(1) @binding(1) var samp0: sampler;

fn sample_c(uv: vec2<f32>) -> vec4<f32>
{
	return textureSample(samp0_tex, samp0, uv);
}

fn ps_crt(uv: vec2<f32>, i: u32) -> vec4<f32>
{
	var mask = array<vec4<f32>, 4>(
		vec4<f32>(1.0, 0.0, 0.0, 0.0),
		vec4<f32>(0.0, 1.0, 0.0, 0.0),
		vec4<f32>(0.0, 0.0, 1.0, 0.0),
		vec4<f32>(1.0, 1.0, 1.0, 0.0));
	return sample_c(uv) * saturate(mask[i] + 0.5);
}

fn ps_scanlines(uv: vec2<f32>, i: u32) -> vec4<f32>
{
	var mask = array<vec4<f32>, 2>(
		vec4<f32>(1.0, 1.0, 1.0, 0.0),
		vec4<f32>(0.0, 0.0, 0.0, 0.0));

	return sample_c(uv) * saturate(mask[i] + 0.5);
}

@fragment
fn ps_copy(input: VSOutput) -> @location(0) vec4<f32>
{
	return sample_c(input.t);
}

@fragment
fn ps_filter_scanlines(input: VSOutput) -> @location(0) vec4<f32> // scanlines
{
	let p = vec4<u32>(input.p);

	return ps_scanlines(input.t, p.y % 2u);
}

@fragment
fn ps_filter_diagonal(input: VSOutput) -> @location(0) vec4<f32> // diagonal
{
	let p = vec4<u32>(input.p);
	return ps_crt(input.t, (p.x + (p.y % 3u)) % 3u);
}

@fragment
fn ps_filter_triangular(input: VSOutput) -> @location(0) vec4<f32> // triangular
{
	let p = vec4<u32>(input.p);

	// output.c = ps_crt(input, ((p.x + (p.y & 1) * 3) >> 1) % 3);
	return ps_crt(input.t, ((p.x + ((p.y >> 1u) & 1u) * 3u) >> 1u) % 3u);
}

@fragment
fn ps_filter_complex(input: VSOutput) -> @location(0) vec4<f32> // triangular
{
	const PI = 3.14159265359;
	let texdim = vec2<f32>(textureDimensions(samp0_tex, 0));

	return (0.9 - 0.4 * cos(2.0 * PI * input.t.y * texdim.y)) * sample_c(vec2<f32>(input.t.x, (floor(input.t.y * texdim.y) + 0.5) / texdim.y));
}

// ps_filter_lottes

const MaskingType = 4;          //[1|2|3|4] The type of CRT shadow masking used. 1: compressed TV style, 2: Aperture-grille, 3: Stretched VGA style, 4: VGA style.
const ScanBrightness = -8.00;   //[-16.0 to 1.0] The overall brightness of the scanline effect. Lower for darker, higher for brighter.
const FilterCRTAmount = -3.00;  //[-4.0 to 1.0] The amount of filtering used, to replicate the TV CRT look. Lower for less, higher for more.
const HorizontalWarp = 0.00;    //[0.0 to 0.1] The distortion warping effect for the horizontal (x) axis of the screen. Use small increments.
const VerticalWarp = 0.00;      //[0.0 to 0.1] The distortion warping effect for the verticle (y) axis of the screen. Use small increments.
const MaskAmountDark = 0.50;    //[0.0 to 1.0] The value of the dark masking line effect used. Lower for darker lower end masking, higher for brighter.
const MaskAmountLight = 1.50;   //[0.0 to 2.0] The value of the light masking line effect used. Lower for darker higher end masking, higher for brighter.
const BloomPixel = -1.50;       //[-2.0 -0.5] Pixel bloom radius. Higher for increased softness of bloom.
const BloomScanLine = -2.0;     //[-4.0 -1.0] Scanline bloom radius. Higher for increased softness of bloom.
const BloomAmount = 0.15;       //[0.0 1.0] Bloom intensity. Higher for brighter.
const Shape = 2.0;              //[0.0 10.0] Kernal filter shape. Lower values will darken image and introduce moire patterns if used with curvature.
const UseShadowMask = true;     //[0 or 1] Enables, or disables the use of the CRT shadow mask. 0 is disabled, 1 is enabled.

fn ToLinear1(c: f32) -> f32
{
	return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045);
}

fn ToLinear(c: vec3<f32>) -> vec3<f32>
{
	return vec3<f32>(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b));
}

fn ToSrgb1(c: f32) -> f32
{
	return select(1.055 * pow(c, 0.41666) - 0.055, c * 12.92, c < 0.0031308);
}

fn ToSrgb(c: vec3<f32>) -> vec3<f32>
{
	return vec3<f32>(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b));
}

fn Fetch(pos_in: vec2<f32>, off: vec2<f32>) -> vec3<f32>
{
	let pos = (floor(pos_in * cb.u_target_size + off) + vec2<f32>(0.5, 0.5)) / cb.u_target_size;
	// WGSL only allows implicit-derivative sampling in uniform control flow, so sample first and
	// then select, instead of sampling in the else branch as the GLSL does.
	let c = ToLinear(textureSample(samp0_tex, samp0, pos.xy).rgb);
	return select(c, vec3<f32>(0.0, 0.0, 0.0), max(abs(pos.x - 0.5), abs(pos.y - 0.5)) > 0.5);
}

fn Dist(pos_in: vec2<f32>) -> vec2<f32>
{
	let pos = pos_in * vec2<f32>(640.0, 480.0);

	return -((pos - floor(pos)) - vec2<f32>(0.5, 0.5));
}

fn Gaus(pos: f32, scale: f32) -> f32
{
	return exp2(scale * pow(abs(pos), Shape));
}

fn Horz3(pos: vec2<f32>, off: f32) -> vec3<f32>
{
	let b = Fetch(pos, vec2<f32>(-1.0, off));
	let c = Fetch(pos, vec2<f32>(0.0, off));
	let d = Fetch(pos, vec2<f32>(1.0, off));
	let dst = Dist(pos).x;

	// Convert distance to weight.
	let scale = FilterCRTAmount;
	let wb = Gaus(dst - 1.0, scale);
	let wc = Gaus(dst + 0.0, scale);
	let wd = Gaus(dst + 1.0, scale);

	return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

fn Horz5(pos: vec2<f32>, off: f32) -> vec3<f32>
{
	let a = Fetch(pos, vec2<f32>(-2.0, off));
	let b = Fetch(pos, vec2<f32>(-1.0, off));
	let c = Fetch(pos, vec2<f32>(0.0, off));
	let d = Fetch(pos, vec2<f32>(1.0, off));
	let e = Fetch(pos, vec2<f32>(2.0, off));
	let dst = Dist(pos).x;

	// Convert distance to weight.
	let scale = FilterCRTAmount;

	let wa = Gaus(dst - 2.0, scale);
	let wb = Gaus(dst - 1.0, scale);
	let wc = Gaus(dst + 0.0, scale);
	let wd = Gaus(dst + 1.0, scale);
	let we = Gaus(dst + 2.0, scale);

	return (a * wa + b * wb + c * wc + d * wd + e * we) / (wa + wb + wc + wd + we);
}

fn Horz7(pos: vec2<f32>, off: f32) -> vec3<f32>
{
	let a = Fetch(pos, vec2<f32>(-3.0, off));
	let b = Fetch(pos, vec2<f32>(-2.0, off));
	let c = Fetch(pos, vec2<f32>(-1.0, off));
	let d = Fetch(pos, vec2<f32>( 0.0, off));
	let e = Fetch(pos, vec2<f32>( 1.0, off));
	let f = Fetch(pos, vec2<f32>( 2.0, off));
	let g = Fetch(pos, vec2<f32>( 3.0, off));

	let dst = Dist(pos).x;
	// Convert distance to weight.
	let scale = BloomPixel;
	let wa = Gaus(dst - 3.0, scale);
	let wb = Gaus(dst - 2.0, scale);
	let wc = Gaus(dst - 1.0, scale);
	let wd = Gaus(dst + 0.0, scale);
	let we = Gaus(dst + 1.0, scale);
	let wf = Gaus(dst + 2.0, scale);
	let wg = Gaus(dst + 3.0, scale);

	// Return filtered sample.
	return (a * wa + b * wb + c * wc + d * wd + e * we + f * wf + g * wg) / (wa + wb + wc + wd + we + wf + wg);
}

// Return scanline weight.
fn Scan(pos: vec2<f32>, off: f32) -> f32
{
	let dst = Dist(pos).y;
	return Gaus(dst + off, ScanBrightness);
}

fn BloomScan(pos: vec2<f32>, off: f32) -> f32
{
	let dst = Dist(pos).y;

	return Gaus(dst + off, BloomScanLine);
}

fn Tri(pos: vec2<f32>) -> vec3<f32>
{
	let a = Horz3(pos, -1.0);
	let b = Horz5(pos, 0.0);
	let c = Horz3(pos, 1.0);

	let wa = Scan(pos, -1.0);
	let wb = Scan(pos, 0.0);
	let wc = Scan(pos, 1.0);

	return (a * wa) + (b * wb) + (c * wc);
}

fn Bloom(pos: vec2<f32>) -> vec3<f32>
{
	let a = Horz5(pos,-2.0);
	let b = Horz7(pos,-1.0);
	let c = Horz7(pos, 0.0);
	let d = Horz7(pos, 1.0);
	let e = Horz5(pos, 2.0);

	let wa = BloomScan(pos,-2.0);
	let wb = BloomScan(pos,-1.0);
	let wc = BloomScan(pos, 0.0);
	let wd = BloomScan(pos, 1.0);
	let we = BloomScan(pos, 2.0);

	return a * wa + b * wb + c * wc + d * wd + e * we;
}

fn Warp(pos_in: vec2<f32>) -> vec2<f32>
{
	var pos = pos_in * 2.0 - 1.0;
	pos *= vec2<f32>(1.0 + (pos.y * pos.y) * HorizontalWarp, 1.0 + (pos.x * pos.x) * VerticalWarp);
	return pos * 0.5 + 0.5;
}

// The GLSL picks one of these with #if MaskingType; here the branches on the constant fold away.
fn Mask(pos_in: vec2<f32>) -> vec3<f32>
{
	var pos = pos_in;
	var mask = vec3<f32>(MaskAmountDark, MaskAmountDark, MaskAmountDark);

	if (MaskingType == 1)
	{
		// Very compressed TV style shadow mask.
		var lines = MaskAmountLight;
		var odd = 0.0;

		if (fract(pos.x / 6.0) < 0.5)
		{
			odd = 1.0;
		}
		if (fract((pos.y + odd) / 2.0) < 0.5)
		{
			lines = MaskAmountDark;
		}
		pos.x = fract(pos.x / 3.0);

		if (pos.x < 0.333)
		{
			mask.r = MaskAmountLight;
		}
		else if (pos.x < 0.666)
		{
			mask.g = MaskAmountLight;
		}
		else
		{
			mask.b = MaskAmountLight;
		}

		mask *= lines;
	}
	else if (MaskingType == 2)
	{
		// Aperture-grille.
		pos.x = fract(pos.x / 3.0);

		if (pos.x < 0.333)
		{
			mask.r = MaskAmountLight;
		}
		else if (pos.x < 0.666)
		{
			mask.g = MaskAmountLight;
		}
		else
		{
			mask.b = MaskAmountLight;
		}
	}
	else if (MaskingType == 3)
	{
		// Stretched VGA style shadow mask (same as prior shaders).
		pos.x += pos.y * 3.0;
		pos.x = fract(pos.x / 6.0);

		if (pos.x < 0.333)
		{
			mask.r = MaskAmountLight;
		}
		else if (pos.x < 0.666)
		{
			mask.g = MaskAmountLight;
		}
		else
		{
			mask.b = MaskAmountLight;
		}
	}
	else
	{
		// VGA style shadow mask.
		pos = floor(pos * vec2<f32>(1.0, 0.5));
		pos.x += pos.y * 3.0;

		pos.x = fract(pos.x / 6.0);

		if (pos.x < 0.333)
		{
			mask.r = MaskAmountLight;
		}
		else if (pos.x < 0.666)
		{
			mask.g = MaskAmountLight;
		}
		else
		{
			mask.b = MaskAmountLight;
		}
	}

	return mask;
}

fn LottesCRTPass(frag_coord: vec4<f32>) -> vec4<f32>
{
	// The GLSL leaves alpha undefined; it is 0 here.
	var color = vec4<f32>(0.0);
	let fragcoord = frag_coord - cb.u_target_rect;
	let inSize = cb.u_target_resolution - (2.0 * cb.u_target_rect.xy);

	let pos = Warp(fragcoord.xy / inSize);
	var rgb = Tri(pos);
	rgb += Bloom(pos) * BloomAmount;
	if (UseShadowMask)
	{
		rgb *= Mask(fragcoord.xy);
	}
	color = vec4<f32>(ToSrgb(rgb), color.a);

	return color;
}

@fragment
fn ps_filter_lottes(input: VSOutput) -> @location(0) vec4<f32>
{
	return LottesCRTPass(input.p);
}

@fragment
fn ps_4x_rgss(input: VSOutput) -> @location(0) vec4<f32>
{
	let dxy = vec2<f32>(dpdx(input.t.x), dpdy(input.t.y));
	var color = vec3<f32>(0.0);

	let s = 1.0 / 8.0;
	let l = 3.0 / 8.0;

	color += sample_c(input.t + vec2<f32>( s, l) * dxy).rgb;
	color += sample_c(input.t + vec2<f32>( l,-s) * dxy).rgb;
	color += sample_c(input.t + vec2<f32>(-s,-l) * dxy).rgb;
	color += sample_c(input.t + vec2<f32>(-l, s) * dxy).rgb;

	return vec4<f32>(color * 0.25, 1.0);
}

@fragment
fn ps_automagical_supersampling(input: VSOutput) -> @location(0) vec4<f32>
{
	let ratio = (cb.u_source_size / cb.u_target_size) * 0.5;
	let steps = floor(ratio);
	var col = sample_c(input.t).rgb;
	var div = 1.0;

	for (var y = 0.0; y < steps.y; y += 1.0)
	{
		for (var x = 0.0; x < steps.x; x += 1.0)
		{
			let offset = vec2<f32>(x, y) - ratio * 0.5;
			col += sample_c(input.t + offset * cb.u_rcp_source_resolution * 2.0).rgb;
			div += 1.0;
		}
	}

	return vec4<f32>(col / div, 1.0);
}
