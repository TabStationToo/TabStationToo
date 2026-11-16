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
//   ps_filter_lottes, ps_4x_rgss, ps_automagical_supersampling, and ps_filter_geom, ps_filter_geom_glow with the ps_glow_* passes (ours)

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

// The glow passes (TabStationToo only, not in vulkan/present.glsl), for ps_filter_geom_glow
//
// Before it presents, the device (GSDeviceWebGPU::UpdateGlowBuffers) keeps one texture up to date: in
// its left half the picture accumulated over frames (phosphor persistence), in its right half that
// picture blurred. The utility layout has one texture, so the accumulation is two blended draws
// (ps_glow_fade multiplies what is there, ps_glow_feed takes the maximum with the new picture) instead
// of a shader reading its own previous output.
// The scheme (accumulate, blur across, blur down) and the blur's weights are those of newpixie CRT by
// Mattias Gustavsson (libretro slang-shaders, crt/shaders/newpixie), MIT licence, Copyright (c) 2016
// Mattias Gustavsson, or public domain (www.unlicense.org), as one chooses. MIT: Permission is hereby
// granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions: The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
// WITHOUT WARRANTY OF ANY KIND.

const GlowPersistence = 0.50; // what is left of the picture one frame later

@fragment
fn ps_glow_fade(input: VSOutput) -> @location(0) vec4<f32>
{
	return vec4<f32>(GlowPersistence); // blended as destination * this
}

@fragment
fn ps_glow_feed(input: VSOutput) -> @location(0) vec4<f32>
{
	return sample_c(input.t); // blended as max(destination, this)
}

// u_source_rect is the part of the texture to blur (taps stay inside it), u_time the step in texels.
fn GlowBlurPass(uv: vec2<f32>, dir: vec2<f32>) -> vec4<f32>
{
	let d = dir * cb.u_time * cb.u_rcp_source_resolution;
	let lo = cb.u_source_rect.xy + 0.5 * cb.u_rcp_source_resolution;
	let hi = cb.u_source_rect.zw - 0.5 * cb.u_rcp_source_resolution;
	var sum = textureSampleLevel(samp0_tex, samp0, uv, 0.0) * 0.2270270270;
	sum += textureSampleLevel(samp0_tex, samp0, clamp(uv - 4.0 * d, lo, hi), 0.0) * 0.0162162162;
	sum += textureSampleLevel(samp0_tex, samp0, clamp(uv - 3.0 * d, lo, hi), 0.0) * 0.0540540541;
	sum += textureSampleLevel(samp0_tex, samp0, clamp(uv - 2.0 * d, lo, hi), 0.0) * 0.1216216216;
	sum += textureSampleLevel(samp0_tex, samp0, clamp(uv - 1.0 * d, lo, hi), 0.0) * 0.1945945946;
	sum += textureSampleLevel(samp0_tex, samp0, clamp(uv + 1.0 * d, lo, hi), 0.0) * 0.1945945946;
	sum += textureSampleLevel(samp0_tex, samp0, clamp(uv + 2.0 * d, lo, hi), 0.0) * 0.1216216216;
	sum += textureSampleLevel(samp0_tex, samp0, clamp(uv + 3.0 * d, lo, hi), 0.0) * 0.0540540541;
	sum += textureSampleLevel(samp0_tex, samp0, clamp(uv + 4.0 * d, lo, hi), 0.0) * 0.0162162162;
	return sum;
}

@fragment
fn ps_glow_blur_h(input: VSOutput) -> @location(0) vec4<f32>
{
	return GlowBlurPass(input.t, vec2<f32>(1.0, 0.0));
}

@fragment
fn ps_glow_blur_v(input: VSOutput) -> @location(0) vec4<f32>
{
	return GlowBlurPass(input.t, vec2<f32>(0.0, 1.0));
}

// ps_filter_geom and ps_filter_geom_glow (TabStationToo only, not in vulkan/present.glsl)
//
// CRT-Geom ("CRT-interlaced"), Copyright (C) 2010-2012 cgwg, Themaister and DOLLS, from libretro
// slang-shaders (crt/shaders/crt-geom.slang). This program is free software; you can redistribute it
// and/or modify it under the terms of the GNU General Public License as published by the Free Software
// Foundation; either version 2 of the License, or (at your option) any later version.
//
// A picture on a spherical tube seen from a distance, with a beam per scanline (wider where it is
// bright), Lanczos filtering along the line, rounded corners and an aperture mask. Ported with the
// slang's default parameters, and with these differences:
//   - no tilt (x_tilt, y_tilt are 0 by default), which makes the sphere maths much shorter;
//   - the aspect is the 4:3 the original shader meant, (1.0, 0.75);
//   - the scanlines are the PS2's lines in pairs, as the slang's interlacing detection gives for a
//     picture of 400 lines or more, whatever the internal resolution; the two fields do not alternate
//     (the slang shows one per frame, which beats against a display that is not at the game's rate);
//   - only the default mask (1: magenta and green columns), whose arithmetic reduces, with no
//     brightness boost, to dimming the phosphors a column does not light.
// ps_filter_geom_glow is ours on top of it: the same tube reading the glow passes' texture, so that
// bright things leave a short trail, with the blurred picture added as light spread by the glass, and
// the corners a little darker.

const GeomCRTGamma = 2.4;
const GeomMonitorGamma = 2.2;
const GeomD = 1.5;               // "Distance"
const GeomR = 2.0;               // "Curvature Radius"
const GeomCornerSize = 0.03;
const GeomCornerSmooth = 1000.0;
const GeomDotMask = 0.3;         // "Mask strength"
const GeomScanlineWeight = 0.3;
const GeomLum = 0.0;             // "Luminance"
const GeomAspect = vec2<f32>(1.0, 0.75);
const GeomPi = 3.141592653589;
const GeomGlow = 0.12;     // ps_filter_geom_glow: how much of the blurred picture's highlights is added
const GeomVignette = 0.18; // and how much darker the corners get

fn GeomIntersect(xy: vec2<f32>) -> f32
{
	let A = dot(xy, xy) + GeomD * GeomD;
	let B = 2.0 * (GeomR * (-GeomD) - GeomD * GeomD);
	let C = GeomD * GeomD + 2.0 * GeomR * GeomD;
	return (-B - sqrt(B * B - 4.0 * A * C)) / (2.0 * A);
}

fn GeomBkwtrans(xy: vec2<f32>) -> vec2<f32>
{
	let c = GeomIntersect(xy);
	let point = vec2<f32>(c, c) * xy / GeomR;
	let a = sqrt(max(1.0 - dot(point, point), 0.0));
	let r = max(abs(GeomR * acos(a)), 1e-5);
	return point * r / sin(r / GeomR);
}

fn GeomFwtrans(uv_in: vec2<f32>) -> vec2<f32>
{
	let r = max(abs(sqrt(dot(uv_in, uv_in))), 1e-5);
	let uv = uv_in * sin(r / GeomR) / r;
	let x = 1.0 - cos(r / GeomR);
	let D = GeomD / GeomR + x;
	return GeomD * uv / D;
}

// The slang's maxscale(): with no tilt its centre is (0, 0) and only the scale is left.
fn GeomMaxScale() -> f32
{
	let a = vec2<f32>(0.5, 0.5) * GeomAspect;
	let hi = vec2<f32>(GeomFwtrans(vec2<f32>(a.x, 0.0)).x, GeomFwtrans(vec2<f32>(0.0, a.y)).y) / GeomAspect;
	return 2.0 * max(hi.x, hi.y);
}

fn GeomTransform(coord: vec2<f32>) -> vec2<f32>
{
	let c = (coord - vec2<f32>(0.5, 0.5)) * GeomAspect * GeomMaxScale();
	return GeomBkwtrans(c) / GeomAspect + vec2<f32>(0.5, 0.5);
}

fn GeomCorner(coord_in: vec2<f32>) -> f32
{
	var coord = min(coord_in, vec2<f32>(1.0) - coord_in) * GeomAspect;
	let cdist = vec2<f32>(GeomCornerSize);
	coord = cdist - min(coord, cdist);
	let dist = sqrt(dot(coord, coord));
	return saturate((cdist.x - dist) * GeomCornerSmooth);
}

// The influence of a scanline of colour `color` at `distance` scanlines from its centre.
fn GeomScanlineWeights(distance: f32, color: vec3<f32>) -> vec3<f32>
{
	let wid = 2.0 + 2.0 * pow(color, vec3<f32>(4.0));
	let weights = vec3<f32>(distance / GeomScanlineWeight);
	return (GeomLum + 1.4) * exp(-pow(weights * inverseSqrt(0.5 * wid), wid)) / (0.6 + 0.2 * wid);
}

// The picture at xy (0..1 over the displayed picture) in linear light.
fn GeomTex(xy: vec2<f32>) -> vec3<f32>
{
	let half_texel = 0.5 * cb.u_rcp_source_resolution;
	let st = clamp(mix(cb.u_source_rect.xy, cb.u_source_rect.zw, xy), cb.u_source_rect.xy + half_texel, cb.u_source_rect.zw - half_texel);
	return pow(textureSampleLevel(samp0_tex, samp0, st, 0.0).rgb, vec3<f32>(GeomCRTGamma));
}

// The blurred picture beside the one u_source_rect names (the glow passes' texture), in linear light.
fn GeomTexGlow(xy: vec2<f32>) -> vec3<f32>
{
	let half_texel = 0.5 * cb.u_rcp_source_resolution;
	var st = clamp(mix(cb.u_source_rect.xy, cb.u_source_rect.zw, xy), cb.u_source_rect.xy + half_texel, cb.u_source_rect.zw - half_texel);
	st.x += cb.u_source_rect.z - cb.u_source_rect.x;
	return pow(textureSampleLevel(samp0_tex, samp0, st, 0.0).rgb, vec3<f32>(GeomCRTGamma));
}

fn GeomPass(frag_coord: vec4<f32>, glow: bool) -> vec4<f32>
{
	let px = frag_coord.xy - cb.u_target_rect.xy;
	let vtc = px / cb.u_target_size;

	var xy = GeomTransform(vtc);
	let cval = GeomCorner(xy);
	let glass_xy = xy;

	// The scanlines: the PS2's lines (the source is a multiple of them when upscaled), in pairs.
	let src_h = cb.u_source_size.y;
	let ps2_lines = src_h / max(1.0, round(src_h / 480.0));
	let ilfac = clamp(floor(ps2_lines / 200.0), 1.0, 2.0);
	let tsize = vec2<f32>(cb.u_source_size.x, ps2_lines / ilfac);
	let one = vec2<f32>(1.0) / tsize;

	let ratio_scale = xy * tsize - vec2<f32>(0.5, 0.5);
	var uv_ratio = fract(ratio_scale);
	let filter_ = fwidth(ratio_scale.y);

	// Snap to the centre of the underlying texel.
	xy = (floor(ratio_scale) + vec2<f32>(0.5, 0.5)) / tsize;

	// Lanczos2 coefficients for the neighbours along the scanline.
	var coeffs = GeomPi * vec4<f32>(1.0 + uv_ratio.x, uv_ratio.x, 1.0 - uv_ratio.x, 2.0 - uv_ratio.x);
	coeffs = max(abs(coeffs), vec4<f32>(1e-5));
	coeffs = 2.0 * sin(coeffs) * sin(coeffs / 2.0) / (coeffs * coeffs);
	coeffs /= dot(coeffs, vec4<f32>(1.0));

	// The colour of the current and the next scanline at this pixel's horizontal position.
	let col = saturate(GeomTex(xy + vec2<f32>(-one.x, 0.0)) * coeffs.x + GeomTex(xy) * coeffs.y +
		GeomTex(xy + vec2<f32>(one.x, 0.0)) * coeffs.z + GeomTex(xy + vec2<f32>(2.0 * one.x, 0.0)) * coeffs.w);
	let col2 = saturate(GeomTex(xy + vec2<f32>(-one.x, one.y)) * coeffs.x + GeomTex(xy + vec2<f32>(0.0, one.y)) * coeffs.y +
		GeomTex(xy + one) * coeffs.z + GeomTex(xy + vec2<f32>(2.0 * one.x, one.y)) * coeffs.w);

	// Their influence on this pixel, the beam profile oversampled three times (against moire).
	var weights = GeomScanlineWeights(uv_ratio.y, col);
	var weights2 = GeomScanlineWeights(1.0 - uv_ratio.y, col2);
	uv_ratio.y = uv_ratio.y + 1.0 / 3.0 * filter_;
	weights = (weights + GeomScanlineWeights(uv_ratio.y, col)) / 3.0;
	weights2 = (weights2 + GeomScanlineWeights(abs(1.0 - uv_ratio.y), col2)) / 3.0;
	uv_ratio.y = uv_ratio.y - 2.0 / 3.0 * filter_;
	weights = weights + GeomScanlineWeights(abs(uv_ratio.y), col) / 3.0;
	weights2 = weights2 + GeomScanlineWeights(abs(1.0 - uv_ratio.y), col2) / 3.0;

	let mul_res = (col * weights + col2 * weights2) * cval;

	// The mask: magenta and green columns.
	let green = floor(px.x % 2.0);
	let mask = mix(vec3<f32>(1.0, 0.0, 1.0), vec3<f32>(0.0, 1.0, 0.0), green);
	var cout = mix((1.0 - GeomDotMask) * mul_res, mul_res, mask);

	if (glow)
	{
		// Light spread by the glass: over the scanlines and the mask, not under them.
		// Only what is bright glows: adding the whole blurred picture lifts the dark parts into a fog.
		let g = GeomTexGlow(glass_xy);
		let bright = smoothstep(0.08, 0.5, dot(g, vec3<f32>(0.299, 0.587, 0.114)));
		cout += g * bright * GeomGlow * cval;
		let v = 16.0 * glass_xy.x * glass_xy.y * (1.0 - glass_xy.x) * (1.0 - glass_xy.y);
		cout *= mix(1.0 - GeomVignette, 1.0, sqrt(saturate(v)));
	}

	return vec4<f32>(pow(saturate(cout), vec3<f32>(1.0 / GeomMonitorGamma)), 1.0);
}

@fragment
fn ps_filter_geom(input: VSOutput) -> @location(0) vec4<f32>
{
	return GeomPass(input.p, false);
}

@fragment
fn ps_filter_geom_glow(input: VSOutput) -> @location(0) vec4<f32>
{
	return GeomPass(input.p, true);
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
