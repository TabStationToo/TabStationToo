// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSTexture.h"

#include <webgpu/webgpu.h>

#include <array>
#include <memory>
#include <vector>

class GSDeviceWebGPU;

// docs/web-port/WEBGPU-DESIGN.md §6.1-6.3. There are no image layouts in WebGPU, so unlike GSTextureVK
// this only tracks which encoder last used the texture: an upload may go straight to the queue
// (queue.writeTexture, which runs before the encoder still being recorded) only when that encoder
// has not used the texture yet; otherwise it is recorded in the encoder.
class GSTextureWebGPU final : public GSTexture
{
public:
	/// Bind groups of the utility layout (group 1: texture + sampler), cached per texture.
	enum class BindGroupKind : u8
	{
		Point,
		Linear,
		Unfilterable, ///< unfilterable-float view (depth aspect or r32float) with a non-filtering sampler
		Count
	};

	/// bytesPerRow of buffer <-> texture copies must be a multiple of this.
	static constexpr u32 COPY_PITCH_ALIGNMENT = 256;

	~GSTextureWebGPU() override;

	static std::unique_ptr<GSTextureWebGPU> Create(Usage usage, Format format, int width, int height, int levels,
		WGPUTextureFormat native_format);

	void* GetNativeHandle() const override;

	bool Update(const GSVector4i& r, const void* data, int pitch, int layer = 0) override;
	bool Map(GSMap& m, const GSVector4i* r = nullptr, int layer = 0) override;
	void Unmap() override;
	void GenerateMipmap() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif

	__fi WGPUTexture GetTexture() const { return m_texture; }
	__fi WGPUTextureView GetRTView() const { return m_rt_view; }
	__fi WGPUTextureFormat GetNativeFormat() const { return m_native_format; }
	__fi u64 GetUseSerial() const { return m_use_serial; }
	__fi void SetUseSerial(u64 serial) { m_use_serial = serial; }

	/// Applies a pending clear (records a clear pass); afterwards the texture is Dirty.
	void CommitClear();

	/// The utility bind group (group 1) for sampling this texture.
	WGPUBindGroup GetUtilityBindGroup(BindGroupKind kind);

	/// Bytes per row of a level-0 row of width texels, rounded up to the 256 bytes copies need.
	u32 GetCopyPitch(u32 width) const;

private:
	GSTextureWebGPU(Usage usage, Format format, int width, int height, int levels, WGPUTexture texture,
		WGPUTextureView view, WGPUTextureView rt_view, WGPUTextureFormat native_format);

	bool UploadRect(const GSVector4i& r, const void* data, u32 pitch, int layer);

	WGPUTexture m_texture = nullptr;
	WGPUTextureView m_view = nullptr;
	WGPUTextureView m_rt_view = nullptr;
	WGPUTextureFormat m_native_format = WGPUTextureFormat_Undefined;
	std::array<WGPUBindGroup, static_cast<size_t>(BindGroupKind::Count)> m_bind_groups = {};
	u64 m_use_serial = 0;

	// Map()/Unmap(): rows at the 256-byte copy pitch, so a late upload copies them as they are.
	std::vector<u8> m_map_buffer;
	GSVector4i m_map_area = GSVector4i::zero();
	int m_map_level = 0;
	u32 m_map_pitch = 0;
};

// docs/web-port/WEBGPU-DESIGN.md §11.2 (basic version for screenshots): a MapRead buffer at the
// 256-byte copy pitch and a CPU shadow. Flush() submits if the copy is still in the open encoder,
// maps the buffer and waits for it with JSPI, then copies it into the shadow and unmaps it at once.
class GSDownloadTextureWebGPU final : public GSDownloadTexture
{
public:
	~GSDownloadTextureWebGPU() override;

	static std::unique_ptr<GSDownloadTextureWebGPU> Create(u32 width, u32 height, GSTexture::Format format);

	void CopyFromTexture(const GSVector4i& drc, GSTexture* stex, const GSVector4i& src, u32 src_level,
		bool use_transfer_pitch = true) override;

	bool Map(const GSVector4i& read_rc) override;
	void Unmap() override;
	void Flush() override;

#ifdef PCSX2_DEVBUILD
	void SetDebugName(std::string_view name) override;
#endif

private:
	GSDownloadTextureWebGPU(u32 width, u32 height, GSTexture::Format format, WGPUBuffer buffer, u32 buffer_size);

	WGPUBuffer m_buffer = nullptr;
	u32 m_buffer_size = 0;
	std::unique_ptr<u8[]> m_shadow;
	u64 m_copy_serial = 0;
	bool m_readback_failed = false; // the last Flush() got no data: Map() fails until a copy succeeds
};
