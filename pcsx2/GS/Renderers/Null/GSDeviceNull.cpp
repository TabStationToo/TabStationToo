// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/Null/GSDeviceNull.h"

#include "common/Assertions.h"
#include "common/StringUtil.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// ----------------------------------------------------------------------------
// CPU helpers
// ----------------------------------------------------------------------------

namespace
{
	// ClearRenderTarget() only records the clear; GPU backends apply it at the next
	// render pass. Here it is applied to the memory before the texture is read or written.
	void CommitClearNull(GSTextureNull* t)
	{
		if (t->GetState() == GSTexture::State::Cleared)
		{
			if (t->GetFormat() == GSTexture::Format::Color)
			{
				const u32 color = t->GetClearColor();
				for (int y = 0; y < t->GetHeight(); y++)
				{
					u32* row = reinterpret_cast<u32*>(t->GetData() + t->GetOffset(0, y));
					std::fill(row, row + t->GetWidth(), color);
				}
			}
			else
			{
				std::memset(t->GetData(), 0, static_cast<size_t>(t->GetPitch()) * t->GetHeight());
			}
		}

		t->SetState(GSTexture::State::Dirty);
	}

	// Nearest-neighbour resampling of a normalised source rectangle into the pixel rectangle
	// dRect of a destination buffer. keep_row(y) selects which destination rows are written
	// (field-based interlacing), op(dst, src) combines each pixel. RGBA8 only: enough to show
	// what the GS displays, not a renderer.
	template <typename KeepRow, typename PixelOp>
	void Resample(const GSTextureNull* src, const GSVector4& sRect, u32* dst, u32 dst_pitch_px, int dst_w, int dst_h,
		const GSVector4& dRect, KeepRow keep_row, PixelOp op)
	{
		const float dw = dRect.z - dRect.x;
		const float dh = dRect.w - dRect.y;
		const int dx0 = std::max(0, static_cast<int>(dRect.x));
		const int dy0 = std::max(0, static_cast<int>(dRect.y));
		const int dx1 = std::min(dst_w, static_cast<int>(dRect.z));
		const int dy1 = std::min(dst_h, static_cast<int>(dRect.w));
		if (dw <= 0.0f || dh <= 0.0f || dx1 <= dx0 || dy1 <= dy0)
			return;

		const int sw = src->GetWidth();
		const int sh = src->GetHeight();
		const float u0 = sRect.x * static_cast<float>(sw);
		const float v0 = sRect.y * static_cast<float>(sh);
		const float du = (sRect.z - sRect.x) * static_cast<float>(sw) / dw;
		const float dv = (sRect.w - sRect.y) * static_cast<float>(sh) / dh;
		const u8* const src_data = const_cast<GSTextureNull*>(src)->GetData();

		// Unscaled columns at whole-pixel offsets (most blits and presents): with du == 1 and integral u0 and
		// dRect.x well inside float precision, the mapping below is exact and comes down to x + sx_offset.
		constexpr float exact_limit = 4194304.0f; // 2^22: x - dRect.x + 0.5f stays exact
		const bool unscaled_x = (du == 1.0f && u0 == std::floor(u0) && dRect.x == std::floor(dRect.x) &&
								 std::fabs(u0) < exact_limit && std::fabs(dRect.x) < exact_limit);
		const int sx_offset = unscaled_x ? static_cast<int>(u0) - static_cast<int>(dRect.x) : 0;

		for (int y = dy0; y < dy1; y++)
		{
			if (!keep_row(y))
				continue;

			const int sy = std::clamp(static_cast<int>(v0 + (static_cast<float>(y) - dRect.y + 0.5f) * dv), 0, sh - 1);
			const u32* srow = reinterpret_cast<const u32*>(src_data + src->GetOffset(0, sy));
			u32* drow = dst + static_cast<size_t>(y) * dst_pitch_px;
			if (unscaled_x)
			{
				for (int x = dx0; x < dx1; x++)
				{
					const int sx = std::clamp(x + sx_offset, 0, sw - 1);
					drow[x] = op(drow[x], srow[sx], sy, sx);
				}
				continue;
			}

			for (int x = dx0; x < dx1; x++)
			{
				const int sx = std::clamp(static_cast<int>(u0 + (static_cast<float>(x) - dRect.x + 0.5f) * du), 0, sw - 1);
				drow[x] = op(drow[x], srow[sx], sy, sx);
			}
		}
	}

	bool IsColorNull(GSTexture* t)
	{
		return t && t->GetFormat() == GSTexture::Format::Color;
	}

	template <typename KeepRow, typename PixelOp>
	void ResampleTo(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect, KeepRow keep_row, PixelOp op)
	{
		if (!IsColorNull(sTex) || !IsColorNull(dTex))
			return;

		GSTextureNull* const src = static_cast<GSTextureNull*>(sTex);
		GSTextureNull* const dst = static_cast<GSTextureNull*>(dTex);
		CommitClearNull(src);
		CommitClearNull(dst);
		Resample(src, sRect, reinterpret_cast<u32*>(dst->GetData()), dst->GetPitch() / sizeof(u32), dst->GetWidth(),
			dst->GetHeight(), dRect, keep_row, op);
	}

	constexpr auto ALL_ROWS = [](int) { return true; };
	constexpr auto COPY = [](u32 d, u32 s, int, int) { return s; };

	void BlitColor(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect)
	{
		ResampleTo(sTex, sRect, dTex, dRect, ALL_ROWS, COPY);
	}

	// out = src * a + dst * (1 - a) per channel, a in [0, 255].
	__fi u32 BlendRGBA8(u32 d, u32 s, u32 a, bool keep_dst_alpha)
	{
		u32 out = 0;
		for (u32 shift = 0; shift < 32; shift += 8)
		{
			const u32 sc = (s >> shift) & 0xFFu;
			const u32 dc = (d >> shift) & 0xFFu;
			const u32 c = (shift == 24 && keep_dst_alpha) ? dc : ((sc * a + dc * (255u - a) + 127u) / 255u);
			out |= c << shift;
		}
		return out;
	}
} // namespace

// ----------------------------------------------------------------------------
// GSTextureNull
// ----------------------------------------------------------------------------

GSTextureNull::GSTextureNull(Usage usage, Format format, int width, int height, int levels)
{
	m_usage = usage;
	m_format = format;
	m_size = GSVector2i(width, height);
	m_mipmap_levels = levels;

	// Only the top level is stored; nothing samples the mips without a GPU.
	m_pitch = CalcUploadPitch(static_cast<u32>(width));
	const u32 block_size = GetCompressedBlockSize();
	const u32 rows = (static_cast<u32>(height) + (block_size - 1)) / block_size;
	m_data.resize(static_cast<size_t>(m_pitch) * rows);
}

GSTextureNull::~GSTextureNull() = default;

void* GSTextureNull::GetNativeHandle() const
{
	return const_cast<u8*>(m_data.data());
}

u32 GSTextureNull::GetOffset(int x, int y) const
{
	const u32 block_size = GetCompressedBlockSize();
	return (static_cast<u32>(y) / block_size) * m_pitch + (static_cast<u32>(x) / block_size) * GetCompressedBytesPerBlock();
}

bool GSTextureNull::Update(const GSVector4i& r, const void* data, int pitch, int layer)
{
	if (layer != 0)
		return true;

	const GSVector4i rc = r.rintersect(GetRect());
	if (rc.rempty())
		return true;

	if (m_state != State::Dirty)
		CommitClearNull(this);

	const u32 block_size = GetCompressedBlockSize();
	const u32 row_bytes = CalcUploadPitch(static_cast<u32>(rc.width()));
	const u32 rows = (static_cast<u32>(rc.height()) + (block_size - 1)) / block_size;
	const u8* src = static_cast<const u8*>(data);
	// The source pointer is for r's origin; skip to the clipped origin.
	src += ((rc.y - r.y) / static_cast<int>(block_size)) * pitch +
		   ((rc.x - r.x) / static_cast<int>(block_size)) * static_cast<int>(GetCompressedBytesPerBlock());

	StringUtil::StrideMemCpy(m_data.data() + GetOffset(rc.x, rc.y), m_pitch, src, static_cast<u32>(pitch), row_bytes, rows);
	m_needs_mipmaps_generated = true;
	return true;
}

bool GSTextureNull::Map(GSMap& m, const GSVector4i* r, int layer)
{
	if (layer != 0)
		return false;

	const GSVector4i rc = r ? *r : GetRect();
	m.bits = m_data.data() + GetOffset(rc.x, rc.y);
	m.pitch = static_cast<int>(m_pitch);
	return true;
}

void GSTextureNull::Unmap()
{
	m_needs_mipmaps_generated = true;
}

void GSTextureNull::GenerateMipmap()
{
}

#ifdef PCSX2_DEVBUILD
void GSTextureNull::SetDebugName(std::string_view name)
{
	m_debug_name = name;
}
#endif

// ----------------------------------------------------------------------------
// GSDownloadTextureNull
// ----------------------------------------------------------------------------

GSDownloadTextureNull::GSDownloadTextureNull(u32 width, u32 height, GSTexture::Format format)
	: GSDownloadTexture(width, height, format)
{
	m_current_pitch = GetTransferPitch(width, 1);
	m_buffer.resize(GetBufferSize(width, height, format, 1));
}

GSDownloadTextureNull::~GSDownloadTextureNull() = default;

void GSDownloadTextureNull::CopyFromTexture(
	const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level, bool use_transfer_pitch)
{
	GSTextureNull* const tex = static_cast<GSTextureNull*>(stex);
	pxAssert(tex->GetFormat() == m_format);
	pxAssert(drc.width() == src.width() && drc.height() == src.height());

	// Keep the full-width pitch, so any rectangle reads back at its own offset.
	u32 copy_offset, copy_size, copy_rows;
	GetTransferSize(drc, &copy_offset, &copy_size, &copy_rows);
	if (src_level == 0)
	{
		StringUtil::StrideMemCpy(m_buffer.data() + copy_offset, m_current_pitch, tex->GetData() + tex->GetOffset(src.x, src.y),
			tex->GetPitch(), copy_size, copy_rows);
	}

	m_needs_flush = false;
}

bool GSDownloadTextureNull::Map(const GSVector4i& read_rc)
{
	m_map_pointer = m_buffer.data();
	return true;
}

void GSDownloadTextureNull::Unmap()
{
	m_map_pointer = nullptr;
}

void GSDownloadTextureNull::Flush()
{
	m_needs_flush = false;
}

#ifdef PCSX2_DEVBUILD
void GSDownloadTextureNull::SetDebugName(std::string_view name)
{
}
#endif

// ----------------------------------------------------------------------------
// GSDeviceNull
// ----------------------------------------------------------------------------

static GSDeviceNull::PresentCallback s_present_callback;

void GSDeviceNull::SetPresentCallback(PresentCallback callback)
{
	s_present_callback = std::move(callback);
}

GSDeviceNull::GSDeviceNull() = default;

GSDeviceNull::~GSDeviceNull() = default;

bool GSDeviceNull::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	if (!GSDevice::Create(vsync_mode, allow_present_throttle))
		return false;

	// No GPU features at all: the HW renderer is never used with this device.
	m_features = FeatureSupport();
	m_max_texture_size = 8192;

	// The host decides the size frames are presented at; default to 640x480 when it has no
	// opinion, so the OSD layout and the presented frame always have a size.
	if (!AcquireWindow(false) || m_window_info.surface_width == 0 || m_window_info.surface_height == 0)
	{
		m_window_info.type = WindowInfo::Type::Surfaceless;
		m_window_info.surface_width = 640;
		m_window_info.surface_height = 480;
		m_window_info.surface_scale = 1.0f;
	}

	return true;
}

void GSDeviceNull::Destroy()
{
	GSDevice::Destroy();
}

RenderAPI GSDeviceNull::GetRenderAPI() const
{
	return RenderAPI::None;
}

bool GSDeviceNull::HasSurface() const
{
	return false;
}

void GSDeviceNull::DestroySurface()
{
}

bool GSDeviceNull::UpdateWindow()
{
	return true;
}

void GSDeviceNull::ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
}

bool GSDeviceNull::SupportsExclusiveFullscreen() const
{
	return false;
}

GSDevice::PresentResult GSDeviceNull::BeginPresent(bool frame_skip)
{
	// Without a host callback there is nothing to present to.
	if (frame_skip || !s_present_callback)
		return PresentResult::FrameSkipped;

	const size_t pixels = static_cast<size_t>(m_window_info.surface_width) * m_window_info.surface_height;
	m_present_buffer.assign(pixels, 0xFF000000u); // opaque black
	m_presenting = true;
	return PresentResult::OK;
}

void GSDeviceNull::EndPresent()
{
	// GPU backends end the ImGui frame when they draw it (ImGui::Render()); the OSD is not
	// drawn here, but the frame must still end before GSRenderer starts the next one.
	ImGui::EndFrame();

	if (!m_presenting)
		return;

	m_presenting = false;
	s_present_callback(m_present_buffer.data(), m_window_info.surface_width, m_window_info.surface_height);
}

void GSDeviceNull::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_vsync_mode = mode;
	m_allow_present_throttle = allow_present_throttle;
}

std::string GSDeviceNull::GetDriverInfo() const
{
	return "Null device (no GPU, textures in memory)";
}

bool GSDeviceNull::SetGPUTimingEnabled(bool enabled)
{
	return false;
}

float GSDeviceNull::GetAndResetAccumulatedGPUTime()
{
	return 0.0f;
}

bool GSDeviceNull::SetGPUPipelineStatisticsEnabled(bool enabled)
{
	return false;
}

GPUPipelineStatistics GSDeviceNull::GetAndResetAccumulatedGPUPipelineStatistics()
{
	return {};
}

void GSDeviceNull::PushDebugGroup(const char* fmt, ...)
{
}

void GSDeviceNull::PopDebugGroup()
{
}

void GSDeviceNull::InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...)
{
}

GSTexture* GSDeviceNull::CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format)
{
	return new GSTextureNull(usage, format, width, height, levels);
}

std::unique_ptr<GSDownloadTexture> GSDeviceNull::CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format)
{
	return std::make_unique<GSDownloadTextureNull>(width, height, format);
}

void GSDeviceNull::CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY)
{
	GSTextureNull* const src = static_cast<GSTextureNull*>(sTex);
	GSTextureNull* const dst = static_cast<GSTextureNull*>(dTex);
	if (src->GetFormat() != dst->GetFormat())
		return;

	// Clip against both textures.
	const GSVector4i src_rc = r.rintersect(src->GetRect());
	const GSVector4i dst_rc = GSVector4i(destX, destY, destX + src_rc.width(), destY + src_rc.height()).rintersect(dst->GetRect());
	if (src_rc.rempty() || dst_rc.rempty())
		return;

	CommitClearNull(src);
	CommitClearNull(dst);

	const u32 block_size = src->GetCompressedBlockSize();
	const u32 row_bytes = src->CalcUploadPitch(static_cast<u32>(dst_rc.width()));
	const u32 rows = (static_cast<u32>(dst_rc.height()) + (block_size - 1)) / block_size;
	StringUtil::StrideMemCpy(dst->GetData() + dst->GetOffset(dst_rc.x, dst_rc.y), dst->GetPitch(),
		src->GetData() + src->GetOffset(src_rc.x, src_rc.y), src->GetPitch(), row_bytes, rows);
}

void GSDeviceNull::PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	PresentShader shader, float shaderTime, Filter filter)
{
	if (dTex)
	{
		BlitColor(sTex, sRect, dTex, dRect);
		return;
	}

	// Into the presented frame. TV shaders and filtering are not applied.
	if (!m_presenting || !IsColorNull(sTex))
		return;

	GSTextureNull* const src = static_cast<GSTextureNull*>(sTex);
	CommitClearNull(src);
	Resample(src, sRect, m_present_buffer.data(), m_window_info.surface_width, static_cast<int>(m_window_info.surface_width),
		static_cast<int>(m_window_info.surface_height), dRect, ALL_ROWS,
		[](u32 d, u32 s, int, int) { return s | 0xFF000000u; });
}

void GSDeviceNull::DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderConvertSelector shader, Filter filter)
{
	// Other convert shaders (depth, palette, colclip...) only matter for the HW renderer.
	if (shader.Shader() == ShaderConvert::TRANSPARENCY_FILTER)
		ResampleTo(sTex, sRect, dTex, dRect, ALL_ROWS, [](u32 d, u32 s, int, int) { return s | 0xFF000000u; });
	else
		BlitColor(sTex, sRect, dTex, dRect);
}

void GSDeviceNull::UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex,
	u32 dOffset, u32 dSize)
{
}

void GSDeviceNull::ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW,
	u32 SPSM, GSTexture* dTex, u32 DBW, u32 DPSM)
{
}

void GSDeviceNull::FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
	const GSVector2i& clamp_min, const GSVector4& dRect)
{
}

void GSDeviceNull::RenderHW(GSHWDrawConfig& config)
{
}

void GSDeviceNull::ClearSamplerCache()
{
}

void GSDeviceNull::DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect,
	const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter)
{
	if (!IsColorNull(dTex))
		return;

	// Same order as the GPU backends (see GSDeviceOGL::DoMerge): background colour, circuit 2
	// copied unless SLBG selects the background, then circuit 1 blended over it with either
	// the constant ALP (MMOD=1, carried in c's alpha byte) or twice its own alpha. The EXTBUF
	// feedback write-back is not done; needs proper testing.
	GSTextureNull* const dst = static_cast<GSTextureNull*>(dTex);
	dst->SetClearColor(c);
	CommitClearNull(dst);

	if (sTex[1] && PMODE.SLBG == 0)
		BlitColor(sTex[1], sRect[1], dTex, dRect[1]);

	if (sTex[0])
	{
		const bool keep_dst_alpha = (PMODE.AMOD == 1);
		if (PMODE.MMOD == 1)
		{
			const u32 a = c >> 24;
			ResampleTo(sTex[0], sRect[0], dTex, dRect[0], ALL_ROWS,
				[a, keep_dst_alpha](u32 d, u32 s, int, int) { return BlendRGBA8(d, s, a, keep_dst_alpha); });
		}
		else
		{
			ResampleTo(sTex[0], sRect[0], dTex, dRect[0], ALL_ROWS, [keep_dst_alpha](u32 d, u32 s, int, int) {
				const u32 a = std::min<u32>((s >> 24) * 2u, 255u);
				return BlendRGBA8(d, s, a, keep_dst_alpha);
			});
		}
	}
}

void GSDeviceNull::DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
	ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb)
{
	// CPU versions of the interlace shaders (bin/resources/shaders/*/interlace.*).
	const int idx = static_cast<int>(cb.ZrH.x);
	const int field = idx & 1;
	switch (shader)
	{
		case ShaderInterlace::WEAVE:
			// Only the current field's lines; the others keep the previous field.
			ResampleTo(sTex, sRect, dTex, dRect, [field](int y) { return (y & 1) == field; }, COPY);
			break;

		case ShaderInterlace::MAD_BUFFER:
		{
			// Each bank of the MAD buffer interleaves both fields; update the current field's lines.
			const int bank = idx >> 1;
			const int vres = static_cast<int>(cb.ZrH.z) >> 1;
			const int lofs = ((((vres + 1) >> 1) << 1) - vres) & bank;
			ResampleTo(sTex, sRect, dTex, dRect, [field, lofs](int y) { return ((y + lofs) & 1) == field; }, COPY);
			break;
		}

		case ShaderInterlace::MAD_RECONSTRUCT:
		{
			// The shader's static path: the current field's lines from the current bank, the
			// missing lines from the bank it calls p_t1 (the previous field), i.e. a weave of the
			// two newest fields. Motion detection is not done.
			const int bank = idx >> 1;
			const int prev_bank = bank ^ (field ^ 1);
			const auto bank_rect = [&sRect](int b) {
				return GSVector4(sRect.x, static_cast<float>(b) * 0.5f, sRect.z, static_cast<float>(b) * 0.5f + 0.5f);
			};
			ResampleTo(sTex, bank_rect(bank), dTex, dRect, [field](int y) { return (y & 1) == field; }, COPY);
			ResampleTo(sTex, bank_rect(prev_bank), dTex, dRect, [field](int y) { return (y & 1) != field; }, COPY);
			break;
		}

		case ShaderInterlace::BLEND:
			// Vertical 1-2-1 filter over neighbouring source lines.
			if (IsColorNull(sTex))
			{
				const GSTextureNull* const src = static_cast<GSTextureNull*>(sTex);
				const u8* const data = const_cast<GSTextureNull*>(src)->GetData();
				const int last_row = src->GetHeight() - 1;
				ResampleTo(sTex, sRect, dTex, dRect, ALL_ROWS, [src, data, last_row](u32 d, u32 s, int sy, int sx) {
					const u32 up = reinterpret_cast<const u32*>(data + src->GetOffset(0, std::max(sy - 1, 0)))[sx];
					const u32 down = reinterpret_cast<const u32*>(data + src->GetOffset(0, std::min(sy + 1, last_row)))[sx];
					u32 out = 0;
					for (u32 shift = 0; shift < 32; shift += 8)
					{
						const u32 v = ((up >> shift) & 0xFFu) + 2u * ((s >> shift) & 0xFFu) + ((down >> shift) & 0xFFu);
						out |= ((v + 2u) / 4u) << shift;
					}
					return out;
				});
			}
			break;

		case ShaderInterlace::BOB:
		default:
			BlitColor(sTex, sRect, dTex, dRect);
			break;
	}
}

void GSDeviceNull::DoFXAA(GSTexture* sTex, GSTexture* dTex)
{
}

void GSDeviceNull::DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4])
{
}

bool GSDeviceNull::DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only,
	const std::array<u32, NUM_CAS_CONSTANTS>& constants)
{
	return false;
}
