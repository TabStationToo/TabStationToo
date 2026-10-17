// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/WebGPU/GSDeviceWebGPU.h"
#include "GS/Renderers/WebGPU/WebGPUHost.h"
#include "GS/GSPerfMon.h"
#include "GS/GSUtil.h"

#include "common/Assertions.h"
#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/HashCombine.h"
#include "common/StringUtil.h"
#include "common/Threading.h"

#include "Host.h"
#include "ImGui/ImGuiManager.h"
#include "PerformanceMetrics.h"

#include "fmt/format.h"
#include "imgui.h"

#include <emscripten/console.h>

#include <algorithm>
#include <cstring>
#include <utility>

// The device is created through this factory so that GS.cpp does not include WebGPU headers.
GSDevice* MakeGSDeviceWebGPU()
{
	return new GSDeviceWebGPU();
}

namespace
{
	// Utility uniforms: one 256-byte slot (minUniformBufferOffsetAlignment) per draw, of which the
	// shaders read at most 96 bytes (CONVERT_PUSH_CONSTANTS_SIZE, the present constants).
	constexpr u32 UNIFORM_SLOT_SIZE = 256;
	constexpr u32 UNIFORM_BINDING_SIZE = 96;

	constexpr u32 VERTEX_BUFFER_SIZE = 4 * 1024 * 1024;
	constexpr u32 INDEX_BUFFER_SIZE = 1 * 1024 * 1024;
	constexpr u32 UNIFORM_BUFFER_SIZE = 1 * 1024 * 1024;
	constexpr u32 UPLOAD_BUFFER_SIZE = 16 * 1024 * 1024;

	// Uploads at least this big go through mapped staging buffers (UploadTextureStaged()): Chrome's
	// queue.writeTexture()/writeBuffer() copy at about 1.5 GB/s here, a mapped range fills at memcpy
	// speed. A buffer is mapped again after the submit that copied from it (WebGPUHost::StagingMap());
	// six cover two uploads a frame (both read circuits) at 60 frames per second while maps complete
	// a frame or two later.
	constexpr u32 STAGING_MIN_UPLOAD = 256 * 1024;
	constexpr u32 STAGING_GRANULARITY = 1024 * 1024;
	constexpr size_t MAX_STAGING_BUFFERS = 6;

	// requestAdapter can answer null while the GPU process starts or restarts (§4 step 2): four tries
	// over 0.75 s. Without any adapter (a blocklisted GPU) that is the boot delay before the 2D path.
	constexpr u32 ADAPTER_ATTEMPTS = 4;
	constexpr double ADAPTER_RETRY_MS = 250.0;
	constexpr double REQUEST_TIMEOUT_MS = 5000.0;
	constexpr double PIPELINE_TIMEOUT_MS = 10000.0;
	constexpr u32 MAX_LOGGED_ERRORS = 10;

	// The largest side the device presents at. The page asks for its canvas's size in device pixels;
	// beyond 4096 (more than a 4K screen) a bigger frame shows nothing more of the SW renderer's
	// output, which is at most 1024 pixels wide, and costs a texture of up to 800 MB a frame (a
	// 16384-pixel canvas): the page scales the frame to the canvas instead.
	constexpr u32 MAX_SURFACE_SIZE = 4096;

	// State the WebGPU callbacks write. Callbacks can arrive after the device object is gone (an
	// error event already queued in the worker), so they never touch the device: they check the
	// generation they were registered with and only set flags here. They run on the GS thread
	// (spontaneously, while it is suspended, or synchronously inside wgpuDeviceDestroy).
	struct CallbackState
	{
		u32 generation = 0;
		bool destroying = false;
		bool lost = false;
		double lost_at = 0.0;
		u32 error_count = 0;
		u32 errors_reported = 0;
		std::string errors[MAX_LOGGED_ERRORS];
		bool in_bounded_turn = false; // inside KeepEventLoopTurning()'s suspending sleep
		u32 bounded_turns = 0;
	};
	CallbackState s_cb;

	void* GenerationToUserdata(u32 generation)
	{
		return reinterpret_cast<void*>(static_cast<uintptr_t>(generation));
	}

	bool IsCurrentGeneration(void* userdata)
	{
		return static_cast<u32>(reinterpret_cast<uintptr_t>(userdata)) == s_cb.generation;
	}

	void OnDeviceLost(WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message, void* userdata1, void*)
	{
		if (!IsCurrentGeneration(userdata1) || (s_cb.destroying && reason == WGPUDeviceLostReason_Destroyed))
			return;

		s_cb.lost = true;
		s_cb.lost_at = WebGPUHost::Now();
		const std::string text = fmt::format("WebGPU: device lost (reason {}, noticed {}): {}", static_cast<int>(reason),
			s_cb.in_bounded_turn ? "in the bounded event-loop turn" : "while the GS thread waited or destroyed it",
			WebGPUHost::StringViewToString(message));
		emscripten_console_error(text.c_str());
	}

	void OnUncapturedError(WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void* userdata1, void*)
	{
		if (!IsCurrentGeneration(userdata1))
			return;

		const u32 index = s_cb.error_count++;
		if (index < MAX_LOGGED_ERRORS)
		{
			s_cb.errors[index] = fmt::format("WebGPU: uncaptured error (type {}): {}", static_cast<int>(type),
				WebGPUHost::StringViewToString(message));
			emscripten_console_error(s_cb.errors[index].c_str());
		}
	}

	WGPUStringView SV(const char* s)
	{
		return {s, WGPU_STRLEN};
	}
} // namespace

namespace
{
	const char* const s_module_files[] = {
		"shaders/webgpu/convert.wgsl",
		"shaders/webgpu/present.wgsl",
		"shaders/webgpu/merge.wgsl",
		"shaders/webgpu/interlace.wgsl",
		"shaders/webgpu/shadeboost.wgsl",
		"shaders/webgpu/imgui.wgsl",
	};
} // namespace

// ----------------------------------------------------------------------------
// Pipeline keys
// ----------------------------------------------------------------------------

bool GSDeviceWebGPU::UtilityPipelineKey::operator==(const UtilityPipelineKey& rhs) const
{
	return module == rhs.module && layout == rhs.layout && write_mask == rhs.write_mask && blend == rhs.blend &&
		   entry == rhs.entry && constants == rhs.constants && color_format == rhs.color_format;
}

size_t GSDeviceWebGPU::UtilityPipelineKeyHash::operator()(const UtilityPipelineKey& k) const
{
	size_t h = 0;
	HashCombine(h, k.module, k.layout, k.write_mask, k.blend, k.entry, k.constants, static_cast<u32>(k.color_format));
	return h;
}

u16 GSDeviceWebGPU::EntryIndex(const char* name)
{
	for (size_t i = 0; i < m_entry_names.size(); i++)
	{
		if (m_entry_names[i] == name)
			return static_cast<u16>(i);
	}
	m_entry_names.emplace_back(name);
	return static_cast<u16>(m_entry_names.size() - 1);
}

GSDeviceWebGPU::UtilityPipelineKey GSDeviceWebGPU::MakeKey(UtilityModule module, const char* entry, WGPUTextureFormat format,
	TexLayout layout, u8 write_mask, UtilityBlend blend, u16 constants)
{
	UtilityPipelineKey key = {};
	key.module = module;
	key.layout = static_cast<u8>(layout);
	key.write_mask = write_mask;
	key.blend = blend;
	key.entry = EntryIndex(entry);
	key.constants = constants;
	key.color_format = format;
	return key;
}

GSDeviceWebGPU::UtilityPipelineKey GSDeviceWebGPU::ConvertPipelineKey(ShaderConvertSelector shader, WGPUTextureFormat format)
{
	// Step 1 draws every convert shader with a colour, integer or r32float target. Depth and stencil
	// outputs (frag_depth, DATM) come with the HW renderer (step 2).
	if (shader.DepthOutput() || shader.StencilOutput())
		return MakeKey(MODULE_COUNT, "", WGPUTextureFormat_Undefined);

	return MakeKey(MODULE_CONVERT, shader.EntryPoint(), format,
		shader.Float32Input() ? TexLayout::Unfilterable : TexLayout::Float, shader.Mask(), BLEND_NONE,
		(shader.SupportsBilinear() && shader.Biln()) ? 1 : 0);
}

// ----------------------------------------------------------------------------
// Creation
// ----------------------------------------------------------------------------

GSDeviceWebGPU::GSDeviceWebGPU() = default;

GSDeviceWebGPU::~GSDeviceWebGPU() = default;

RenderAPI GSDeviceWebGPU::GetRenderAPI() const
{
	return RenderAPI::WebGPU;
}

void GSDeviceWebGPU::MarkDeviceLost(const char* what)
{
	// A wait for the GPU that timed out is treated as a device loss (§11.4, §13): the device is
	// recreated at the next present instead of the GS waiting for a GPU that does not answer.
	if (s_cb.lost)
		return;
	s_cb.lost = true;
	s_cb.lost_at = WebGPUHost::Now();
	Console.ErrorFmt("WebGPU: {} timed out; treating it as a device loss.", what);
}

bool GSDeviceWebGPU::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	if (!GSDevice::Create(vsync_mode, allow_present_throttle))
		return false;

	const double t0 = WebGPUHost::Now();
	s_cb.generation++;
	s_cb.destroying = false;
	s_cb.lost = false;
	s_cb.error_count = 0;
	s_cb.errors_reported = 0;

	if (!AcquireWindow(true))
		return false;

	if (!CreateInstanceAndDevice())
		return false;
	const double t_device = WebGPUHost::Now();

	if (!CheckFeatures())
		return false;

	// Everything below must validate; a validation error fails Create() (§4 step 6).
	wgpuDevicePushErrorScope(m_device, WGPUErrorFilter_Validation);
	bool ok = CreateBuffers() && CreateLayoutsAndSamplers() && LoadShaderModules() && CreateSurfaceForWindow();
	const double t_setup = WebGPUHost::Now();
	ok = ok && CompileUtilityPipelines();
	const double t_pipelines = WebGPUHost::Now();

	struct ScopeResult
	{
		WGPUErrorType type = WGPUErrorType_NoError;
		std::string message;
	};
	using ScopeRequest = WebGPUHost::RequestResult<ScopeResult>;
	ScopeRequest* const scope_req = new ScopeRequest();
	WGPUPopErrorScopeCallbackInfo pop = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
	pop.mode = WGPUCallbackMode_WaitAnyOnly;
	pop.userdata1 = scope_req;
	pop.callback = [](WGPUPopErrorScopeStatus, WGPUErrorType type, WGPUStringView message, void* u1, void*) {
		if (ScopeRequest* r = ScopeRequest::Arrive(u1))
			r->value = {type, WebGPUHost::StringViewToString(message)};
	};
	WebGPUHost::Wait(m_instance, wgpuDevicePopErrorScope(m_device, pop), REQUEST_TIMEOUT_MS);
	ScopeResult scope;
	if (!scope_req->Take(&scope))
	{
		Console.Error("WebGPU: timed out waiting for the device setup's validation result.");
		ok = false;
	}
	else if (scope.type != WGPUErrorType_NoError)
	{
		Console.ErrorFmt("WebGPU: device setup failed validation: {}", scope.message);
		ok = false;
	}

	if (!ok)
		return false;

	// While this device exists, the GS thread's idle wait suspends instead of blocking, so its
	// worker keeps delivering WebGPU callbacks (device loss, errors) when it has no work (§3.2).
	// A test switch keeps it blocking, leaving only the bounded event-loop turn in EndPresent().
	Threading::SetWaitsSuspend(WebGPUHost::IdleWaitsSuspend());
	m_last_event_loop_turn = WebGPUHost::Now();
	WebGPUHost::NoteDeviceCreated();

	const double t_ready = WebGPUHost::Now();
	Console.WriteLnFmt("WebGPU: device ready in {:.0f} ms (adapter and device {:.0f} ms, resources {:.0f} ms, "
					   "{} utility pipelines {:.0f} ms); surface {}x{} format {}",
		t_ready - t0, t_device - t0, t_setup - t_device, m_pipelines.size(), t_pipelines - t_setup, m_surface_width,
		m_surface_height, static_cast<int>(m_surface_format));
	if (s_cb.lost_at != 0.0)
	{
		Console.WriteLnFmt("WebGPU: recovered {:.0f} ms after the device was lost.", t_ready - s_cb.lost_at);
		s_cb.lost_at = 0.0;
	}
	return true;
}

bool GSDeviceWebGPU::CreateInstanceAndDevice()
{
	static const WGPUInstanceFeatureName instance_features[] = {WGPUInstanceFeatureName_TimedWaitAny};
	WGPUInstanceDescriptor idesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
	idesc.requiredFeatureCount = std::size(instance_features);
	idesc.requiredFeatures = instance_features;
	m_instance = wgpuCreateInstance(&idesc);
	if (!m_instance)
	{
		Console.Error("WebGPU: wgpuCreateInstance() failed (needs JSPI for timed waits).");
		return false;
	}

	// Results of the requests below; the callbacks release what they get after a timed-out wait.
	struct AdapterResult
	{
		WGPUAdapter adapter = nullptr;
		std::string message;
	};
	struct DeviceResult
	{
		WGPUDevice device = nullptr;
		std::string message;
	};
	using AdapterRequest = WebGPUHost::RequestResult<AdapterResult>;
	using DeviceRequest = WebGPUHost::RequestResult<DeviceResult>;

	// A fresh browser profile can answer null while its GPU process starts, and so can a GPU process
	// restarting after a crash: retry (§4 step 2).
	for (u32 attempt = 1; attempt <= ADAPTER_ATTEMPTS && !m_adapter; attempt++)
	{
		AdapterRequest* const req = new AdapterRequest();
		WGPURequestAdapterOptions opts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
		opts.featureLevel = WGPUFeatureLevel_Core;
		opts.powerPreference = WGPUPowerPreference_HighPerformance;
		WGPURequestAdapterCallbackInfo cb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
		cb.mode = WGPUCallbackMode_WaitAnyOnly;
		cb.userdata1 = req;
		cb.callback = [](WGPURequestAdapterStatus, WGPUAdapter adapter, WGPUStringView message, void* u1, void*) {
			if (AdapterRequest* r = AdapterRequest::Arrive(u1))
				r->value = {adapter, WebGPUHost::StringViewToString(message)};
			else if (adapter)
				wgpuAdapterRelease(adapter);
		};
		const double t0 = WebGPUHost::Now();
		const WGPUWaitStatus ws = WebGPUHost::Wait(m_instance, wgpuInstanceRequestAdapter(m_instance, &opts, cb), REQUEST_TIMEOUT_MS);
		AdapterResult res;
		req->Take(&res);
		if (res.adapter)
		{
			m_adapter = res.adapter;
			break;
		}

		Console.WarningFmt("WebGPU: requestAdapter attempt {} gave no adapter after {:.0f} ms (wait {}): {}", attempt,
			WebGPUHost::Now() - t0, static_cast<int>(ws), res.message);
		if (attempt < ADAPTER_ATTEMPTS)
			WebGPUHost::SleepSuspending(ADAPTER_RETRY_MS);
	}
	if (!m_adapter)
		return false;

	WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
	wgpuAdapterGetInfo(m_adapter, &info);
	m_adapter_name = fmt::format("{} {}", WebGPUHost::StringViewToString(info.vendor),
		WebGPUHost::StringViewToString(info.architecture));
	m_driver_info = fmt::format("WebGPU adapter: vendor \"{}\", architecture \"{}\", device \"{}\", description \"{}\", "
								"backend {}, type {}",
		WebGPUHost::StringViewToString(info.vendor), WebGPUHost::StringViewToString(info.architecture),
		WebGPUHost::StringViewToString(info.device), WebGPUHost::StringViewToString(info.description),
		static_cast<int>(info.backendType), static_cast<int>(info.adapterType));
	wgpuAdapterInfoFreeMembers(info);
	m_name = m_adapter_name;

	WGPULimits adapter_limits = WGPU_LIMITS_INIT;
	wgpuAdapterGetLimits(m_adapter, &adapter_limits);

	// Optional features (§4); none is required by step 1.
	static const WGPUFeatureName wanted[] = {WGPUFeatureName_Depth32FloatStencil8, WGPUFeatureName_TextureCompressionBC,
		WGPUFeatureName_Float32Filterable, WGPUFeatureName_Float32Blendable, WGPUFeatureName_DualSourceBlending,
		WGPUFeatureName_PrimitiveIndex, WGPUFeatureName_TextureFormatsTier1};
	std::vector<WGPUFeatureName> features;
	for (const WGPUFeatureName f : wanted)
	{
		if (wgpuAdapterHasFeature(m_adapter, f))
			features.push_back(f);
	}

	WGPULimits limits = WGPU_LIMITS_INIT;
	limits.maxTextureDimension2D = adapter_limits.maxTextureDimension2D;
	limits.maxBufferSize = adapter_limits.maxBufferSize;
	limits.maxStorageBufferBindingSize = adapter_limits.maxStorageBufferBindingSize;

	DeviceRequest* const req = new DeviceRequest();
	WGPUDeviceDescriptor ddesc = WGPU_DEVICE_DESCRIPTOR_INIT;
	ddesc.label = SV("GSDeviceWebGPU");
	ddesc.requiredFeatureCount = features.size();
	ddesc.requiredFeatures = features.data();
	ddesc.requiredLimits = &limits;
	ddesc.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
	ddesc.deviceLostCallbackInfo.callback = OnDeviceLost;
	ddesc.deviceLostCallbackInfo.userdata1 = GenerationToUserdata(s_cb.generation);
	ddesc.uncapturedErrorCallbackInfo.callback = OnUncapturedError;
	ddesc.uncapturedErrorCallbackInfo.userdata1 = GenerationToUserdata(s_cb.generation);
	WGPURequestDeviceCallbackInfo cb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
	cb.mode = WGPUCallbackMode_WaitAnyOnly;
	cb.userdata1 = req;
	cb.callback = [](WGPURequestDeviceStatus, WGPUDevice device, WGPUStringView message, void* u1, void*) {
		if (DeviceRequest* r = DeviceRequest::Arrive(u1))
			r->value = {device, WebGPUHost::StringViewToString(message)};
		else if (device)
			wgpuDeviceRelease(device);
	};
	WebGPUHost::Wait(m_instance, wgpuAdapterRequestDevice(m_adapter, &ddesc, cb), REQUEST_TIMEOUT_MS);
	DeviceResult res;
	if (!req->Take(&res))
		res.message = "timed out";
	if (!res.device)
	{
		Console.ErrorFmt("WebGPU: requestDevice failed: {}", res.message);
		return false;
	}

	m_device = res.device;
	m_queue = wgpuDeviceGetQueue(m_device);
	m_has_depth32_stencil8 = wgpuDeviceHasFeature(m_device, WGPUFeatureName_Depth32FloatStencil8);
	m_has_bc = wgpuDeviceHasFeature(m_device, WGPUFeatureName_TextureCompressionBC);
	m_has_tier1 = wgpuDeviceHasFeature(m_device, WGPUFeatureName_TextureFormatsTier1);

	WGPULimits device_limits = WGPU_LIMITS_INIT;
	wgpuDeviceGetLimits(m_device, &device_limits);
	m_max_texture_size = device_limits.maxTextureDimension2D;
	m_driver_info += fmt::format("\nmaxTextureDimension2D {}, features:", m_max_texture_size);
	for (const WGPUFeatureName f : features)
		m_driver_info += fmt::format(" {}", static_cast<int>(f));
	return true;
}

bool GSDeviceWebGPU::CheckFeatures()
{
	// Step 1 drives the SW renderer only: no HW-renderer features (§4, §15 part 6).
	m_features = FeatureSupport();
	m_features.prefer_new_textures = true;

	// Every format the utility passes render to must be renderable and sampleable; the core
	// formats used here always are (spec), the optional ones are resolved in LookupNativeFormat().
	if (LookupNativeFormat(GSTexture::Format::Color) == WGPUTextureFormat_Undefined)
		return false;

	return true;
}

WGPUTextureFormat GSDeviceWebGPU::LookupNativeFormat(GSTexture::Format format) const
{
	switch (format)
	{
		case GSTexture::Format::Color:
			return WGPUTextureFormat_RGBA8Unorm;
		case GSTexture::Format::ColorHQ:
			return WGPUTextureFormat_RGB10A2Unorm;
		case GSTexture::Format::ColorHDR:
			return WGPUTextureFormat_RGBA16Float;
		case GSTexture::Format::ColorClip:
			return m_has_tier1 ? WGPUTextureFormat_RGBA16Unorm : WGPUTextureFormat_RGBA16Float;
		case GSTexture::Format::DepthStencil:
			return m_has_depth32_stencil8 ? WGPUTextureFormat_Depth32FloatStencil8 : WGPUTextureFormat_Depth32Float;
		case GSTexture::Format::DepthColor:
		case GSTexture::Format::PrimID:
			return WGPUTextureFormat_R32Float;
		case GSTexture::Format::UNorm8:
			return WGPUTextureFormat_R8Unorm;
		case GSTexture::Format::UInt16:
			return WGPUTextureFormat_R16Uint;
		case GSTexture::Format::UInt32:
			return WGPUTextureFormat_R32Uint;
		case GSTexture::Format::BC1:
			return m_has_bc ? WGPUTextureFormat_BC1RGBAUnorm : WGPUTextureFormat_Undefined;
		case GSTexture::Format::BC2:
			return m_has_bc ? WGPUTextureFormat_BC2RGBAUnorm : WGPUTextureFormat_Undefined;
		case GSTexture::Format::BC3:
			return m_has_bc ? WGPUTextureFormat_BC3RGBAUnorm : WGPUTextureFormat_Undefined;
		case GSTexture::Format::BC7:
			return m_has_bc ? WGPUTextureFormat_BC7RGBAUnorm : WGPUTextureFormat_Undefined;
		default:
			return WGPUTextureFormat_Undefined;
	}
}

bool GSDeviceWebGPU::CreateBuffers()
{
	if (!m_vertex_buffer.Create(m_device, WGPUBufferUsage_Vertex, VERTEX_BUFFER_SIZE, "vertex stream buffer") ||
		!m_index_buffer.Create(m_device, WGPUBufferUsage_Index, INDEX_BUFFER_SIZE, "index stream buffer") ||
		!m_uniform_buffer.Create(m_device, WGPUBufferUsage_Uniform, UNIFORM_BUFFER_SIZE, "uniform stream buffer") ||
		!m_upload_buffer.Create(m_device, WGPUBufferUsage_CopySrc, UPLOAD_BUFFER_SIZE, "texture upload buffer"))
	{
		return false;
	}

	return true;
}

bool GSDeviceWebGPU::CreateLayoutsAndSamplers()
{
	// §8.2: group 0 the per-draw constants (dynamic offset), group 1 the source texture and sampler.
	{
		WGPUBindGroupLayoutEntry e = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
		e.binding = 0;
		e.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
		e.buffer.type = WGPUBufferBindingType_Uniform;
		e.buffer.hasDynamicOffset = true;
		e.buffer.minBindingSize = UNIFORM_BINDING_SIZE;
		WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		d.label = SV("utility constants");
		d.entryCount = 1;
		d.entries = &e;
		m_utility_cb_bgl = wgpuDeviceCreateBindGroupLayout(m_device, &d);
	}
	for (u32 i = 0; i < static_cast<u32>(TexLayout::Count); i++)
	{
		const bool unfilterable = (i == static_cast<u32>(TexLayout::Unfilterable));
		WGPUBindGroupLayoutEntry e[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
		e[0].binding = 0;
		e[0].visibility = WGPUShaderStage_Fragment;
		e[0].texture.sampleType = unfilterable ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
		e[0].texture.viewDimension = WGPUTextureViewDimension_2D;
		e[1].binding = 1;
		e[1].visibility = WGPUShaderStage_Fragment;
		e[1].sampler.type = unfilterable ? WGPUSamplerBindingType_NonFiltering : WGPUSamplerBindingType_Filtering;
		WGPUBindGroupLayoutDescriptor d = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
		d.label = SV(unfilterable ? "utility texture (unfilterable)" : "utility texture");
		d.entryCount = 2;
		d.entries = e;
		m_utility_tex_bgl[i] = wgpuDeviceCreateBindGroupLayout(m_device, &d);

		const WGPUBindGroupLayout groups[2] = {m_utility_cb_bgl, m_utility_tex_bgl[i]};
		WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
		pd.label = d.label;
		pd.bindGroupLayoutCount = 2;
		pd.bindGroupLayouts = groups;
		m_utility_layout[i] = wgpuDeviceCreatePipelineLayout(m_device, &pd);
	}

	{
		WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
		e.binding = 0;
		e.buffer = m_uniform_buffer.GetBuffer();
		e.offset = 0;
		e.size = UNIFORM_BINDING_SIZE;
		WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		d.label = SV("utility constants");
		d.layout = m_utility_cb_bgl;
		d.entryCount = 1;
		d.entries = &e;
		m_utility_cb_bg = wgpuDeviceCreateBindGroup(m_device, &d);
	}

	m_samplers.SetDevice(m_device);
	m_point_sampler = m_samplers.Get(GSHWDrawConfig::SamplerSelector::Point());
	m_linear_sampler = m_samplers.Get(GSHWDrawConfig::SamplerSelector::Linear());
	return (m_utility_cb_bgl && m_utility_cb_bg && m_point_sampler && m_linear_sampler);
}

WGPUShaderModule GSDeviceWebGPU::CreateShaderModule(const char* name, const std::string& source)
{
	WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
	wgsl.code = {source.data(), source.size()};
	WGPUShaderModuleDescriptor d = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
	d.nextInChain = &wgsl.chain;
	d.label = SV(name);
	return wgpuDeviceCreateShaderModule(m_device, &d);
}

bool GSDeviceWebGPU::LoadShaderModules()
{
	// The shaders are preloaded at /resources/shaders/webgpu/ (pcsx2-wasm/CMakeLists.txt).
	for (u32 i = 0; i < MODULE_COUNT; i++)
	{
		const std::optional<std::string> source = ReadShaderSource(s_module_files[i]);
		if (!source.has_value())
		{
			Console.ErrorFmt("WebGPU: failed to read {}", s_module_files[i]);
			return false;
		}

		m_modules[i] = CreateShaderModule(s_module_files[i], *source);
		if (!m_modules[i])
			return false;
	}

	return true;
}

GSDeviceWebGPU::PendingPipeline GSDeviceWebGPU::CreatePipelineAsync(const UtilityPipelineKey& key)
{
	const bool imgui = (key.module == MODULE_IMGUI);

	static constexpr WGPUVertexAttribute pt1_attrs[] = {
		{nullptr, WGPUVertexFormat_Float32x4, 0, 0},
		{nullptr, WGPUVertexFormat_Float32x2, 16, 1},
	};
	static constexpr WGPUVertexAttribute imgui_attrs[] = {
		{nullptr, WGPUVertexFormat_Float32x2, 0, 0},
		{nullptr, WGPUVertexFormat_Float32x2, 8, 1},
		{nullptr, WGPUVertexFormat_Unorm8x4, 16, 2},
	};
	WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
	vbl.stepMode = WGPUVertexStepMode_Vertex;
	vbl.arrayStride = imgui ? sizeof(ImDrawVert) : sizeof(GSVertexPT1);
	vbl.attributeCount = imgui ? std::size(imgui_attrs) : std::size(pt1_attrs);
	vbl.attributes = imgui ? imgui_attrs : pt1_attrs;

	WGPUBlendState blend = {};
	blend.color.operation = WGPUBlendOperation_Add;
	blend.color.srcFactor = WGPUBlendFactor_SrcAlpha;
	blend.color.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
	blend.alpha.operation = WGPUBlendOperation_Add;
	blend.alpha.srcFactor = WGPUBlendFactor_One;
	blend.alpha.dstFactor = WGPUBlendFactor_Zero;

	WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
	target.format = key.color_format;
	target.writeMask = key.write_mask;
	target.blend = (key.blend != BLEND_NONE) ? &blend : nullptr;

	WGPUConstantEntry constant = WGPU_CONSTANT_ENTRY_INIT;
	constant.key = SV("HAS_BILN");
	constant.value = 1.0;

	WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
	fs.module = m_modules[key.module];
	fs.entryPoint = SV(m_entry_names[key.entry].c_str());
	fs.targetCount = 1;
	fs.targets = &target;
	fs.constantCount = (key.constants & 1) ? 1 : 0;
	fs.constants = &constant;

	WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
	const std::string label = fmt::format("{} {} fmt={} mask={:x}", s_module_files[key.module], m_entry_names[key.entry],
		static_cast<int>(key.color_format), key.write_mask);
	d.label = {label.data(), label.size()};
	d.layout = m_utility_layout[key.layout];
	d.vertex.module = m_modules[key.module];
	d.vertex.entryPoint = SV("vs_main");
	d.vertex.bufferCount = 1;
	d.vertex.buffers = &vbl;
	d.primitive.topology = imgui ? WGPUPrimitiveTopology_TriangleList : WGPUPrimitiveTopology_TriangleStrip;
	d.primitive.cullMode = WGPUCullMode_None;
	d.fragment = &fs;

	PipelineRequest* const req = new PipelineRequest();
	WGPUCreateRenderPipelineAsyncCallbackInfo cb = WGPU_CREATE_RENDER_PIPELINE_ASYNC_CALLBACK_INFO_INIT;
	cb.mode = WGPUCallbackMode_WaitAnyOnly;
	cb.userdata1 = req;
	cb.callback = [](WGPUCreatePipelineAsyncStatus status, WGPURenderPipeline pipeline, WGPUStringView message, void* u1, void*) {
		PipelineRequest* const r = PipelineRequest::Arrive(u1);
		if (status != WGPUCreatePipelineAsyncStatus_Success || !r)
		{
			if (r)
				Console.ErrorFmt("WebGPU: pipeline creation failed: {}", WebGPUHost::StringViewToString(message));
			if (pipeline)
				wgpuRenderPipelineRelease(pipeline);
			pipeline = nullptr;
		}
		if (r)
			r->value = pipeline;
	};
	return {key, wgpuDeviceCreateRenderPipelineAsync(m_device, &d, cb), req};
}

bool GSDeviceWebGPU::WaitForPipelines(std::vector<PendingPipeline>& pending, double timeout_ms)
{
	// A finished future returns at once from WaitAny, so waiting on each in turn waits for them all.
	const double deadline = WebGPUHost::Now() + timeout_ms;
	bool ok = true;
	for (PendingPipeline& p : pending)
	{
		const double left = std::max(deadline - WebGPUHost::Now(), 1.0);
		const WGPUWaitStatus ws = WebGPUHost::Wait(m_instance, p.future, left);
		WGPURenderPipeline pipeline = nullptr;
		if (!p.request->Take(&pipeline))
		{
			Console.ErrorFmt("WebGPU: timed out waiting for the {} pipeline (wait {})", m_entry_names[p.key.entry],
				static_cast<int>(ws));
			ok = false;
			continue;
		}
		m_pipelines[p.key] = pipeline;
		ok = ok && (pipeline != nullptr);
	}
	return ok;
}

WGPURenderPipeline GSDeviceWebGPU::GetUtilityPipeline(const UtilityPipelineKey& key)
{
	if (key.module >= MODULE_COUNT)
		return nullptr;

	const auto it = m_pipelines.find(key);
	if (it != m_pipelines.end())
		return it->second;

	// Not compiled in Create() (another target format): create it now and wait (§8.4).
	std::vector<PendingPipeline> pending{CreatePipelineAsync(key)};
	if (!WaitForPipelines(pending, PIPELINE_TIMEOUT_MS))
	{
		const auto created = m_pipelines.find(key);
		if (created == m_pipelines.end())
			MarkDeviceLost("a pipeline creation");
		return (created != m_pipelines.end()) ? created->second : nullptr;
	}
	return m_pipelines[key];
}

bool GSDeviceWebGPU::CompileUtilityPipelines()
{
	// All pipelines of the utility shaders that step 1 can draw, created together and waited for
	// together (§9.4). They are stored in m_pipelines as their futures complete.
	std::vector<UtilityPipelineKey> keys;
	const WGPUTextureFormat color = LookupNativeFormat(GSTexture::Format::Color);

	for (u32 i = 0; i < ShaderConvertSelector::NUM_TOTAL_SHADERS; i++)
	{
		const ShaderConvertSelector shader = ShaderConvertSelector::Get(i);
		const UtilityPipelineKey key = ConvertPipelineKey(shader, LookupNativeFormat(shader.OutputFormat()));
		if (key.module < MODULE_COUNT && key.color_format != WGPUTextureFormat_Undefined)
			keys.push_back(key);
	}
	for (u32 i = 0; i < static_cast<u32>(PresentShader::Count); i++)
	{
		const char* entry = ShaderEntryPoint(static_cast<PresentShader>(i));
		keys.push_back(MakeKey(MODULE_PRESENT, entry, m_surface_format));
		if (m_surface_format != color)
			keys.push_back(MakeKey(MODULE_PRESENT, entry, color));
	}
	for (u32 i = 0; i < 2; i++)
		keys.push_back(MakeKey(MODULE_MERGE, i ? "ps_main1" : "ps_main0", color, TexLayout::Float, 0xf, BLEND_MERGE));
	for (u32 i = 0; i < NUM_INTERLACE_SHADERS; i++)
		keys.push_back(MakeKey(MODULE_INTERLACE, fmt::format("ps_main{}", i).c_str(), color));
	keys.push_back(MakeKey(MODULE_SHADEBOOST, "ps_main", color));
	keys.push_back(MakeKey(MODULE_IMGUI, "ps_main", m_surface_format, TexLayout::Float, 0xf, BLEND_IMGUI));

	// Keys can repeat (selectors that share an entry point and target); create each once.
	std::sort(keys.begin(), keys.end(), [](const UtilityPipelineKey& a, const UtilityPipelineKey& b) {
		return std::memcmp(&a, &b, sizeof(a)) < 0;
	});
	keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

	std::vector<PendingPipeline> pending;
	pending.reserve(keys.size());
	for (const UtilityPipelineKey& key : keys)
		pending.push_back(CreatePipelineAsync(key));

	return WaitForPipelines(pending, PIPELINE_TIMEOUT_MS);
}

void GSDeviceWebGPU::ClampSurfaceSize(u32* width, u32* height) const
{
	// A canvas bigger than the largest texture cannot be presented at its size, and one bigger than
	// MAX_SURFACE_SIZE is not worth it: present smaller, keeping the aspect ratio (the page scales the
	// frame to the canvas).
	*width = std::max<u32>(*width, 1);
	*height = std::max<u32>(*height, 1);
	const u32 max_size = std::clamp<u32>(m_max_texture_size, 1, MAX_SURFACE_SIZE);
	if (*width <= max_size && *height <= max_size)
		return;

	const double scale = std::min(static_cast<double>(max_size) / *width, static_cast<double>(max_size) / *height);
	*width = std::clamp<u32>(static_cast<u32>(*width * scale), 1, max_size);
	*height = std::clamp<u32>(static_cast<u32>(*height * scale), 1, max_size);
}

bool GSDeviceWebGPU::CreateSurfaceForWindow()
{
	u32 width = m_window_info.surface_width;
	u32 height = m_window_info.surface_height;
	ClampSurfaceSize(&width, &height);
	m_window_info.surface_width = width;
	m_window_info.surface_height = height;
	if (!WebGPUHost::CreateCanvas(width, height))
	{
		Console.Error("WebGPU: this worker cannot create an OffscreenCanvas.");
		return false;
	}

	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
	canvas.selector = SV(WebGPUHost::CanvasSelector());
	WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
	sd.nextInChain = &canvas.chain;
	sd.label = SV("GS OffscreenCanvas");
	m_surface = wgpuInstanceCreateSurface(m_instance, &sd);
	if (!m_surface)
	{
		Console.Error("WebGPU: failed to create the surface on the GS OffscreenCanvas.");
		return false;
	}

	WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
	wgpuSurfaceGetCapabilities(m_surface, m_adapter, &caps);
	m_surface_format = caps.formatCount ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
	wgpuSurfaceCapabilitiesFreeMembers(caps);
	m_surface_width = width;
	m_surface_height = height;
	return ConfigureSurface();
}

bool GSDeviceWebGPU::ConfigureSurface()
{
	WGPUSurfaceConfiguration sc = WGPU_SURFACE_CONFIGURATION_INIT;
	sc.device = m_device;
	sc.format = m_surface_format;
	sc.usage = WGPUTextureUsage_RenderAttachment;
	sc.width = m_surface_width;
	sc.height = m_surface_height;
	sc.alphaMode = WGPUCompositeAlphaMode_Opaque;
	sc.presentMode = WGPUPresentMode_Fifo;
	wgpuSurfaceConfigure(m_surface, &sc);
	m_surface_configured = true;
	return true;
}

void GSDeviceWebGPU::Destroy()
{
	if (m_device)
	{
		EndRenderPass();
		m_presenting = false;
	}

	GSDevice::Destroy();

	if (m_device)
	{
		ExecuteCommandBuffer("destroy");
		DestroyResources();
	}

	if (m_surface)
	{
		if (m_surface_configured)
			wgpuSurfaceUnconfigure(m_surface);
		wgpuSurfaceRelease(m_surface);
		m_surface = nullptr;
		m_surface_configured = false;
	}
	WebGPUHost::DestroyCanvas();

	if (m_queue)
	{
		wgpuQueueRelease(m_queue);
		m_queue = nullptr;
	}
	if (m_device)
	{
		// Calls the lost callback synchronously with reason Destroyed, which it ignores now.
		s_cb.destroying = true;
		wgpuDeviceDestroy(m_device);
		// emdawnwebgpu keeps the lost future's entry (WebGPU.Internals.futures) until a timed wait on
		// it, one per device; the loss is reported by now, so this returns at once and drops it.
		if (m_instance)
			WebGPUHost::Wait(m_instance, wgpuDeviceGetLostFuture(m_device), 1.0);
		wgpuDeviceRelease(m_device);
		m_device = nullptr;
	}
	if (m_adapter)
	{
		wgpuAdapterRelease(m_adapter);
		m_adapter = nullptr;
	}
	if (m_instance)
	{
		wgpuInstanceRelease(m_instance);
		m_instance = nullptr;
	}

	// No callback of this device may touch anything from now on.
	s_cb.generation++;
	Threading::SetWaitsSuspend(false);
	if (s_cb.bounded_turns != 0)
		Console.WriteLnFmt("WebGPU: {} bounded event-loop turns so far.", s_cb.bounded_turns);
}

void GSDeviceWebGPU::DestroyResources()
{
	for (auto& it : m_pipelines)
	{
		if (it.second)
			wgpuRenderPipelineRelease(it.second);
	}
	m_pipelines.clear();

	for (WGPUShaderModule& m : m_modules)
	{
		if (m)
			wgpuShaderModuleRelease(m);
		m = nullptr;
	}

	if (m_utility_cb_bg)
		wgpuBindGroupRelease(m_utility_cb_bg);
	m_utility_cb_bg = nullptr;
	for (WGPUPipelineLayout& l : m_utility_layout)
	{
		if (l)
			wgpuPipelineLayoutRelease(l);
		l = nullptr;
	}
	for (WGPUBindGroupLayout& l : m_utility_tex_bgl)
	{
		if (l)
			wgpuBindGroupLayoutRelease(l);
		l = nullptr;
	}
	if (m_utility_cb_bgl)
		wgpuBindGroupLayoutRelease(m_utility_cb_bgl);
	m_utility_cb_bgl = nullptr;

	m_samplers.Clear();
	m_point_sampler = nullptr;
	m_linear_sampler = nullptr;

	m_vertex_buffer.Destroy();
	m_index_buffer.Destroy();
	m_uniform_buffer.Destroy();
	m_upload_buffer.Destroy();
	DestroyStagingBuffers();
}

void GSDeviceWebGPU::DestroyStagingBuffers()
{
	for (StagingBuffer& sb : m_staging)
	{
		WebGPUHost::StagingForget(sb.buffer);
		wgpuBufferDestroy(sb.buffer); // aborts a pending map
		wgpuBufferRelease(sb.buffer);
	}
	m_staging.clear();
}

void GSDeviceWebGPU::RemapStagingBuffers(u64 submitted_serial)
{
	for (StagingBuffer& sb : m_staging)
	{
		if (sb.used_serial != submitted_serial)
			continue;

		// The map completes once the submitted copy has run; the worker's event loop delivers it.
		sb.used_serial = 0;
		WebGPUHost::StagingMap(sb.buffer);
	}
}

// ----------------------------------------------------------------------------
// Window and surface
// ----------------------------------------------------------------------------

bool GSDeviceWebGPU::HasSurface() const
{
	return m_surface_configured;
}

void GSDeviceWebGPU::DestroySurface()
{
	if (m_surface && m_surface_configured)
	{
		wgpuSurfaceUnconfigure(m_surface);
		m_surface_configured = false;
	}
}

bool GSDeviceWebGPU::UpdateWindow()
{
	ExecuteCommandBuffer("UpdateWindow");
	if (m_surface)
	{
		DestroySurface();
		wgpuSurfaceRelease(m_surface);
		m_surface = nullptr;
	}

	if (!AcquireWindow(false))
		return false;

	return CreateSurfaceForWindow();
}

void GSDeviceWebGPU::ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
	m_window_info.surface_scale = new_window_scale;
	if (new_window_width == 0 || new_window_height == 0)
		return;

	ClampSurfaceSize(&new_window_width, &new_window_height);
	if (m_surface_width == new_window_width && m_surface_height == new_window_height)
		return;

	m_window_info.surface_width = new_window_width;
	m_window_info.surface_height = new_window_height;
	m_surface_width = new_window_width;
	m_surface_height = new_window_height;
	if (m_surface)
		ConfigureSurface();
}

bool GSDeviceWebGPU::SupportsExclusiveFullscreen() const
{
	return false;
}

void GSDeviceWebGPU::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	// The page shows each posted frame at its next rendering update; the frame limiter paces (§12.4).
	m_vsync_mode = mode;
	m_allow_present_throttle = allow_present_throttle;
}

std::string GSDeviceWebGPU::GetDriverInfo() const
{
	return m_driver_info;
}

bool GSDeviceWebGPU::SetGPUTimingEnabled(bool enabled)
{
	return false; // step 3: timestamp queries
}

float GSDeviceWebGPU::GetAndResetAccumulatedGPUTime()
{
	return 0.0f;
}

bool GSDeviceWebGPU::SetGPUPipelineStatisticsEnabled(bool enabled)
{
	return false; // WebGPU has no pipeline statistics
}

GPUPipelineStatistics GSDeviceWebGPU::GetAndResetAccumulatedGPUPipelineStatistics()
{
	return {};
}

void GSDeviceWebGPU::PushDebugGroup(const char* fmt, ...)
{
}

void GSDeviceWebGPU::PopDebugGroup()
{
}

void GSDeviceWebGPU::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
}

// ----------------------------------------------------------------------------
// Encoder, passes and state
// ----------------------------------------------------------------------------

WGPUCommandEncoder GSDeviceWebGPU::GetCommandEncoder()
{
	if (!m_encoder)
		m_encoder = wgpuDeviceCreateCommandEncoder(m_device, nullptr);
	return m_encoder;
}

void GSDeviceWebGPU::InvalidatePassState()
{
	m_pass_pipeline = nullptr;
	m_pass_texture_group = nullptr;
	m_pass_uniform_offset = UINT32_MAX;
	m_pass_vertex_bound = false;
	m_pass_index_bound = false;
	// A render pass starts with the viewport and scissor on the whole attachment (spec), which is
	// what the utility draws use: they need no setViewport/setScissorRect calls.
	m_pass_viewport = m_pass ? GSVector4i(0, 0, m_pass_size.x, m_pass_size.y) : GSVector4i::zero();
	m_pass_scissor = m_pass_viewport;
}

void GSDeviceWebGPU::EndRenderPass()
{
	if (!m_pass)
		return;

	wgpuRenderPassEncoderEnd(m_pass);
	wgpuRenderPassEncoderRelease(m_pass);
	m_pass = nullptr;
	m_pass_rt = nullptr;
	m_pass_is_present = false;
	InvalidatePassState();
}

WGPULoadOp GSDeviceWebGPU::LoadOpFor(GSTextureWebGPU* tex, GSVector4* clear_color) const
{
	// §6.2: a recorded clear becomes the load op. WebGPU has no "don't care", so an invalidated
	// target is cleared too.
	switch (tex->GetState())
	{
		case GSTexture::State::Cleared:
			*clear_color = tex->GetUNormClearColor();
			return WGPULoadOp_Clear;
		case GSTexture::State::Invalidated:
			*clear_color = GSVector4::zero();
			return WGPULoadOp_Clear;
		case GSTexture::State::Dirty:
		default:
			return WGPULoadOp_Load;
	}
}

void GSDeviceWebGPU::BeginRenderPass(GSTextureWebGPU* rt, WGPULoadOp load_op, const GSVector4& clear_color)
{
	EndRenderPass();

	WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	ca.view = rt->GetRTView();
	ca.loadOp = load_op;
	ca.storeOp = WGPUStoreOp_Store;
	ca.clearValue = {clear_color.x, clear_color.y, clear_color.z, clear_color.w};
	WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	rp.colorAttachmentCount = 1;
	rp.colorAttachments = &ca;
	m_pass = wgpuCommandEncoderBeginRenderPass(GetCommandEncoder(), &rp);
	m_pass_rt = rt;
	m_pass_is_present = false;
	m_pass_size = rt->GetSize();
	rt->SetUseSerial(m_encoder_serial);
	rt->SetState(GSTexture::State::Dirty);
	InvalidatePassState();
	g_perfmon.Put(GSPerfMon::RenderPasses, 1);
}

void GSDeviceWebGPU::BeginPresentPass(WGPULoadOp load_op)
{
	EndRenderPass();

	WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	ca.view = m_present_view;
	ca.loadOp = load_op;
	ca.storeOp = WGPUStoreOp_Store;
	ca.clearValue = {0.0, 0.0, 0.0, 1.0};
	WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	rp.colorAttachmentCount = 1;
	rp.colorAttachments = &ca;
	m_pass = wgpuCommandEncoderBeginRenderPass(GetCommandEncoder(), &rp);
	m_pass_rt = nullptr;
	m_pass_is_present = true;
	m_pass_size = GSVector2i(static_cast<int>(m_surface_width), static_cast<int>(m_surface_height));
	m_present_cleared = true;
	InvalidatePassState();
	g_perfmon.Put(GSPerfMon::RenderPasses, 1);
}

void GSDeviceWebGPU::EnsurePresentPass()
{
	pxAssert(m_presenting);
	if (!m_pass || !m_pass_is_present)
		BeginPresentPass(m_present_cleared ? WGPULoadOp_Load : WGPULoadOp_Clear);
}

void GSDeviceWebGPU::CommitClear(GSTextureWebGPU* tex)
{
	if (tex->GetState() != GSTexture::State::Cleared)
		return;

	if (tex->IsDepthStencil())
	{
		// Depth targets only come with the HW renderer (step 2); drop the clear there.
		tex->SetState(GSTexture::State::Dirty);
		return;
	}

	const bool was_present = m_pass && m_pass_is_present;
	BeginRenderPass(tex, WGPULoadOp_Clear, tex->GetUNormClearColor());
	EndRenderPass();
	if (was_present)
		EnsurePresentPass();
}

void GSDeviceWebGPU::PrepareForSampling(GSTextureWebGPU* tex)
{
	if (tex->GetState() == GSTexture::State::Cleared)
		CommitClear(tex);
}

void GSDeviceWebGPU::ExecuteCommandBuffer(const char* reason)
{
	pxAssertMsg(!WebGPUHost::InSuspendingWait(), "GS code entered while the GS thread is suspended");
	EndRenderPass();
	if (!m_device)
		return;

	// The stream buffers' shadows go up first: queue.writeBuffer runs before the commands below.
	m_vertex_buffer.Flush(m_queue);
	m_index_buffer.Flush(m_queue);
	m_uniform_buffer.Flush(m_queue);
	m_upload_buffer.Flush(m_queue);

	if (m_encoder)
	{
		WGPUCommandBuffer cb = wgpuCommandEncoderFinish(m_encoder, nullptr);
		wgpuQueueSubmit(m_queue, 1, &cb);
		wgpuCommandBufferRelease(cb);
		wgpuCommandEncoderRelease(m_encoder);
		m_encoder = nullptr;
		RemapStagingBuffers(m_encoder_serial);
	}
	m_encoder_serial++;

	// Objects the submitted encoder used can go now; WebGPU keeps what submitted work still needs.
	for (WGPUTexture t : m_pending_destroy_textures)
	{
		wgpuTextureDestroy(t);
		wgpuTextureRelease(t);
	}
	m_pending_destroy_textures.clear();
	for (WGPUTextureView v : m_pending_release_views)
		wgpuTextureViewRelease(v);
	m_pending_release_views.clear();
	for (WGPUBindGroup g : m_pending_release_groups)
		wgpuBindGroupRelease(g);
	m_pending_release_groups.clear();
}

void GSDeviceWebGPU::OnTextureDestroyed(GSTextureWebGPU* tex, WGPUTexture texture,
	std::initializer_list<WGPUTextureView> views, std::initializer_list<WGPUBindGroup> groups)
{
	if (m_want_texture == tex)
		m_want_texture = nullptr;
	if (m_pass_rt == tex)
		EndRenderPass();
	for (WGPUBindGroup g : groups)
	{
		if (g && g == m_pass_texture_group)
			m_pass_texture_group = nullptr;
	}

	// Destroying a texture that the open encoder uses would make its submit fail (§6.1).
	const bool defer = (tex->GetUseSerial() == m_encoder_serial && m_encoder);
	for (WGPUBindGroup g : groups)
	{
		if (!g)
			continue;
		if (defer)
			m_pending_release_groups.push_back(g);
		else
			wgpuBindGroupRelease(g);
	}
	for (WGPUTextureView v : views)
	{
		if (!v)
			continue;
		if (defer)
			m_pending_release_views.push_back(v);
		else
			wgpuTextureViewRelease(v);
	}
	if (texture)
	{
		if (defer)
		{
			m_pending_destroy_textures.push_back(texture);
		}
		else
		{
			wgpuTextureDestroy(texture);
			wgpuTextureRelease(texture);
		}
	}
}

WGPUBindGroup GSDeviceWebGPU::CreateUtilityBindGroup(WGPUTextureView view, BindGroupKind kind)
{
	const bool unfilterable = (kind == BindGroupKind::Unfilterable);
	WGPUBindGroupEntry e[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
	e[0].binding = 0;
	e[0].textureView = view;
	e[1].binding = 1;
	e[1].sampler = unfilterable ? m_samplers.GetNonFiltering() :
				   (kind == BindGroupKind::Linear) ? m_linear_sampler : m_point_sampler;
	WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
	d.layout = m_utility_tex_bgl[static_cast<u32>(unfilterable ? TexLayout::Unfilterable : TexLayout::Float)];
	d.entryCount = 2;
	d.entries = e;
	return wgpuDeviceCreateBindGroup(m_device, &d);
}

void GSDeviceWebGPU::SetPipeline(WGPURenderPipeline pipeline)
{
	m_want_pipeline = pipeline;
}

void GSDeviceWebGPU::SetUtilityTexture(GSTextureWebGPU* tex, BindGroupKind kind)
{
	m_want_texture = tex;
	m_want_kind = kind;
}

void GSDeviceWebGPU::SetUtilityUniforms(const void* data, u32 size)
{
	pxAssert(size <= sizeof(m_want_uniforms));
	std::memcpy(m_want_uniforms, data, size);
	if (size < sizeof(m_want_uniforms))
		std::memset(m_want_uniforms + size, 0, sizeof(m_want_uniforms) - size);
	m_want_uniform_size = size;
}

bool GSDeviceWebGPU::ApplyUtilityState(u32 vertex_bytes, u32 vertex_stride, u32 index_bytes)
{
	if (!m_pass || !m_want_pipeline || !m_want_texture)
		return false;

	// Room for this draw in every ring, or submit and reopen the pass (the rings start over).
	const bool fits = m_vertex_buffer.ReserveMemory(vertex_bytes, vertex_stride) &&
					  m_uniform_buffer.ReserveMemory(UNIFORM_SLOT_SIZE, UNIFORM_SLOT_SIZE) &&
					  (index_bytes == 0 || m_index_buffer.ReserveMemory(index_bytes, 4));
	if (!fits)
	{
		GSTextureWebGPU* const rt = m_pass_rt;
		const bool present = m_pass_is_present;
		ExecuteCommandBuffer("stream buffer full");
		if (present)
			BeginPresentPass(WGPULoadOp_Load);
		else
			BeginRenderPass(rt, WGPULoadOp_Load, GSVector4::zero());
		if (!m_vertex_buffer.ReserveMemory(vertex_bytes, vertex_stride) ||
			!m_uniform_buffer.ReserveMemory(UNIFORM_SLOT_SIZE, UNIFORM_SLOT_SIZE) ||
			(index_bytes != 0 && !m_index_buffer.ReserveMemory(index_bytes, 4)))
		{
			Console.Error("WebGPU: a draw does not fit in the stream buffers.");
			return false;
		}
	}

	if (m_pass_pipeline != m_want_pipeline)
	{
		wgpuRenderPassEncoderSetPipeline(m_pass, m_want_pipeline);
		m_pass_pipeline = m_want_pipeline;
	}

	// Constants: a new slot per draw, taken from the ring right here so it lives in this encoder.
	const u32 uniform_offset = m_uniform_buffer.GetCurrentOffset();
	std::memcpy(m_uniform_buffer.GetCurrentHostPointer(), m_want_uniforms, UNIFORM_BINDING_SIZE);
	m_uniform_buffer.CommitMemory(UNIFORM_SLOT_SIZE);
	if (m_pass_uniform_offset != uniform_offset)
	{
		wgpuRenderPassEncoderSetBindGroup(m_pass, 0, m_utility_cb_bg, 1, &uniform_offset);
		m_pass_uniform_offset = uniform_offset;
	}

	WGPUBindGroup group = m_want_texture->GetUtilityBindGroup(m_want_kind);
	m_want_texture->SetUseSerial(m_encoder_serial);
	if (m_pass_texture_group != group)
	{
		wgpuRenderPassEncoderSetBindGroup(m_pass, 1, group, 0, nullptr);
		m_pass_texture_group = group;
	}

	if (!m_pass_vertex_bound)
	{
		wgpuRenderPassEncoderSetVertexBuffer(m_pass, 0, m_vertex_buffer.GetBuffer(), 0, m_vertex_buffer.GetSize());
		m_pass_vertex_bound = true;
	}
	if (index_bytes != 0 && !m_pass_index_bound)
	{
		wgpuRenderPassEncoderSetIndexBuffer(m_pass, m_index_buffer.GetBuffer(), WGPUIndexFormat_Uint16, 0,
			m_index_buffer.GetSize());
		m_pass_index_bound = true;
	}

	return true;
}

bool GSDeviceWebGPU::DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds)
{
	if (!ApplyUtilityState(4 * sizeof(GSVertexPT1), sizeof(GSVertexPT1), 0))
		return false;

	// D3D/WebGPU clip space (+Y up), as GSDeviceVK::DrawStretchRect, whose shaders flip Y instead.
	const float inv_x = 2.0f / static_cast<float>(ds.x);
	const float inv_y = 2.0f / static_cast<float>(ds.y);
	const float left = dRect.x * inv_x - 1.0f;
	const float right = dRect.z * inv_x - 1.0f;
	const float top = 1.0f - dRect.y * inv_y;
	const float bottom = 1.0f - dRect.w * inv_y;
	const GSVertexPT1 vertices[] = {
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(sRect.x, sRect.y), {}, {}},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(sRect.z, sRect.y), {}, {}},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(sRect.x, sRect.w), {}, {}},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(sRect.z, sRect.w), {}, {}},
	};
	const u32 first_vertex = m_vertex_buffer.GetCurrentOffset() / sizeof(GSVertexPT1);
	std::memcpy(m_vertex_buffer.GetCurrentHostPointer(), vertices, sizeof(vertices));
	m_vertex_buffer.CommitMemory(sizeof(vertices));

	// The viewport and scissor are the whole attachment; the quad's clip coordinates place it.
	const GSVector4i full(0, 0, m_pass_size.x, m_pass_size.y);
	if (!m_pass_viewport.eq(full))
	{
		wgpuRenderPassEncoderSetViewport(m_pass, 0.0f, 0.0f, static_cast<float>(full.z), static_cast<float>(full.w), 0.0f, 1.0f);
		m_pass_viewport = full;
	}
	if (!m_pass_scissor.eq(full))
	{
		wgpuRenderPassEncoderSetScissorRect(m_pass, 0, 0, static_cast<u32>(full.z), static_cast<u32>(full.w));
		m_pass_scissor = full;
	}

	wgpuRenderPassEncoderDraw(m_pass, 4, 1, first_vertex, 0);
	g_perfmon.Put(GSPerfMon::DrawCalls, 1);
	return true;
}

void GSDeviceWebGPU::DoUtilityStretchRect(GSTextureWebGPU* sTex, const GSVector4& sRect, GSTextureWebGPU* dTex,
	const GSVector4& dRect, WGPURenderPipeline pipeline, BindGroupKind kind, bool allow_discard)
{
	if (!pipeline)
		return;

	if (sTex == dTex)
	{
		// A texture cannot be sampled while it is the attachment (§8.1 hazard rule).
		GSTexture* const tmp = CreateCompatible(sTex, false, true);
		if (!tmp)
			return;
		CopyRect(sTex, tmp, sTex->GetRect(), 0, 0);
		DoUtilityStretchRect(static_cast<GSTextureWebGPU*>(tmp), sRect, dTex, dRect, pipeline, kind, allow_discard);
		Recycle(tmp);
		return;
	}

	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	PrepareForSampling(sTex);

	const GSVector4i dtex_rc = dTex->GetRect();
	const GSVector4i dst_rc = GSVector4i(dRect).rintersect(dtex_rc);
	if (!m_pass || m_pass_rt != dTex || dTex->GetState() != GSTexture::State::Dirty)
	{
		GSVector4 clear_color = GSVector4::zero();
		const WGPULoadOp load_op = (allow_discard && dst_rc.eq(dtex_rc)) ? WGPULoadOp_Clear : LoadOpFor(dTex, &clear_color);
		BeginRenderPass(dTex, load_op, clear_color);
	}

	SetUtilityTexture(sTex, kind);
	SetPipeline(pipeline);
	DrawStretchRect(sRect, dRect, dTex->GetSize());
}

// ----------------------------------------------------------------------------
// Textures and copies
// ----------------------------------------------------------------------------

GSTexture* GSDeviceWebGPU::CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format)
{
	const WGPUTextureFormat native = LookupNativeFormat(format);
	if (native == WGPUTextureFormat_Undefined)
	{
		Console.ErrorFmt("WebGPU: no texture format for {} on this device.", GSTexture::GetFormatName(format));
		return nullptr;
	}

	std::unique_ptr<GSTextureWebGPU> tex = GSTextureWebGPU::Create(usage, format, width, height, levels, native);
	return tex.release();
}

std::unique_ptr<GSDownloadTexture> GSDeviceWebGPU::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return GSDownloadTextureWebGPU::Create(width, height, format);
}

bool GSDeviceWebGPU::UploadTextureLate(GSTextureWebGPU* tex, int level, const GSVector4i& rect, const void* data, u32 pitch)
{
	const u32 block = tex->GetCompressedBlockSize();
	const u32 rows = Common::AlignUpPow2(static_cast<u32>(rect.height()), block) / block;
	const u32 row_bytes = tex->CalcUploadPitch(rect.width());
	const u32 upload_pitch = tex->GetCopyPitch(rect.width());
	const u32 size = upload_pitch * rows;

	// Copies end the pass (a present pass reopens at its next draw).
	EndRenderPass();

	WGPUBuffer buffer;
	u64 offset;
	WGPUBuffer temp = nullptr;
	if (size <= m_upload_buffer.GetSize() / 2)
	{
		if (!m_upload_buffer.ReserveMemory(size, 256))
		{
			ExecuteCommandBuffer("texture upload buffer full");
			if (!m_upload_buffer.ReserveMemory(size, 256))
				return false;
		}
		offset = m_upload_buffer.GetCurrentOffset();
		StringUtil::StrideMemCpy(m_upload_buffer.GetCurrentHostPointer(), upload_pitch, data, pitch, row_bytes, rows);
		m_upload_buffer.CommitMemory(size);
		buffer = m_upload_buffer.GetBuffer();
	}
	else
	{
		// Too big for the ring: a buffer of its own, filled now (before the submit that reads it).
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
		bd.size = size;
		temp = wgpuDeviceCreateBuffer(m_device, &bd);
		std::unique_ptr<u8[]> staging = std::make_unique<u8[]>(size);
		StringUtil::StrideMemCpy(staging.get(), upload_pitch, data, pitch, row_bytes, rows);
		wgpuQueueWriteBuffer(m_queue, temp, 0, staging.get(), size);
		buffer = temp;
		offset = 0;
	}

	WGPUTexelCopyBufferInfo src = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	src.buffer = buffer;
	src.layout.offset = offset;
	src.layout.bytesPerRow = upload_pitch;
	src.layout.rowsPerImage = rows;
	WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	dst.texture = tex->GetTexture();
	dst.mipLevel = static_cast<u32>(level);
	dst.origin = {static_cast<u32>(rect.x), static_cast<u32>(rect.y), 0};
	const WGPUExtent3D extent = {static_cast<u32>(rect.width()), static_cast<u32>(rect.height()), 1};
	wgpuCommandEncoderCopyBufferToTexture(GetCommandEncoder(), &src, &dst, &extent);
	tex->SetUseSerial(m_encoder_serial);

	if (temp)
		wgpuBufferRelease(temp); // destroyed once the copy no longer needs it
	return true;
}

bool GSDeviceWebGPU::UploadTextureStaged(GSTextureWebGPU* tex, int level, const GSVector4i& rect, const void* data, u32 pitch)
{
	const u32 block = tex->GetCompressedBlockSize();
	const u32 rows = Common::AlignUpPow2(static_cast<u32>(rect.height()), block) / block;
	const u32 row_bytes = tex->CalcUploadPitch(rect.width());

	// Source rows whose pitch copies allow (a multiple of 256, as the software renderer's 4096-byte
	// output rows) are copied into the buffer as they are, padding included: one copy. Others are
	// repacked at the copy pitch first.
	const bool direct = (pitch % GSTextureWebGPU::COPY_PITCH_ALIGNMENT) == 0 && pitch >= row_bytes;
	const u32 copy_pitch = direct ? pitch : tex->GetCopyPitch(rect.width());
	const u32 size = copy_pitch * (rows - 1) + row_bytes;
	const u32 write_size = Common::AlignUpPow2(size, 4); // mapped ranges come in multiples of 4 bytes
	if (size < STAGING_MIN_UPLOAD || !m_device)
		return false;

	StagingBuffer* sb = nullptr;
	for (StagingBuffer& it : m_staging)
	{
		if (it.used_serial == 0 && it.size >= write_size && (!sb || it.size < sb->size) &&
			(it.fresh || WebGPUHost::StagingIsMapped(it.buffer)))
		{
			sb = &it;
		}
	}
	if (!sb)
	{
		if (m_staging.size() >= MAX_STAGING_BUFFERS)
			return false; // all in use or being mapped again

		// A new buffer starts out mapped, so it can be used at once.
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.label = SV("texture staging buffer");
		bd.usage = WGPUBufferUsage_MapWrite | WGPUBufferUsage_CopySrc;
		bd.size = Common::AlignUpPow2(write_size, STAGING_GRANULARITY);
		bd.mappedAtCreation = true;
		WGPUBuffer buffer = wgpuDeviceCreateBuffer(m_device, &bd);
		if (!buffer)
			return false;
		m_staging.push_back({buffer, static_cast<u32>(bd.size), 0, true});
		sb = &m_staging.back();
	}

	const void* src = data;
	if (!direct || write_size != size)
	{
		if (m_staging_scratch.size() < write_size)
			m_staging_scratch.resize(write_size);
		StringUtil::StrideMemCpy(m_staging_scratch.data(), copy_pitch, data, pitch, row_bytes, rows);
		src = m_staging_scratch.data();
	}
	bool written;
	if (sb->fresh)
	{
		// Mapped at creation through webgpu.h, so unmapped through it too; later maps are JS's.
		written = (wgpuBufferWriteMappedRange(sb->buffer, 0, src, write_size) == WGPUStatus_Success);
		wgpuBufferUnmap(sb->buffer);
		sb->fresh = false;
	}
	else
	{
		written = WebGPUHost::StagingWrite(sb->buffer, src, write_size);
	}
	sb->used_serial = m_encoder_serial; // mapped again after the submit
	if (!written)
		return false;

	EndRenderPass();
	WGPUTexelCopyBufferInfo s = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	s.buffer = sb->buffer;
	s.layout.offset = 0;
	s.layout.bytesPerRow = copy_pitch;
	s.layout.rowsPerImage = rows;
	WGPUTexelCopyTextureInfo d = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	d.texture = tex->GetTexture();
	d.mipLevel = static_cast<u32>(level);
	d.origin = {static_cast<u32>(rect.x), static_cast<u32>(rect.y), 0};
	const WGPUExtent3D extent = {static_cast<u32>(rect.width()), static_cast<u32>(rect.height()), 1};
	wgpuCommandEncoderCopyBufferToTexture(GetCommandEncoder(), &s, &d, &extent);
	tex->SetUseSerial(m_encoder_serial);
	return true;
}

void GSDeviceWebGPU::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	if (r.rempty())
		return;

	GSTextureWebGPU* const s = static_cast<GSTextureWebGPU*>(sTex);
	GSTextureWebGPU* const d = static_cast<GSTextureWebGPU*>(dTex);
	const GSVector4i dst_rect(0, 0, d->GetWidth(), d->GetHeight());
	const bool full_draw_copy = dst_rect.eq(r) && destX == 0 && destY == 0;

	// Depth copies must cover whole subresources (§2); the partial ones come with step 2.
	if ((s->IsDepthStencil() || d->IsDepthStencil()) && !(full_draw_copy && s->GetSize() == d->GetSize()))
	{
		static bool warned = false;
		if (!warned)
		{
			Console.Warning("WebGPU: partial depth copies are not implemented yet.");
			warned = true;
		}
		return;
	}

	// A cleared source carries its clear forward when the copy covers the destination (§6.2).
	if (s->GetState() == GSTexture::State::Cleared)
	{
		if (d->IsRenderTargetOrDepthStencil() && ProcessClearsBeforeCopy(sTex, dTex, full_draw_copy))
			return;
		s->CommitClear();
	}

	g_perfmon.Put(GSPerfMon::TextureCopies, 1);
	if (d->GetState() == GSTexture::State::Cleared && !full_draw_copy)
		d->CommitClear();

	if (s == d)
	{
		// Source and destination subresources must differ: bounce through a temporary texture.
		GSTexture* const tmp = CreateCompatible(sTex, r.width(), r.height(), false, true);
		if (!tmp)
			return;
		CopyRect(sTex, tmp, r, 0, 0);
		CopyRect(tmp, dTex, GSVector4i(0, 0, r.width(), r.height()), destX, destY);
		Recycle(tmp);
		return;
	}

	EndRenderPass();
	WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	src.texture = s->GetTexture();
	src.origin = {static_cast<u32>(r.x), static_cast<u32>(r.y), 0};
	WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	dst.texture = d->GetTexture();
	dst.origin = {destX, destY, 0};
	const WGPUExtent3D extent = {static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1};
	wgpuCommandEncoderCopyTextureToTexture(GetCommandEncoder(), &src, &dst, &extent);
	s->SetUseSerial(m_encoder_serial);
	d->SetUseSerial(m_encoder_serial);
	d->SetState(GSTexture::State::Dirty);
}

void GSDeviceWebGPU::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvertSelector shader, Filter filter)
{
	pxAssert(dTex);
	GSTextureWebGPU* const d = static_cast<GSTextureWebGPU*>(dTex);
	const UtilityPipelineKey key = ConvertPipelineKey(shader, d->GetNativeFormat());
	if (key.module >= MODULE_COUNT || d->IsDepthStencil())
	{
		static bool warned = false;
		if (!warned)
		{
			Console.WarningFmt("WebGPU: {} into a depth or stencil target is not implemented yet.", shader.Name());
			warned = true;
		}
		return;
	}

	filter = shader.SupportsBilinear() ? Nearest : filter; // no HW bilinear when the shader filters
	const BindGroupKind kind = shader.Float32Input() ? BindGroupKind::Unfilterable :
							   (filter == Biln) ? BindGroupKind::Linear : BindGroupKind::Point;
	DoUtilityStretchRect(static_cast<GSTextureWebGPU*>(sTex), sRect, d, dRect, GetUtilityPipeline(key), kind,
		shader.Mask() == 0xf);
}

void GSDeviceWebGPU::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, const GSVector4& dRect,
	PresentShader shader, Filter filter)
{
	// Into the frame being presented.
	if (!m_presenting)
		return;

	GSTextureWebGPU* const s = static_cast<GSTextureWebGPU*>(sTex);
	PrepareForSampling(s);
	EnsurePresentPass();
	SetUtilityTexture(s, (filter == Biln) ? BindGroupKind::Linear : BindGroupKind::Point);
	SetPipeline(GetUtilityPipeline(MakeKey(MODULE_PRESENT, ShaderEntryPoint(shader), m_surface_format)));
	DrawStretchRect(sRect, dRect, GSVector2i(static_cast<int>(m_surface_width), static_cast<int>(m_surface_height)));
}

void GSDeviceWebGPU::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, Filter filter)
{
	DisplayConstantBuffer cb;
	cb.SetSource(sRect, sTex->GetSize());
	cb.SetTarget(dRect, dTex ? dTex->GetSize() : GSVector2i(static_cast<int>(m_surface_width), static_cast<int>(m_surface_height)));
	cb.SetTime(shaderTime);
	SetUtilityUniforms(&cb, sizeof(cb));

	if (!dTex)
	{
		DoStretchRect(sTex, sRect, dRect, shader, filter);
		return;
	}

	GSTextureWebGPU* const d = static_cast<GSTextureWebGPU*>(dTex);
	DoUtilityStretchRect(static_cast<GSTextureWebGPU*>(sTex), sRect, d, dRect,
		GetUtilityPipeline(MakeKey(MODULE_PRESENT, ShaderEntryPoint(shader), d->GetNativeFormat())),
		(filter == Biln) ? BindGroupKind::Linear : BindGroupKind::Point, true);
}

void GSDeviceWebGPU::DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex,
	ShaderConvertSelector shader)
{
	// One draw per rectangle; GSDeviceVK batches runs with the same source (an optimisation for the
	// HW renderer's sprite copies, step 3).
	for (u32 i = 0; i < num_rects; i++)
		DoStretchRect(rects[i].src, rects[i].src_rect, dTex, rects[i].dst_rect, shader.SetMask(rects[i].wmask.wrgba),
			rects[i].filter);
}

// ----------------------------------------------------------------------------
// Merge, interlace, post-processing
// ----------------------------------------------------------------------------

void GSDeviceWebGPU::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter)
{
	// The pass sequence of GSDeviceVK::DoMerge.
	const GSVector4 full_r(0.0f, 0.0f, 1.0f, 1.0f);
	const u32 yuv_constants[4] = {EXTBUF.EMODA, EXTBUF.EMODC, 0, 0};
	const GSVector4 bg_color = GSVector4::unorm8(c);
	const bool feedback_write_2 = PMODE.EN2 && sTex[2] != nullptr && EXTBUF.FBIN == 1;
	const bool feedback_write_1 = PMODE.EN1 && sTex[2] != nullptr && EXTBUF.FBIN == 0;
	const bool feedback_write_2_but_blend_bg = feedback_write_2 && PMODE.SLBG == 1;
	const BindGroupKind kind = (filter == Biln) ? BindGroupKind::Linear : BindGroupKind::Point;
	GSTextureWebGPU* const s0 = static_cast<GSTextureWebGPU*>(sTex[0]);
	GSTextureWebGPU* const s1 = static_cast<GSTextureWebGPU*>(sTex[1]);
	GSTextureWebGPU* const s2 = static_cast<GSTextureWebGPU*>(sTex[2]);
	GSTextureWebGPU* const d = static_cast<GSTextureWebGPU*>(dTex);
	const WGPURenderPipeline copy = GetUtilityPipeline(ConvertPipelineKey(ShaderConvert::COPY, d->GetNativeFormat()));
	const WGPURenderPipeline yuv = GetUtilityPipeline(ConvertPipelineKey(ShaderConvert::YUV, d->GetNativeFormat()));
	EndRenderPass();

	const bool has_input_0 = (s0 && (s0->GetState() == GSTexture::State::Dirty ||
										(s0->GetState() == GSTexture::State::Cleared || s0->GetClearColor() != 0)));
	const bool has_input_1 = (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg) && s1 &&
							 (s1->GetState() == GSTexture::State::Dirty ||
								 (s1->GetState() == GSTexture::State::Cleared || s1->GetClearColor() != 0));
	if (has_input_0)
		s0->CommitClear();
	if (has_input_1)
		s1->CommitClear();

	const GSVector2i dsize(dTex->GetSize());
	bool dcleared = false;
	if (s1 && (PMODE.SLBG == 0 || feedback_write_2_but_blend_bg))
	{
		// 2nd output is enabled and selected. Copy it to destination so we can blend it with 1st output.
		// Note: value outside of dRect must contains the background color (c).
		if (s1->GetState() == GSTexture::State::Dirty)
		{
			BeginRenderPass(d, WGPULoadOp_Clear, bg_color);
			SetUtilityTexture(s1, kind);
			SetPipeline(copy);
			DrawStretchRect(sRect[1], PMODE.SLBG ? dRect[2] : dRect[1], dsize);
			dcleared = true;
		}
	}

	// Upload constant to select YUV algo.
	const GSVector2i fbsize(s2 ? s2->GetSize() : GSVector2i(0, 0));
	const GSVector4i fbarea(0, 0, fbsize.x, fbsize.y);
	if (feedback_write_2)
	{
		EndRenderPass();
		// sTex[2] can be sTex[0], in which case it might be cleared (e.g. Xenosaga).
		GSVector4 clear_color = GSVector4::zero();
		const GSVector4i drc = GSVector4i(dRect[2]).rintersect(fbarea);
		BeginRenderPass(s2, drc.eq(fbarea) ? WGPULoadOp_Clear : LoadOpFor(s2, &clear_color), clear_color);
		if (dcleared)
		{
			SetUtilityTexture(d, kind);
			SetPipeline(yuv);
			SetUtilityUniforms(yuv_constants, sizeof(yuv_constants));
			DrawStretchRect(full_r, dRect[2], fbsize);
		}
		EndRenderPass();
	}

	// Restore background color to process the normal merge.
	if (feedback_write_2_but_blend_bg || !dcleared)
		BeginRenderPass(d, WGPULoadOp_Clear, bg_color);
	else if (!m_pass || m_pass_rt != d)
		BeginRenderPass(d, WGPULoadOp_Load, GSVector4::zero());

	if (s0 && s0->GetState() == GSTexture::State::Dirty)
	{
		// 1st output is enabled. It must be blended.
		SetUtilityTexture(s0, kind);
		SetPipeline(GetUtilityPipeline(MakeKey(MODULE_MERGE, PMODE.MMOD ? "ps_main1" : "ps_main0",
			d->GetNativeFormat(), TexLayout::Float, 0xf, BLEND_MERGE)));
		SetUtilityUniforms(&bg_color, sizeof(bg_color));
		DrawStretchRect(sRect[0], dRect[0], dsize);
	}

	if (feedback_write_1)
	{
		EndRenderPass();
		BeginRenderPass(s2, WGPULoadOp_Load, GSVector4::zero());
		SetUtilityTexture(d, kind);
		SetPipeline(yuv);
		SetUtilityUniforms(yuv_constants, sizeof(yuv_constants));
		DrawStretchRect(full_r, dRect[2], fbsize);
	}

	EndRenderPass();

	// This texture is going to be used as an input, so make sure it holds defined data.
	d->CommitClear();
}

void GSDeviceWebGPU::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb)
{
	GSTextureWebGPU* const s = static_cast<GSTextureWebGPU*>(sTex);
	GSTextureWebGPU* const d = static_cast<GSTextureWebGPU*>(dTex);
	PrepareForSampling(s);

	// Weave and MAD buffering keep the lines of the other field: load what the target holds.
	GSVector4 clear_color = GSVector4::zero();
	BeginRenderPass(d, LoadOpFor(d, &clear_color), clear_color);
	SetUtilityTexture(s, (filter == Biln) ? BindGroupKind::Linear : BindGroupKind::Point);
	SetPipeline(GetUtilityPipeline(MakeKey(MODULE_INTERLACE, fmt::format("ps_main{}", static_cast<int>(shader)).c_str(),
		d->GetNativeFormat())));
	SetUtilityUniforms(&cb, sizeof(cb));
	DrawStretchRect(sRect, dRect, dTex->GetSize());
	EndRenderPass();
}

void GSDeviceWebGPU::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
	GSTextureWebGPU* const s = static_cast<GSTextureWebGPU*>(sTex);
	GSTextureWebGPU* const d = static_cast<GSTextureWebGPU*>(dTex);
	PrepareForSampling(s);

	BeginRenderPass(d, WGPULoadOp_Clear, GSVector4::zero());
	SetUtilityTexture(s, BindGroupKind::Point);
	SetPipeline(GetUtilityPipeline(MakeKey(MODULE_SHADEBOOST, "ps_main", d->GetNativeFormat())));
	SetUtilityUniforms(params, sizeof(float) * 4);
	DrawStretchRect(GSVector4(0.0f, 0.0f, 1.0f, 1.0f), GSVector4(dTex->GetRect()), dTex->GetSize());
	EndRenderPass();
}

void GSDeviceWebGPU::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
	// fxaa.wgsl comes with step 3; until then FXAA is a plain copy (it is off by default).
	static bool warned = false;
	if (!warned)
	{
		Console.Warning("WebGPU: FXAA is not implemented yet; the frame is copied unchanged.");
		warned = true;
	}
	StretchRect(sTex, GSVector4(0.0f, 0.0f, 1.0f, 1.0f), dTex, GSVector4(dTex->GetRect()), ShaderConvert::COPY, Nearest);
}

bool GSDeviceWebGPU::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only,
	const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	return false; // cas_sharpening is 0 until cas.wgsl (step 3)
}

// ----------------------------------------------------------------------------
// HW renderer paths (step 2): the HW renderer cannot select this device yet
// ----------------------------------------------------------------------------

static void WarnNotImplemented(const char* what)
{
	Console.ErrorFmt("WebGPU: {} is not implemented yet (HW renderer, step 2).", what);
}

void GSDeviceWebGPU::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex,
	u32 dOffset, u32 dSize)
{
	static bool warned = false;
	if (!std::exchange(warned, true))
		WarnNotImplemented("UpdateCLUTTexture");
}

void GSDeviceWebGPU::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW,
	u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
	static bool warned = false;
	if (!std::exchange(warned, true))
		WarnNotImplemented("ConvertToIndexedTexture");
}

void GSDeviceWebGPU::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
	const GSVector2i& clamp_min, const GSVector4& dRect)
{
	static bool warned = false;
	if (!std::exchange(warned, true))
		WarnNotImplemented("FilteredDownsampleTexture");
}

void GSDeviceWebGPU::RenderHW(GSHWDrawConfig& config)
{
	static bool warned = false;
	if (!std::exchange(warned, true))
		WarnNotImplemented("RenderHW");
}

void GSDeviceWebGPU::ClearSamplerCache()
{
	// Only the HW renderer's samplers change; the utility point and linear samplers stay.
}

// ----------------------------------------------------------------------------
// Presenting
// ----------------------------------------------------------------------------

void GSDeviceWebGPU::ReportPendingErrors()
{
	const u32 count = s_cb.error_count;
	while (s_cb.errors_reported < std::min(count, MAX_LOGGED_ERRORS))
		Console.ErrorFmt("{}", s_cb.errors[s_cb.errors_reported++]);
	if (count > MAX_LOGGED_ERRORS && s_cb.errors_reported == MAX_LOGGED_ERRORS)
	{
		Console.ErrorFmt("WebGPU: further uncaptured errors are only counted.");
		s_cb.errors_reported++;
	}
}

GSDevice::PresentResult GSDeviceWebGPU::BeginPresent(bool frame_skip)
{
	pxAssertMsg(!WebGPUHost::InSuspendingWait(), "GS code entered while the GS thread is suspended");
	EndRenderPass();
	ReportPendingErrors();

	// The lost callback is the only reliable sign: on a lost device getCurrentTexture still
	// succeeds (§12.2). A test can ask for a loss (WebGPUHost::RequestTestDeviceLoss).
	if (WebGPUHost::ConsumeTestDeviceLossRequest() && m_device)
	{
		Console.Warning("WebGPU: destroying the device on request (test).");
		wgpuDeviceDestroy(m_device);
	}
	if (s_cb.lost)
	{
		WebGPUHost::NoteDeviceLost();
		return PresentResult::DeviceLost;
	}

	// The page's size feed: present at the canvas's backing size.
	u32 width, height;
	float scale;
	if (WebGPUHost::GetSurfaceSize(&width, &height, &scale) &&
		(width != m_page_width || height != m_page_height || scale != m_window_info.surface_scale))
	{
		m_page_width = width;
		m_page_height = height;
		ResizeWindow(width, height, scale);
		ImGuiManager::WindowResized();
	}

	if (frame_skip || !m_surface_configured)
	{
		ExecuteCommandBuffer("frame skipped");
		return PresentResult::FrameSkipped;
	}

	WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
	wgpuSurfaceGetCurrentTexture(m_surface, &st);
	if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
		st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
	{
		if (st.texture)
			wgpuTextureRelease(st.texture);
		if (st.status == WGPUSurfaceGetCurrentTextureStatus_Outdated || st.status == WGPUSurfaceGetCurrentTextureStatus_Lost)
			ConfigureSurface();
		ExecuteCommandBuffer("no surface texture");
		return PresentResult::FrameSkipped;
	}

	// The present pass cannot apply a pending clear of the frame it shows: do it now.
	if (m_current)
		PrepareForSampling(static_cast<GSTextureWebGPU*>(m_current));

	m_present_texture = st.texture;
	m_present_view = wgpuTextureCreateView(m_present_texture, nullptr);
	m_presenting = true;
	m_present_cleared = false;
	BeginPresentPass(WGPULoadOp_Clear);
	return PresentResult::OK;
}

void GSDeviceWebGPU::EndPresent()
{
	RenderImGui();
	EnsurePresentPass(); // a frame with nothing drawn still clears to black
	EndRenderPass();
	ExecuteCommandBuffer("present");

	m_presenting = false;
	if (m_present_view)
		wgpuTextureViewRelease(m_present_view);
	if (m_present_texture)
		wgpuTextureRelease(m_present_texture);
	m_present_view = nullptr;
	m_present_texture = nullptr;

	// No wgpuSurfacePresent: the frame goes to the page as an ImageBitmap (§12.1), tagged with the
	// vsync it shows (GSRenderer::VSync() counts it after presenting), like the 2D path's frames. A
	// frame drawn on a device known to be lost is blank: the page keeps the one it has.
	if (!s_cb.lost)
		WebGPUHost::PostFrame(static_cast<u32>(PerformanceMetrics::GetFrameNumber()) + 1);

	KeepEventLoopTurning();
}

void GSDeviceWebGPU::KeepEventLoopTurning()
{
	// Device loss and errors arrive through the GS worker's event loop, which runs while the GS
	// thread waits for work (it suspends then). A GS thread that has not let it run for 100 ms gets
	// a 1 ms turn (§3.2). A microtask queued here, still pending, tells how long the loop has not
	// run (WebGPUHost::EventLoopStalledMs()). (Not queue.onSubmittedWorkDone(): after a GPU-process
	// crash its promise rejects, and emdawnwebgpu leaves that rejection unhandled, which kills the
	// worker.)
	const double now = WebGPUHost::Now();
	if (WebGPUHost::EventLoopStalledMs() > 100.0 && now - m_last_event_loop_turn > 100.0)
	{
		s_cb.in_bounded_turn = true;
		WebGPUHost::SleepSuspending(1.0);
		s_cb.in_bounded_turn = false;
		s_cb.bounded_turns++;
		m_last_event_loop_turn = WebGPUHost::Now();
	}
}

void GSDeviceWebGPU::RenderImGui()
{
	// Port of GSDeviceVK::RenderImGui (§12.2): ImGui's vertices and indices into the stream buffers,
	// a clamped scissor per command, the command's texture with the linear sampler.
	ImGui::Render();
	const ImDrawData* draw_data = ImGui::GetDrawData();
	if (!m_presenting || draw_data->CmdLists.Size == 0)
		return;

	UpdateImGuiTextures();
	EnsurePresentPass();

	// WebGPU clip space has +Y up (imgui.wgsl's header).
	const float uniforms[4] = {2.0f / static_cast<float>(m_surface_width), -2.0f / static_cast<float>(m_surface_height),
		-1.0f, 1.0f};
	SetUtilityUniforms(uniforms, sizeof(uniforms));
	SetPipeline(GetUtilityPipeline(MakeKey(MODULE_IMGUI, "ps_main", m_surface_format, TexLayout::Float, 0xf, BLEND_IMGUI)));

	const GSVector4i surface_rect(0, 0, static_cast<int>(m_surface_width), static_cast<int>(m_surface_height));
	for (int n = 0; n < draw_data->CmdLists.Size; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];
		const u32 vertex_bytes = sizeof(ImDrawVert) * static_cast<u32>(cmd_list->VtxBuffer.Size);
		const u32 index_bytes = sizeof(ImDrawIdx) * static_cast<u32>(cmd_list->IdxBuffer.Size);
		static_assert(sizeof(ImDrawIdx) == sizeof(u16));

		// The first command's texture makes ApplyUtilityState() bind something; each command
		// rebinds its own below.
		GSTextureWebGPU* first_tex = nullptr;
		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size && !first_tex; cmd_i++)
			first_tex = reinterpret_cast<GSTextureWebGPU*>(cmd_list->CmdBuffer[cmd_i].GetTexID());
		if (!first_tex)
			continue;
		SetUtilityTexture(first_tex, BindGroupKind::Linear);
		if (!ApplyUtilityState(vertex_bytes, sizeof(ImDrawVert), Common::AlignUpPow2(index_bytes, 4)))
			return;

		const u32 base_vertex = m_vertex_buffer.GetCurrentOffset() / sizeof(ImDrawVert);
		std::memcpy(m_vertex_buffer.GetCurrentHostPointer(), cmd_list->VtxBuffer.Data, vertex_bytes);
		m_vertex_buffer.CommitMemory(vertex_bytes);
		const u32 first_index = m_index_buffer.GetCurrentOffset() / sizeof(ImDrawIdx);
		std::memcpy(m_index_buffer.GetCurrentHostPointer(), cmd_list->IdxBuffer.Data, index_bytes);
		m_index_buffer.CommitMemory(Common::AlignUpPow2(index_bytes, 4));

		if (!m_pass_viewport.eq(surface_rect))
		{
			wgpuRenderPassEncoderSetViewport(m_pass, 0.0f, 0.0f, static_cast<float>(surface_rect.z),
				static_cast<float>(surface_rect.w), 0.0f, 1.0f);
			m_pass_viewport = surface_rect;
		}

		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
			pxAssert(!pcmd->UserCallback);

			const GSVector4 clip = GSVector4::load<false>(&pcmd->ClipRect);
			if ((clip.zwzw() <= clip.xyxy()).mask() != 0)
				continue;

			// WebGPU validates the scissor against the attachment.
			const GSVector4i scissor = GSVector4i(clip).max_i32(GSVector4i::zero()).rintersect(surface_rect);
			if (scissor.rempty())
				continue;
			if (!m_pass_scissor.eq(scissor))
			{
				wgpuRenderPassEncoderSetScissorRect(m_pass, static_cast<u32>(scissor.x), static_cast<u32>(scissor.y),
					static_cast<u32>(scissor.width()), static_cast<u32>(scissor.height()));
				m_pass_scissor = scissor;
			}

			if (GSTextureWebGPU* tex = reinterpret_cast<GSTextureWebGPU*>(pcmd->GetTexID()))
			{
				WGPUBindGroup group = tex->GetUtilityBindGroup(BindGroupKind::Linear);
				tex->SetUseSerial(m_encoder_serial);
				if (group != m_pass_texture_group)
				{
					wgpuRenderPassEncoderSetBindGroup(m_pass, 1, group, 0, nullptr);
					m_pass_texture_group = group;
				}
			}

			wgpuRenderPassEncoderDrawIndexed(m_pass, pcmd->ElemCount, 1, first_index + pcmd->IdxOffset,
				static_cast<s32>(base_vertex + pcmd->VtxOffset), 0);
		}

		g_perfmon.Put(GSPerfMon::DrawCalls, cmd_list->CmdBuffer.Size);
	}
}
