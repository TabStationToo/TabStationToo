// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/WebGPU/GSTextureWebGPU.h"
#include "GS/Renderers/WebGPU/GSDeviceWebGPU.h"
#include "GS/Renderers/WebGPU/WebGPUHost.h"
#include "GS/GSPerfMon.h"

#include "common/Assertions.h"
#include "common/BitUtils.h"
#include "common/Console.h"
#include "common/StringUtil.h"

static constexpr double READBACK_TIMEOUT_MS = 2000.0;

static bool IsDepthNativeFormat(WGPUTextureFormat format)
{
	return (format == WGPUTextureFormat_Depth32Float || format == WGPUTextureFormat_Depth32FloatStencil8);
}

static bool IsRenderableFormat(GSTexture::Format format)
{
	return !GSTexture::IsCompressedFormat(format);
}

GSTextureWebGPU::GSTextureWebGPU(Usage usage, Format format, int width, int height, int levels, WGPUTexture texture,
	WGPUTextureView view, WGPUTextureView rt_view, WGPUTextureFormat native_format)
	: m_texture(texture)
	, m_view(view)
	, m_rt_view(rt_view)
	, m_native_format(native_format)
{
	m_usage = usage;
	m_format = format;
	m_size = GSVector2i(width, height);
	m_mipmap_levels = levels;
}

GSTextureWebGPU::~GSTextureWebGPU()
{
	GSDeviceWebGPU::GetInstance()->OnTextureDestroyed(this, m_texture, {m_view, (m_rt_view != m_view) ? m_rt_view : nullptr},
		{m_bind_groups[0], m_bind_groups[1], m_bind_groups[2]});
}

std::unique_ptr<GSTextureWebGPU> GSTextureWebGPU::Create(
	Usage usage, Format format, int width, int height, int levels, WGPUTextureFormat native_format)
{
	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();
	if (native_format == WGPUTextureFormat_Undefined)
		return {};

	WGPUTextureUsage wusage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
	if (IsRenderTargetOrDepthStencil(usage) || (levels > 1 && IsRenderableFormat(format)))
		wusage |= WGPUTextureUsage_RenderAttachment;
	if (IsShaderWrite(usage))
		wusage |= WGPUTextureUsage_StorageBinding;

	WGPUTextureDescriptor desc = WGPU_TEXTURE_DESCRIPTOR_INIT;
	desc.usage = wusage;
	desc.dimension = WGPUTextureDimension_2D;
	desc.size = {static_cast<u32>(width), static_cast<u32>(height), 1};
	desc.format = native_format;
	desc.mipLevelCount = static_cast<u32>(levels);
	desc.sampleCount = 1;
	WGPUTexture texture = wgpuDeviceCreateTexture(dev->GetDevice(), &desc);
	if (!texture)
		return {};

	// Sampling view: every level, and only the depth aspect of depth formats (bound as
	// unfilterable-float, read with textureLoad/point sampling).
	WGPUTextureViewDescriptor vdesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
	vdesc.dimension = WGPUTextureViewDimension_2D;
	vdesc.aspect = IsDepthNativeFormat(native_format) ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
	vdesc.usage = WGPUTextureUsage_TextureBinding;
	WGPUTextureView view = wgpuTextureCreateView(texture, &vdesc);

	WGPUTextureView rt_view = view;
	if (wusage & WGPUTextureUsage_RenderAttachment)
	{
		WGPUTextureViewDescriptor rdesc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
		rdesc.dimension = WGPUTextureViewDimension_2D;
		rdesc.mipLevelCount = 1;
		rdesc.aspect = WGPUTextureAspect_All;
		rdesc.usage = WGPUTextureUsage_RenderAttachment;
		rt_view = wgpuTextureCreateView(texture, &rdesc);
	}

	return std::unique_ptr<GSTextureWebGPU>(
		new GSTextureWebGPU(usage, format, width, height, levels, texture, view, rt_view, native_format));
}

void* GSTextureWebGPU::GetNativeHandle() const
{
	return const_cast<GSTextureWebGPU*>(this);
}

u32 GSTextureWebGPU::GetCopyPitch(u32 width) const
{
	return Common::AlignUpPow2(CalcUploadPitch(width), COPY_PITCH_ALIGNMENT);
}

void GSTextureWebGPU::CommitClear()
{
	if (m_state != State::Cleared)
		return;

	GSDeviceWebGPU::GetInstance()->CommitClear(this);
}

WGPUBindGroup GSTextureWebGPU::GetUtilityBindGroup(BindGroupKind kind)
{
	WGPUBindGroup& group = m_bind_groups[static_cast<size_t>(kind)];
	if (!group)
		group = GSDeviceWebGPU::GetInstance()->CreateUtilityBindGroup(m_view, kind);
	return group;
}

bool GSTextureWebGPU::UploadRect(const GSVector4i& r, const void* data, u32 pitch, int layer)
{
	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();

	// A cleared render target only partially overwritten keeps the clear elsewhere: record the clear
	// first, which also makes the upload a late one (it has to come after the clear).
	if (IsRenderTargetOrDepthStencil())
	{
		if (!r.eq(GetRect()) || layer != 0)
			CommitClear();
		m_state = State::Dirty;
	}

	// Large uploads are copied in the encoder from a mapped staging buffer, which is correct either way:
	// a texture the open encoder has not used yet is not read before this point.
	if (dev->UploadTextureStaged(this, layer, r, data, pitch))
		return true;

	// The open encoder used the texture already: the upload has to be recorded in it, after those uses.
	if (m_use_serial == dev->GetEncoderSerial())
		return dev->UploadTextureLate(this, layer, r, data, pitch);

	// Otherwise the queue can take it now: it runs before everything the open encoder records.
	const u32 block = GetCompressedBlockSize();
	WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	dst.texture = m_texture;
	dst.mipLevel = static_cast<u32>(layer);
	dst.origin = {static_cast<u32>(r.x), static_cast<u32>(r.y), 0};
	WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
	layout.offset = 0;
	layout.bytesPerRow = pitch;
	layout.rowsPerImage = Common::AlignUpPow2(static_cast<u32>(r.height()), block) / block;
	const WGPUExtent3D size = {static_cast<u32>(r.width()), static_cast<u32>(r.height()), 1};
	const u32 rows = layout.rowsPerImage;
	const size_t data_size = static_cast<size_t>(pitch) * (rows - 1) + CalcUploadPitch(r.width());
	wgpuQueueWriteTexture(dev->GetQueue(), &dst, data, data_size, &layout, &size);
	return true;
}

bool GSTextureWebGPU::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	if (layer >= m_mipmap_levels)
		return false;

	const GSVector4i level_rect = GSVector4i(0, 0, std::max(m_size.x >> layer, 1), std::max(m_size.y >> layer, 1));
	if (r.rempty() || !r.rintersect(level_rect).eq(r))
		return false;

	g_perfmon.Put(GSPerfMon::TextureUploads, 1);
	if (!UploadRect(r, data, static_cast<u32>(pitch), layer))
		return false;

	if (IsTexture())
		m_needs_mipmaps_generated |= (layer == 0);
	return true;
}

bool GSTextureWebGPU::Map(GSMap& m, const GSVector4i* r, int layer)
{
	if (layer >= m_mipmap_levels || IsCompressedFormat())
		return false;

	m_map_area = r ? *r : GetRect();
	m_map_level = layer;
	m_map_pitch = GetCopyPitch(m_map_area.width());
	const size_t size = static_cast<size_t>(m_map_pitch) * m_map_area.height();
	if (m_map_buffer.size() < size)
		m_map_buffer.resize(size);

	m.bits = m_map_buffer.data();
	m.pitch = static_cast<int>(m_map_pitch);
	return true;
}

void GSTextureWebGPU::Unmap()
{
	pxAssert(m_map_level < m_mipmap_levels && !IsCompressedFormat());
	g_perfmon.Put(GSPerfMon::TextureUploads, 1);
	UploadRect(m_map_area, m_map_buffer.data(), m_map_pitch, m_map_level);

	if (IsTexture())
		m_needs_mipmaps_generated |= (m_map_level == 0);

	// Keep small buffers (ImGui updates every frame); drop big ones.
	if (m_map_buffer.size() > 1024 * 1024)
		std::vector<u8>().swap(m_map_buffer);
}

void GSTextureWebGPU::GenerateMipmap()
{
	// Step 2 (docs/web-port/WEBGPU-DESIGN.md §6.4): one COPY draw per level. Only the HW renderer
	// samples mips, and it cannot use this device yet.
	static bool warned = false;
	if (!warned)
	{
		Console.Warning("WebGPU: GenerateMipmap() is not implemented yet.");
		warned = true;
	}
}

#ifdef PCSX2_DEVBUILD
void GSTextureWebGPU::SetDebugName(std::string_view name)
{
	m_debug_name = name;
	if (m_texture)
		wgpuTextureSetLabel(m_texture, {name.data(), name.size()});
}
#endif

// ----------------------------------------------------------------------------
// GSDownloadTextureWebGPU
// ----------------------------------------------------------------------------

GSDownloadTextureWebGPU::GSDownloadTextureWebGPU(
	u32 width, u32 height, GSTexture::Format format, WGPUBuffer buffer, u32 buffer_size)
	: GSDownloadTexture(width, height, format)
	, m_buffer(buffer)
	, m_buffer_size(buffer_size)
	, m_shadow(std::make_unique<u8[]>(buffer_size))
{
}

GSDownloadTextureWebGPU::~GSDownloadTextureWebGPU()
{
	if (m_buffer)
	{
		wgpuBufferDestroy(m_buffer);
		wgpuBufferRelease(m_buffer);
	}
}

std::unique_ptr<GSDownloadTextureWebGPU> GSDownloadTextureWebGPU::Create(u32 width, u32 height, GSTexture::Format format)
{
	const u32 size = GetBufferSize(width, height, format, GSTextureWebGPU::COPY_PITCH_ALIGNMENT);
	WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	desc.label = {"GSDownloadTextureWebGPU", WGPU_STRLEN};
	desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
	desc.size = size;
	WGPUBuffer buffer = wgpuDeviceCreateBuffer(GSDeviceWebGPU::GetInstance()->GetDevice(), &desc);
	if (!buffer)
		return {};

	return std::unique_ptr<GSDownloadTextureWebGPU>(new GSDownloadTextureWebGPU(width, height, format, buffer, size));
}

void GSDownloadTextureWebGPU::CopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	GSTextureWebGPU* const tex = static_cast<GSTextureWebGPU*>(stex);
	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();
	pxAssert(tex->GetFormat() == m_format);
	pxAssert(drc.width() == src.width() && drc.height() == src.height());
	pxAssert(src.z <= tex->GetWidth() && src.w <= tex->GetHeight());

	m_current_pitch = GetTransferPitch(use_transfer_pitch ? static_cast<u32>(drc.width()) : m_width, GSTextureWebGPU::COPY_PITCH_ALIGNMENT);
	u32 copy_offset, copy_size, copy_rows;
	GetTransferSize(drc, &copy_offset, &copy_size, &copy_rows);

	g_perfmon.Put(GSPerfMon::Readbacks, 1);
	tex->CommitClear();
	dev->EndRenderPass();

	WGPUTexelCopyTextureInfo s = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
	s.texture = tex->GetTexture();
	s.mipLevel = src_level;
	s.origin = {static_cast<u32>(src.x), static_cast<u32>(src.y), 0};
	s.aspect = IsDepthNativeFormat(tex->GetNativeFormat()) ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
	WGPUTexelCopyBufferInfo d = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
	d.buffer = m_buffer;
	d.layout.offset = copy_offset;
	d.layout.bytesPerRow = m_current_pitch;
	d.layout.rowsPerImage = copy_rows;
	const WGPUExtent3D size = {static_cast<u32>(src.width()), static_cast<u32>(src.height()), 1};
	wgpuCommandEncoderCopyTextureToBuffer(dev->GetCommandEncoder(), &s, &d, &size);
	tex->SetUseSerial(dev->GetEncoderSerial());

	m_copy_serial = dev->GetEncoderSerial();
	m_needs_flush = true;
}

bool GSDownloadTextureWebGPU::Map(const GSVector4i& read_rc)
{
	if (m_needs_flush)
		Flush();

	// After a failed readback the shadow holds no copy of the texture (a screenshot of it would be
	// blank): fail, so the caller reports the error instead of using it.
	if (m_readback_failed)
		return false;

	m_map_pointer = m_shadow.get();
	return true;
}

void GSDownloadTextureWebGPU::Unmap()
{
	// Nothing stays mapped: the buffer was unmapped as soon as Flush() copied it.
	m_map_pointer = nullptr;
}

void GSDownloadTextureWebGPU::Flush()
{
	if (!m_needs_flush)
		return;

	m_needs_flush = false;
	GSDeviceWebGPU* const dev = GSDeviceWebGPU::GetInstance();
	if (m_copy_serial == dev->GetEncoderSerial())
		dev->ExecuteCommandBuffer("readback");

	using MapRequest = WebGPUHost::RequestResult<WGPUMapAsyncStatus>;
	MapRequest* const req = new MapRequest();
	WGPUBufferMapCallbackInfo cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
	cb.mode = WGPUCallbackMode_WaitAnyOnly;
	cb.userdata1 = req;
	cb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* userdata1, void*) {
		if (MapRequest* r = MapRequest::Arrive(userdata1))
			r->value = status;
	};

	const double t0 = WebGPUHost::Now();
	const WGPUInstance instance = dev->GetWGPUInstance();
	const WGPUFuture future = wgpuBufferMapAsync(m_buffer, WGPUMapMode_Read, 0, m_buffer_size, cb);
	const WGPUWaitStatus ws = WebGPUHost::Wait(instance, future, READBACK_TIMEOUT_MS);
	if (ws == WGPUWaitStatus_TimedOut)
	{
		// Cancel the map, so the buffer can be copied into again; the cancelled map's callback is
		// ready at once and runs in the poll that follows. A GPU that does not answer in time is
		// treated as lost (§11.4).
		wgpuBufferUnmap(m_buffer);
		WebGPUHost::Wait(instance, future, 0.0);
		dev->MarkDeviceLost("a readback");
	}
	WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
	if (!req->Take(&status) || status != WGPUMapAsyncStatus_Success)
	{
		// A lost device fails maps; leave the shadow as it was (§11.4).
		Console.ErrorFmt("WebGPU: readback failed (wait {}, map status {}) after {:.1f} ms.", static_cast<int>(ws),
			static_cast<int>(status), WebGPUHost::Now() - t0);
		m_readback_failed = true;
		return;
	}

	wgpuBufferReadMappedRange(m_buffer, 0, m_shadow.get(), m_buffer_size);
	wgpuBufferUnmap(m_buffer);
	m_readback_failed = false;
}

#ifdef PCSX2_DEVBUILD
void GSDownloadTextureWebGPU::SetDebugName(std::string_view name)
{
	if (m_buffer)
		wgpuBufferSetLabel(m_buffer, {name.data(), name.size()});
}
#endif
