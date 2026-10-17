// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// WebGPU port of vulkan/convert.glsl (last synced with commit bd9b6daaad) and dx11/convert.fx.
// Keep the order and names of vulkan/convert.glsl so upstream changes can be carried over by
// diffing that file: `git log bd9b6daaad.. -- bin/resources/shaders/vulkan/convert.glsl`.
//
// WGSL has no preprocessor. Where the GLSL source is compiled once per ShaderConvertSelector with
// HAS_* macros, this file instead has:
//   - one entry point per output kind. The output type is fixed by the shader (HasFloat32Output,
//     IntegerOutputBpp, HasStencilOutput), except for depth output: an entry point with the "_ds"
//     suffix writes @builtin(frag_depth) instead of an r32float colour (selector DepthOutput()).
//     Entry point name: ShaderEntryPoint(shader) + (selector.DepthOutput() ? "_ds" : "").
//   - the HAS_BILN override for selector.Biln() (pipeline constant, 0 or 1).
//   - the PRIMID_MAX/PRIMID_MIN overrides; their defaults match GSShader::PRIMID_MAX/MIN
//     (GSShaderEnums.h), and the device should pass those values.
//
// Bindings (one pipeline layout for every utility shader: convert, present, merge, interlace):
//   @group(0) @binding(0)  uniform buffer, dynamic offset, VERTEX|FRAGMENT: the per-draw constants
//                          that Vulkan passes as push constants. Bind 96 bytes
//                          (CONVERT_PUSH_CONSTANTS_SIZE). Only ps_downsample_copy,
//                          ps_convert_rgb5a1_8i, ps_convert_rgba_8i, ps_convert_clut_4/8 and ps_yuv
//                          read it; each declares its own struct, laid out like the C++ Uniforms
//                          struct that GSDeviceVK fills for it.
//   @group(1) @binding(0)  samp0_tex: texture_2d<f32>, the source texture.
//   @group(1) @binding(1)  samp0: sampler.
//   [float input] entry points read a depth texture (bind a depth-only aspect view) or an r32float
//   texture: give them a group(1) layout with sampleType "unfilterable-float" and a "non-filtering"
//   sampler. Every other entry point uses sampleType "float" with a "filtering" sampler.
//
// Vertex input (GSVertexPT1, arrayStride 32): @location(0) float32x4 at offset 0 (position),
// @location(1) float32x2 at offset 16 (texture coordinate). Positions are in D3D/WebGPU clip space
// (+Y up), as in dx11/convert.fx: unlike vulkan/convert.glsl there is no Y flip.
//
// Entry points (output -> target format; [f] = float input):
//   ps_copy                     vec4 -> Color (rgba8unorm); write mask from the selector
//   ps_depth_copy [f]           f32  -> DepthColor (r32float); _ds -> frag_depth
//   ps_downsample_copy          vec4 -> Color
//   ps_filter_transparency      vec4 -> Color
//   ps_convert_rgb5a1_16bits    u32  -> UInt16 (r16uint)
//   ps_datm1, ps_datm0, ps_datm1_rta_correction, ps_datm0_rta_correction
//                               none: discard only, stencil-only pass (DATE setup)
//   ps_rta_correction           vec4 -> Color; write mask from the selector
//   ps_rta_decorrection         vec4 -> Color
//   ps_colclip_init             vec4 -> ColorClip
//   ps_colclip_resolve          vec4 -> Color
//   ps_convert_depth32_32bits [f]  u32 -> UInt32 (r32uint), or UInt16 (r16uint) for DEPTH32_TO_16_BITS
//   ps_convert_depth32_rgba8 [f]   vec4 -> Color (DEPTH32_TO_RGB8 uses write mask 0x7)
//   ps_convert_depth16_rgb5a1 [f]  vec4 -> Color
//   ps_convert_depth32_depth24 [f] f32 -> DepthColor; _ds -> frag_depth
//   ps_convert_primid_rgba8 [f]    vec4 -> Color (source is the r32float PrimID texture)
//   ps_convert_rgba8_depth32, ps_convert_rgba8_depth24, ps_convert_rgba8_depth16,
//   ps_convert_rgb5a1_depth16   f32 -> DepthColor; _ds -> frag_depth; read HAS_BILN
//   ps_convert_rgb5a1_8i, ps_convert_rgba_8i   vec4 -> the 8-bit index target
//   ps_convert_clut_4, ps_convert_clut_8       vec4 -> Color
//   ps_yuv                      vec4 -> Color
//   ps_primid_image_init_0..3   f32  -> PrimID (r32float)

override HAS_BILN: bool = false;
override PRIMID_MAX: f32 = 16777215.0;
override PRIMID_MIN: f32 = -1.0;

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

@group(1) @binding(0) var samp0_tex: texture_2d<f32>;
@group(1) @binding(1) var samp0: sampler;

fn sample_c(uv: vec2<f32>) -> vec4<f32>
{
	return textureSample(samp0_tex, samp0, uv);
}

// HAS_FLOAT32_INPUT variant of sample_c.
fn sample_f(uv: vec2<f32>) -> f32
{
	return textureSample(samp0_tex, samp0, uv).r;
}

fn rgba8_to_uint(c: vec4<f32>) -> u32
{
	let i = vec4<u32>(c * 255.5) & vec4<u32>(0xFFu);
	return i.r | (i.g << 8u) | (i.b << 16u) | (i.a << 24u);
}

fn rgb5a1_to_uint(c: vec4<f32>) -> u32
{
	let i = vec4<u32>(c * 255.5) & vec4<u32>(0xF8u, 0xF8u, 0xF8u, 0x80u);
	return (i.r >> 3u) | (i.g << 2u) | (i.b << 7u) | (i.a << 8u);
}

fn depth_to_uint(d: f32) -> u32
{
	// WGSL saturates out-of-range float to integer conversions (1.0 gives 0xFFFFFFFF).
	return u32(d * exp2(32.0));
}

fn uint_to_rgba8(i: u32) -> vec4<f32>
{
	return vec4<f32>(vec4<u32>(i & 0xFFu, (i >> 8u) & 0xFFu, (i >> 16u) & 0xFFu, (i >> 24u) & 0xFFu)) / 255.0;
}

fn uint_to_rgb5a1(i: u32) -> vec4<f32>
{
	return vec4<f32>(vec4<u32>(i << 3u, i >> 2u, i >> 7u, i >> 8u) & vec4<u32>(0xF8u, 0xF8u, 0xF8u, 0x80u)) / 255.0;
}

fn uint_to_depth32(i: u32) -> f32
{
	return f32(i) * exp2(-32.0);
}

fn uint_to_depth24(i: u32) -> f32
{
	return f32(i & 0xFFFFFFu) * exp2(-32.0);
}

fn uint_to_depth16(i: u32) -> f32
{
	return f32(i & 0xFFFFu) * exp2(-32.0);
}

fn rgba8_to_depth32(val: vec4<f32>) -> f32
{
	return uint_to_depth32(rgba8_to_uint(val));
}

fn rgba8_to_depth24(val: vec4<f32>) -> f32
{
	return uint_to_depth24(rgba8_to_uint(val));
}

fn rgba8_to_depth16(val: vec4<f32>) -> f32
{
	return uint_to_depth16(rgba8_to_uint(val));
}

fn rgb5a1_to_depth16(val: vec4<f32>) -> f32
{
	return uint_to_depth16(rgb5a1_to_uint(val));
}

fn depth32_to_rgba8(d: f32) -> vec4<f32>
{
	return uint_to_rgba8(depth_to_uint(d));
}

fn depth16_to_rgb5a1(d: f32) -> vec4<f32>
{
	return uint_to_rgb5a1(depth_to_uint(d));
}

fn depth32_to_depth24(d: f32) -> f32
{
	return uint_to_depth24(depth_to_uint(d));
}

fn primid_to_rgba8(p: f32) -> vec4<f32>
{
	return uint_to_rgba8(bitcast<u32>(p));
}

@fragment
fn ps_copy(input: VSOutput) -> @location(0) vec4<f32>
{
	return sample_c(input.t);
}

@fragment
fn ps_depth_copy(input: VSOutput) -> @location(0) f32
{
	return sample_f(input.t);
}

@fragment
fn ps_depth_copy_ds(input: VSOutput) -> @builtin(frag_depth) f32
{
	return sample_f(input.t);
}

// GSDeviceVK::FilteredDownsampleTexture
struct DownsampleUniforms
{
	ClampMin: vec2<i32>,
	DownsampleFactor: i32,
	pad0: i32,
	Weight: f32,
	step_multiplier: f32,
	pad1: vec2<f32>,
};
@group(0) @binding(0) var<uniform> downsample_cb: DownsampleUniforms;

@fragment
fn ps_downsample_copy(input: VSOutput) -> @location(0) vec4<f32>
{
	let coord = max(vec2<i32>(input.p.xy) * downsample_cb.DownsampleFactor, downsample_cb.ClampMin);
	var result = vec4<f32>(0.0);
	for (var yoff = 0; yoff < downsample_cb.DownsampleFactor; yoff++)
	{
		for (var xoff = 0; xoff < downsample_cb.DownsampleFactor; xoff++)
		{
			let offset = vec2<i32>(vec2<f32>(f32(xoff), f32(yoff)) * downsample_cb.step_multiplier);
			result += textureLoad(samp0_tex, coord + offset, 0);
		}
	}
	return result / downsample_cb.Weight;
}

@fragment
fn ps_filter_transparency(input: VSOutput) -> @location(0) vec4<f32>
{
	let c = sample_c(input.t);
	return vec4<f32>(c.rgb, 1.0);
}

@fragment
fn ps_convert_rgb5a1_16bits(input: VSOutput) -> @location(0) u32
{
	// Need to be careful with precision here, it can break games like Spider-Man 3 and Dogs Life
	return rgb5a1_to_uint(sample_c(input.t));
}

@fragment
fn ps_datm1(input: VSOutput)
{
	if (sample_c(input.t).a < (127.5 / 255.0)) // >= 0x80 pass
	{
		discard;
	}
}

@fragment
fn ps_datm0(input: VSOutput)
{
	if ((127.5 / 255.0) < sample_c(input.t).a) // < 0x80 pass (== 0x80 should not pass)
	{
		discard;
	}
}

@fragment
fn ps_datm1_rta_correction(input: VSOutput)
{
	if (sample_c(input.t).a < (254.5 / 255.0)) // >= 0x80 pass
	{
		discard;
	}
}

@fragment
fn ps_datm0_rta_correction(input: VSOutput)
{
	if ((254.5 / 255.0) < sample_c(input.t).a) // < 0x80 pass (== 0x80 should not pass)
	{
		discard;
	}
}

@fragment
fn ps_rta_correction(input: VSOutput) -> @location(0) vec4<f32>
{
	let value = sample_c(input.t);
	return vec4<f32>(value.rgb, value.a / (128.25 / 255.0));
}

@fragment
fn ps_rta_decorrection(input: VSOutput) -> @location(0) vec4<f32>
{
	let value = sample_c(input.t);
	return vec4<f32>(value.rgb, value.a * (128.25 / 255.0));
}

@fragment
fn ps_colclip_init(input: VSOutput) -> @location(0) vec4<f32>
{
	let value = sample_c(input.t);
	// WGSL round() rounds half to even, like GLSL roundEven().
	return vec4<f32>(round(value.rgb * 255.0) / 65535.0, value.a);
}

@fragment
fn ps_colclip_resolve(input: VSOutput) -> @location(0) vec4<f32>
{
	let value = sample_c(input.t);
	return vec4<f32>(vec3<f32>(vec3<u32>(value.rgb * 65535.5) & vec3<u32>(255u)) / 255.0, value.a);
}

@fragment
fn ps_convert_depth32_32bits(input: VSOutput) -> @location(0) u32
{
	// Convert a vec32 depth texture into a 32 bits UINT texture
	return depth_to_uint(sample_f(input.t));
}

@fragment
fn ps_convert_depth32_rgba8(input: VSOutput) -> @location(0) vec4<f32>
{
	// Convert a vec32 depth texture into a RGBA color texture
	return depth32_to_rgba8(sample_f(input.t));
}

@fragment
fn ps_convert_depth16_rgb5a1(input: VSOutput) -> @location(0) vec4<f32>
{
	// Convert a vec32 (only 16 lsb) depth into a RGB5A1 color texture
	return depth16_to_rgb5a1(sample_f(input.t));
}

@fragment
fn ps_convert_depth32_depth24(input: VSOutput) -> @location(0) f32
{
	// Truncates depth value to 24bits
	return depth32_to_depth24(sample_f(input.t));
}

@fragment
fn ps_convert_depth32_depth24_ds(input: VSOutput) -> @builtin(frag_depth) f32
{
	return depth32_to_depth24(sample_f(input.t));
}

@fragment
fn ps_convert_primid_rgba8(input: VSOutput) -> @location(0) vec4<f32>
{
	return primid_to_rgba8(sample_f(input.t));
}

// SAMPLE_RGBA_DEPTH_BILN: the four texels around uv and the bilinear weights. The caller converts
// each texel to depth and blends with biln_depth().
struct BilnTexels
{
	tl: vec4<f32>,
	tr: vec4<f32>,
	bl: vec4<f32>,
	br: vec4<f32>,
	mix_vals: vec2<f32>,
};

fn fetch_biln(uv: vec2<f32>) -> BilnTexels
{
	let dims = vec2<i32>(textureDimensions(samp0_tex, 0));
	let top_left_f = uv * vec2<f32>(dims) - 0.5;
	let top_left = vec2<i32>(floor(top_left_f));
	let coords = clamp(vec4<i32>(top_left, top_left + 1), vec4<i32>(0), dims.xyxy - 1);
	var texels: BilnTexels;
	texels.mix_vals = fract(top_left_f);
	texels.tl = textureLoad(samp0_tex, coords.xy, 0);
	texels.tr = textureLoad(samp0_tex, coords.zy, 0);
	texels.bl = textureLoad(samp0_tex, coords.xw, 0);
	texels.br = textureLoad(samp0_tex, coords.zw, 0);
	return texels;
}

fn biln_depth(depthTL: f32, depthTR: f32, depthBL: f32, depthBR: f32, mix_vals: vec2<f32>) -> f32
{
	return floor(mix(mix(depthTL, depthTR, mix_vals.x), mix(depthBL, depthBR, mix_vals.x), mix_vals.y) * exp2(32.0)) * exp2(-32.0);
}

fn convert_rgba8_depth32(uv: vec2<f32>) -> f32
{
	// Convert an RGBA texture into a float depth texture
	if (HAS_BILN)
	{
		let s = fetch_biln(uv);
		return biln_depth(rgba8_to_depth32(s.tl), rgba8_to_depth32(s.tr), rgba8_to_depth32(s.bl), rgba8_to_depth32(s.br), s.mix_vals);
	}
	return rgba8_to_depth32(sample_c(uv));
}

fn convert_rgba8_depth24(uv: vec2<f32>) -> f32
{
	// Same as above but without the alpha channel (24 bits Z)
	if (HAS_BILN)
	{
		let s = fetch_biln(uv);
		return biln_depth(rgba8_to_depth24(s.tl), rgba8_to_depth24(s.tr), rgba8_to_depth24(s.bl), rgba8_to_depth24(s.br), s.mix_vals);
	}
	return rgba8_to_depth24(sample_c(uv));
}

fn convert_rgba8_depth16(uv: vec2<f32>) -> f32
{
	// Same as above but without the A/B channels (16 bits Z)
	if (HAS_BILN)
	{
		let s = fetch_biln(uv);
		return biln_depth(rgba8_to_depth16(s.tl), rgba8_to_depth16(s.tr), rgba8_to_depth16(s.bl), rgba8_to_depth16(s.br), s.mix_vals);
	}
	return rgba8_to_depth16(sample_c(uv));
}

fn convert_rgb5a1_depth16(uv: vec2<f32>) -> f32
{
	// Convert an RGB5A1 (saved as RGBA8) color to a 16 bit Z
	if (HAS_BILN)
	{
		let s = fetch_biln(uv);
		return biln_depth(rgb5a1_to_depth16(s.tl), rgb5a1_to_depth16(s.tr), rgb5a1_to_depth16(s.bl), rgb5a1_to_depth16(s.br), s.mix_vals);
	}
	return rgb5a1_to_depth16(sample_c(uv));
}

@fragment
fn ps_convert_rgba8_depth32(input: VSOutput) -> @location(0) f32
{
	return convert_rgba8_depth32(input.t);
}

@fragment
fn ps_convert_rgba8_depth32_ds(input: VSOutput) -> @builtin(frag_depth) f32
{
	return convert_rgba8_depth32(input.t);
}

@fragment
fn ps_convert_rgba8_depth24(input: VSOutput) -> @location(0) f32
{
	return convert_rgba8_depth24(input.t);
}

@fragment
fn ps_convert_rgba8_depth24_ds(input: VSOutput) -> @builtin(frag_depth) f32
{
	return convert_rgba8_depth24(input.t);
}

@fragment
fn ps_convert_rgba8_depth16(input: VSOutput) -> @location(0) f32
{
	return convert_rgba8_depth16(input.t);
}

@fragment
fn ps_convert_rgba8_depth16_ds(input: VSOutput) -> @builtin(frag_depth) f32
{
	return convert_rgba8_depth16(input.t);
}

@fragment
fn ps_convert_rgb5a1_depth16(input: VSOutput) -> @location(0) f32
{
	return convert_rgb5a1_depth16(input.t);
}

@fragment
fn ps_convert_rgb5a1_depth16_ds(input: VSOutput) -> @builtin(frag_depth) f32
{
	return convert_rgb5a1_depth16(input.t);
}

// GSDeviceVK::ConvertToIndexedTexture
struct IndexedUniforms
{
	SBW: u32,
	DBW: u32,
	PSM: u32,
	cb_pad1: f32,
	ScaleFactor: f32,
	cb_pad2: f32,
	cb_pad3: vec2<f32>,
};
@group(0) @binding(0) var<uniform> indexed_cb: IndexedUniforms;

@fragment
fn ps_convert_rgb5a1_8i(input: VSOutput) -> @location(0) vec4<f32>
{
	// Convert a RGB5A1 texture into a 8 bits packed texture
	// Input column: 16x2 RGB5A1 pixels
	// 0: 16 RGBA
	// 1: 16 RGBA
	// Output column: 16x4 Index pixels
	// 0: 16 R5G2
	// 1: 16 R5G2
	// 2: 16 G2B5A1
	// 3: 16 G2B5A1

	let pos = vec2<u32>(input.p.xy);
	let PSM = indexed_cb.PSM;

	// Collapse separate R G B A areas into their base pixel
	var column = (pos & ~vec2<u32>(0u, 3u)) / vec2<u32>(1u, 2u);
	let subcolumn = (pos & vec2<u32>(0u, 1u));
	column.x -= (column.x / 128u) * 64u;
	column.y += (column.y / 32u) * 32u;

	// Deal with swizzling differences
	if ((PSM & 0x8u) != 0u) // PSMCT16S
	{
		if ((pos.x & 32u) != 0u)
		{
			column.y += 32u; // 4 columns high times 4 to get bottom 4 blocks
			column.x &= ~32u;
		}

		if ((pos.x & 64u) != 0u)
		{
			column.x -= 32u;
		}

		if (((pos.x & 16u) != 0u) != ((pos.y & 16u) != 0u))
		{
			column.x ^= 16u;
			column.y ^= 8u;
		}

		if ((PSM & 0x30u) != 0u) // PSMZ16S - Untested but hopefully ok if anything uses it.
		{
			column.x ^= 32u;
			column.y ^= 16u;
		}
	}
	else // PSMCT16
	{
		if ((pos.y & 32u) != 0u)
		{
			column.y -= 16u;
			column.x += 32u;
		}

		if ((pos.x & 96u) != 0u)
		{
			let multi = (pos.x & 96u) / 32u;
			column.y += 16u * multi; // 4 columns high times 4 to get bottom 4 blocks
			column.x -= (pos.x & 96u);
		}

		if (((pos.x & 16u) != 0u) != ((pos.y & 16u) != 0u))
		{
			column.x ^= 16u;
			column.y ^= 8u;
		}

		if ((PSM & 0x30u) != 0u) // PSMZ16 - Untested but hopefully ok if anything uses it.
		{
			column.x ^= 32u;
			column.y ^= 32u;
		}
	}
	var coord = column | subcolumn;

	// Compensate for potentially differing page pitch.
	let block_xy = coord / vec2<u32>(64u, 64u);
	let block_num = (block_xy.y * (indexed_cb.DBW / 128u)) + block_xy.x;
	let block_offset = vec2<u32>((block_num % (indexed_cb.SBW / 64u)) * 64u, (block_num / (indexed_cb.SBW / 64u)) * 64u);
	coord = (coord % vec2<u32>(64u, 64u)) + block_offset;

	// Apply offset to cols 1 and 2
	let is_col23 = pos.y & 4u;
	let is_col13 = pos.y & 2u;
	let is_col12 = is_col23 ^ (is_col13 << 1u);
	coord.x ^= is_col12; // If cols 1 or 2, flip bit 3 of x

	let ScaleFactor = indexed_cb.ScaleFactor;
	if (floor(ScaleFactor) != ScaleFactor)
	{
		coord = vec2<u32>(vec2<f32>(coord) * ScaleFactor);
	}
	else
	{
		coord *= u32(ScaleFactor);
	}

	let pixel = textureLoad(samp0_tex, vec2<i32>(coord), 0);

	let denorm_c = vec4<u32>(pixel * 255.5);
	if ((pos.y & 2u) == 0u)
	{
		let red = (denorm_c.r >> 3u) & 0x1Fu;
		let green = (denorm_c.g >> 3u) & 0x1Fu;

		return vec4<f32>(f32(((green << 5u) | red) & 0xFFu) / 255.0);
	}
	else
	{
		let green = (denorm_c.g >> 3u) & 0x1Fu;
		let blue = (denorm_c.b >> 3u) & 0x1Fu;
		let alpha = denorm_c.a & 0x80u;

		return vec4<f32>(f32((alpha | (blue << 2u) | (green >> 3u)) & 0xFFu) / 255.0);
	}
}

@fragment
fn ps_convert_rgba_8i(input: VSOutput) -> @location(0) vec4<f32>
{
	// Convert a RGBA texture into a 8 bits packed texture
	// Input column: 8x2 RGBA pixels
	// 0: 8 RGBA
	// 1: 8 RGBA
	// Output column: 16x4 Index pixels
	// 0: 8 R | 8 B
	// 1: 8 R | 8 B
	// 2: 8 G | 8 A
	// 3: 8 G | 8 A
	let pos = vec2<u32>(input.p.xy);

	// Collapse separate R G B A areas into their base pixel
	let block = (pos & ~vec2<u32>(15u, 3u)) >> vec2<u32>(1u);
	let subblock = pos & vec2<u32>(7u, 1u);
	var coord = block | subblock;

	// Compensate for potentially differing page pitch.
	let block_xy = coord / vec2<u32>(64u, 32u);
	let block_num = (block_xy.y * (indexed_cb.DBW / 128u)) + block_xy.x;
	let block_offset = vec2<u32>((block_num % (indexed_cb.SBW / 64u)) * 64u, (block_num / (indexed_cb.SBW / 64u)) * 32u);
	coord = (coord % vec2<u32>(64u, 32u)) + block_offset;

	// Apply offset to cols 1 and 2
	let is_col23 = pos.y & 4u;
	let is_col13 = pos.y & 2u;
	let is_col12 = is_col23 ^ (is_col13 << 1u);
	coord.x ^= is_col12; // If cols 1 or 2, flip bit 3 of x

	let ScaleFactor = indexed_cb.ScaleFactor;
	if (floor(ScaleFactor) != ScaleFactor)
	{
		coord = vec2<u32>(vec2<f32>(coord) * ScaleFactor);
	}
	else
	{
		coord *= u32(ScaleFactor);
	}

	let pixel = textureLoad(samp0_tex, vec2<i32>(coord), 0);
	let sel0 = select(pixel.ga, pixel.rb, (pos.y & 2u) == 0u);
	let sel1 = select(sel0.y, sel0.x, (pos.x & 8u) == 0u);
	return vec4<f32>(sel1); // Divide by something here?
}

// GSDeviceVK::UpdateCLUTTexture
struct ClutUniforms
{
	offset: vec2<u32>,
	doffset: u32,
	cb_pad1: u32,
	scale: f32,
	cb_pad2: f32,
	cb_pad3: vec2<f32>,
};
@group(0) @binding(0) var<uniform> clut_cb: ClutUniforms;

@fragment
fn ps_convert_clut_4(input: VSOutput) -> @location(0) vec4<f32>
{
	// CLUT4 is easy, just two rows of 8x8.
	let index = u32(input.p.x) + clut_cb.doffset;
	let pos = vec2<u32>(index % 8u, index / 8u);

	let final_coord = vec2<i32>(floor(vec2<f32>(clut_cb.offset + pos) * clut_cb.scale));
	return textureLoad(samp0_tex, final_coord, 0);
}

@fragment
fn ps_convert_clut_8(input: VSOutput) -> @location(0) vec4<f32>
{
	let index = min(u32(input.p.x) + clut_cb.doffset, 255u);

	// CLUT is arranged into 8 groups of 16x2, with the top-right and bottom-left quadrants swapped.
	// This can probably be done better..
	let subgroup = (index / 8u) % 4u;
	var pos: vec2<u32>;
	pos.x = (index % 8u) + select(0u, 8u, subgroup >= 2u);
	pos.y = ((index / 32u) * 2u) + (subgroup % 2u);

	let final_coord = vec2<i32>(floor(vec2<f32>(clut_cb.offset + pos) * clut_cb.scale));
	return textureLoad(samp0_tex, final_coord, 0);
}

// GSDeviceVK::DoMerge (yuv_constants)
struct YuvUniforms
{
	EMODA: i32,
	EMODC: i32,
};
@group(0) @binding(0) var<uniform> yuv_cb: YuvUniforms;

@fragment
fn ps_yuv(input: VSOutput) -> @location(0) vec4<f32>
{
	let i = sample_c(input.t);
	var o = vec4<f32>(0.0);

	let rgb2yuv = mat3x3<f32>(
		vec3<f32>(0.587, -0.311, -0.419),
		vec3<f32>(0.114, 0.500, -0.081),
		vec3<f32>(0.299, -0.169, 0.500));

	let yuv = rgb2yuv * i.gbr;

	let Y = f32(0xDB) / 255.0 * yuv.x + f32(0x10) / 255.0;
	let Cr = f32(0xE0) / 255.0 * yuv.y + f32(0x80) / 255.0;
	let Cb = f32(0xE0) / 255.0 * yuv.z + f32(0x80) / 255.0;

	switch (yuv_cb.EMODA)
	{
		case 0:
		{
			o.a = i.a;
		}
		case 1:
		{
			o.a = Y;
		}
		case 2:
		{
			o.a = Y / 2.0;
		}
		case 3:
		{
			o.a = 0.0;
		}
		default:
		{
		}
	}

	switch (yuv_cb.EMODC)
	{
		case 0:
		{
			o = vec4<f32>(i.rgb, o.a);
		}
		case 1:
		{
			o = vec4<f32>(vec3<f32>(Y), o.a);
		}
		case 2:
		{
			o = vec4<f32>(Y, Cb, Cr, o.a);
		}
		case 3:
		{
			o = vec4<f32>(vec3<f32>(i.a), o.a);
		}
		default:
		{
		}
	}

	return o;
}

// The GLSL source builds these four from one main() with #ifdef ps_primid_image_init_N.
@fragment
fn ps_primid_image_init_0(input: VSOutput) -> @location(0) f32
{
	var c = PRIMID_MAX;
	if ((127.5 / 255.0) < sample_c(input.t).a) // < 0x80 pass (== 0x80 should not pass)
	{
		c = PRIMID_MIN;
	}
	return c;
}

@fragment
fn ps_primid_image_init_1(input: VSOutput) -> @location(0) f32
{
	var c = PRIMID_MAX;
	if (sample_c(input.t).a < (127.5 / 255.0)) // >= 0x80 pass
	{
		c = PRIMID_MIN;
	}
	return c;
}

@fragment
fn ps_primid_image_init_2(input: VSOutput) -> @location(0) f32
{
	var c = PRIMID_MAX;
	if ((254.5 / 255.0) < sample_c(input.t).a) // < 0x80 pass (== 0x80 should not pass)
	{
		c = PRIMID_MIN;
	}
	return c;
}

@fragment
fn ps_primid_image_init_3(input: VSOutput) -> @location(0) f32
{
	var c = PRIMID_MAX;
	if (sample_c(input.t).a < (254.5 / 255.0)) // >= 0x80 pass
	{
		c = PRIMID_MIN;
	}
	return c;
}
