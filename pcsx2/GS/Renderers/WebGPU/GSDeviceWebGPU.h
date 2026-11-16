// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/WebGPU/GSTextureWebGPU.h"
#include "GS/Renderers/WebGPU/WebGPUCaches.h"
#include "GS/Renderers/WebGPU/WebGPUHost.h"
#include "GS/Renderers/WebGPU/WebGPUStreamBuffer.h"

#include <webgpu/webgpu.h>

#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

// The WebGPU GS device (docs/web-port/WEBGPU-DESIGN.md). Step 1 (§15): the utility passes on the GPU
// behind the software renderer: uploads, copies, StretchRect and the convert shaders with colour or
// r32float targets, merge, interlace, shadeboost, present and ImGui, into an OffscreenCanvas whose
// frames are posted to the page. RenderHW and the other HW-renderer paths are stubs, and the
// feature profile reports no HW features, because the HW renderer cannot select this device yet.
//
// Threading: everything runs on the GS thread, which owns the WebGPU objects (they live in its
// worker). Waits for WebGPU (device creation, readbacks) suspend that thread with JSPI, and while the
// device exists its idle wait for work suspends too (Threading::SetWaitsSuspend), so the worker's
// event loop delivers WebGPU callbacks. Callbacks only set flags or fill in heap-allocated results
// (WebGPUHost::RequestResult); recovery runs on the GS thread.
class GSDeviceWebGPU final : public GSDevice
{
public:
	using BindGroupKind = GSTextureWebGPU::BindGroupKind;

	GSDeviceWebGPU();
	~GSDeviceWebGPU() override;

	__fi static GSDeviceWebGPU* GetInstance() { return static_cast<GSDeviceWebGPU*>(g_gs_device.get()); }

	// ---- GSDevice ----

	bool Create(GSVSyncMode vsync_mode, bool allow_present_throttle) override;
	void Destroy() override;

	RenderAPI GetRenderAPI() const override;
	bool HasSurface() const override;
	void DestroySurface() override;
	bool UpdateWindow() override;
	void ResizeWindow(u32 new_window_width, u32 new_window_height, float new_window_scale) override;
	bool SupportsExclusiveFullscreen() const override;

	PresentResult BeginPresent(bool frame_skip) override;
	void EndPresent() override;
	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle) override;

	std::string GetDriverInfo() const override;

	bool SetGPUTimingEnabled(bool enabled) override;
	float GetAndResetAccumulatedGPUTime() override;
	bool SetGPUPipelineStatisticsEnabled(bool enabled) override;
	GPUPipelineStatistics GetAndResetAccumulatedGPUPipelineStatistics() override;

	void PushDebugGroup(const char* fmt, ...) override;
	void PopDebugGroup() override;
	void InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...) override;

	std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(u32 width, u32 height, GSTexture::Format format) override;

	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r, u32 destX, u32 destY) override;

	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		PresentShader shader, float shaderTime, Filter filter) override;
	void DrawMultiStretchRects(const MultiStretchRect* rects, u32 num_rects, GSTexture* dTex,
		ShaderConvertSelector shader) override;

	void UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, GSTexture* dTex, u32 dOffset,
		u32 dSize) override;
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX, u32 offsetY, u32 SBW, u32 SPSM,
		GSTexture* dTex, u32 DBW, u32 DPSM) override;
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex, u32 downsample_factor,
		const GSVector2i& clamp_min, const GSVector4& dRect) override;

	void RenderHW(GSHWDrawConfig& config) override;
	void ClearSamplerCache() override;

	// ---- for GSTextureWebGPU and GSDownloadTextureWebGPU ----

	__fi WGPUDevice GetDevice() const { return m_device; }
	__fi WGPUQueue GetQueue() const { return m_queue; }
	__fi WGPUInstance GetWGPUInstance() const { return m_instance; }
	/// Serial of the command encoder being recorded; incremented by every submit.
	__fi u64 GetEncoderSerial() const { return m_encoder_serial; }

	/// A wait for the GPU timed out: handled as a device loss at the next present (§13).
	void MarkDeviceLost(const char* what);

	WGPUTextureFormat LookupNativeFormat(GSTexture::Format format) const;

	WGPUCommandEncoder GetCommandEncoder();
	/// Ends the open render pass. A present pass is reopened (loading what it drew) at its next draw.
	void EndRenderPass();
	/// Submits the commands recorded so far (uploads the stream buffers first).
	void ExecuteCommandBuffer(const char* reason);

	void CommitClear(GSTextureWebGPU* tex);
	/// Records an upload of rows at the 256-byte copy pitch into the encoder ("late" upload).
	bool UploadTextureLate(GSTextureWebGPU* tex, int level, const GSVector4i& rect, const void* data, u32 pitch);
	/// Large uploads (the software renderer's output, every frame): copies the rows into a staging
	/// buffer that is already mapped and records a copy into the texture. False when the upload is
	/// small or no staging buffer is mapped: the caller uses the queue or the upload ring instead.
	bool UploadTextureStaged(GSTextureWebGPU* tex, int level, const GSVector4i& rect, const void* data, u32 pitch);
	WGPUBindGroup CreateUtilityBindGroup(WGPUTextureView view, BindGroupKind kind);
	/// The texture is going away: drop cached state that refers to it, and keep its WebGPU objects
	/// until the open encoder is submitted if that encoder used it.
	void OnTextureDestroyed(GSTextureWebGPU* tex, WGPUTexture texture, std::initializer_list<WGPUTextureView> views,
		std::initializer_list<WGPUBindGroup> groups);

protected:
	GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height, int levels, GSTexture::Format format) override;

	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex, GSVector4* dRect, const GSRegPMODE& PMODE,
		const GSRegEXTBUF& EXTBUF, u32 c, const Filter filter) override;
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderInterlace shader, Filter filter, const InterlaceConstantBuffer& cb) override;
	void DoFXAA(GSTexture* sTex, GSTexture* dTex) override;
	void DoShadeBoost(GSTexture* sTex, GSTexture* dTex, const float params[4]) override;
	bool DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only,
		const std::array<u32, NUM_CAS_CONSTANTS>& constants) override;

	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex, const GSVector4& dRect,
		ShaderConvertSelector shader, Filter filter) override;
	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect, const GSVector4& dRect, PresentShader shader,
		Filter filter) override;

private:
	enum class TexLayout : u8
	{
		Float, ///< float texture, filtering sampler
		Unfilterable, ///< unfilterable-float texture (depth, r32float), non-filtering sampler
		Count
	};

	/// Everything a utility pipeline depends on.
	struct UtilityPipelineKey
	{
		u8 module; ///< UtilityModule
		u8 layout; ///< TexLayout
		u8 write_mask;
		u8 blend; ///< UtilityBlend
		u16 entry; ///< index into m_entry_names
		u16 constants; ///< bit 0: HAS_BILN
		WGPUTextureFormat color_format; ///< Undefined: no colour target
		WGPUTextureFormat ds_format; ///< Undefined: no depth-stencil attachment
		u8 ds_mode; ///< UtilityDepthMode
		u8 pad[3];

		bool operator==(const UtilityPipelineKey& rhs) const;
	};
	struct UtilityPipelineKeyHash
	{
		size_t operator()(const UtilityPipelineKey& k) const;
	};

	enum UtilityModule : u8
	{
		MODULE_CONVERT,
		MODULE_PRESENT,
		MODULE_MERGE,
		MODULE_INTERLACE,
		MODULE_SHADEBOOST,
		MODULE_IMGUI,
		MODULE_COUNT
	};

	enum UtilityDepthMode : u8
	{
		DS_NONE, ///< no depth-stencil state
		DS_WRITE_DEPTH, ///< frag_depth output, compare always, depth written (the convert *_ds entry points)
		DS_WRITE_STENCIL, ///< DATM: no colour, stencil replaced with 1 where the fragment is kept
		DS_IGNORE, ///< depth-stencil attachment present but neither tested nor written
	};

	enum UtilityBlend : u8
	{
		BLEND_NONE,
		BLEND_MERGE, ///< src alpha / one minus src alpha on colour, alpha replaced (GSDeviceVK m_merge)
		BLEND_IMGUI, ///< the same as merge, as GSDeviceVK's ImGui pipeline
		BLEND_MULTIPLY, ///< destination * source (the glow passes fading the accumulated picture)
		BLEND_MAX, ///< max(destination, source) (the glow passes taking the new picture)
	};

	using PipelineRequest = WebGPUHost::RequestResult<WGPURenderPipeline>;
	struct PendingPipeline
	{
		UtilityPipelineKey key;
		WGPUFuture future;
		PipelineRequest* request; ///< taken (or abandoned) by WaitForPipelines()
	};

	/// A MapWrite buffer for UploadTextureStaged(), mapped again after each submit that used it.
	struct StagingBuffer
	{
		WGPUBuffer buffer;
		u32 size;
		u64 used_serial; ///< encoder that copies from it (0 = none since it was last mapped)
		bool fresh; ///< still mapped at creation (through webgpu.h); JS maps it from then on
	};

	// Creation
	bool CreateInstanceAndDevice();
	bool CreateSurfaceForWindow();
	bool ConfigureSurface();
	void ClampSurfaceSize(u32* width, u32* height) const;
	bool CheckFeatures();
	bool CreateBuffers();
	bool CreateLayoutsAndSamplers();
	bool LoadShaderModules();
	bool CompileUtilityPipelines();
	void DestroyResources();
	void RemapStagingBuffers(u64 submitted_serial);
	void DestroyStagingBuffers();

	WGPUShaderModule CreateShaderModule(const char* name, const std::string& source);
	PendingPipeline CreatePipelineAsync(const UtilityPipelineKey& key);
	bool WaitForPipelines(std::vector<PendingPipeline>& pending, double timeout_ms);
	/// The pipeline for a key, created (and waited for) on first use. Null if creation failed.
	WGPURenderPipeline GetUtilityPipeline(const UtilityPipelineKey& key);
	UtilityPipelineKey MakeKey(UtilityModule module, const char* entry, WGPUTextureFormat format,
		TexLayout layout = TexLayout::Float, u8 write_mask = 0xf, UtilityBlend blend = BLEND_NONE, u16 constants = 0,
		WGPUTextureFormat ds_format = WGPUTextureFormat_Undefined, UtilityDepthMode ds_mode = DS_NONE);
	/// Convert pipeline key, or a key with module MODULE_COUNT for selectors step 1 does not draw.
	UtilityPipelineKey ConvertPipelineKey(ShaderConvertSelector shader, WGPUTextureFormat format);
	u16 EntryIndex(const char* name);

	// Recording
	WGPULoadOp LoadOpFor(GSTextureWebGPU* tex, GSVector4* clear_color) const;
	void BeginRenderPass(GSTextureWebGPU* rt, WGPULoadOp load_op, const GSVector4& clear_color);
	/// A pass with an optional colour and an optional depth-stencil attachment, load ops from their
	/// clear state (§6.2). stencil_clear >= 0 clears the stencil to that value; colour_load_override
	/// replaces the colour load op when not Undefined.
	void BeginAttachmentPass(GSTextureWebGPU* rt, GSTextureWebGPU* ds, int stencil_clear = -1,
		WGPULoadOp colour_load_override = WGPULoadOp_Undefined);
	/// True when the open pass draws exactly into rt with no depth attachment (utility passes reuse it).
	bool IsColorOnlyPass(GSTextureWebGPU* rt) const { return m_pass && !m_pass_is_present && m_pass_rt == rt && !m_pass_ds; }
	void BeginPresentPass(WGPULoadOp load_op);
	void EnsurePresentPass();
	void InvalidatePassState();
	/// Prepares a texture to be sampled by the next draw: applies a pending clear (outside a pass).
	void PrepareForSampling(GSTextureWebGPU* tex);
	void SetPipeline(WGPURenderPipeline pipeline);
	void SetUtilityTexture(GSTextureWebGPU* tex, BindGroupKind kind);
	void SetUtilityUniforms(const void* data, u32 size);
	/// Reserves ring space for a draw, submitting and reopening the pass when a ring is full, then
	/// applies the wanted pipeline, bind groups and buffers. False if there is nothing to draw with.
	bool ApplyUtilityState(u32 vertex_bytes, u32 vertex_stride, u32 index_bytes);
	bool DrawStretchRect(const GSVector4& sRect, const GSVector4& dRect, const GSVector2i& ds);
	void DoUtilityStretchRect(GSTextureWebGPU* sTex, const GSVector4& sRect, GSTextureWebGPU* dTex,
		const GSVector4& dRect, WGPURenderPipeline pipeline, BindGroupKind kind, bool allow_discard);
	void RenderImGui();

	// HW renderer (GSDeviceWebGPUHW.cpp)
	struct TFXPipelineKey
	{
		GSHWDrawConfig::PSSelector ps;
		u32 blend; ///< BlendState key
		u8 vs; ///< VSSelector key
		u8 dss; ///< DepthStencilSelector key
		u8 cms; ///< ColorMaskSelector key
		u8 topology;
		WGPUTextureFormat rt_format;
		WGPUTextureFormat ds_format;
		u32 pad;

		TFXPipelineKey() { std::memset(static_cast<void*>(this), 0, sizeof(*this)); }
		bool operator==(const TFXPipelineKey& rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
	};
	struct TFXPipelineKeyHash
	{
		size_t operator()(const TFXPipelineKey& k) const;
	};
	struct TFXGroupKey
	{
		WGPUTextureView views[3];
		WGPUSampler sampler;
		bool operator==(const TFXGroupKey& rhs) const { return std::memcmp(this, &rhs, sizeof(*this)) == 0; }
	};
	struct TFXGroupKeyHash
	{
		size_t operator()(const TFXGroupKey& k) const;
	};

	bool CreateHWResources();
	void DestroyHWResources();
	WGPUShaderModule GetTFXVertexModule(GSHWDrawConfig::VSSelector sel);
	WGPUShaderModule GetTFXFragmentModule(const GSHWDrawConfig::PSSelector& sel);
	/// A TFX pipeline being created (§8.4): asked for by a draw, or prewarmed from the game's stored list.
	struct PendingTFXPipeline
	{
		WGPUFuture future;
		PipelineRequest* request; ///< null once taken or dropped (or if creation could not start)
		double started; ///< WebGPUHost::Now() when it was started
		bool prewarm; ///< started from the stored list, not for a draw
		bool logged; ///< a draw asked for it (its key is in the key log)
		bool skipped; ///< a draw was skipped because it was not ready
	};
	struct TFXPipeline
	{
		WGPURenderPipeline pipeline; ///< null if creation failed (its draws are skipped)
		bool logged;
	};
	/// The pipelines of a draw's passes, out[i] for keys[i] (null for one that failed). False when one
	/// is still being created after a bounded wait: the draw is skipped.
	bool GetTFXPipelines(const TFXPipelineKey* keys, u32 count, WGPURenderPipeline* out);
	PendingTFXPipeline StartTFXPipeline(const TFXPipelineKey& key, bool prewarm);
	/// If its creation finished, moves the result into m_tfx_pipelines (unless a prewarm failed) and
	/// returns true (the caller erases it from m_tfx_pending).
	bool TakeTFXPipeline(const TFXPipelineKey& key, PendingTFXPipeline& p);
	void DropTFXPending(PendingTFXPipeline& p);
	/// Once per vsync: collects finished creations and refills the wait budget.
	void PollTFXPipelines();
	/// Once per vsync: takes a list the page handed over and starts a few of its pipelines.
	void PrewarmTFXPipelines();
	void LogTFXKey(const TFXPipelineKey& key);
	bool IsUsableStoredTFXKey(const TFXPipelineKey& key) const;
	u64 ComputeTFXKeyVersion() const;
	static std::string TFXPipelineLabel(const TFXPipelineKey& key);
	WGPUBindGroup GetTFXGroup(u32 group, const TFXGroupKey& key);
	void DropTFXGroupsUsing(std::initializer_list<WGPUTextureView> views);
	void SetupDATE(GSTextureWebGPU* rt, GSTextureWebGPU* ds, SetDATM datm, const GSVector4i& bbox);
	/// Binds pipeline, groups, buffers and uniforms for a TFX draw in the open pass.
	bool ApplyTFXState(WGPURenderPipeline pipeline, const GSHWDrawConfig& config, WGPUBindGroup group1, WGPUBindGroup group2);
	void DrawTFX(const GSHWDrawConfig& config, u32 offset, u32 count);
	void SendHWDraw(GSHWDrawConfig& config, WGPURenderPipeline pipeline, GSTextureWebGPU* draw_rt, GSTextureWebGPU* draw_ds,
		GSTextureWebGPU* rt_clone, GSTextureWebGPU* ds_clone, bool one_barrier, bool full_barrier, int stencil_clear);
	void FeedbackCopy(const GSHWDrawConfig& config, GSTextureWebGPU* rt, GSTextureWebGPU* rt_clone, GSTextureWebGPU* ds,
		GSTextureWebGPU* ds_clone, const GSVector4i& area);
	TFXPipelineKey MakeTFXKey(const GSHWDrawConfig& config, const GSHWDrawConfig::PSSelector& ps,
		GSHWDrawConfig::BlendState bs, GSHWDrawConfig::ColorMaskSelector cms, GSHWDrawConfig::DepthStencilSelector dss,
		GSTextureWebGPU* rt, GSTextureWebGPU* ds) const;
	WGPUBindGroup MakeTFXGroup1(const GSHWDrawConfig& config, GSTextureWebGPU* tex);
	/// Replaces the open pass's bind state caches after a utility draw in the same pass (and back).
	void InvalidateBindState();

	// Feedback draws as compute dispatches (GSDeviceWebGPUHW.cpp, TryComputeFeedbackDraw())
	/// Two textures of one target size that a run of dispatches reads and writes in turn, with the bind
	/// groups that read texture i (group 2) and write texture i (group 3).
	struct ComputeScratch
	{
		GSVector2i size;
		WGPUTexture tex[2];
		WGPUTextureView view[2];
		WGPUBindGroup read_group[2];
		WGPUBindGroup write_group[2];
	};
	bool CreateComputeFeedbackResources();
	void DestroyComputeFeedbackResources();
	WGPUComputePipeline GetTFXComputePipeline(const GSHWDrawConfig::PSSelector& sel);
	ComputeScratch* GetComputeScratch(const GSVector2i& size);
	/// Runs a feedback draw that reads only its own pixel as one dispatch of the open compute run on rt
	/// (starting one), instead of a copy and a render pass. False when the draw does not qualify.
	bool TryComputeFeedbackDraw(GSHWDrawConfig& config, GSTextureWebGPU* rt);
	/// Ends the compute run, if one is open, and copies its result into its target. Called before the
	/// encoder records anything else (GetCommandEncoder()), and before a submit.
	void FlushComputeRun();

	// Device loss and errors
	void ReportPendingErrors();
	void KeepEventLoopTurning();

	// Context
	WGPUInstance m_instance = nullptr;
	WGPUAdapter m_adapter = nullptr;
	WGPUDevice m_device = nullptr;
	WGPUQueue m_queue = nullptr;
	WGPUSurface m_surface = nullptr;
	WGPUTextureFormat m_surface_format = WGPUTextureFormat_Undefined;
	bool m_surface_configured = false;
	u32 m_surface_width = 0;
	u32 m_surface_height = 0;
	u32 m_page_width = 0; ///< the size the page asked for (the surface may be clamped)
	u32 m_page_height = 0;
	std::string m_adapter_name;
	std::string m_driver_info;
	bool m_has_depth32_stencil8 = false;
	bool m_has_bc = false;
	bool m_has_tier1 = false;

	// Recording state
	WGPUCommandEncoder m_encoder = nullptr;
	WGPURenderPassEncoder m_pass = nullptr;
	bool m_pass_is_present = false;
	u64 m_encoder_serial = 1;
	GSTextureWebGPU* m_pass_rt = nullptr;
	GSVector2i m_pass_size = GSVector2i(0, 0);
	WGPURenderPipeline m_pass_pipeline = nullptr;
	WGPUBindGroup m_pass_texture_group = nullptr;
	u32 m_pass_uniform_offset = UINT32_MAX;
	bool m_pass_vertex_bound = false;
	bool m_pass_index_bound = false;
	GSVector4i m_pass_viewport = GSVector4i::zero();
	GSVector4i m_pass_scissor = GSVector4i::zero();
	std::vector<WGPUTexture> m_pending_destroy_textures;
	std::vector<WGPUTextureView> m_pending_release_views;
	std::vector<WGPUBindGroup> m_pending_release_groups;

	// Presenting
	/// The glow passes of ps_filter_geom_glow (present.wgsl): updates and returns the texture it
	/// samples (accumulated picture | blurred), or null. advance is false for a second
	/// picture of the same frame (a screenshot), which must not fade the accumulated one again.
	GSTextureWebGPU* UpdateGlowBuffers(GSTextureWebGPU* sTex, const GSVector4& sRect, bool advance);
	void ReleaseGlowBuffers();
	GSTextureWebGPU* m_glow_tex = nullptr; ///< 2w x h: accumulated picture, then its blur
	GSTextureWebGPU* m_glow_blur = nullptr; ///< w x h: blurred horizontally

	bool m_presenting = false;
	WGPUTexture m_present_texture = nullptr;
	WGPUTextureView m_present_view = nullptr;
	bool m_present_cleared = false;

	// Buffers
	WebGPUStreamBuffer m_vertex_buffer;
	WebGPUStreamBuffer m_index_buffer;
	WebGPUStreamBuffer m_uniform_buffer;
	WebGPUStreamBuffer m_upload_buffer;
	std::vector<StagingBuffer> m_staging;
	std::vector<u8> m_staging_scratch;

	// Layouts, samplers, shaders, pipelines
	WGPUBindGroupLayout m_utility_cb_bgl = nullptr;
	std::array<WGPUBindGroupLayout, static_cast<size_t>(TexLayout::Count)> m_utility_tex_bgl = {};
	std::array<WGPUPipelineLayout, static_cast<size_t>(TexLayout::Count)> m_utility_layout = {};
	WGPUBindGroup m_utility_cb_bg = nullptr;
	WebGPUSamplerCache m_samplers;
	WGPUSampler m_point_sampler = nullptr;
	WGPUSampler m_linear_sampler = nullptr;
	std::array<WGPUShaderModule, MODULE_COUNT> m_modules = {};
	std::vector<std::string> m_entry_names;
	std::unordered_map<UtilityPipelineKey, WGPURenderPipeline, UtilityPipelineKeyHash> m_pipelines;

	// Wanted utility state, applied by ApplyUtilityState() right before a draw
	WGPURenderPipeline m_want_pipeline = nullptr;
	GSTextureWebGPU* m_want_texture = nullptr;
	BindGroupKind m_want_kind = BindGroupKind::Point;
	alignas(16) u8 m_want_uniforms[96] = {};
	u32 m_want_uniform_size = 0;

	double m_last_event_loop_turn = 0.0;

	// HW renderer state
	GSTextureWebGPU* m_pass_ds = nullptr;
	int m_pass_stencil_clear = -1;
	WGPUBindGroup m_pass_tfx_groups[3] = {};
	u32 m_pass_tfx_offsets[2] = {UINT32_MAX, UINT32_MAX};
	WGPUBuffer m_pass_index_buffer = nullptr;
	int m_pass_blend_constant = -1;
	u32 m_pass_stencil_ref = UINT32_MAX;
	bool m_has_dual_source = false;
	std::string m_tfx_source;
	WGPUBindGroupLayout m_tfx_bgl[3] = {};
	WGPUPipelineLayout m_tfx_layout = nullptr;
	WGPUBindGroup m_tfx_group0 = nullptr;
	WGPUBuffer m_expand_ib = nullptr;
	WGPUTexture m_null_texture = nullptr;
	WGPUTextureView m_null_view = nullptr;
	WGPUBindGroup m_null_group2 = nullptr;
	std::unordered_map<u8, WGPUShaderModule> m_tfx_vs_modules;
	std::unordered_map<GSHWDrawConfig::PSSelector, WGPUShaderModule, GSHWDrawConfig::PSSelectorHash> m_tfx_fs_modules;
	std::unordered_map<TFXPipelineKey, TFXPipeline, TFXPipelineKeyHash> m_tfx_pipelines;
	std::unordered_map<TFXPipelineKey, PendingTFXPipeline, TFXPipelineKeyHash> m_tfx_pending;
	std::vector<WGPUFutureWaitInfo> m_tfx_wait_infos;
	double m_tfx_wait_budget_ms = 0.0;
	u64 m_tfx_key_version = 0;
	std::vector<TFXPipelineKey> m_tfx_prewarm_queue;
	size_t m_tfx_prewarm_next = 0;
	u32 m_tfx_prewarm_in_flight = 0;
	u32 m_tfx_skipped_pending = 0; ///< draws skipped because a pipeline was not ready
	u32 m_tfx_async_ready = 0; ///< pipelines that became ready after a draw gave up waiting
	double m_tfx_async_ms = 0.0; ///< their total creation time
	u32 m_tfx_prewarm_started = 0;
	u32 m_tfx_prewarmed = 0;
	u32 m_tfx_prewarm_failed = 0;
	u32 m_tfx_prewarm_waited = 0; ///< prewarmed pipelines a draw asked for before they were ready
	std::unordered_map<TFXGroupKey, WGPUBindGroup, TFXGroupKeyHash> m_tfx_groups[3];
	GSHWDrawConfig::VSConstantBuffer m_vs_cb_cache;
	GSHWDrawConfig::PSConstantBuffer m_ps_cb_cache;
	u32 m_vs_cb_offset = UINT32_MAX;
	u32 m_ps_cb_offset = UINT32_MAX;
	u64 m_cb_serial = 0;
	u32 m_vertex_start = 0;
	u32 m_index_start = 0;
	u32 m_skipped_draws = 0;
	u64 m_hw_draws = 0;
	u32 m_presents = 0;

	// Feedback draws as compute dispatches
	bool m_cs_enabled = false; ///< EmuCore/GS WebGPUComputeFeedback (default on)
	WGPUBindGroupLayout m_cs_bgl[4] = {};
	WGPUPipelineLayout m_cs_layout = nullptr;
	WGPUBindGroup m_cs_group0 = nullptr; ///< the PS constants (dynamic offset)
	WGPUBindGroup m_cs_group1 = nullptr; ///< no source textures (feedback draws read only the target)
	std::unordered_map<GSHWDrawConfig::PSSelector, WGPUComputePipeline, GSHWDrawConfig::PSSelectorHash> m_cs_pipelines;
	std::vector<std::unique_ptr<ComputeScratch>> m_cs_scratch;
	GSTextureWebGPU* m_cs_rt = nullptr; ///< the target of the open run
	ComputeScratch* m_cs_run = nullptr; ///< its scratch textures; null when no run is open
	WGPUComputePassEncoder m_cs_pass = nullptr;
	u32 m_cs_current = 0; ///< the scratch texture holding the target's contents so far
	WGPUComputePipeline m_cs_pass_pipeline = nullptr;
	bool m_cs_encoding = false; ///< the run's own commands are being recorded (GetCommandEncoder())
	u64 m_cs_draws = 0;
	u64 m_cs_runs = 0;
	u64 m_cs_copied_draws = 0; ///< feedback draws that went through copies (did not qualify)
};
