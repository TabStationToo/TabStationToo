// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// WebGPU port of vulkan/shadeboost.glsl (last synced with commit d983b2b066). Keep its order and names
// so upstream changes can be carried over: `git log d983b2b066.. -- bin/resources/shaders/vulkan/shadeboost.glsl`.
//
// Bindings (the utility pipeline layout shared with convert.wgsl, present.wgsl and merge.wgsl):
//   @group(0) @binding(0)  uniform buffer, dynamic offset: ShadeBoostUniforms, the 4 floats that
//                          GSDeviceVK::DoShadeBoost passes as push constants (brightness, contrast,
//                          saturation, gamma; already divided by 50, so 1.0 = 100%).
//   @group(1) @binding(0)  samp0_tex: texture_2d<f32>, sampleType "float".
//   @group(1) @binding(1)  samp0: sampler, type "filtering" (the device binds a point sampler).
//
// Vertex input (GSVertexPT1, arrayStride 32): @location(0) float32x4 at offset 0 (position),
// @location(1) float32x2 at offset 16 (texture coordinate). D3D/WebGPU clip space: no Y flip.
//
// Entry points: vs_main, ps_main (vec4 to the shadeboost target, Color).

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

/*
** Contrast, saturation, brightness
** Code of this function is from TGM's shader pack
** http://irrlicht.sourceforge.net/phpBB2/viewtopic.php?t=21057
** TGM's author comment about the license (included in the previous link)
** "do with it, what you want! its total free!
** (but would be nice, if you say that you used my shaders  :wink: ) but not necessary"
*/

struct ShadeBoostUniforms
{
	params: vec4<f32>,
};
@group(0) @binding(0) var<uniform> cb: ShadeBoostUniforms;

@group(1) @binding(0) var samp0_tex: texture_2d<f32>;
@group(1) @binding(1) var samp0: sampler;

// For all settings: 1.0 = 100% 0.5=50% 1.5 = 150%
fn ContrastSaturationBrightness(color: vec4<f32>) -> vec4<f32>
{
	let brt = cb.params.x;
	let con = cb.params.y;
	let sat = cb.params.z;
	let gam = cb.params.w;

	// Increase or decrease these values to adjust r, g and b color channels separately
	const AvgLumR = 0.5;
	const AvgLumG = 0.5;
	const AvgLumB = 0.5;

	const LumCoeff = vec3<f32>(0.2125, 0.7154, 0.0721);

	let AvgLumin = vec3<f32>(AvgLumR, AvgLumG, AvgLumB);
	let brtColor = color.rgb * brt;
	let dot_intensity = dot(brtColor, LumCoeff);
	let intensity = vec3<f32>(dot_intensity, dot_intensity, dot_intensity);
	let satColor = mix(intensity, brtColor, sat);
	let conColor = mix(AvgLumin, satColor, con);

	var csb = conColor;
	csb = pow(csb, vec3<f32>(1.0 / gam));
	return vec4<f32>(csb, color.a);
}

@fragment
fn ps_main(input: VSOutput) -> @location(0) vec4<f32>
{
	let c = textureSample(samp0_tex, samp0, input.t);
	return ContrastSaturationBrightness(c);
}
