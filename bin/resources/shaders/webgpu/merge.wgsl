// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// WebGPU port of vulkan/merge.glsl (last synced with commit d983b2b066). Keep its order and names
// so upstream changes can be carried over: `git log d983b2b066.. -- bin/resources/shaders/vulkan/merge.glsl`.
//
// Bindings (the utility pipeline layout shared with convert.wgsl, present.wgsl and interlace.wgsl):
//   @group(0) @binding(0)  uniform buffer, dynamic offset: MergeUniforms (the BGColor vector that
//                          GSDeviceVK::DoMerge passes as push constants). Only ps_main1 reads it.
//   @group(1) @binding(0)  samp0_tex: texture_2d<f32>, sampleType "float".
//   @group(1) @binding(1)  samp0: sampler, type "filtering".
//
// Vertex input (GSVertexPT1, arrayStride 32): @location(0) float32x4 at offset 0 (position),
// @location(1) float32x2 at offset 16 (texture coordinate). D3D/WebGPU clip space: no Y flip.
//
// Entry points, indexed by PMODE.MMOD, each writing vec4 to the merge target (Color):
//   ps_main0  alpha from the circuit (doubled; the blend unit clamps)
//   ps_main1  alpha from BGColor.a

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

struct MergeUniforms
{
	BGColor: vec4<f32>,
};
@group(0) @binding(0) var<uniform> cb: MergeUniforms;

@group(1) @binding(0) var samp0_tex: texture_2d<f32>;
@group(1) @binding(1) var samp0: sampler;

@fragment
fn ps_main0(input: VSOutput) -> @location(0) vec4<f32>
{
	var c = textureSample(samp0_tex, samp0, input.t);
	// Note: clamping will be done by fixed unit
	c.a *= 2.0;
	return c;
}

@fragment
fn ps_main1(input: VSOutput) -> @location(0) vec4<f32>
{
	var c = textureSample(samp0_tex, samp0, input.t);
	c.a = cb.BGColor.a;
	return c;
}
