// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/WebGPU/WebGPUHost.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Threading.h"

#include <atomic>
#include <cmath>

// webgpu_host.js
extern "C" {
int tst_gpu_available();
int tst_canvas_create(const char* selector, u32 width, u32 height);
void tst_canvas_destroy(const char* selector);
double tst_post_frame(u32 frame, u32 device, u32 vsync);
void tst_post_fallback(const char* reason, int permanent);
void tst_sleep_suspending(double ms);
double tst_event_loop_stalled_ms();
void tst_staging_map(WGPUBuffer buffer);
int tst_staging_is_mapped(WGPUBuffer buffer);
int tst_staging_write(WGPUBuffer buffer, const void* data, u32 size);
void tst_staging_forget(WGPUBuffer buffer);
double tst_now();
}

namespace
{
	std::atomic<bool> s_enabled{true};
	std::atomic<bool> s_given_up{false};
	std::atomic<bool> s_test_loss_requested{false};
	std::atomic<bool> s_idle_waits_suspend{true};

	// GS thread
	u32 s_posted_frames = 0;
	u32 s_devices = 0;
	bool s_recovering = false;
	bool s_suspended = false;

	// Surface size from the page: width and height packed in one word so they change together.
	std::atomic<u32> s_surface_size{0};
	std::atomic<float> s_surface_scale{1.0f};

	constexpr const char* CANVAS_SELECTOR = "#tst-gs-canvas";
} // namespace

void WebGPUHost::SetEnabled(bool enabled)
{
	s_enabled.store(enabled, std::memory_order_release);
}

void WebGPUHost::SetSurfaceSize(u32 width, u32 height, float scale)
{
	width = std::min<u32>(width, 0xFFFFu);
	height = std::min<u32>(height, 0xFFFFu);
	s_surface_scale.store((scale > 0.0f && std::isfinite(scale)) ? scale : 1.0f, std::memory_order_relaxed);
	s_surface_size.store((width << 16) | height, std::memory_order_release);
}

void WebGPUHost::RequestTestDeviceLoss()
{
	s_test_loss_requested.store(true, std::memory_order_release);
}

void WebGPUHost::SetIdleWaitsSuspend(bool enabled)
{
	s_idle_waits_suspend.store(enabled, std::memory_order_release);
}

bool WebGPUHost::IdleWaitsSuspend()
{
	return s_idle_waits_suspend.load(std::memory_order_acquire);
}

void WebGPUHost::NoteDeviceCreated()
{
	s_devices++;
	s_recovering = false;
}

void WebGPUHost::NoteDeviceLost()
{
	s_recovering = true;
}

bool WebGPUHost::IsRecoveringFromLoss()
{
	return s_recovering;
}

bool WebGPUHost::IsAvailable()
{
	return s_enabled.load(std::memory_order_acquire) && !s_given_up.load(std::memory_order_acquire) &&
		   tst_gpu_available() != 0;
}

void WebGPUHost::GiveUp(const char* reason)
{
	if (s_given_up.exchange(true, std::memory_order_acq_rel))
		return;

	s_recovering = false;
	Console.ErrorFmt("WebGPU: giving up the GPU device for this page ({}); drawing through the 2D path.", reason);
	tst_post_fallback(reason, 1);
}

void WebGPUHost::NoteUnavailable(const char* reason)
{
	Console.WarningFmt("WebGPU: {}; this session draws through the 2D path.", reason);
	tst_post_fallback(reason, 0);
}

bool WebGPUHost::ConsumeTestDeviceLossRequest()
{
	return s_test_loss_requested.exchange(false, std::memory_order_acq_rel);
}

bool WebGPUHost::GetSurfaceSize(u32* width, u32* height, float* scale)
{
	const u32 packed = s_surface_size.load(std::memory_order_acquire);
	if (packed == 0)
		return false;

	*width = packed >> 16;
	*height = packed & 0xFFFFu;
	*scale = s_surface_scale.load(std::memory_order_relaxed);
	return (*width != 0 && *height != 0);
}

WGPUWaitStatus WebGPUHost::Wait(WGPUInstance instance, WGPUFuture future, double timeout_ms)
{
	WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;
	info.future = future;
	const u64 timeout_ns = (timeout_ms <= 0.0) ? 0 : static_cast<u64>(timeout_ms * 1e6);
	pxAssertMsg(!InSuspendingWait(), "GS code entered while the GS thread is suspended");
	s_suspended = (timeout_ns != 0);
	const WGPUWaitStatus status = wgpuInstanceWaitAny(instance, 1, &info, timeout_ns);
	s_suspended = false;
	return status;
}

void WebGPUHost::SleepSuspending(double ms)
{
	pxAssertMsg(!InSuspendingWait(), "GS code entered while the GS thread is suspended");
	s_suspended = true;
	tst_sleep_suspending(ms);
	s_suspended = false;
}

bool WebGPUHost::InSuspendingWait()
{
	// Its own waits, or the idle wait for work (Threading::SetWaitsSuspend()).
	return s_suspended || Threading::InSuspendingSemaphoreWait();
}

bool WebGPUHost::CreateCanvas(u32 width, u32 height)
{
	return tst_canvas_create(CANVAS_SELECTOR, std::max(width, 1u), std::max(height, 1u)) != 0;
}

void WebGPUHost::DestroyCanvas()
{
	tst_canvas_destroy(CANVAS_SELECTOR);
}

const char* WebGPUHost::CanvasSelector()
{
	return CANVAS_SELECTOR;
}

double WebGPUHost::PostFrame(u32 vsync)
{
	const double ms = tst_post_frame(s_posted_frames + 1, s_devices, vsync);
	if (ms >= 0.0)
		s_posted_frames++;
	return ms;
}

void WebGPUHost::StagingMap(WGPUBuffer buffer)
{
	tst_staging_map(buffer);
}

bool WebGPUHost::StagingIsMapped(WGPUBuffer buffer)
{
	return tst_staging_is_mapped(buffer) != 0;
}

bool WebGPUHost::StagingWrite(WGPUBuffer buffer, const void* data, u32 size)
{
	return tst_staging_write(buffer, data, size) != 0;
}

void WebGPUHost::StagingForget(WGPUBuffer buffer)
{
	tst_staging_forget(buffer);
}

double WebGPUHost::EventLoopStalledMs()
{
	return tst_event_loop_stalled_ms();
}

double WebGPUHost::Now()
{
	return tst_now();
}

void WebGPUHost::NoteAbandonedRequestDone()
{
	Console.Warning("WebGPU: a request given up after a timed-out wait completed; its result was dropped.");
}

std::string WebGPUHost::StringViewToString(WGPUStringView sv)
{
	if (!sv.data)
		return {};
	return (sv.length == WGPU_STRLEN) ? std::string(sv.data) : std::string(sv.data, sv.length);
}
