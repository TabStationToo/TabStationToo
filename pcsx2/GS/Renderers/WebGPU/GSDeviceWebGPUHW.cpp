// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// The HW renderer's half of GSDeviceWebGPU (docs/web-port/WEBGPU-DESIGN.md §8, §10, step 2 of §15):
// TFX shaders generated from tfx.wgsl, their pipelines and bind groups, RenderHW with feedback
// through copies (the DX11 profile), stencil DATE, and the convert paths the texture cache uses.

#include "GS/Renderers/WebGPU/GSDeviceWebGPU.h"
#include "GS/Renderers/WebGPU/WGSLPreprocessor.h"
#include "GS/GSPerfMon.h"
#include "Host.h"
#include "wasm/PerfCounters.h"

#include "common/Console.h"
#include "common/HashCombine.h"
#include "common/ScopedGuard.h"

#include "fmt/format.h"

#include <emscripten/console.h>

#include <array>
#include <cmath>
#include <cstring>

namespace
{
	constexpr u32 UNIFORM_SLOT = 256;
	constexpr u32 VS_CB_SIZE = sizeof(GSHWDrawConfig::VSConstantBuffer);
	constexpr u32 PS_CB_SIZE = sizeof(GSHWDrawConfig::PSConstantBuffer);
	constexpr u32 VS_CB_SLOT = (VS_CB_SIZE + UNIFORM_SLOT - 1) / UNIFORM_SLOT * UNIFORM_SLOT;
	constexpr u32 PS_CB_SLOT = (PS_CB_SIZE + UNIFORM_SLOT - 1) / UNIFORM_SLOT * UNIFORM_SLOT;
	constexpr double TFX_PIPELINE_TIMEOUT_MS = 10000.0;
	// A draw whose TFX pipeline is not ready waits this long for it, then is skipped while the pipeline
	// is created in the background (WebGPUHost::PipelinesWait() restores waiting to the end) ...
	constexpr double TFX_PIPELINE_WAIT_MS = 3.0;
	// ... and all such waits of one vsync together take at most this long.
	constexpr double TFX_PIPELINE_FRAME_BUDGET_MS = 4.0;
	// Prewarming a game's stored list: pipelines in creation at once, started per present, and the
	// GS thread's time for starting them per present.
	constexpr u32 TFX_PREWARM_IN_FLIGHT = 8;
	constexpr u32 TFX_PREWARM_PER_PRESENT = 4;
	constexpr double TFX_PREWARM_MS_PER_PRESENT = 2.0;
	constexpr size_t MAX_CACHED_GROUPS = 4096;

	static_assert(VS_CB_SIZE == 48 && PS_CB_SIZE == 272, "tfx.wgsl's uniform structs follow these sizes");

	// tfx.wgsl's CSParams (TryComputeFeedbackDraw()).
	constexpr u32 CS_MAX_RECTS = 8;
	struct alignas(16) CSParams
	{
		s32 rects[CS_MAX_RECTS][4];
		u32 count;
		u32 colormask;
		u32 pad[2];
	};
	static_assert(sizeof(CSParams) == 144);
	constexpr u32 CS_PARAMS_SLOT = (sizeof(CSParams) + UNIFORM_SLOT - 1) / UNIFORM_SLOT * UNIFORM_SLOT;

	WGPUStringView SV(const char* s)
	{
		return {s, WGPU_STRLEN};
	}

	// GSDevice::BlendFactor order, as GSDeviceVK's table.
	constexpr WGPUBlendFactor s_blend_factors[16] = {
		WGPUBlendFactor_Src, WGPUBlendFactor_OneMinusSrc, WGPUBlendFactor_Dst, WGPUBlendFactor_OneMinusDst,
		WGPUBlendFactor_Src1, WGPUBlendFactor_OneMinusSrc1, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha,
		WGPUBlendFactor_DstAlpha, WGPUBlendFactor_OneMinusDstAlpha, WGPUBlendFactor_Src1Alpha,
		WGPUBlendFactor_OneMinusSrc1Alpha, WGPUBlendFactor_Constant, WGPUBlendFactor_OneMinusConstant,
		WGPUBlendFactor_One, WGPUBlendFactor_Zero};
	constexpr WGPUBlendOperation s_blend_ops[3] = {
		WGPUBlendOperation_Add, WGPUBlendOperation_Subtract, WGPUBlendOperation_ReverseSubtract};
	constexpr WGPUCompareFunction s_ztst[4] = {
		WGPUCompareFunction_Never, WGPUCompareFunction_Always, WGPUCompareFunction_GreaterEqual, WGPUCompareFunction_Greater};

	bool HasStencil(WGPUTextureFormat f)
	{
		return f == WGPUTextureFormat_Depth32FloatStencil8;
	}

	/// Formats a float/filterable texture binding accepts (the others are unfilterable or integer).
	bool IsFilterableFloat(WGPUTextureFormat f)
	{
		switch (f)
		{
			case WGPUTextureFormat_Depth32Float:
			case WGPUTextureFormat_Depth32FloatStencil8:
			case WGPUTextureFormat_R32Float:
			case WGPUTextureFormat_R16Uint:
			case WGPUTextureFormat_R32Uint:
				return false;
			default:
				return true;
		}
	}

	bool IsUnfilterableCompatible(WGPUTextureFormat f)
	{
		return f != WGPUTextureFormat_R16Uint && f != WGPUTextureFormat_R32Uint;
	}

	void LogOnce(bool& flag, const std::string& msg)
	{
		if (!std::exchange(flag, true))
			Console.Warning(msg);
	}
} // namespace

size_t GSDeviceWebGPU::TFXPipelineKeyHash::operator()(const TFXPipelineKey& k) const
{
	size_t h = 0;
	HashCombine(h, k.ps.key_lo, k.ps.key_hi, k.blend, k.vs, k.dss, k.cms, k.topology, static_cast<u32>(k.rt_format),
		static_cast<u32>(k.ds_format));
	return h;
}

size_t GSDeviceWebGPU::TFXGroupKeyHash::operator()(const TFXGroupKey& k) const
{
	size_t h = 0;
	HashCombine(h, reinterpret_cast<uintptr_t>(k.views[0]), reinterpret_cast<uintptr_t>(k.views[1]),
		reinterpret_cast<uintptr_t>(k.views[2]), reinterpret_cast<uintptr_t>(k.sampler));
	return h;
}

// ----------------------------------------------------------------------------
// Resources
// ----------------------------------------------------------------------------

bool GSDeviceWebGPU::CreateHWResources()
{
	const std::optional<std::string> source = ReadShaderSource("shaders/webgpu/tfx.wgsl");
	if (!source.has_value())
	{
		Console.Error("WebGPU: failed to read shaders/webgpu/tfx.wgsl");
		return false;
	}
	m_tfx_source = *source;

	// The keys draws use from now on go to a fresh log (pcsx2web_tfx_keys()), stamped with what they mean.
	static_assert(sizeof(TFXPipelineKey) == WebGPUHost::TFX_KEY_BYTES);
	m_tfx_key_version = ComputeTFXKeyVersion();
	m_tfx_wait_budget_ms = TFX_PIPELINE_FRAME_BUDGET_MS;
	WebGPUHost::ResetTFXKeyLog(m_tfx_key_version);

	// §8.1: group 0 the per-draw constants and the vertex ring (VS expansion), group 1 the source
	// textures and sampler, group 2 the feedback copies.
	{
		WGPUBindGroupLayoutEntry e[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
			WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
		e[0].binding = 0;
		e[0].visibility = WGPUShaderStage_Vertex;
		e[0].buffer.type = WGPUBufferBindingType_Uniform;
		e[0].buffer.hasDynamicOffset = true;
		e[0].buffer.minBindingSize = VS_CB_SIZE;
		e[1].binding = 1;
		e[1].visibility = WGPUShaderStage_Fragment;
		e[1].buffer.type = WGPUBufferBindingType_Uniform;
		e[1].buffer.hasDynamicOffset = true;
		e[1].buffer.minBindingSize = PS_CB_SIZE;
		e[2].binding = 2;
		e[2].visibility = WGPUShaderStage_Vertex;
		e[2].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
		WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		d.label = SV("tfx constants");
		d.entryCount = 3;
		d.entries = e;
		m_tfx_bgl[0] = wgpuDeviceCreateBindGroupLayout(m_device, &d);
	}
	{
		WGPUBindGroupLayoutEntry e[4] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
			WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
		for (u32 i = 0; i < 3; i++)
		{
			e[i].binding = i;
			e[i].visibility = WGPUShaderStage_Fragment;
			e[i].texture.sampleType = (i == 2) ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
			e[i].texture.viewDimension = WGPUTextureViewDimension_2D;
		}
		e[3].binding = 3;
		e[3].visibility = WGPUShaderStage_Fragment;
		e[3].sampler.type = WGPUSamplerBindingType_Filtering;
		WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		d.label = SV("tfx textures");
		d.entryCount = 4;
		d.entries = e;
		m_tfx_bgl[1] = wgpuDeviceCreateBindGroupLayout(m_device, &d);
	}
	{
		WGPUBindGroupLayoutEntry e[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
			WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
		for (u32 i = 0; i < 3; i++)
		{
			e[i].binding = i;
			e[i].visibility = WGPUShaderStage_Fragment;
			e[i].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
			e[i].texture.viewDimension = WGPUTextureViewDimension_2D;
		}
		WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		d.label = SV("tfx feedback");
		d.entryCount = 3;
		d.entries = e;
		m_tfx_bgl[2] = wgpuDeviceCreateBindGroupLayout(m_device, &d);
	}
	{
		WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pd.label = SV("tfx");
		pd.bindGroupLayoutCount = 3;
		pd.bindGroupLayouts = m_tfx_bgl;
		m_tfx_layout = wgpuDeviceCreatePipelineLayout(m_device, &pd);
	}
	{
		WGPUBindGroupEntry e[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
		e[0].binding = 0;
		e[0].buffer = m_uniform_buffer.GetBuffer();
		e[0].size = VS_CB_SIZE;
		e[1].binding = 1;
		e[1].buffer = m_uniform_buffer.GetBuffer();
		e[1].size = PS_CB_SIZE;
		e[2].binding = 2;
		e[2].buffer = m_vertex_buffer.GetBuffer();
		e[2].size = m_vertex_buffer.GetSize();
		WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		d.label = SV("tfx constants");
		d.layout = m_tfx_bgl[0];
		d.entryCount = 3;
		d.entries = e;
		m_tfx_group0 = wgpuDeviceCreateBindGroup(m_device, &d);
	}
	{
		// The fixed index buffer for point and sprite expansion.
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.label = SV("expand index buffer");
		bd.usage = WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
		bd.size = Common::AlignUpPow2(EXPAND_BUFFER_SIZE, 4);
		m_expand_ib = wgpuDeviceCreateBuffer(m_device, &bd);
		std::vector<u8> data(bd.size);
		GenerateExpansionIndexBuffer(data.data());
		wgpuQueueWriteBuffer(m_queue, m_expand_ib, 0, data.data(), data.size());
	}
	{
		WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
		td.label = SV("null texture");
		td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
		td.size = {1, 1, 1};
		td.format = WGPUTextureFormat_RGBA8Unorm;
		m_null_texture = wgpuDeviceCreateTexture(m_device, &td);
		m_null_view = wgpuTextureCreateView(m_null_texture, nullptr);
		const u32 zero = 0;
		WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		dst.texture = m_null_texture;
		WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
		layout.bytesPerRow = 4;
		layout.rowsPerImage = 1;
		const WGPUExtent3D size = {1, 1, 1};
		wgpuQueueWriteTexture(m_queue, &dst, &zero, sizeof(zero), &layout, &size);
	}
	m_null_group2 = GetTFXGroup(2, TFXGroupKey{{m_null_view, m_null_view, m_null_view}, nullptr});

	m_cs_enabled = Host::GetBaseBoolSettingValue("EmuCore/GS", "WebGPUComputeFeedback", true) &&
				   m_null_view && CreateComputeFeedbackResources();
	Console.WriteLnFmt("WebGPU: feedback draws that read only their own pixel run as compute: {}.", m_cs_enabled ? "yes" : "no");

	return m_tfx_bgl[0] && m_tfx_bgl[1] && m_tfx_bgl[2] && m_tfx_layout && m_tfx_group0 && m_expand_ib &&
		   m_null_texture && m_null_group2;
}

void GSDeviceWebGPU::DestroyHWResources()
{
	DestroyComputeFeedbackResources();
	// Requests in flight are dropped; their callbacks free them (when the instance is released at the latest).
	for (auto& it : m_tfx_pending)
		DropTFXPending(it.second);
	m_tfx_pending.clear();
	m_tfx_prewarm_in_flight = 0;
	m_tfx_prewarm_queue = {};
	m_tfx_prewarm_next = 0;
	for (auto& it : m_tfx_pipelines)
	{
		if (it.second.pipeline)
			wgpuRenderPipelineRelease(it.second.pipeline);
	}
	m_tfx_pipelines.clear();
	for (auto& it : m_tfx_vs_modules)
	{
		if (it.second)
			wgpuShaderModuleRelease(it.second);
	}
	m_tfx_vs_modules.clear();
	for (auto& it : m_tfx_fs_modules)
	{
		if (it.second)
			wgpuShaderModuleRelease(it.second);
	}
	m_tfx_fs_modules.clear();
	for (auto& groups : m_tfx_groups)
	{
		for (auto& it : groups)
			wgpuBindGroupRelease(it.second);
		groups.clear();
	}
	m_null_group2 = nullptr;
	if (m_tfx_group0)
		wgpuBindGroupRelease(m_tfx_group0);
	m_tfx_group0 = nullptr;
	if (m_tfx_layout)
		wgpuPipelineLayoutRelease(m_tfx_layout);
	m_tfx_layout = nullptr;
	for (WGPUBindGroupLayout& l : m_tfx_bgl)
	{
		if (l)
			wgpuBindGroupLayoutRelease(l);
		l = nullptr;
	}
	if (m_expand_ib)
	{
		wgpuBufferDestroy(m_expand_ib);
		wgpuBufferRelease(m_expand_ib);
		m_expand_ib = nullptr;
	}
	if (m_null_view)
		wgpuTextureViewRelease(m_null_view);
	m_null_view = nullptr;
	if (m_null_texture)
	{
		wgpuTextureDestroy(m_null_texture);
		wgpuTextureRelease(m_null_texture);
		m_null_texture = nullptr;
	}
}

// ----------------------------------------------------------------------------
// Shaders and pipelines
// ----------------------------------------------------------------------------

static WGSLPreprocessor::Macros TFXVertexMacros(GSHWDrawConfig::VSSelector sel)
{
	return {
		{"VERTEX_SHADER", "1"},
		{"VS_TME", std::to_string(sel.tme)},
		{"VS_FST", std::to_string(sel.fst)},
		{"VS_IIP", std::to_string(sel.iip)},
		{"VS_POINT_SIZE", std::to_string(sel.point_size)},
		{"VS_EXPAND", std::to_string(static_cast<int>(sel.expand))},
		{"VS_PROVOKING_VERTEX_LAST", "0"},
	};
}

// The macro list of GSDeviceVK::GetTFXFragmentShader.
static WGSLPreprocessor::Macros TFXFragmentMacros(const GSHWDrawConfig::PSSelector& sel)
{
	WGSLPreprocessor::Macros macros;
	macros.reserve(64);
	auto add = [&macros](const char* n, u32 v) { macros.emplace_back(n, std::to_string(v)); };
	macros.emplace_back("FRAGMENT_SHADER", "1");
	add("PS_FST", sel.fst);
	add("PS_WMS", sel.wms);
	add("PS_WMT", sel.wmt);
	add("PS_ADJS", sel.adjs);
	add("PS_ADJT", sel.adjt);
	add("PS_AEM_FMT", sel.aem_fmt);
	add("PS_PAL_FMT", sel.pal_fmt);
	add("PS_DST_FMT", sel.dst_fmt);
	add("PS_DEPTH_FMT", sel.depth_fmt);
	add("PS_CHANNEL_FETCH", sel.channel);
	add("PS_URBAN_CHAOS_HLE", sel.urban_chaos_hle);
	add("PS_TALES_OF_ABYSS_HLE", sel.tales_of_abyss_hle);
	add("PS_AEM", sel.aem);
	add("PS_TFX", sel.tfx);
	add("PS_TCC", sel.tcc);
	add("PS_ATST", static_cast<u32>(sel.atst));
	add("PS_AFAIL", static_cast<u32>(sel.afail));
	add("PS_FOG", sel.fog);
	add("PS_BLEND_HW", sel.blend_hw);
	add("PS_A_MASKED", sel.a_masked);
	add("PS_FBA", sel.fba);
	add("PS_LTF", sel.ltf);
	add("PS_AUTOMATIC_LOD", sel.automatic_lod);
	add("PS_MANUAL_LOD", sel.manual_lod);
	add("PS_COLCLIP", sel.colclip);
	add("PS_DATE", sel.date);
	add("PS_TCOFFSETHACK", sel.tcoffsethack);
	add("PS_REGION_RECT", sel.region_rect);
	add("PS_BLEND_A", sel.blend_a);
	add("PS_BLEND_B", sel.blend_b);
	add("PS_BLEND_C", sel.blend_c);
	add("PS_BLEND_D", sel.blend_d);
	add("PS_BLEND_MIX", sel.blend_mix);
	add("PS_ROUND_INV", sel.round_inv);
	add("PS_FIXED_ONE_A", sel.fixed_one_a);
	add("PS_IIP", sel.iip);
	add("PS_SHUFFLE", sel.shuffle);
	add("PS_SHUFFLE_SAME", sel.shuffle_same);
	add("PS_PROCESS_BA", sel.process_ba);
	add("PS_PROCESS_RG", sel.process_rg);
	add("PS_SHUFFLE_ACROSS", sel.shuffle_across);
	add("PS_READ16_SRC", sel.real16src);
	add("PS_WRITE_RG", sel.write_rg);
	add("PS_FBMASK", sel.fbmask);
	add("PS_COLCLIP_HW", sel.colclip_hw);
	add("PS_RTA_CORRECTION", sel.rta_correction);
	add("PS_RTA_SRC_CORRECTION", sel.rta_source_correction);
	add("PS_DITHER", sel.dither);
	add("PS_DITHER_ADJUST", sel.dither_adjust);
	add("PS_ZCLAMP", sel.zclamp);
	add("PS_ZFLOOR", sel.zfloor);
	add("PS_PABE", sel.pabe);
	add("PS_SCANMSK", sel.scanmsk);
	add("PS_TEX_IS_FB", sel.tex_is_fb);
	add("PS_NO_COLOR", sel.no_color);
	add("PS_NO_COLOR1", sel.no_color1);
	add("PS_ZTST", sel.ztst);
	add("PS_AA1", static_cast<u32>(sel.aa1));
	add("PS_ABE", sel.abe);
	add("PS_ANISOTROPIC_FILTERING", sel.sw_aniso);
	add("PS_ROV_COLOR", 0);
	add("PS_ROV_DEPTH", 0);
	return macros;
}

WGPUShaderModule GSDeviceWebGPU::GetTFXVertexModule(GSHWDrawConfig::VSSelector sel)
{
	const auto it = m_tfx_vs_modules.find(sel.key);
	if (it != m_tfx_vs_modules.end())
		return it->second;

	const WGSLPreprocessor::Macros macros = TFXVertexMacros(sel);
	std::string error;
	const std::string code = WGSLPreprocessor::Process(m_tfx_source, macros, &error);
	if (!error.empty())
		Console.ErrorFmt("WebGPU: tfx.wgsl (vertex {:02x}): {}", sel.key, error);
	WGPUShaderModule mod = CreateShaderModule(fmt::format("tfx vs {:02x}", sel.key).c_str(), code);
	m_tfx_vs_modules.emplace(sel.key, mod);
	return mod;
}

WGPUShaderModule GSDeviceWebGPU::GetTFXFragmentModule(const GSHWDrawConfig::PSSelector& sel)
{
	const auto it = m_tfx_fs_modules.find(sel);
	if (it != m_tfx_fs_modules.end())
		return it->second;

	const WGSLPreprocessor::Macros macros = TFXFragmentMacros(sel);

	std::string error;
	const std::string code = WGSLPreprocessor::Process(m_tfx_source, macros, &error);
	if (!error.empty())
		Console.ErrorFmt("WebGPU: tfx.wgsl (fragment {:016x}_{:016x}): {}", sel.key_hi, sel.key_lo, error);
	WGPUShaderModule mod = CreateShaderModule(fmt::format("tfx ps {:016x}_{:016x}", sel.key_hi, sel.key_lo).c_str(), code);
	m_tfx_fs_modules.emplace(sel, mod);
	return mod;
}

GSDeviceWebGPU::TFXPipelineKey GSDeviceWebGPU::MakeTFXKey(const GSHWDrawConfig& config,
	const GSHWDrawConfig::PSSelector& ps, GSHWDrawConfig::BlendState bs, GSHWDrawConfig::ColorMaskSelector cms,
	GSHWDrawConfig::DepthStencilSelector dss, GSTextureWebGPU* rt, GSTextureWebGPU* ds) const
{
	TFXPipelineKey key;
	key.ps = ps;
	key.vs = config.vs.key;
	key.topology = static_cast<u8>(config.topology);
	key.rt_format = rt ? rt->GetNativeFormat() : WGPUTextureFormat_Undefined;
	key.ds_format = ds ? ds->GetNativeFormat() : WGPUTextureFormat_Undefined;

	if (!ds)
		dss = GSHWDrawConfig::DepthStencilSelector::NoDepth();
	if (!HasStencil(key.ds_format))
	{
		dss.date = 0;
		dss.date_one = 0;
	}
	if (!rt)
	{
		cms = GSHWDrawConfig::ColorMaskSelector(0);
		key.ps.no_color = 1;
	}

	// Normalisation as GSDeviceVK::CreateTFXPipeline, and the second output only when a factor reads it
	// (dual-source output needs exactly one colour target and is pointless otherwise).
	if (key.ps.no_color || !bs.IsEffective(cms))
	{
		bs = {};
		key.ps.no_color1 = 1;
	}
	else if (!bs.enable || (!IsDualSourceBlendFactor(bs.src_factor) && !IsDualSourceBlendFactor(bs.dst_factor) &&
							   !IsDualSourceBlendFactor(bs.src_factor_alpha) && !IsDualSourceBlendFactor(bs.dst_factor_alpha)))
	{
		key.ps.no_color1 = 1;
	}
	if (!bs.enable)
		bs = {};

	key.blend = bs.key;
	key.dss = dss.key;
	key.cms = cms.key;
	return key;
}

std::string GSDeviceWebGPU::TFXPipelineLabel(const TFXPipelineKey& key)
{
	return fmt::format("tfx {:02x}/{:016x}_{:016x} b{:08x} d{:02x} m{:x} t{} rt{} ds{}", key.vs, key.ps.key_hi,
		key.ps.key_lo, key.blend, key.dss, key.cms, key.topology, static_cast<int>(key.rt_format),
		static_cast<int>(key.ds_format));
}

GSDeviceWebGPU::PendingTFXPipeline GSDeviceWebGPU::StartTFXPipeline(const TFXPipelineKey& key, bool prewarm)
{
	PendingTFXPipeline p = {};
	p.prewarm = prewarm;
	p.started = WebGPUHost::Now();

	GSHWDrawConfig::VSSelector vs(key.vs);
	GSHWDrawConfig::BlendState bs;
	bs.key = key.blend;
	GSHWDrawConfig::ColorMaskSelector cms;
	cms.key = key.cms;
	const GSHWDrawConfig::DepthStencilSelector dss(key.dss);

	WGPUShaderModule vsm = GetTFXVertexModule(vs);
	WGPUShaderModule fsm = GetTFXFragmentModule(key.ps);
	if (!vsm || !fsm)
		return p;

	static constexpr WGPUVertexAttribute attrs[] = {
		{nullptr, WGPUVertexFormat_Float32x2, 0, 0}, // ST
		{nullptr, WGPUVertexFormat_Uint8x4, 8, 1}, // RGBA
		{nullptr, WGPUVertexFormat_Float32, 12, 2}, // Q
		{nullptr, WGPUVertexFormat_Uint16x2, 16, 3}, // XY
		{nullptr, WGPUVertexFormat_Uint32, 20, 4}, // Z
		{nullptr, WGPUVertexFormat_Uint16x2, 24, 5}, // UV
		{nullptr, WGPUVertexFormat_Unorm8x4, 28, 6}, // FOG
	};
	WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
	vbl.stepMode = WGPUVertexStepMode_Vertex;
	vbl.arrayStride = sizeof(GSVertex);
	vbl.attributeCount = std::size(attrs);
	vbl.attributes = attrs;

	WGPUBlendState blend = {};
	blend.color.operation = s_blend_ops[std::min<u32>(bs.op, 2)];
	blend.color.srcFactor = s_blend_factors[bs.src_factor];
	blend.color.dstFactor = s_blend_factors[bs.dst_factor];
	blend.alpha.operation = WGPUBlendOperation_Add;
	blend.alpha.srcFactor = s_blend_factors[bs.src_factor_alpha];
	blend.alpha.dstFactor = s_blend_factors[bs.dst_factor_alpha];

	WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
	target.format = key.rt_format;
	target.writeMask = key.ps.no_color ? 0 : cms.wrgba;
	target.blend = bs.enable ? &blend : nullptr;

	WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
	fs.module = fsm;
	fs.entryPoint = SV("ps_main");
	fs.targetCount = (key.rt_format != WGPUTextureFormat_Undefined) ? 1 : 0;
	fs.targets = &target;

	WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
	ds.format = key.ds_format;
	const bool depth_test = (dss.ztst != ZTST_ALWAYS || dss.zwe);
	ds.depthWriteEnabled = dss.zwe ? WGPUOptionalBool_True : WGPUOptionalBool_False;
	ds.depthCompare = depth_test ? s_ztst[dss.ztst] : WGPUCompareFunction_Always;
	if (dss.date)
	{
		ds.stencilFront.compare = WGPUCompareFunction_Equal;
		ds.stencilFront.failOp = WGPUStencilOperation_Keep;
		ds.stencilFront.depthFailOp = WGPUStencilOperation_Keep;
		ds.stencilFront.passOp = dss.date_one ? WGPUStencilOperation_Zero : WGPUStencilOperation_Keep;
		ds.stencilBack = ds.stencilFront;
		ds.stencilReadMask = 1;
		ds.stencilWriteMask = 1;
	}
	else
	{
		ds.stencilReadMask = 0;
		ds.stencilWriteMask = 0;
	}

	static constexpr WGPUPrimitiveTopology topologies[] = {
		WGPUPrimitiveTopology_PointList, WGPUPrimitiveTopology_LineList, WGPUPrimitiveTopology_TriangleList};

	WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	const std::string label = TFXPipelineLabel(key);
	d.label = {label.data(), label.size()};
	d.layout = m_tfx_layout;
	d.vertex.module = vsm;
	d.vertex.entryPoint = SV("vs_main");
	d.vertex.bufferCount = (vs.expand == GSHWDrawConfig::VSExpand::None) ? 1 : 0;
	d.vertex.buffers = &vbl;
	d.primitive.topology = topologies[std::min<u32>(key.topology, 2)];
	d.primitive.cullMode = WGPUCullMode_None;
	d.depthStencil = (key.ds_format != WGPUTextureFormat_Undefined) ? &ds : nullptr;
	d.fragment = &fs;

	// §8.4: created asynchronously. GetTFXPipelines() waits for it briefly and PollTFXPipelines()
	// collects it later; its callback runs inside one of their waits (WaitAnyOnly). A prewarmed
	// pipeline (userdata2 set) that fails is dropped quietly: a draw that needs it asks again.
	PipelineRequest* const req = new PipelineRequest();
	WGPUCreateRenderPipelineAsyncCallbackInfo cb = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
	cb.mode = WGPUCallbackMode_WaitAnyOnly;
	cb.userdata1 = req;
	cb.userdata2 = prewarm ? req : nullptr;
	cb.callback = [](WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipeline, WGPUStringView message, void* u1, void* u2) {
		PipelineRequest* const r = PipelineRequest::Arrive(u1);
		if (status != WGPUCreatePipelineAsyncStatus_Success || !r)
		{
			if (r && !u2)
			{
				const std::string text = fmt::format("WebGPU: TFX pipeline creation failed: {}", WebGPUHost::StringViewToString(message));
				emscripten_console_error(text.c_str());
			}
			if (pipeline)
				wgpuRenderPipelineRelease(pipeline);
			pipeline = nullptr;
		}
		if (r)
			r->value = pipeline;
	};
	p.future = wgpuDeviceCreateRenderPipelineAsync(m_device, &d, cb);
	p.request = req;
	return p;
}

bool GSDeviceWebGPU::TakeTFXPipeline(const TFXPipelineKey& key, PendingTFXPipeline& p)
{
	WGPURenderPipeline pipeline = nullptr;
	if (p.request)
	{
		if (!p.request->done)
			return false;
		p.request->Take(&pipeline);
		p.request = nullptr;
	}

	const double ms = WebGPUHost::Now() - p.started;
	if (p.prewarm)
	{
		m_tfx_prewarm_in_flight--;
		if (pipeline)
			m_tfx_prewarmed++;
		else
			m_tfx_prewarm_failed++;
		// A failed prewarm leaves no entry, unless a draw asked for it meanwhile.
		if (!pipeline && !p.logged)
			return true;
	}
	else if (p.skipped)
	{
		m_tfx_async_ready++;
		m_tfx_async_ms += ms;
	}

	if (!pipeline)
		Console.ErrorFmt("WebGPU: {} could not be created; its draws are skipped.", TFXPipelineLabel(key));
	else if (ms > 250.0)
		Console.WriteLnFmt("WebGPU: {} took {:.0f} ms", TFXPipelineLabel(key), ms);
	m_tfx_pipelines.emplace(key, TFXPipeline{pipeline, p.logged});
	return true;
}

void GSDeviceWebGPU::DropTFXPending(PendingTFXPipeline& p)
{
	if (!p.request)
		return;
	// A request still running frees itself in its callback (at the latest when the instance goes).
	p.request->quiet = true;
	WGPURenderPipeline pipeline = nullptr;
	if (p.request->Take(&pipeline) && pipeline)
		wgpuRenderPipelineRelease(pipeline);
	p.request = nullptr;
}

void GSDeviceWebGPU::LogTFXKey(const TFXPipelineKey& key)
{
	WebGPUHost::TFXKeyBlock* const log = WebGPUHost::TFXKeyLog();
	const u32 count = log->count.load(std::memory_order_relaxed);
	if (count >= WebGPUHost::TFX_KEY_CAPACITY)
		return;
	std::memcpy(&log->records[count * WebGPUHost::TFX_KEY_BYTES], &key, sizeof(key));
	log->count.store(count + 1, std::memory_order_release);
}

bool GSDeviceWebGPU::GetTFXPipelines(const TFXPipelineKey* keys, u32 count, WGPURenderPipeline* out)
{
	bool missing = false;
	for (u32 i = 0; i < count; i++)
	{
		const auto it = m_tfx_pipelines.find(keys[i]);
		if (it == m_tfx_pipelines.end())
		{
			missing = true;
			continue;
		}
		out[i] = it->second.pipeline;
		if (!it->second.logged)
		{
			it->second.logged = true;
			LogTFXKey(keys[i]);
		}
	}
	if (!missing)
		return true;

	// Start what is not being created yet. The keys are logged in the order draws first ask for them,
	// the order a later boot of the game prewarms them in.
	PendingTFXPipeline* wait_for[3] = {};
	u32 waits = 0;
	for (u32 i = 0; i < count; i++)
	{
		if (m_tfx_pipelines.find(keys[i]) != m_tfx_pipelines.end())
			continue;
		auto it = m_tfx_pending.find(keys[i]);
		if (it == m_tfx_pending.end())
		{
			it = m_tfx_pending.emplace(keys[i], StartTFXPipeline(keys[i], false)).first;
			PerfCounters::gpu_pipelines.fetch_add(1, std::memory_order_relaxed);
		}
		else if (it->second.prewarm && !it->second.logged)
		{
			m_tfx_prewarm_waited++;
		}
		if (!it->second.logged)
		{
			it->second.logged = true;
			LogTFXKey(keys[i]);
		}
		wait_for[waits++] = &it->second;
	}

	// Wait for them: to the end in the test mode (WebGPUHost::PipelinesWait()), else at most
	// TFX_PIPELINE_WAIT_MS and no longer than what is left of this vsync's budget.
	const bool always = WebGPUHost::PipelinesWait();
	const double t0 = WebGPUHost::Now();
	const double limit = always ? TFX_PIPELINE_TIMEOUT_MS : std::min(TFX_PIPELINE_WAIT_MS, m_tfx_wait_budget_ms);
	for (;;)
	{
		WGPUFutureWaitInfo infos[3];
		u32 n = 0;
		for (u32 i = 0; i < waits; i++)
		{
			if (wait_for[i]->request && !wait_for[i]->request->done)
			{
				infos[n] = WGPU_FUTURE_WAIT_INFO_INIT;
				infos[n++].future = wait_for[i]->future;
			}
		}
		if (n == 0)
			break;
		const double left = limit - (WebGPUHost::Now() - t0);
		WebGPUHost::WaitAny(m_instance, infos, n, std::max(left, 0.0));
		if (left <= 0.0)
			break;
	}
	const double waited = WebGPUHost::Now() - t0;
	PerfCounters::Add(PerfCounters::gpu_pipeline_ns, static_cast<u64>(waited * 1e6));
	m_tfx_wait_budget_ms = std::max(m_tfx_wait_budget_ms - waited, 0.0);

	bool ready = true;
	for (u32 i = 0; i < count; i++)
	{
		const auto pit = m_tfx_pending.find(keys[i]);
		if (pit != m_tfx_pending.end())
		{
			if (TakeTFXPipeline(keys[i], pit->second))
			{
				m_tfx_pending.erase(pit);
			}
			else if (always)
			{
				// As before asynchronous creation: a wait that timed out is handled as a device loss.
				Console.ErrorFmt("WebGPU: timed out creating {}", TFXPipelineLabel(keys[i]));
				if (pit->second.prewarm)
					m_tfx_prewarm_in_flight--;
				DropTFXPending(pit->second);
				m_tfx_pending.erase(pit);
				MarkDeviceLost("a TFX pipeline creation");
				return false;
			}
			else
			{
				pit->second.skipped = true;
				ready = false;
				continue;
			}
		}
		const auto it = m_tfx_pipelines.find(keys[i]);
		out[i] = (it != m_tfx_pipelines.end()) ? it->second.pipeline : nullptr;
	}
	if (!ready)
	{
		m_tfx_skipped_pending++;
#if defined(ARCH_WASM) && TST_WASM_DIAG_GS
		if ((m_tfx_skipped_pending & (m_tfx_skipped_pending - 1)) == 0)
			Console.WarningFmt("TST_GS_DIAG pending_pipeline_skips={}", m_tfx_skipped_pending);
#endif
	}
	return ready;
}

void GSDeviceWebGPU::PollTFXPipelines()
{
	m_tfx_wait_budget_ms = TFX_PIPELINE_FRAME_BUDGET_MS;
	if (m_tfx_pending.empty())
		return;

	// Completes every request whose result has arrived (their callbacks run inside this poll).
	m_tfx_wait_infos.clear();
	for (const auto& it : m_tfx_pending)
	{
		if (it.second.request && !it.second.request->done)
		{
			WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;
			info.future = it.second.future;
			m_tfx_wait_infos.push_back(info);
		}
	}
	if (!m_tfx_wait_infos.empty())
		WebGPUHost::WaitAny(m_instance, m_tfx_wait_infos.data(), static_cast<u32>(m_tfx_wait_infos.size()), 0.0);

	const double now = WebGPUHost::Now();
	bool timed_out = false;
	for (auto it = m_tfx_pending.begin(); it != m_tfx_pending.end();)
	{
		if (TakeTFXPipeline(it->first, it->second))
		{
			it = m_tfx_pending.erase(it);
		}
		else if (now - it->second.started > TFX_PIPELINE_TIMEOUT_MS)
		{
			Console.ErrorFmt("WebGPU: timed out creating {}", TFXPipelineLabel(it->first));
			if (it->second.prewarm)
				m_tfx_prewarm_in_flight--;
			DropTFXPending(it->second);
			it = m_tfx_pending.erase(it);
			timed_out = true;
		}
		else
		{
			++it;
		}
	}
	if (timed_out)
		MarkDeviceLost("a TFX pipeline creation");
}

bool GSDeviceWebGPU::IsUsableStoredTFXKey(const TFXPipelineKey& key) const
{
	// Rebuilt field by field: a stored key with padding or fields MakeTFXKey() would not write is not
	// one this build makes.
	TFXPipelineKey k;
	k.ps = key.ps;
	k.blend = key.blend;
	k.vs = key.vs;
	k.dss = key.dss;
	k.cms = key.cms;
	k.topology = key.topology;
	k.rt_format = key.rt_format;
	k.ds_format = key.ds_format;
	if (!(k == key) || key.topology > 2 ||
		(key.rt_format == WGPUTextureFormat_Undefined && key.ds_format == WGPUTextureFormat_Undefined))
		return false;

	// Only formats this device makes targets in (an unknown enum value must not reach the browser).
	auto known = [this](WGPUTextureFormat f, bool depth) {
		if (f == WGPUTextureFormat_Undefined)
			return true;
		for (u32 i = static_cast<u32>(GSTexture::Format::Color); i <= static_cast<u32>(GSTexture::Format::UInt32); i++)
		{
			const GSTexture::Format gf = static_cast<GSTexture::Format>(i);
			if ((gf == GSTexture::Format::DepthStencil) == depth && LookupNativeFormat(gf) == f)
				return true;
		}
		return false;
	};
	return known(key.rt_format, false) && known(key.ds_format, true);
}

u64 GSDeviceWebGPU::ComputeTFXKeyVersion() const
{
	// FNV-1a over everything that gives a stored key its meaning: the key's layout, tfx.wgsl, the macros
	// each selector bit turns on, the blend, depth and colour mask fields each bit sets, and the texture
	// format values. TFX_KEY_FORMAT covers the code: bump it when MakeTFXKey() or StartTFXPipeline()
	// change how a key maps to a pipeline.
	static constexpr u32 TFX_KEY_FORMAT = 1;
	u64 h = 0xcbf29ce484222325ull;
	auto bytes = [&h](const void* data, size_t size) {
		for (size_t i = 0; i < size; i++)
		{
			h ^= static_cast<const u8*>(data)[i];
			h *= 0x100000001b3ull;
		}
	};
	auto value = [&bytes](u32 v) { bytes(&v, sizeof(v)); };
	auto macros = [&](u32 bit, const WGSLPreprocessor::Macros& list) {
		for (const auto& [name, val] : list)
		{
			if (val == "0")
				continue;
			value(bit);
			bytes(name.data(), name.size());
			bytes(val.data(), val.size());
		}
	};

	value(TFX_KEY_FORMAT);
	value(WebGPUHost::TFX_KEY_BYTES);
	const TFXPipelineKey k;
	const u8* base = reinterpret_cast<const u8*>(&k);
	for (const void* field : {static_cast<const void*>(&k.ps), static_cast<const void*>(&k.blend),
			 static_cast<const void*>(&k.vs), static_cast<const void*>(&k.dss), static_cast<const void*>(&k.cms),
			 static_cast<const void*>(&k.topology), static_cast<const void*>(&k.rt_format),
			 static_cast<const void*>(&k.ds_format)})
		value(static_cast<u32>(static_cast<const u8*>(field) - base));
	bytes(m_tfx_source.data(), m_tfx_source.size());

	for (u32 bit = 0; bit < 128; bit++)
	{
		GSHWDrawConfig::PSSelector ps;
		if (bit < 64)
			ps.key_lo = 1ull << bit;
		else
			ps.key_hi = 1ull << (bit - 64);
		macros(bit, TFXFragmentMacros(ps));
	}
	for (u32 bit = 0; bit < 8; bit++)
		macros(bit, TFXVertexMacros(GSHWDrawConfig::VSSelector(static_cast<u8>(1u << bit))));
	for (u32 bit = 0; bit < 32; bit++)
	{
		GSHWDrawConfig::BlendState bs;
		bs.key = 1u << bit;
		for (u32 v : {static_cast<u32>(bs.enable), static_cast<u32>(bs.constant_enable), static_cast<u32>(bs.op),
				 static_cast<u32>(bs.src_factor), static_cast<u32>(bs.dst_factor), static_cast<u32>(bs.src_factor_alpha),
				 static_cast<u32>(bs.dst_factor_alpha), static_cast<u32>(bs.constant)})
			value(v);
	}
	for (u32 bit = 0; bit < 8; bit++)
	{
		const GSHWDrawConfig::DepthStencilSelector dss(static_cast<u8>(1u << bit));
		GSHWDrawConfig::ColorMaskSelector cms;
		cms.key = static_cast<u8>(1u << bit);
		for (u32 v : {static_cast<u32>(dss.ztst), static_cast<u32>(dss.zwe), static_cast<u32>(dss.date),
				 static_cast<u32>(dss.date_one), static_cast<u32>(cms.wrgba)})
			value(v);
	}
	for (u32 i = 0; i <= static_cast<u32>(GSTexture::Format::Last); i++)
		value(static_cast<u32>(LookupNativeFormat(static_cast<GSTexture::Format>(i))));
	return h;
}

void GSDeviceWebGPU::PrewarmTFXPipelines()
{
	// A list the page handed over (pcsx2web_tfx_prewarm()), taken once the HW resources exist.
	WebGPUHost::TFXKeyBlock* const slot = WebGPUHost::TFXPrewarmSlot();
	u32 state = WebGPUHost::TFX_SLOT_READY;
	if (m_tfx_layout && slot->state.compare_exchange_strong(state, WebGPUHost::TFX_SLOT_TAKING, std::memory_order_acquire))
	{
		const u32 count = std::min(slot->count.load(std::memory_order_relaxed), WebGPUHost::TFX_KEY_CAPACITY);
		if (slot->version_lo == static_cast<u32>(m_tfx_key_version) &&
			slot->version_hi == static_cast<u32>(m_tfx_key_version >> 32))
		{
			m_tfx_prewarm_queue.resize(count);
			for (u32 i = 0; i < count; i++)
				std::memcpy(static_cast<void*>(&m_tfx_prewarm_queue[i]), &slot->records[i * WebGPUHost::TFX_KEY_BYTES], sizeof(TFXPipelineKey));
			m_tfx_prewarm_next = 0;
			Console.WriteLnFmt("WebGPU: creating {} TFX pipelines this game used before, in the background.", count);
		}
		else
		{
			Console.WriteLnFmt("WebGPU: the stored list of {} TFX pipelines is for another version; ignored.", count);
		}
		slot->state.store(WebGPUHost::TFX_SLOT_FREE, std::memory_order_release);
	}

	// A few at a time: each costs the GS thread its shader preprocessing, and the browser compiles them
	// alongside the ones draws ask for.
	const double t0 = WebGPUHost::Now();
	u32 started = 0;
	while (m_tfx_prewarm_next < m_tfx_prewarm_queue.size() && m_tfx_prewarm_in_flight < TFX_PREWARM_IN_FLIGHT &&
		   started < TFX_PREWARM_PER_PRESENT && WebGPUHost::Now() - t0 < TFX_PREWARM_MS_PER_PRESENT)
	{
		const TFXPipelineKey key = m_tfx_prewarm_queue[m_tfx_prewarm_next++];
		if (!IsUsableStoredTFXKey(key) || m_tfx_pipelines.find(key) != m_tfx_pipelines.end() ||
			m_tfx_pending.find(key) != m_tfx_pending.end())
			continue;
		const PendingTFXPipeline p = StartTFXPipeline(key, true);
		if (!p.request)
			continue;
		m_tfx_pending.emplace(key, p);
		m_tfx_prewarm_in_flight++;
		m_tfx_prewarm_started++;
		started++;
	}
	if (!m_tfx_prewarm_queue.empty() && m_tfx_prewarm_next >= m_tfx_prewarm_queue.size())
	{
		m_tfx_prewarm_queue = {};
		m_tfx_prewarm_next = 0;
	}
}

// ----------------------------------------------------------------------------
// Bind groups
// ----------------------------------------------------------------------------

WGPUBindGroup GSDeviceWebGPU::GetTFXGroup(u32 group, const TFXGroupKey& key)
{
	auto& cache = m_tfx_groups[group];
	const auto it = cache.find(key);
	if (it != cache.end())
		return it->second;

	if (cache.size() >= MAX_CACHED_GROUPS)
	{
		// Groups the open encoder used stay alive until it is submitted.
		for (auto& e : cache)
		{
			if (e.second == m_null_group2)
				continue;
			if (m_encoder)
				m_pending_release_groups.push_back(e.second);
			else
				wgpuBindGroupRelease(e.second);
		}
		cache.clear();
		for (WGPUBindGroup& g : m_pass_tfx_groups)
			g = nullptr;
		if (group == 2 && m_null_group2)
			cache.emplace(TFXGroupKey{{m_null_view, m_null_view, m_null_view}, nullptr}, m_null_group2);
	}

	WGPUBindGroupEntry e[4] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
		WGPU_BIND_GROUP_ENTRY_INIT};
	for (u32 i = 0; i < 3; i++)
	{
		e[i].binding = i;
		e[i].textureView = key.views[i];
	}
	u32 count = 3;
	if (group == 1)
	{
		e[3].binding = 3;
		e[3].sampler = key.sampler;
		count = 4;
	}
	WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	d.layout = m_tfx_bgl[group];
	d.entryCount = count;
	d.entries = e;
	WGPUBindGroup bg = wgpuDeviceCreateBindGroup(m_device, &d);
	cache.emplace(key, bg);
	return bg;
}

void GSDeviceWebGPU::DropTFXGroupsUsing(std::initializer_list<WGPUTextureView> views)
{
	for (u32 g = 1; g < 3; g++)
	{
		auto& cache = m_tfx_groups[g];
		for (auto it = cache.begin(); it != cache.end();)
		{
			bool uses = false;
			for (WGPUTextureView v : views)
				uses |= (v && (it->first.views[0] == v || it->first.views[1] == v || it->first.views[2] == v));
			if (!uses)
			{
				++it;
				continue;
			}
			for (WGPUBindGroup& pg : m_pass_tfx_groups)
			{
				if (pg == it->second)
					pg = nullptr;
			}
			if (m_encoder)
				m_pending_release_groups.push_back(it->second);
			else
				wgpuBindGroupRelease(it->second);
			it = cache.erase(it);
		}
	}
}

WGPUBindGroup GSDeviceWebGPU::MakeTFXGroup1(const GSHWDrawConfig& config, GSTextureWebGPU* tex)
{
	TFXGroupKey key{{m_null_view, m_null_view, m_null_view}, m_samplers.Get(config.sampler)};
	if (tex)
	{
		static bool warned = false;
		const WGPUTextureFormat f = tex->GetNativeFormat();
		if (config.ps.depth_fmt != 0 && IsUnfilterableCompatible(f))
			key.views[2] = tex->GetView();
		else if (config.ps.depth_fmt == 0 && IsFilterableFloat(f))
			key.views[0] = tex->GetView();
		else
			LogOnce(warned, fmt::format("WebGPU: a {} source cannot be sampled by this draw (depth_fmt {}).",
								GSTexture::GetFormatName(tex->GetFormat()), static_cast<u32>(config.ps.depth_fmt)));
		tex->SetUseSerial(m_encoder_serial);
	}
	if (GSTextureWebGPU* pal = static_cast<GSTextureWebGPU*>(config.pal))
	{
		key.views[1] = pal->GetView();
		pal->SetUseSerial(m_encoder_serial);
	}
	return GetTFXGroup(1, key);
}

// ----------------------------------------------------------------------------
// Passes and state
// ----------------------------------------------------------------------------

void GSDeviceWebGPU::BeginAttachmentPass(GSTextureWebGPU* rt, GSTextureWebGPU* ds, int stencil_clear, WGPULoadOp colour_load_override)
{
	EndRenderPass();

	WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	if (rt)
	{
		GSVector4 clear_color = GSVector4::zero();
		ca.view = rt->GetRTView();
		ca.loadOp = LoadOpFor(rt, &clear_color);
		if (colour_load_override != WGPULoadOp_Undefined)
			ca.loadOp = colour_load_override;
		ca.storeOp = WGPUStoreOp_Store;
		ca.clearValue = {clear_color.x, clear_color.y, clear_color.z, clear_color.w};
	}

	WGPURenderPassDepthStencilAttachment da = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
	if (ds)
	{
		da.view = ds->GetRTView();
		const GSTexture::State state = ds->GetState();
		const bool cleared = (state == GSTexture::State::Cleared || state == GSTexture::State::Invalidated);
		da.depthLoadOp = cleared ? WGPULoadOp_Clear : WGPULoadOp_Load;
		da.depthClearValue = (state == GSTexture::State::Cleared) ? ds->GetClearDepth() : 0.0f;
		da.depthStoreOp = WGPUStoreOp_Store;
		if (HasStencil(ds->GetNativeFormat()))
		{
			da.stencilLoadOp = (stencil_clear >= 0 || cleared) ? WGPULoadOp_Clear : WGPULoadOp_Load;
			da.stencilClearValue = (stencil_clear >= 0) ? static_cast<u32>(stencil_clear) : 0;
			da.stencilStoreOp = WGPUStoreOp_Store;
		}
	}

	WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	rp.colorAttachmentCount = rt ? 1 : 0;
	rp.colorAttachments = rt ? &ca : nullptr;
	rp.depthStencilAttachment = ds ? &da : nullptr;
	m_pass = wgpuCommandEncoderBeginRenderPass(GetCommandEncoder(), &rp);
	m_pass_rt = rt;
	m_pass_ds = ds;
	m_pass_stencil_clear = stencil_clear;
	m_pass_is_present = false;
	m_pass_size = (rt ? rt : ds)->GetSize();
	if (rt)
	{
		rt->SetUseSerial(m_encoder_serial);
		rt->SetState(GSTexture::State::Dirty);
	}
	if (ds)
	{
		ds->SetUseSerial(m_encoder_serial);
		ds->SetState(GSTexture::State::Dirty);
	}
	InvalidatePassState();
	g_perfmon.Put(GSPerfMon::RenderPasses, 1);
}

bool GSDeviceWebGPU::ApplyTFXState(WGPURenderPipeline pipeline, const GSHWDrawConfig& config, WGPUBindGroup group1,
	WGPUBindGroup group2)
{
	if (!m_pass || !pipeline)
		return false;

	if (m_pass_pipeline != pipeline)
	{
		wgpuRenderPassEncoderSetPipeline(m_pass, pipeline);
		m_pass_pipeline = pipeline;
	}
	if (m_pass_tfx_groups[0] != m_tfx_group0 || m_pass_tfx_offsets[0] != m_vs_cb_offset ||
		m_pass_tfx_offsets[1] != m_ps_cb_offset)
	{
		const u32 offsets[2] = {m_vs_cb_offset, m_ps_cb_offset};
		wgpuRenderPassEncoderSetBindGroup(m_pass, 0, m_tfx_group0, 2, offsets);
		m_pass_tfx_groups[0] = m_tfx_group0;
		m_pass_tfx_offsets[0] = m_vs_cb_offset;
		m_pass_tfx_offsets[1] = m_ps_cb_offset;
		m_pass_uniform_offset = UINT32_MAX; // the utility group 0 must be set again
	}
	if (m_pass_tfx_groups[1] != group1)
	{
		wgpuRenderPassEncoderSetBindGroup(m_pass, 1, group1, 0, nullptr);
		m_pass_tfx_groups[1] = group1;
		m_pass_texture_group = nullptr;
	}
	if (m_pass_tfx_groups[2] != group2)
	{
		wgpuRenderPassEncoderSetBindGroup(m_pass, 2, group2, 0, nullptr);
		m_pass_tfx_groups[2] = group2;
	}

	if (!m_pass_vertex_bound)
	{
		wgpuRenderPassEncoderSetVertexBuffer(m_pass, 0, m_vertex_buffer.GetBuffer(), 0, m_vertex_buffer.GetSize());
		m_pass_vertex_bound = true;
	}
	WGPUBuffer ib = config.vs.UseFixedExpandIndexBuffer() ? m_expand_ib : m_index_buffer.GetBuffer();
	if (m_pass_index_buffer != ib)
	{
		wgpuRenderPassEncoderSetIndexBuffer(m_pass, ib, WGPUIndexFormat_Uint16, 0, WGPU_WHOLE_SIZE);
		m_pass_index_buffer = ib;
	}

	// WebGPU validates the scissor against the attachment.
	const GSVector4i pass_rect(0, 0, m_pass_size.x, m_pass_size.y);
	GSVector4i scissor = config.scissor.rintersect(pass_rect);
	if (scissor.rempty())
		scissor = GSVector4i::zero();
	if (!m_pass_scissor.eq(scissor))
	{
		wgpuRenderPassEncoderSetScissorRect(m_pass, static_cast<u32>(scissor.x), static_cast<u32>(scissor.y),
			static_cast<u32>(scissor.width()), static_cast<u32>(scissor.height()));
		m_pass_scissor = scissor;
	}
	if (!m_pass_viewport.eq(pass_rect))
	{
		wgpuRenderPassEncoderSetViewport(m_pass, 0.0f, 0.0f, static_cast<float>(pass_rect.z), static_cast<float>(pass_rect.w),
			0.0f, 1.0f);
		m_pass_viewport = pass_rect;
	}

	if (config.blend.constant_enable && m_pass_blend_constant != config.blend.constant)
	{
		const double c = static_cast<double>(config.blend.constant) / 128.0;
		const WGPUColor col = {c, c, c, c};
		wgpuRenderPassEncoderSetBlendConstant(m_pass, &col);
		m_pass_blend_constant = config.blend.constant;
	}
	if (m_pass_ds && HasStencil(m_pass_ds->GetNativeFormat()) && m_pass_stencil_ref != 1)
	{
		wgpuRenderPassEncoderSetStencilReference(m_pass, 1);
		m_pass_stencil_ref = 1;
	}
	return true;
}

void GSDeviceWebGPU::DrawTFX(const GSHWDrawConfig& config, u32 offset, u32 count)
{
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	if (config.vs.expand == GSHWDrawConfig::VSExpand::None)
	{
		wgpuRenderPassEncoderDrawIndexed(m_pass, count, 1, m_index_start + offset, static_cast<s32>(m_vertex_start), 0);
	}
	else
	{
		// §8.3: the base vertex travels as the first instance; vertex_index is the expanded index.
		const u32 first = config.vs.UseFixedExpandIndexBuffer() ? offset : (m_index_start + offset);
		wgpuRenderPassEncoderDrawIndexed(m_pass, count, 1, first, 0, m_vertex_start);
	}
}

void GSDeviceWebGPU::FeedbackCopy(const GSHWDrawConfig& config, GSTextureWebGPU* rt, GSTextureWebGPU* rt_clone,
	GSTextureWebGPU* ds, GSTextureWebGPU* ds_clone, const GSVector4i& area)
{
	if (rt_clone && rt)
	{
		const GSVector4i r = area.rintersect(rt->GetRect());
		if (!r.rempty())
			CopyRect(rt, rt_clone, r, r.left, r.top);
	}
	if (ds_clone && ds)
	{
		// Depth copies must cover the whole texture (§2); the clone has the depth buffer's size.
		CopyRect(ds, ds_clone, ds->GetRect(), 0, 0);
	}
}

void GSDeviceWebGPU::SendHWDraw(GSHWDrawConfig& config, WGPURenderPipeline pipeline, GSTextureWebGPU* draw_rt,
	GSTextureWebGPU* draw_ds, GSTextureWebGPU* rt_clone, GSTextureWebGPU* ds_clone, bool one_barrier, bool full_barrier,
	int stencil_clear)
{
	GSTextureWebGPU* tex = static_cast<GSTextureWebGPU*>(config.tex);
	if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_RT && rt_clone)
		tex = rt_clone;
	else if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_DEPTH && ds_clone)
		tex = ds_clone;

	const WGPUBindGroup group1 = MakeTFXGroup1(config, tex);
	WGPUBindGroup group2 = m_null_group2;
	if (rt_clone || ds_clone)
	{
		group2 = GetTFXGroup(2, TFXGroupKey{{rt_clone ? rt_clone->GetView() : m_null_view, m_null_view,
									  ds_clone ? ds_clone->GetView() : m_null_view},
									 nullptr});
		if (rt_clone)
			rt_clone->SetUseSerial(m_encoder_serial);
		if (ds_clone)
			ds_clone->SetUseSerial(m_encoder_serial);
	}

	auto ensure_pass = [&]() {
		// A GS clear can be deferred while these attachments are still bound.
		// Begin a new pass so its load operations consume the pending state.
		if (!m_pass || m_pass_is_present || m_pass_rt != draw_rt || m_pass_ds != draw_ds ||
			(draw_rt && draw_rt->GetState() != GSTexture::State::Dirty) ||
			(draw_ds && draw_ds->GetState() != GSTexture::State::Dirty))
		{
			BeginAttachmentPass(draw_rt, draw_ds, stencil_clear);
			stencil_clear = -1; // passes resumed after a copy keep the stencil
		}
	};

	if (full_barrier && config.drawlist && !config.drawlist->empty())
	{
		const u32 draw_list_size = static_cast<u32>(config.drawlist->size());
		const u32 indices_per_prim = config.indices_per_prim;
		pxAssert(config.drawlist_bbox && static_cast<u32>(config.drawlist_bbox->size()) == draw_list_size);

		if (config.tex_hazard != GSHWDrawConfig::TEX_HAZARD_NONE)
			FeedbackCopy(config, draw_rt, rt_clone, draw_ds, ds_clone, config.samplearea);

		for (u32 n = 0, p = 0; n < draw_list_size; n++)
		{
			const u32 count = static_cast<u32>(config.drawlist->at(n)) * indices_per_prim;
			const GSVector4i bbox = config.drawlist_bbox->at(n).rintersect(config.drawarea);
			FeedbackCopy(config, draw_rt, rt_clone, draw_ds, ds_clone, bbox);
			ensure_pass();
			if (ApplyTFXState(pipeline, config, group1, group2))
				DrawTFX(config, p, count);
			p += count;
		}
		return;
	}

	if (one_barrier || full_barrier)
	{
		// Choose the smaller copy: the union of draw and sample areas, or both separately (as GSDevice11).
		if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_RT)
		{
			const GSVector4i union_rect = config.drawarea.runion(config.samplearea);
			const u32 size_union = union_rect.width() * union_rect.height();
			const u32 size_indiv = config.drawarea.width() * config.drawarea.height() +
								   config.samplearea.width() * config.samplearea.height();
			if (size_union > size_indiv)
			{
				FeedbackCopy(config, draw_rt, rt_clone, draw_ds, ds_clone, config.drawarea);
				FeedbackCopy(config, draw_rt, rt_clone, nullptr, nullptr, config.samplearea);
			}
			else
			{
				FeedbackCopy(config, draw_rt, rt_clone, draw_ds, ds_clone, union_rect);
			}
		}
		else
		{
			FeedbackCopy(config, draw_rt, rt_clone, draw_ds, ds_clone, config.drawarea);
		}
	}

	ensure_pass();
	if (ApplyTFXState(pipeline, config, group1, group2))
		DrawTFX(config, 0, config.nindices);
}

void GSDeviceWebGPU::SetupDATE(GSTextureWebGPU* rt, GSTextureWebGPU* ds, SetDATM datm, const GSVector4i& bbox)
{
	// GSDevice11::SetupDATE: stencil cleared to 0, then 1 where the RT's alpha passes (the DATM shader).
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	PrepareForSampling(rt);

	const WGPURenderPipeline pipeline =
		GetUtilityPipeline(ConvertPipelineKey(ShaderConvertSelector(SetDATMShader(datm)), ds->GetNativeFormat()));
	if (!pipeline)
		return;

	BeginAttachmentPass(nullptr, ds, 0);
	wgpuRenderPassEncoderSetStencilReference(m_pass, 1);
	m_pass_stencil_ref = 1;
	SetUtilityTexture(rt, BindGroupKind::Point);
	SetPipeline(pipeline);
	const GSVector4 src = GSVector4(bbox) / GSVector4(ds->GetSize()).xyxy();
	DrawStretchRect(src, GSVector4(bbox), ds->GetSize());
}

// ----------------------------------------------------------------------------
// RenderHW
// ----------------------------------------------------------------------------

void GSDeviceWebGPU::RenderHW(GSHWDrawConfig& config)
{
	if (m_hw_draws++ == 0)
		Console.WriteLn("WebGPU: the hardware renderer draws its first primitives.");
#if defined(ARCH_WASM) && TST_WASM_DIAG_GS
	// Log a bounded summary; do not change the renderer's scheduling or output.
	if ((m_hw_draws & 0x3fffu) == 0)
		Console.WriteLnFmt("TST_GS_DIAG hw_draws={} total_skipped_draws={} feedback_copy_draws={}",
			m_hw_draws, m_skipped_draws, m_cs_copied_draws);
#endif
	GSTextureWebGPU* const cfg_rt = static_cast<GSTextureWebGPU*>(config.rt);
	GSTextureWebGPU* cfg_ds = static_cast<GSTextureWebGPU*>(config.ds);
	const GSVector2i rtsize = (cfg_rt ? cfg_rt : cfg_ds)->GetSize();
	GSTextureWebGPU* colclip_rt = static_cast<GSTextureWebGPU*>(g_gs_device->GetColorClipTexture());
	GSTextureWebGPU* draw_rt = cfg_rt;
	GSTextureWebGPU* rt_clone = nullptr;
	GSTextureWebGPU* ds_clone = nullptr;

	ScopedGuard recycle_temp_textures([&]() {
		if (rt_clone)
			Recycle(rt_clone);
		if (ds_clone)
			Recycle(ds_clone);
	});

	// WebGPU attachments must all have the same size (DX11 and VK accept a larger one).
	if (cfg_rt && cfg_ds && cfg_rt->GetSize() != cfg_ds->GetSize())
	{
		static bool warned = false;
		const bool depth_unused = (config.depth.ztst == ZTST_ALWAYS && !config.depth.zwe &&
								   config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Stencil &&
								   config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::StencilOne);
		LogOnce(warned, fmt::format("WebGPU: colour {}x{} and depth {}x{} targets differ in size; {}.", cfg_rt->GetWidth(),
							cfg_rt->GetHeight(), cfg_ds->GetWidth(), cfg_ds->GetHeight(),
							depth_unused ? "the unused depth is left out" : "the draw is skipped"));
		if (!depth_unused)
		{
#if defined(ARCH_WASM) && TST_WASM_DIAG_GS
			static thread_local u64 mismatch_skips = 0;
			++mismatch_skips;
			// Power-of-two sampling keeps heavy scenes from flooding the console.
			if ((mismatch_skips & (mismatch_skips - 1)) == 0)
				Console.WarningFmt("TST_GS_DIAG rt_ds_size_mismatch_skips={} rt={}x{} ds={}x{}",
					mismatch_skips, cfg_rt->GetWidth(), cfg_rt->GetHeight(), cfg_ds->GetWidth(), cfg_ds->GetHeight());
#endif
			m_skipped_draws++;
			return;
		}
		cfg_ds = nullptr;
	}
	GSTextureWebGPU* draw_ds = cfg_ds;

	if (colclip_rt)
	{
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::EarlyResolve)
		{
			const GSVector2i size = config.rt->GetSize();
			const GSVector4 dRect(config.colclip_update_area);
			const GSVector4 sRect = dRect / GSVector4(size.x, size.y).xyxy();
			StretchRect(colclip_rt, sRect, config.rt, dRect, ShaderConvert::COLCLIP_RESOLVE, Nearest);
			Recycle(colclip_rt);
			g_gs_device->SetColorClipTexture(nullptr);
			colclip_rt = nullptr;
		}
		else
		{
			config.ps.colclip_hw = 1;
		}
	}

	if (config.ps.colclip_hw)
	{
		if (!colclip_rt)
		{
			config.colclip_update_area = config.drawarea;
			colclip_rt = static_cast<GSTextureWebGPU*>(CreateFeedbackTarget(rtsize.x, rtsize.y, GSTexture::Format::ColorClip, false));
			if (!colclip_rt)
			{
				Console.Warning("WebGPU: Failed to allocate ColorClip render target, aborting draw.");
				return;
			}
			g_gs_device->SetColorClipTexture(colclip_rt);
			const GSVector4 dRect = GSVector4((config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertOnly) ?
												  GSVector4i::loadh(rtsize) :
												  config.drawarea);
			const GSVector4 sRect = dRect / GSVector4(rtsize.x, rtsize.y).xyxy();
			StretchRect(config.rt, sRect, colclip_rt, dRect, ShaderConvert::COLCLIP_INIT, Nearest);
		}
		draw_rt = colclip_rt;
	}

	// Destination alpha (§10.3): stencil only; PrimID tracking is not offered (primitive_id = 0).
	const bool need_barrier = config.require_one_barrier || (config.require_full_barrier && m_features.multidraw_fb_copy);
	int stencil_clear = -1;
	if (draw_ds && HasStencil(draw_ds->GetNativeFormat()))
	{
		if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::Stencil ||
			(config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne && !need_barrier))
		{
			SetupDATE(draw_rt, draw_ds, config.datm, config.drawarea);
		}
		else if (config.destination_alpha == GSHWDrawConfig::DestinationAlphaMode::StencilOne)
		{
			stencil_clear = 1;
		}
	}

	// Keep the open pass when this draw needs only its colour or only its depth (GSDevice11/VK).
	if (!draw_ds && draw_rt && m_pass && !m_pass_is_present && m_pass_rt == draw_rt && m_pass_ds &&
		m_pass_ds->GetSize() == draw_rt->GetSize() && config.tex != m_pass_ds)
	{
		draw_ds = m_pass_ds;
		config.depth = GSHWDrawConfig::DepthStencilSelector::NoDepth();
		config.alpha_second_pass.depth = GSHWDrawConfig::DepthStencilSelector::NoDepth();
	}
	else if (!draw_rt && draw_ds && m_pass && !m_pass_is_present && m_pass_ds == draw_ds && m_pass_rt &&
			 m_pass_rt->GetSize() == draw_ds->GetSize() && config.tex != m_pass_rt)
	{
		draw_rt = m_pass_rt;
		config.ps.no_color = 1;
		config.alpha_second_pass.ps.no_color = 1;
	}

	// Vertices, indices and constants for every draw below go into the rings now, so that no submit
	// (ring full) can come between them and their draws.
	const u32 vertex_bytes = config.nverts * sizeof(GSVertex);
	const u32 index_bytes = config.vs.UseFixedExpandIndexBuffer() ? 0 : Common::AlignUpPow2(config.nindices * sizeof(u16), 4);
	auto reserve = [&]() {
		return m_vertex_buffer.ReserveMemory(vertex_bytes, sizeof(GSVertex)) &&
			   (index_bytes == 0 || m_index_buffer.ReserveMemory(index_bytes, 4)) &&
			   m_uniform_buffer.ReserveMemory(VS_CB_SLOT + 2 * PS_CB_SLOT + CS_PARAMS_SLOT, UNIFORM_SLOT);
	};
	if (!reserve())
	{
		ExecuteCommandBuffer("stream buffers full (HW draw)");
		if (!reserve())
		{
			Console.ErrorFmt("WebGPU: a draw of {} vertices and {} indices does not fit in the stream buffers.",
				config.nverts, config.nindices);
			return;
		}
	}
	m_vertex_start = m_vertex_buffer.GetCurrentOffset() / sizeof(GSVertex);
	std::memcpy(m_vertex_buffer.GetCurrentHostPointer(), config.verts, vertex_bytes);
	m_vertex_buffer.CommitMemory(vertex_bytes);
	m_index_start = 0;
	if (index_bytes)
	{
		m_index_start = m_index_buffer.GetCurrentOffset() / sizeof(u16);
		std::memcpy(m_index_buffer.GetCurrentHostPointer(), config.indices, config.nindices * sizeof(u16));
		m_index_buffer.CommitMemory(index_bytes);
	}
	if (m_cb_serial != m_encoder_serial)
	{
		m_cb_serial = m_encoder_serial;
		m_vs_cb_offset = UINT32_MAX;
		m_ps_cb_offset = UINT32_MAX;
	}
	if (m_vs_cb_offset == UINT32_MAX || std::memcmp(&m_vs_cb_cache, &config.cb_vs, sizeof(config.cb_vs)) != 0)
	{
		m_uniform_buffer.ReserveMemory(VS_CB_SLOT, UNIFORM_SLOT);
		m_vs_cb_offset = m_uniform_buffer.GetCurrentOffset();
		std::memcpy(m_uniform_buffer.GetCurrentHostPointer(), &config.cb_vs, VS_CB_SIZE);
		m_uniform_buffer.CommitMemory(VS_CB_SLOT);
		m_vs_cb_cache = config.cb_vs;
	}
	auto upload_ps_cb = [&]() {
		if (m_ps_cb_offset != UINT32_MAX && m_ps_cb_cache == config.cb_ps)
			return;
		m_uniform_buffer.ReserveMemory(PS_CB_SLOT, UNIFORM_SLOT);
		m_ps_cb_offset = m_uniform_buffer.GetCurrentOffset();
		std::memcpy(m_uniform_buffer.GetCurrentHostPointer(), &config.cb_ps, PS_CB_SIZE);
		m_uniform_buffer.CommitMemory(PS_CB_SLOT);
		m_ps_cb_cache = config.cb_ps;
	};
	upload_ps_cb();

	// A feedback draw that reads only its own pixel: a compute dispatch instead of a copy and a render pass.
	if (m_cs_enabled && cfg_rt && draw_rt == cfg_rt && !cfg_ds && !colclip_rt && TryComputeFeedbackDraw(config, cfg_rt))
		return;
	if (config.require_one_barrier || config.require_full_barrier)
		m_cs_copied_draws++;

	// The pipelines of every pass of this draw: asked for together, so that they are created in parallel,
	// and the draw is skipped whole when one of them is not ready yet.
	GSHWDrawConfig::PSSelector mp_ps = config.ps;
	mp_ps.no_color1 = config.blend_multi_pass.no_color1;
	mp_ps.blend_hw = config.blend_multi_pass.blend_hw;
	mp_ps.dither = config.blend_multi_pass.dither;
	TFXPipelineKey keys[3];
	WGPURenderPipeline pipelines[3] = {};
	u32 num_keys = 0;
	keys[num_keys++] = MakeTFXKey(config, config.ps, config.blend, config.colormask, config.depth, draw_rt, draw_ds);
	const u32 mp_index = config.blend_multi_pass.enable ? num_keys++ : 0;
	if (mp_index)
		keys[mp_index] = MakeTFXKey(config, mp_ps, config.blend_multi_pass.blend, config.colormask, config.depth, draw_rt, draw_ds);
	const u32 ap_index = config.alpha_second_pass.enable ? num_keys++ : 0;
	if (ap_index)
	{
		keys[ap_index] = MakeTFXKey(config, config.alpha_second_pass.ps, config.blend, config.alpha_second_pass.colormask,
			config.alpha_second_pass.depth, draw_rt, draw_ds);
	}
	auto finish_colclip = [&]() {
		if (!colclip_rt)
			return;
		config.colclip_update_area = config.colclip_update_area.runion(config.drawarea);
		if (config.colclip_mode == GSHWDrawConfig::ColClipMode::ResolveOnly ||
			config.colclip_mode == GSHWDrawConfig::ColClipMode::ConvertAndResolve)
		{
			const GSVector2i size = config.rt->GetSize();
			const GSVector4 dRect(config.colclip_update_area);
			const GSVector4 sRect = dRect / GSVector4(size.x, size.y).xyxy();
			StretchRect(colclip_rt, sRect, config.rt, dRect, ShaderConvert::COLCLIP_RESOLVE, Nearest);
			Recycle(colclip_rt);
			g_gs_device->SetColorClipTexture(nullptr);
		}
	};
	const bool ready = GetTFXPipelines(keys, num_keys, pipelines);
	const WGPURenderPipeline pipeline = ready ? pipelines[0] : nullptr;
	if (!pipeline)
	{
#if defined(ARCH_WASM) && TST_WASM_DIAG_GS
		if (ready)
		{
			static thread_local u64 pipeline_failed = 0;
			++pipeline_failed;
			if ((pipeline_failed & (pipeline_failed - 1)) == 0)
				Console.WarningFmt("TST_GS_DIAG failed_pipeline_draws={}", pipeline_failed);
		}
#endif
		m_skipped_draws++;
		// A draw skipped while its pipeline is created still resolves the colour clip target as the draw
		// would have. (A pipeline that failed keeps the old early return.) Needs proper testing: no game
		// tried so far skipped a colour clip draw.
		if (!ready)
			finish_colclip();
		return;
	}

	// Feedback through copies (§10.2).
	const bool rt_feedback = config.IsFeedbackLoopRT(config.ps) ||
							 (config.alpha_second_pass.enable && config.IsFeedbackLoopRT(config.alpha_second_pass.ps));
	const bool ds_feedback = config.IsFeedbackLoopDepth(config.ps) ||
							 (config.alpha_second_pass.enable && config.IsFeedbackLoopDepth(config.alpha_second_pass.ps));
	if (draw_rt && need_barrier && rt_feedback)
	{
		rt_clone = static_cast<GSTextureWebGPU*>(CreateTexture(rtsize.x, rtsize.y, 1, draw_rt->GetFormat(), true));
		if (!rt_clone)
			Console.Warning("WebGPU: Failed to allocate temp texture for RT copy.");
	}
	if (draw_ds && need_barrier && ds_feedback)
	{
		ds_clone = static_cast<GSTextureWebGPU*>(CreateTexture(draw_ds->GetWidth(), draw_ds->GetHeight(), 1, draw_ds->GetFormat(), true));
		if (!ds_clone)
			Console.Warning("WebGPU: Failed to allocate temp texture for DS copy.");
	}

	// Sources must hold defined data, and no source may be an attachment of the pass.
	GSTexture* tex_copy = nullptr;
	ScopedGuard recycle_tex_copy([&]() {
		if (tex_copy)
			Recycle(tex_copy);
	});
	if (config.tex && config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_NONE &&
		(config.tex == draw_rt || config.tex == draw_ds))
	{
		GSTexture* src = config.tex;
		tex_copy = CreateTexture(src->GetWidth(), src->GetHeight(), 1, src->GetFormat(), true);
		if (tex_copy)
			CopyRect(src, tex_copy, src->GetRect(), 0, 0);
		config.tex = tex_copy;
	}
	if (config.tex && config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_NONE)
		PrepareForSampling(static_cast<GSTextureWebGPU*>(config.tex));
	if (config.pal)
		PrepareForSampling(static_cast<GSTextureWebGPU*>(config.pal));

	SendHWDraw(config, pipeline, draw_rt, draw_ds, rt_clone, ds_clone, config.require_one_barrier,
		config.require_full_barrier && m_features.multidraw_fb_copy, stencil_clear);

	if (config.blend_multi_pass.enable)
	{
		const WGPURenderPipeline mp = pipelines[mp_index];
		GSHWDrawConfig mp_config = config;
		mp_config.ps = mp_ps;
		mp_config.blend = config.blend_multi_pass.blend;
		if (mp)
			SendHWDraw(mp_config, mp, draw_rt, draw_ds, rt_clone, ds_clone, false, false, -1);
	}

	if (config.alpha_second_pass.enable)
	{
		if (config.cb_ps.FogColor_AREF.a != config.alpha_second_pass.ps_aref)
		{
			config.cb_ps.FogColor_AREF.a = config.alpha_second_pass.ps_aref;
			upload_ps_cb();
		}
		const WGPURenderPipeline ap = pipelines[ap_index];
		GSHWDrawConfig ap_config = config;
		ap_config.ps = config.alpha_second_pass.ps;
		ap_config.colormask = config.alpha_second_pass.colormask;
		ap_config.depth = config.alpha_second_pass.depth;
		if (ap)
		{
			SendHWDraw(ap_config, ap, draw_rt, draw_ds, rt_clone, ds_clone,
				config.alpha_second_pass.require_one_barrier && m_features.multidraw_fb_copy,
				config.alpha_second_pass.require_full_barrier && m_features.multidraw_fb_copy, -1);
		}
	}

	finish_colclip();
}

void GSDeviceWebGPU::ClearSamplerCache()
{
	// The sampler cache is keyed by every field a sampler depends on, so nothing goes stale.
}

// ----------------------------------------------------------------------------
// Texture cache conversions (GSDeviceVK's uniform layouts)
// ----------------------------------------------------------------------------

void GSDeviceWebGPU::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex,
	u32 dOffset, u32 dSize)
{
	struct alignas(16) Uniforms
	{
		u32 offsetX, offsetY, dOffset, pad1;
		float scale;
		float pad2[3];
	};
	Uniforms uniforms = {};
	uniforms.offsetX = offsetX;
	uniforms.offsetY = offsetY;
	uniforms.dOffset = dOffset;
	uniforms.scale = sScale;
	SetUtilityUniforms(&uniforms, sizeof(uniforms));

	const GSVector4 dRect(0, 0, dSize, 1);
	const ShaderConvert shader = (dSize == 16) ? ShaderConvert::CLUT_4 : ShaderConvert::CLUT_8;
	DoStretchRect(sTex, GSVector4::zero(), dTex, dRect, ShaderConvertSelector(shader), Nearest);
}

void GSDeviceWebGPU::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW,
	u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	struct alignas(16) Uniforms
	{
		u32 SBW;
		u32 DBW;
		u32 PSM;
		u32 pad1[1];
		float ScaleFactor;
		float pad2[3];
	};
	Uniforms uniforms = {};
	uniforms.SBW = SBW;
	uniforms.DBW = DBW;
	uniforms.PSM = SPSM;
	uniforms.ScaleFactor = sScale;
	SetUtilityUniforms(&uniforms, sizeof(uniforms));

	const ShaderConvert shader = ((SPSM & 0xE) == 0) ? ShaderConvert::RGBA_TO_8I : ShaderConvert::RGB5A1_TO_8I;
	const GSVector4 dRect(0, 0, dTex->GetWidth(), dTex->GetHeight());
	DoStretchRect(sTex, GSVector4::zero(), dTex, dRect, ShaderConvertSelector(shader), Nearest);
}

void GSDeviceWebGPU::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
	const GSVector2i& clamp_min, const GSVector4& dRect)
{
	struct alignas(16) Uniforms
	{
		GSVector2i clamp_min;
		int downsample_factor;
		int pad0;
		float weight;
		float step_multiplier;
		float pad1[2];
	};
	Uniforms uniforms = {};
	uniforms.clamp_min = clamp_min;
	uniforms.downsample_factor = downsample_factor;
	uniforms.weight = static_cast<float>(downsample_factor * downsample_factor);
	uniforms.step_multiplier = (GSConfig.UserHacks_NativeScaling > GSNativeScaling::Aggressive) ? 2.0f : 1.0f;
	SetUtilityUniforms(&uniforms, sizeof(uniforms));

	DoStretchRect(sTex, GSVector4::zero(), dTex, dRect, ShaderConvertSelector(ShaderConvert::DOWNSAMPLE_COPY), Nearest);
}

// ----------------------------------------------------------------------------
// Feedback draws as compute dispatches
// ----------------------------------------------------------------------------
//
// A draw that reads the colour target it draws into goes through a copy (FeedbackCopy()): the copy ends the
// render pass, the draw starts a new one. Games that do many such draws (FFXII's effects, hundreds of small
// quads per frame on a 256x256 target) spend nearly all their GS time on those passes and copies (§10.2).
// When a run of such draws reads only the pixel each fragment writes, it runs in one compute pass instead:
// each draw is a dispatch that reads the target as it was before the draw from one scratch texture (as the
// render path reads its copy, with textureLoad) and writes the whole target to the other, the fragment code's
// result where the draw covers the pixel and the old value elsewhere (tfx.wgsl, cs_main). Dispatches in a
// compute pass see each other's writes in order, which draws in a render pass do not. The target is copied
// into the scratch textures once at the start of the run and back once at its end (FlushComputeRun()).
//
// Exactness: the fragment code is tfx.wgsl's own (ps_main_cs, the same body), it gets only the pixel's
// position (selectors that use texture coordinates, colours, fog or depth do not qualify), and coverage is
// that of the rasterizer: the draw's rectangles in framebuffer coordinates as the vertex shader and the
// viewport place them, pixel centres inside with the top-left rule (left and top edges in, right and bottom
// edges out), restricted to edges far enough from the pixel centres that the rasterizer's sub-pixel snapping
// and float rounding cannot change which pixels are in.

namespace
{
	/// The selectors whose fragment code uses only the pixel's position and the target's value there.
	bool IsOwnPixelFeedbackSelector(const GSHWDrawConfig::PSSelector& ps)
	{
		// tex_is_fb: the texture is the target, read at the fragment's own position (sample_from_rt()); with
		// ltf 0 the texture coordinates only select which of four equal reads to use. tfx 1 (decal) with tcc 1
		// leaves the vertex colour unused.
		if (!ps.tex_is_fb || ps.ltf || ps.depth_fmt != 0 || ps.channel != 0 || ps.pal_fmt != 0)
			return false;
		if (ps.tfx != 1 || !ps.tcc || ps.fog || ps.aa1 != GSShader::PS_AA1::NONE || ps.abe)
			return false;
		// No depth: no depth test reading the depth copy, no depth output, no stencil DATE, no PrimID DATE.
		if (ps.ztst >= 2 || ps.zclamp || ps.zfloor || (ps.date != 0 && ps.date < 5))
			return false;
		if (ps.afail == GSShader::PS_AFAIL::FB_ONLY || ps.afail == GSShader::PS_AFAIL::RGB_ONLY_DSB || ps.afail == GSShader::PS_AFAIL::RGB_ONLY_SW_Z)
			return false;
		// Colour written to an RGBA8 target, no hardware blending folded into the shader.
		if (ps.no_color || ps.colclip_hw || ps.blend_hw != 0)
			return false;
		if (ps.urban_chaos_hle || ps.tales_of_abyss_hle || ps.rov_color || ps.rov_depth != GSShader::PS_ROV_DEPTH::NONE)
			return false;
		return true;
	}

	/// Whether the rasterizer's decision for an edge at framebuffer coordinate e cannot differ from the exact
	/// one: e is not just above a pixel centre (where snapping down to a 1/256 grid, or a float rounding
	/// down, would bring the centre inside), nor so close below one that float rounding could put it above.
	bool IsRobustEdge(double e)
	{
		const double d = e - (std::floor(e - 0.5) + 0.5); // above the nearest centre at or below e, [0, 1)
		return d >= (1.0 / 256.0 + 1.0 / 4096.0) && d <= 1.0 - (1.0 / 4096.0);
	}

	/// The pixels the draw covers, as up to CS_MAX_RECTS half-open rectangles: two-triangle quads (a triangle
	/// list in groups of six indices) or expanded sprites, each an axis-aligned rectangle. False if the draw
	/// has another shape or an edge that is not robust.
	bool FeedbackDrawRects(const GSHWDrawConfig& config, const GSVector2i& size, std::array<GSVector4i, CS_MAX_RECTS>* out,
		u32* count)
	{
		if (config.topology != GSHWDrawConfig::Topology::Triangle)
			return false;
		const double sx = config.cb_vs.vertex_scale.x, sy = config.cb_vs.vertex_scale.y;
		const double ox = config.cb_vs.vertex_offset.x, oy = config.cb_vs.vertex_offset.y;
		// tfx.wgsl process_vertex(): clip = (p - 0.05) * (sx, -sy) - (ox, -oy), then the viewport over the
		// whole target (WebGPU's framebuffer y goes down).
		auto fb_x = [&](u32 x) { return ((static_cast<double>(x) - 0.05) * sx - ox + 1.0) * 0.5 * size.x; };
		auto fb_y = [&](u32 y) { return (1.0 - (-(static_cast<double>(y) - 0.05) * sy + oy)) * 0.5 * size.y; };

		const GSVector4i scissor = config.scissor.rintersect(GSVector4i(0, 0, size.x, size.y));
		u32 n = 0;
		auto add = [&](u32 xa, u32 xb, u32 ya, u32 yb) {
			if (xa == xb || ya == yb)
				return true; // empty
			if (n >= CS_MAX_RECTS)
				return false;
			const double e[4] = {fb_x(std::min(xa, xb)), fb_y(std::min(ya, yb)), fb_x(std::max(xa, xb)), fb_y(std::max(ya, yb))};
			for (double v : e)
			{
				if (!std::isfinite(v) || !IsRobustEdge(v))
					return false;
			}
			// Pixel i is in when e0 <= i + 0.5 < e1.
			const double x0 = std::min(e[0], e[2]), x1 = std::max(e[0], e[2]);
			const double y0 = std::min(e[1], e[3]), y1 = std::max(e[1], e[3]);
			GSVector4i r(static_cast<int>(std::ceil(x0 - 0.5)), static_cast<int>(std::ceil(y0 - 0.5)),
				static_cast<int>(std::ceil(x1 - 0.5)), static_cast<int>(std::ceil(y1 - 0.5)));
			r = r.rintersect(scissor);
			if (!r.rempty())
				(*out)[n++] = r;
			return true;
		};

		if (config.vs.expand == GSHWDrawConfig::VSExpand::Sprite)
		{
			// Sprites come in vertex pairs (tfx.wgsl: VS_EXPAND_SPRITE); the fixed index buffer splits each
			// rectangle along a diagonal.
			if (config.nverts % 2 != 0 || config.nindices != config.nverts * 3)
				return false;
			for (u32 i = 0; i < config.nverts; i += 2)
			{
				const GSVertex& a = config.verts[i];
				const GSVertex& b = config.verts[i + 1];
				if (!add(a.XYZ.X, b.XYZ.X, a.XYZ.Y, b.XYZ.Y))
					return false;
			}
		}
		else if (config.vs.expand == GSHWDrawConfig::VSExpand::None)
		{
			if (config.nindices % 6 != 0 || config.indices_per_prim != 3)
				return false;
			for (u32 i = 0; i < config.nindices; i += 6)
			{
				const u16* idx = &config.indices[i];
				for (u32 k = 0; k < 6; k++)
				{
					if (idx[k] >= config.nverts)
						return false;
				}
				// Two triangles that share two vertices, which are opposite corners of an axis-aligned rectangle
				// whose other corners are the triangles' third vertices.
				u32 shared[2], ns = 0, other[2], no = 0;
				for (u32 k = 0; k < 3; k++)
				{
					const bool in_b = (idx[k] == idx[3] || idx[k] == idx[4] || idx[k] == idx[5]);
					if (in_b && ns < 2)
						shared[ns++] = idx[k];
					else if (!in_b && no < 1)
						other[no++] = idx[k];
					else
						return false;
				}
				for (u32 k = 3; k < 6; k++)
				{
					if (idx[k] != shared[0] && idx[k] != shared[1])
					{
						if (no != 1)
							return false;
						other[no++] = idx[k];
					}
				}
				if (ns != 2 || no != 2)
					return false;
				const GSVertex& s0 = config.verts[shared[0]];
				const GSVertex& s1 = config.verts[shared[1]];
				const GSVertex& o0 = config.verts[other[0]];
				const GSVertex& o1 = config.verts[other[1]];
				const u32 x0 = s0.XYZ.X, y0 = s0.XYZ.Y, x1 = s1.XYZ.X, y1 = s1.XYZ.Y;
				if (x0 == x1 || y0 == y1)
					return false; // the shared edge is not a diagonal
				const bool corners = (o0.XYZ.X == x0 && o0.XYZ.Y == y1 && o1.XYZ.X == x1 && o1.XYZ.Y == y0) ||
									 (o0.XYZ.X == x1 && o0.XYZ.Y == y0 && o1.XYZ.X == x0 && o1.XYZ.Y == y1);
				if (!corners || !add(x0, x1, y0, y1))
					return false;
			}
		}
		else
		{
			return false;
		}
		*count = n;
		return true;
	}
} // namespace

bool GSDeviceWebGPU::CreateComputeFeedbackResources()
{
	auto entry = [](u32 binding) {
		WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
		e.binding = binding;
		e.visibility = WGPUShaderStage_Compute;
		return e;
	};
	auto layout = [this](const char* label, const WGPUBindGroupLayoutEntry* e, u32 n) {
		WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		d.label = SV(label);
		d.entryCount = n;
		d.entries = e;
		return wgpuDeviceCreateBindGroupLayout(m_device, &d);
	};
	{
		WGPUBindGroupLayoutEntry e = entry(1);
		e.buffer.type = WGPUBufferBindingType_Uniform;
		e.buffer.hasDynamicOffset = true;
		e.buffer.minBindingSize = PS_CB_SIZE;
		m_cs_bgl[0] = layout("tfx cs constants", &e, 1);
	}
	{
		WGPUBindGroupLayoutEntry e[4] = {entry(0), entry(1), entry(2), entry(3)};
		for (u32 i = 0; i < 3; i++)
		{
			e[i].texture.sampleType = (i == 2) ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
			e[i].texture.viewDimension = WGPUTextureViewDimension_2D;
		}
		e[3].sampler.type = WGPUSamplerBindingType_Filtering;
		m_cs_bgl[1] = layout("tfx cs textures", e, 4);
	}
	{
		WGPUBindGroupLayoutEntry e[3] = {entry(0), entry(1), entry(2)};
		for (WGPUBindGroupLayoutEntry& x : e)
		{
			x.texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
			x.texture.viewDimension = WGPUTextureViewDimension_2D;
		}
		m_cs_bgl[2] = layout("tfx cs feedback", e, 3);
	}
	{
		WGPUBindGroupLayoutEntry e[2] = {entry(0), entry(1)};
		e[0].storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
		e[0].storageTexture.format = WGPUTextureFormat_RGBA8Unorm;
		e[0].storageTexture.viewDimension = WGPUTextureViewDimension_2D;
		e[1].buffer.type = WGPUBufferBindingType_Uniform;
		e[1].buffer.hasDynamicOffset = true;
		e[1].buffer.minBindingSize = sizeof(CSParams);
		m_cs_bgl[3] = layout("tfx cs output", e, 2);
	}
	for (WGPUBindGroupLayout l : m_cs_bgl)
	{
		if (!l)
			return false;
	}
	{
		WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pd.label = SV("tfx cs");
		pd.bindGroupLayoutCount = 4;
		pd.bindGroupLayouts = m_cs_bgl;
		m_cs_layout = wgpuDeviceCreatePipelineLayout(m_device, &pd);
	}
	{
		WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
		e.binding = 1;
		e.buffer = m_uniform_buffer.GetBuffer();
		e.size = PS_CB_SIZE;
		WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		d.label = SV("tfx cs constants");
		d.layout = m_cs_bgl[0];
		d.entryCount = 1;
		d.entries = &e;
		m_cs_group0 = wgpuDeviceCreateBindGroup(m_device, &d);
	}
	{
		WGPUBindGroupEntry e[4] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
			WGPU_BIND_GROUP_ENTRY_INIT};
		for (u32 i = 0; i < 3; i++)
		{
			e[i].binding = i;
			e[i].textureView = m_null_view;
		}
		e[3].binding = 3;
		e[3].sampler = m_linear_sampler;
		WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		d.label = SV("tfx cs no textures");
		d.layout = m_cs_bgl[1];
		d.entryCount = 4;
		d.entries = e;
		m_cs_group1 = wgpuDeviceCreateBindGroup(m_device, &d);
	}
	return m_cs_layout && m_cs_group0 && m_cs_group1;
}

void GSDeviceWebGPU::DestroyComputeFeedbackResources()
{
	if (m_cs_pass)
	{
		wgpuComputePassEncoderEnd(m_cs_pass);
		wgpuComputePassEncoderRelease(m_cs_pass);
		m_cs_pass = nullptr;
	}
	m_cs_run = nullptr;
	m_cs_rt = nullptr;
	m_cs_pass_pipeline = nullptr;
	for (auto& it : m_cs_pipelines)
	{
		if (it.second)
			wgpuComputePipelineRelease(it.second);
	}
	m_cs_pipelines.clear();
	for (auto& sc : m_cs_scratch)
	{
		for (u32 i = 0; i < 2; i++)
		{
			if (sc->read_group[i])
				wgpuBindGroupRelease(sc->read_group[i]);
			if (sc->write_group[i])
				wgpuBindGroupRelease(sc->write_group[i]);
			if (sc->view[i])
				wgpuTextureViewRelease(sc->view[i]);
			if (sc->tex[i])
			{
				wgpuTextureDestroy(sc->tex[i]);
				wgpuTextureRelease(sc->tex[i]);
			}
		}
	}
	m_cs_scratch.clear();
	if (m_cs_group0)
		wgpuBindGroupRelease(m_cs_group0);
	if (m_cs_group1)
		wgpuBindGroupRelease(m_cs_group1);
	m_cs_group0 = m_cs_group1 = nullptr;
	if (m_cs_layout)
		wgpuPipelineLayoutRelease(m_cs_layout);
	m_cs_layout = nullptr;
	for (WGPUBindGroupLayout& l : m_cs_bgl)
	{
		if (l)
			wgpuBindGroupLayoutRelease(l);
		l = nullptr;
	}
	m_cs_enabled = false;
}

WGPUComputePipeline GSDeviceWebGPU::GetTFXComputePipeline(const GSHWDrawConfig::PSSelector& sel)
{
	const auto it = m_cs_pipelines.find(sel);
	if (it != m_cs_pipelines.end())
		return it->second;

	WGSLPreprocessor::Macros macros = TFXFragmentMacros(sel);
	macros.emplace_back("COMPUTE_SHADER", "1");
	std::string error;
	const std::string code = WGSLPreprocessor::Process(m_tfx_source, macros, &error);
	if (!error.empty())
		Console.ErrorFmt("WebGPU: tfx.wgsl (compute {:016x}_{:016x}): {}", sel.key_hi, sel.key_lo, error);
	const std::string label = fmt::format("tfx cs {:016x}_{:016x}", sel.key_hi, sel.key_lo);
	WGPUShaderModule mod = CreateShaderModule(label.c_str(), code);
	WGPUComputePipeline pipeline = nullptr;
	if (mod)
	{
		// Created synchronously: the browser compiles it in the background, and the first dispatch waits for
		// it on the GPU timeline (no draw is skipped).
		WGPUComputePipelineDescriptor d = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
		d.label = {label.data(), label.size()};
		d.layout = m_cs_layout;
		d.compute.module = mod;
		d.compute.entryPoint = SV("cs_main");
		pipeline = wgpuDeviceCreateComputePipeline(m_device, &d);
		wgpuShaderModuleRelease(mod);
	}
	if (!pipeline)
		Console.ErrorFmt("WebGPU: {} could not be created; its draws use copies.", label);
	m_cs_pipelines.emplace(sel, pipeline);
	return pipeline;
}

GSDeviceWebGPU::ComputeScratch* GSDeviceWebGPU::GetComputeScratch(const GSVector2i& size)
{
	for (auto& sc : m_cs_scratch)
	{
		if (sc->size == size)
			return sc.get();
	}

	auto sc = std::make_unique<ComputeScratch>();
	std::memset(static_cast<void*>(sc.get()), 0, sizeof(ComputeScratch));
	sc->size = size;
	bool ok = true;
	for (u32 i = 0; i < 2; i++)
	{
		WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
		td.label = SV("tfx cs scratch");
		td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopySrc |
				   WGPUTextureUsage_CopyDst;
		td.size = {static_cast<u32>(size.x), static_cast<u32>(size.y), 1};
		td.format = WGPUTextureFormat_RGBA8Unorm;
		sc->tex[i] = wgpuDeviceCreateTexture(m_device, &td);
		sc->view[i] = sc->tex[i] ? wgpuTextureCreateView(sc->tex[i], nullptr) : nullptr;
		ok = ok && sc->view[i];
	}
	for (u32 i = 0; ok && i < 2; i++)
	{
		WGPUBindGroupEntry r[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
		for (u32 k = 0; k < 3; k++)
		{
			r[k].binding = k;
			r[k].textureView = (k == 0) ? sc->view[i] : m_null_view;
		}
		WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		d.label = SV("tfx cs read");
		d.layout = m_cs_bgl[2];
		d.entryCount = 3;
		d.entries = r;
		sc->read_group[i] = wgpuDeviceCreateBindGroup(m_device, &d);

		WGPUBindGroupEntry w[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
		w[0].binding = 0;
		w[0].textureView = sc->view[i];
		w[1].binding = 1;
		w[1].buffer = m_uniform_buffer.GetBuffer();
		w[1].size = sizeof(CSParams);
		d.label = SV("tfx cs write");
		d.layout = m_cs_bgl[3];
		d.entryCount = 2;
		d.entries = w;
		sc->write_group[i] = wgpuDeviceCreateBindGroup(m_device, &d);
		ok = sc->read_group[i] && sc->write_group[i];
	}
	m_cs_scratch.push_back(std::move(sc));
	return ok ? m_cs_scratch.back().get() : nullptr;
}

bool GSDeviceWebGPU::TryComputeFeedbackDraw(GSHWDrawConfig& config, GSTextureWebGPU* rt)
{
	if (!config.require_one_barrier || config.require_full_barrier || config.blend.enable ||
		config.blend_multi_pass.enable || config.alpha_second_pass.enable ||
		config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off || config.depth.ztst != ZTST_ALWAYS ||
		config.depth.zwe || config.pal || config.colormask.wrgba == 0 || !IsOwnPixelFeedbackSelector(config.ps) ||
		rt->GetNativeFormat() != WGPUTextureFormat_RGBA8Unorm)
	{
		return false;
	}

	const GSVector2i size = rt->GetSize();
	std::array<GSVector4i, CS_MAX_RECTS> rects;
	u32 nrects = 0;
	if (!FeedbackDrawRects(config, size, &rects, &nrects))
		return false;

	const WGPUComputePipeline pipeline = GetTFXComputePipeline(config.ps);
	ComputeScratch* const sc = pipeline ? GetComputeScratch(size) : nullptr;
	if (!sc || !m_uniform_buffer.ReserveMemory(CS_PARAMS_SLOT, UNIFORM_SLOT))
		return false;

	CSParams params = {};
	for (u32 i = 0; i < nrects; i++)
	{
		params.rects[i][0] = rects[i].x;
		params.rects[i][1] = rects[i].y;
		params.rects[i][2] = rects[i].z;
		params.rects[i][3] = rects[i].w;
	}
	params.count = nrects;
	params.colormask = config.colormask.wrgba;
	const u32 params_offset = m_uniform_buffer.GetCurrentOffset();
	std::memcpy(m_uniform_buffer.GetCurrentHostPointer(), &params, sizeof(params));
	m_uniform_buffer.CommitMemory(CS_PARAMS_SLOT);

	// A run on another target, or none: start one with the target's contents in scratch texture 0.
	if (m_cs_rt != rt || m_cs_run != sc)
	{
		FlushComputeRun();
		EndRenderPass();
		PrepareForSampling(rt);
		m_cs_encoding = true;
		WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		src.texture = rt->GetTexture();
		WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
		dst.texture = sc->tex[0];
		const WGPUExtent3D extent = {static_cast<u32>(size.x), static_cast<u32>(size.y), 1};
		wgpuCommandEncoderCopyTextureToTexture(GetCommandEncoder(), &src, &dst, &extent);
		WGPUComputePassDescriptor cd = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
		m_cs_pass = wgpuCommandEncoderBeginComputePass(GetCommandEncoder(), &cd);
		m_cs_encoding = false;
		m_cs_rt = rt;
		m_cs_run = sc;
		m_cs_current = 0;
		m_cs_pass_pipeline = nullptr;
		m_cs_runs++;
		g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	}

	if (m_cs_pass_pipeline != pipeline)
	{
		wgpuComputePassEncoderSetPipeline(m_cs_pass, pipeline);
		m_cs_pass_pipeline = pipeline;
	}
	const u32 ps_offset = m_ps_cb_offset;
	wgpuComputePassEncoderSetBindGroup(m_cs_pass, 0, m_cs_group0, 1, &ps_offset);
	wgpuComputePassEncoderSetBindGroup(m_cs_pass, 1, m_cs_group1, 0, nullptr);
	wgpuComputePassEncoderSetBindGroup(m_cs_pass, 2, sc->read_group[m_cs_current], 0, nullptr);
	wgpuComputePassEncoderSetBindGroup(m_cs_pass, 3, sc->write_group[m_cs_current ^ 1], 1, &params_offset);
	wgpuComputePassEncoderDispatchWorkgroups(m_cs_pass, (static_cast<u32>(size.x) + 7) / 8, (static_cast<u32>(size.y) + 7) / 8, 1);
	m_cs_current ^= 1;
	m_cs_draws++;
	rt->SetUseSerial(m_encoder_serial);
	rt->SetState(GSTexture::State::Dirty);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	return true;
}

void GSDeviceWebGPU::FlushComputeRun()
{
	if (!m_cs_run)
		return;
	m_cs_encoding = true;
	wgpuComputePassEncoderEnd(m_cs_pass);
	wgpuComputePassEncoderRelease(m_cs_pass);
	m_cs_pass = nullptr;
	WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	src.texture = m_cs_run->tex[m_cs_current];
	WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	dst.texture = m_cs_rt->GetTexture();
	const WGPUExtent3D extent = {static_cast<u32>(m_cs_run->size.x), static_cast<u32>(m_cs_run->size.y), 1};
	wgpuCommandEncoderCopyTextureToTexture(GetCommandEncoder(), &src, &dst, &extent);
	m_cs_rt->SetUseSerial(m_encoder_serial);
	m_cs_rt->SetState(GSTexture::State::Dirty);
	m_cs_encoding = false;
	m_cs_run = nullptr;
	m_cs_rt = nullptr;
	m_cs_pass_pipeline = nullptr;
	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
}
