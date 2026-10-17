// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// WebGPU port of vulkan/imgui.glsl (last synced with commit d983b2b066). Keep its order and names
// so upstream changes can be carried over: `git log d983b2b066.. -- bin/resources/shaders/vulkan/imgui.glsl`.
//
// Bindings (the utility pipeline layout):
//   @group(0) @binding(0)  uniform buffer, dynamic offset, VERTEX visibility: ImGuiUniforms.
//                          WebGPU clip space has +Y up, so where GSDeviceVK::RenderImGui passes
//                          (2/w, 2/h, -1, -1) the WebGPU device passes uScale = (2/w, -2/h) and
//                          uTranslate = (-1, 1), w and h being the surface size.
//   @group(1) @binding(0)  Texture: texture_2d<f32>, sampleType "float" (font atlas and other
//                          ImGui textures).
//   @group(1) @binding(1)  Sampler: sampler, type "filtering" (linear).
//
// Vertex input (ImDrawVert, arrayStride 20): @location(0) float32x2 at offset 0 (pos),
// @location(1) float32x2 at offset 8 (uv), @location(2) unorm8x4 at offset 16 (col).
//
// Entry points: vs_main, ps_main (vec4 to the swap chain format, with alpha blending).

struct ImGuiUniforms
{
	uScale: vec2<f32>,
	uTranslate: vec2<f32>,
};
@group(0) @binding(0) var<uniform> pc: ImGuiUniforms;

struct VSOutput
{
	@builtin(position) pos: vec4<f32>,
	@location(0) Frag_UV: vec2<f32>,
	@location(1) Frag_Color: vec4<f32>,
};

@vertex
fn vs_main(@location(0) Position: vec2<f32>, @location(1) UV: vec2<f32>, @location(2) Color: vec4<f32>) -> VSOutput
{
	var output: VSOutput;
	output.Frag_UV = UV;
	output.Frag_Color = Color;
	output.pos = vec4<f32>(Position * pc.uScale + pc.uTranslate, 0.0, 1.0);
	return output;
}

@group(1) @binding(0) var Texture: texture_2d<f32>;
@group(1) @binding(1) var Sampler: sampler;

@fragment
fn ps_main(input: VSOutput) -> @location(0) vec4<f32>
{
	return input.Frag_Color * textureSample(Texture, Sampler, input.Frag_UV);
}
