// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <webgpu/webgpu.h>

#include <string>
#include <utility>

// Platform layer of the WebGPU GS device (docs/web-port/WEBGPU-DESIGN.md §3.2, §12), made from the
// runtime spike's primitives (tools/wasm/webgpu-spike/spike.cpp):
//   - timed waits for WebGPU futures, which suspend the GS thread's stack with JSPI;
//   - a suspending sleep, which lets the GS thread's worker run its event loop;
//   - the GS-owned OffscreenCanvas the device draws into, and the frame port: each frame is posted
//     to the page as an ImageBitmap, without returning to the event loop;
//   - write-mapped staging buffers, mapped from JS (see StagingMap());
//   - the page's surface-size feed and the frontend's switches.
// The frontend functions are lock-free and may be called from any thread (the page's exports); the
// others run on the GS thread, which owns every WebGPU object (they live in its worker).
namespace WebGPUHost
{
	// ---- frontend, any thread ----

	/// False keeps the GS on GSDeviceNull and the page on its 2D canvas (?gpu=0).
	void SetEnabled(bool enabled);

	/// The page's canvas backing size in device pixels, and its device pixel ratio.
	void SetSurfaceSize(u32 width, u32 height, float scale);

	/// Test hook: the device destroys itself at its next present, as on a device loss.
	void RequestTestDeviceLoss();

	/// Test hook: false keeps the GS thread's idle wait blocking while it owns a device, so that only
	/// the bounded event-loop turn in EndPresent() delivers WebGPU callbacks (default true).
	void SetIdleWaitsSuspend(bool enabled);
	bool IdleWaitsSuspend();

	// ---- GS thread ----

	/// The frontend allows WebGPU, the GS thread's worker has navigator.gpu and JSPI, and WebGPU was
	/// not given up.
	bool IsAvailable();

	/// Gives up WebGPU for this page after repeated device loss: tells the page, which switches its
	/// canvas back to the 2D path and shows a notice. IsAvailable() is false from now on.
	void GiveUp(const char* reason);

	/// No device could be created for this GS session (at boot): it draws through the 2D path, and
	/// the next session tries WebGPU again. Tells the page why.
	void NoteUnavailable(const char* reason);

	bool ConsumeTestDeviceLossRequest();

	/// Called when a device is ready: frames posted from now on carry the next device number, so
	/// the page can tell when a recovered device's frames arrive. Ends a recovery.
	void NoteDeviceCreated();

	/// The device reported a loss: the device opened next is a recovery (until one is ready).
	void NoteDeviceLost();
	bool IsRecoveringFromLoss();

	/// The latest size from the page. False if it never sent one.
	bool GetSurfaceSize(u32* width, u32* height, float* scale);

	/// Waits for a future (suspending through JSPI) for at most timeout_ms; 0 only polls.
	WGPUWaitStatus Wait(WGPUInstance instance, WGPUFuture future, double timeout_ms);

	/// Suspends the GS thread for ms milliseconds, letting its worker run its event loop.
	void SleepSuspending(double ms);

	/// True while the GS thread is suspended in Wait(), SleepSuspending() or its idle wait for work: its
	/// worker's event loop runs then, and nothing it runs may enter GS code (§3.2; checked in debug
	/// builds). Nothing is proxied to the GS thread, so there is no proxy entry to check.
	bool InSuspendingWait();

	/// How long the GS thread's worker has not run its event loop, in milliseconds: the age of a
	/// microtask it queued that has not run yet (it runs once the thread suspends). Queues one when
	/// none is pending (and returns 0).
	double EventLoopStalledMs();

	/// Creates the GS-owned OffscreenCanvas (replacing an earlier one) and registers it under
	/// CanvasSelector() for wgpuInstanceCreateSurface. The first call also hands the frame port to the
	/// page (Module.tstFramePort). wgpuSurfaceConfigure() sizes it.
	bool CreateCanvas(u32 width, u32 height);
	void DestroyCanvas();
	const char* CanvasSelector();

	/// Takes the canvas's frame with transferToImageBitmap() and posts it to the page with its post
	/// count, the device number and the number of the vsync it shows (counted from boot). Returns
	/// the time it took, in milliseconds, or a negative value if nothing was posted.
	double PostFrame(u32 vsync);

	/// Write-mapped staging buffers (MapWrite | CopySrc). After their first use (they are mapped at
	/// creation) they are mapped, written and unmapped by JS directly rather than through webgpu.h,
	/// whose futures table keeps an entry for every mapAsync that no timed wait collects (one per
	/// frame here). StagingMap() starts mapping, after the submit that read the buffer;
	/// StagingIsMapped() says whether it is mapped now; StagingWrite() copies size bytes from data to
	/// its start and unmaps it (false if it was not mapped); StagingForget() before destroying it.
	void StagingMap(WGPUBuffer buffer);
	bool StagingIsMapped(WGPUBuffer buffer);
	bool StagingWrite(WGPUBuffer buffer, const void* data, u32 size);
	void StagingForget(WGPUBuffer buffer);

	/// Milliseconds on the same clock on every thread (performance.now() of the time origin).
	double Now();

	std::string StringViewToString(WGPUStringView sv);

	/// Logs that the callback of a request whose wait had timed out ran at last (RequestResult).
	void NoteAbandonedRequestDone();

	/// The result of a request waited for with Wait() (WaitAnyOnly callback mode). Its callback runs
	/// inside the wait that sees the request complete; a wait that times out leaves the callback
	/// registered, and it runs later (at the latest with a "cancelled" status when the instance is
	/// released), so the result cannot live on the waiter's stack. The waiter allocates it with new
	/// and passes it as the callback's userdata1; the callback calls Arrive(), the waiter Take().
	template <typename T>
	struct RequestResult
	{
		T value = {};
		bool done = false;
		bool abandoned = false;

		/// In the callback: the result to fill in, or null when the waiter has given up (the result is
		/// freed then, and the callback releases whatever it was handed).
		static RequestResult* Arrive(void* userdata)
		{
			RequestResult* r = static_cast<RequestResult*>(userdata);
			if (r->abandoned)
			{
				delete r;
				NoteAbandonedRequestDone();
				return nullptr;
			}
			r->done = true;
			return r;
		}

		/// In the waiter, after waiting: if the callback ran, moves its value to *out, frees the result
		/// and returns true. Otherwise leaves the result to the callback and returns false.
		bool Take(T* out)
		{
			if (!done)
			{
				abandoned = true;
				return false;
			}
			*out = std::move(value);
			delete this;
			return true;
		}
	};
} // namespace WebGPUHost
