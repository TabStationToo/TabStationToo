// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// WebGPU port of vulkan/interlace.glsl (last synced with commit 95b50e7f81). Keep its order and
// names so upstream changes can be carried over: `git log 95b50e7f81.. -- bin/resources/shaders/vulkan/interlace.glsl`.
//
// Bindings (the utility pipeline layout shared with convert.wgsl, present.wgsl and merge.wgsl):
//   @group(0) @binding(0)  uniform buffer, dynamic offset: InterlaceUniforms, the 16-byte
//                          InterlaceConstantBuffer (GSDevice.h) that Vulkan passes as push constants.
//   @group(1) @binding(0)  samp0_tex: texture_2d<f32>, sampleType "float".
//   @group(1) @binding(1)  samp0: sampler, type "filtering".
//
// Vertex input (GSVertexPT1, arrayStride 32): @location(0) float32x4 at offset 0 (position),
// @location(1) float32x2 at offset 16 (texture coordinate). D3D/WebGPU clip space: no Y flip.
//
// Entry points (ShaderInterlace), each writing vec4 to the interlace target (Color):
//   ps_main0 WEAVE, ps_main1 BOB, ps_main2 BLEND, ps_main3 MAD_BUFFER, ps_main4 MAD_RECONSTRUCT
// All sampling uses an explicit LOD 0, like the GLSL textureLod calls.

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

struct InterlaceUniforms
{
	ZrH: vec4<f32>,
};
@group(0) @binding(0) var<uniform> cb: InterlaceUniforms;

@group(1) @binding(0) var samp0_tex: texture_2d<f32>;
@group(1) @binding(1) var samp0: sampler;

fn sample_lod0(uv: vec2<f32>) -> vec4<f32>
{
	return textureSampleLevel(samp0_tex, samp0, uv, 0.0);
}

// Weave shader
@fragment
fn ps_main0(input: VSOutput) -> @location(0) vec4<f32>
{
	let idx   = i32(cb.ZrH.x);      // buffer index passed from CPU
	let field = idx & 1;            // current field
	let vpos  = i32(input.p.y);     // vertical position of destination texture

	if ((vpos & 1) != field)
	{
		discard;
	}
	return sample_lod0(input.t);
}

// Bob shader
@fragment
fn ps_main1(input: VSOutput) -> @location(0) vec4<f32>
{
	return sample_lod0(input.t);
}

// Blend shader
@fragment
fn ps_main2(input: VSOutput) -> @location(0) vec4<f32>
{
	let vstep = vec2<f32>(0.0, cb.ZrH.y);
	let c0 = sample_lod0(input.t - vstep);
	let c1 = sample_lod0(input.t);
	let c2 = sample_lod0(input.t + vstep);

	return (c0 + c1 * 2.0 + c2) / 4.0;
}

// MAD shader - buffering
@fragment
fn ps_main3(input: VSOutput) -> @location(0) vec4<f32>
{
	// We take half the lines from the current frame and stores them in the MAD frame buffer.
	// the MAD frame buffer is split in 2 consecutive banks of 2 fields each, the fields in each bank
	// are interleaved (top field at even lines and bottom field at odd lines).
	// When the source texture has an odd vres, the first line of bank 1 would be an odd index
	// causing the wrong lines to be discarded, so a vertical offset (lofs) is added to the vertical
	// position of the destination texture to force the proper field alignment

	let idx    = i32(cb.ZrH.x);                              // buffer index passed from CPU
	let bank   = idx >> 1u;                                  // current bank
	let field  = idx & 1;                                    // current field
	let vres   = i32(cb.ZrH.z) >> 1u;                        // vertical resolution of source texture
	let lofs   = ((((vres + 1) >> 1u) << 1u) - vres) & bank; // line alignment offset for bank 1
	let vpos   = i32(input.p.y) + lofs;                      // vertical position of destination texture

	// if the index of current destination line belongs to the current fiels we update it, otherwise
	// we leave the old line in the destination buffer
	if ((vpos & 1) != field)
	{
		discard;
	}
	return sample_lod0(input.t);
}

// MAD shader - reconstruction
@fragment
fn ps_main4(input: VSOutput) -> @location(0) vec4<f32>
{
	// we use the contents of the MAD frame buffer to reconstruct the missing lines from the current
	// field.

	let idx          = i32(cb.ZrH.x);                         // buffer index passed from CPU
	let bank         = idx >> 1u;                             // current bank
	let field        = idx & 1;                               // current field
	let vpos         = i32(input.p.y);                        // vertical position of destination texture
	let sensitivity  = cb.ZrH.w;                              // passed from CPU, higher values mean more likely to use weave
	let motion_thr   = vec3<f32>(1.0, 1.0, 1.0) * sensitivity; //
	let bofs         = vec2<f32>(0.0, 0.5);                   // position of the bank 1 relative to source texture size
	let vscale       = vec2<f32>(1.0, 0.5);                   // scaling factor from source to destination texture
	let lofs         = vec2<f32>(0.0, cb.ZrH.y) * vscale;     // distance between two adjacent lines relative to source texture size
	let iptr         = input.t * vscale;                      // pointer to the current pixel in the source texture

	var p_t0: vec2<f32>; // pointer to current pixel (missing or not) from most recent frame
	var p_t1: vec2<f32>; // pointer to current pixel (missing or not) from one frame back
	var p_t2: vec2<f32>; // pointer to current pixel (missing or not) from two frames back
	var p_t3: vec2<f32>; // pointer to current pixel (missing or not) from three frames back

	switch (idx)
	{
		case 1:
		{
			p_t0 = iptr;
			p_t1 = iptr;
			p_t2 = iptr + bofs;
			p_t3 = iptr + bofs;
		}
		case 2:
		{
			p_t0 = iptr + bofs;
			p_t1 = iptr;
			p_t2 = iptr;
			p_t3 = iptr + bofs;
		}
		case 3:
		{
			p_t0 = iptr + bofs;
			p_t1 = iptr + bofs;
			p_t2 = iptr;
			p_t3 = iptr;
		}
		default:
		{
			p_t0 = iptr;
			p_t1 = iptr + bofs;
			p_t2 = iptr + bofs;
			p_t3 = iptr;
		}
	}

	// calculating motion, only relevant for missing lines where the "center line" is pointed by p_t1

	let hn = sample_lod0(p_t0 - lofs); // new high pixel
	let cn = sample_lod0(p_t1);        // new center pixel
	let ln = sample_lod0(p_t0 + lofs); // new low pixel

	let ho = sample_lod0(p_t2 - lofs); // old high pixel
	let co = sample_lod0(p_t3);        // old center pixel
	let lo = sample_lod0(p_t2 + lofs); // old low pixel

	var mh = hn.rgb - ho.rgb; // high pixel motion
	var mc = cn.rgb - co.rgb; // center pixel motion
	var ml = ln.rgb - lo.rgb; // low pixel motion

	mh = max(mh, -mh) - motion_thr;
	mc = max(mc, -mc) - motion_thr;
	ml = max(ml, -ml) - motion_thr;

	// evaluate each color motion separately (the GLSL's "#if 1" branch; its average-motion
	// alternative is left out)
	let mh_max = max(max(mh.x, mh.y), mh.z);
	let mc_max = max(max(mc.x, mc.y), mc.z);
	let ml_max = max(max(ml.x, ml.y), ml.z);

	// selecting deinterlacing output

	if ((vpos & 1) == field) // output coordinate present on current field
	{
		// output coordinate present on current field
		return sample_lod0(p_t0);
	}
	else if ((iptr.y > 0.5 - lofs.y) || (iptr.y < 0.0 + lofs.y))
	{
		// top and bottom lines are always weaved
		return cn;
	}
	else
	{
		// missing line needs to be reconstructed
		if (((mh_max > 0.0) || (ml_max > 0.0)) || (mc_max > 0.0))
		{
			// high motion -> interpolate pixels above and below
			return (hn + ln) / 2.0;
		}
		else
		{
			// Check if it's completely static first, we don't need to mess with any of that.
			if ((mh_max != -motion_thr.x) || (ml_max != -motion_thr.x) || (mc_max != -motion_thr.x))
			{
				// Check the diff with the above and below lines, if the difference is smaller between the new high and low lines
				// compared to the new centre line and the high line (with some threshold of about 25 color steps), then reconstruct.
				var mhln = hn.rgb - ln.rgb;
				var mchn = hn.rgb - cn.rgb;

				mhln = max(mhln, -mhln) - motion_thr;
				mchn = max(mchn, -mchn) - motion_thr;

				let mhln_max = max(max(mhln.x, mhln.y), mhln.z);
				let mchn_max = max(max(mchn.x, mchn.y), mchn.z);

				// The new centre line is a fair chunk different from those surrounding it, so quite likely incorrect.
				if (mhln_max < 0.0 && mchn_max >= (mhln_max * 0.90))
				{
					return (hn + ln) / 2.0;
				}
				else
				{
					// low motion -> weave
					return cn;
				}
			}
			else
			{
				// low motion -> weave
				return cn;
			}
		}
	}
}
