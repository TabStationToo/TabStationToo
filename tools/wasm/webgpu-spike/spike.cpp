// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// WebGPU runtime spike (M4 opening task, docs/web-port/PLAN.md).
//
// Models PCSX2's threads: main() runs on a pthread (PROXY_TO_PTHREAD) like the CPU thread, and
// creates a "GS" pthread, like MTGS, that owns the WebGPU device (emdawnwebgpu, webgpu.h). The GS
// thread draws into the page's #canvas, transferred to it as an OffscreenCanvas, and into an
// OffscreenCanvas of its own whose frames it posts to the page as ImageBitmaps. It runs plain
// loops and blocking waits, and only returns to its worker's event loop where a test says so.
// Each test prints its results; lines starting with SHOT, OBSERVE, GPUCRASH and DONE drive the
// page and the headless driver (index.html, run-headless.mjs). See README.md.
//
// Builds (build.sh): SPIKE_SUSPEND=1 with -sJSPI or -sASYNCIFY; SPIKE_SUSPEND=0 with neither.

#include <webgpu/webgpu.h>

#include <emscripten.h>
#include <emscripten/atomic.h>
#include <emscripten/threading.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <setjmp.h>
#include <string>
#include <vector>

#ifndef SPIKE_SUSPEND
#define SPIKE_SUSPEND 0
#endif
#ifndef SPIKE_SJLJ
#define SPIKE_SJLJ "wasm"
#endif
// 0: no -sOFFSCREENCANVAS_SUPPORT; the GS thread only uses OffscreenCanvases it creates itself.
#ifndef SPIKE_TRANSFER_CANVAS
#define SPIKE_TRANSFER_CANVAS 1
#endif

static double Now() { return emscripten_get_now(); } // ms, same clock on every thread

static std::string Str(WGPUStringView s)
{
	if (!s.data)
		return {};
	return s.length == WGPU_STRLEN ? std::string(s.data) : std::string(s.data, s.length);
}

struct Stats
{
	std::vector<double> v;
	void Add(double x) { v.push_back(x); }
	std::string Summary()
	{
		if (v.empty())
			return "no samples";
		std::vector<double> s = v;
		std::sort(s.begin(), s.end());
		double sum = 0;
		for (double x : s)
			sum += x;
		char buf[160];
		snprintf(buf, sizeof(buf), "n=%zu median=%.3f p90=%.3f min=%.3f max=%.3f mean=%.3f ms", s.size(),
			s[s.size() / 2], s[std::min(s.size() - 1, s.size() * 9 / 10)], s.front(), s.back(), sum / s.size());
		return buf;
	}
};

// ------------------------------------------------------------------------------------------------
// Yielding to the worker's event loop (suspending builds only)
// ------------------------------------------------------------------------------------------------

enum YieldKind
{
	YIELD_MICROTASK = 0, // await an already resolved promise
	YIELD_TIMEOUT = 1, // setTimeout(ms)
	YIELD_MESSAGE = 2, // a MessageChannel round trip (a new task, no clamping)
	YIELD_RAF = 3, // the worker's requestAnimationFrame
};
static const char* const s_yield_names[] = {"microtask", "setTimeout0", "MessageChannel", "rAF"};

#if SPIKE_SUSPEND
// Suspends the calling thread's wasm stack (JSPI or Asyncify) until the promise settles. Returns
// the time spent away, in ms.
EM_ASYNC_JS(double, spike_yield_js, (int kind, int ms), {
	const t0 = performance.now();
	if (kind == 0) {
		await Promise.resolve();
	} else if (kind == 1) {
		await new Promise((r) => setTimeout(r, ms));
	} else if (kind == 2) {
		if (!globalThis.spikeChannel) {
			const c = new MessageChannel();
			const q = [];
			c.port1.onmessage = () => q.shift()();
			globalThis.spikeChannel = { c, q };
		}
		const s = globalThis.spikeChannel;
		await new Promise((r) => { s.q.push(r); s.c.port2.postMessage(0); });
	} else {
		if (typeof requestAnimationFrame != 'function') return -1;
		await new Promise((r) => requestAnimationFrame(r));
	}
	return performance.now() - t0;
});
static double Yield(YieldKind kind, int ms = 0) { return spike_yield_js(kind, ms); }
#endif

// ------------------------------------------------------------------------------------------------
// GPU state, owned by the GS thread (emdawnwebgpu keeps its JS objects per worker)
// ------------------------------------------------------------------------------------------------

struct Gpu
{
	bool compat = false;
	WGPUInstance instance = nullptr;
	WGPUAdapter adapter = nullptr;
	WGPUDevice device = nullptr;
	WGPUQueue queue = nullptr;
	WGPUSurface surface = nullptr; // the page's #canvas, transferred to this thread
	WGPUSurface bitmap_surface = nullptr; // an OffscreenCanvas created by this thread (bitmap test)
	WGPUTextureFormat format = WGPUTextureFormat_Undefined;
	uint32_t width = 320, height = 240;
	std::atomic<int> lost{0}; // 0, or WGPUDeviceLostReason
	double lost_at = 0;
	std::atomic<int> errors{0};
};
static Gpu g;

static void OnDeviceLost(WGPUDevice const*, WGPUDeviceLostReason reason, WGPUStringView message, void*, void*)
{
	g.lost_at = Now();
	g.lost.store(static_cast<int>(reason));
	printf("[gs] device lost callback: reason=%d message=\"%s\"\n", static_cast<int>(reason), Str(message).c_str());
}

static void OnUncapturedError(WGPUDevice const*, WGPUErrorType type, WGPUStringView message, void*, void*)
{
	g.errors++;
	printf("[gs] uncaptured error type=%d: %s\n", static_cast<int>(type), Str(message).c_str());
}

static void PrintAdapter()
{
	WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
	wgpuAdapterGetInfo(g.adapter, &info);
	printf("[gs] adapter: vendor=\"%s\" architecture=\"%s\" description=\"%s\" backend=%d type=%d core=%d dual-source-blending=%d\n",
		Str(info.vendor).c_str(), Str(info.architecture).c_str(), Str(info.description).c_str(),
		static_cast<int>(info.backendType), static_cast<int>(info.adapterType),
		wgpuAdapterHasFeature(g.adapter, WGPUFeatureName_CoreFeaturesAndLimits),
		wgpuAdapterHasFeature(g.adapter, WGPUFeatureName_DualSourceBlending));
	wgpuAdapterInfoFreeMembers(info);
}

static WGPUDeviceDescriptor DeviceDescriptor()
{
	WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
	dd.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
	dd.deviceLostCallbackInfo.callback = OnDeviceLost;
	dd.uncapturedErrorCallbackInfo.callback = OnUncapturedError;
	return dd;
}

static WGPURequestAdapterOptions AdapterOptions()
{
	WGPURequestAdapterOptions opts = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
	opts.featureLevel = g.compat ? WGPUFeatureLevel_Compatibility : WGPUFeatureLevel_Core;
	opts.powerPreference = WGPUPowerPreference_HighPerformance;
	return opts;
}

static WGPUSurface CreateSurface(const char* selector)
{
	WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
	canvas.selector = {selector, WGPU_STRLEN};
	WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
	sd.nextInChain = &canvas.chain;
	WGPUSurface surface = wgpuInstanceCreateSurface(g.instance, &sd);
	printf("[gs] surface from \"%s\" on this pthread: %s\n", selector, surface ? "ok" : "FAILED");
	return surface;
}

static void ConfigureSurface(WGPUSurface surface)
{
	WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
	wgpuSurfaceGetCapabilities(surface, g.adapter, &caps);
	g.format = caps.formatCount ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
	wgpuSurfaceCapabilitiesFreeMembers(caps);
	WGPUSurfaceConfiguration sc = WGPU_SURFACE_CONFIGURATION_INIT;
	sc.device = g.device;
	sc.format = g.format;
	sc.usage = WGPUTextureUsage_RenderAttachment;
	sc.width = g.width;
	sc.height = g.height;
	sc.alphaMode = WGPUCompositeAlphaMode_Opaque;
	sc.presentMode = WGPUPresentMode_Fifo;
	wgpuSurfaceConfigure(surface, &sc);
}

// Presenting without the event loop: the thread draws into an OffscreenCanvas it created itself,
// takes the frame with transferToImageBitmap() and posts it to the page, which shows it in a
// "bitmaprenderer" canvas. The message uses Emscripten's own "call a Module handler on the main
// thread" command (CMD_CALL_HANDLER = 9 in 6.0.12, src/lib/libpthread.js); a real frontend would
// set up its own MessagePort once.
EM_JS(void, spike_offscreen_create, (const char* selector, int width, int height), {
	specialHTMLTargets[UTF8ToString(selector)] = new OffscreenCanvas(width, height);
});
EM_JS(double, spike_offscreen_post, (const char* selector), {
	const t0 = performance.now();
	const bitmap = specialHTMLTargets[UTF8ToString(selector)].transferToImageBitmap();
	postMessage({ cmd: 9, handler: "spikePresent", args: [bitmap] }, [bitmap]);
	return performance.now() - t0;
});

static bool CreateBitmapSurface()
{
	if (g.bitmap_surface)
		wgpuSurfaceRelease(g.bitmap_surface);
	spike_offscreen_create("#gs-offscreen", static_cast<int>(g.width), static_cast<int>(g.height));
	g.bitmap_surface = CreateSurface("#gs-offscreen");
	if (g.bitmap_surface)
		ConfigureSurface(g.bitmap_surface);
	return g.bitmap_surface != nullptr;
}

// Clears the canvas's current texture. Without a yield, every call in the same task draws into
// the same texture; the browser only takes it when the worker's task ends.
static bool DrawFrame(double r, double gr, double b, WGPUSurface surface = nullptr)
{
	WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
	wgpuSurfaceGetCurrentTexture(surface ? surface : g.surface, &st);
	if (st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
		st.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
	{
		printf("[gs] getCurrentTexture status=%d\n", static_cast<int>(st.status));
		return false;
	}
	WGPUTextureView view = wgpuTextureCreateView(st.texture, nullptr);
	WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
	ca.view = view;
	ca.loadOp = WGPULoadOp_Clear;
	ca.storeOp = WGPUStoreOp_Store;
	ca.clearValue = {r, gr, b, 1.0};
	WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
	rp.colorAttachmentCount = 1;
	rp.colorAttachments = &ca;
	WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
	WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
	wgpuRenderPassEncoderEnd(pass);
	WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
	wgpuQueueSubmit(g.queue, 1, &cb);
	wgpuCommandBufferRelease(cb);
	wgpuRenderPassEncoderRelease(pass);
	wgpuCommandEncoderRelease(enc);
	wgpuTextureViewRelease(view);
	wgpuTextureRelease(st.texture);
	return true;
}

// Frame n of a run: red encodes n, green 200 marks a spike frame (decoded by index.html).
static bool DrawNumberedFrame(int n, WGPUSurface surface = nullptr)
{
	return DrawFrame((16 + n % 224) / 255.0, 200 / 255.0, 50 / 255.0, surface);
}

#if SPIKE_SUSPEND
// ------------------------------------------------------------------------------------------------
// Synchronous waits from C++: wgpuInstanceWaitAny with a timeout suspends (JSPI/Asyncify)
// ------------------------------------------------------------------------------------------------

static WGPUWaitStatus Wait(WGPUFuture f, double timeout_ms)
{
	WGPUFutureWaitInfo info = WGPU_FUTURE_WAIT_INFO_INIT;
	info.future = f;
	return wgpuInstanceWaitAny(g.instance, 1, &info, static_cast<uint64_t>(timeout_ms * 1e6));
}

static bool AcquireDevice()
{
	struct Result
	{
		int status = 0;
		WGPUAdapter adapter = nullptr;
		WGPUDevice device = nullptr;
		std::string message;
	} res;

	WGPURequestAdapterOptions opts = AdapterOptions();
	WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
	acb.mode = WGPUCallbackMode_WaitAnyOnly;
	acb.userdata1 = &res;
	acb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* u1, void*) {
		Result* r = static_cast<Result*>(u1);
		r->status = static_cast<int>(status);
		r->adapter = adapter;
		r->message = Str(message);
	};
	double t0 = Now();
	WGPUWaitStatus ws = Wait(wgpuInstanceRequestAdapter(g.instance, &opts, acb), 5000);
	double t1 = Now();
	printf("[gs] requestAdapter via WaitAny(5 s): wait=%d status=%d adapter=%s in %.2f ms %s\n", static_cast<int>(ws),
		res.status, res.adapter ? "yes" : "no", t1 - t0, res.message.c_str());
	if (!res.adapter)
		return false;
	g.adapter = res.adapter;

	WGPUDeviceDescriptor dd = DeviceDescriptor();
	WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
	dcb.mode = WGPUCallbackMode_WaitAnyOnly;
	dcb.userdata1 = &res;
	dcb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* u1, void*) {
		Result* r = static_cast<Result*>(u1);
		r->status = static_cast<int>(status);
		r->device = device;
		r->message = Str(message);
	};
	t0 = Now();
	ws = Wait(wgpuAdapterRequestDevice(g.adapter, &dd, dcb), 5000);
	t1 = Now();
	printf("[gs] requestDevice via WaitAny(5 s): wait=%d status=%d device=%s in %.2f ms %s\n", static_cast<int>(ws),
		res.status, res.device ? "yes" : "no", t1 - t0, res.message.c_str());
	if (!res.device)
		return false;
	g.device = res.device;
	g.queue = wgpuDeviceGetQueue(g.device);
	g.lost.store(0);
	return true;
}

static void ReleaseDevice()
{
	if (g.queue)
		wgpuQueueRelease(g.queue);
	if (g.device)
		wgpuDeviceRelease(g.device);
	if (g.adapter)
		wgpuAdapterRelease(g.adapter);
	g.queue = nullptr;
	g.device = nullptr;
	g.adapter = nullptr;
}

// Prints SHOT and keeps the frame on screen (suspended) while the page and the driver capture it.
static void Shot(const char* name)
{
	printf("SHOT %s\n", name);
	Yield(YIELD_TIMEOUT, 700);
}

// ------------------------------------------------------------------------------------------------
// Tests
// ------------------------------------------------------------------------------------------------

// Q2/Q3: does a frame drawn by a loop that never yields reach the page, and what does a yield cost?
static void TestPresent()
{
	printf("[gs] --- present\n");
	DrawFrame(0.5, 0.5, 0.5);
	Yield(YIELD_TIMEOUT, 100);
	Shot("present_baseline_grey");

	// A loop that never returns to the event loop, then a blocking wait (like MTGS in WaitForWork).
	double t0 = Now();
	for (int i = 0; i < 120; i++)
		DrawFrame(1.0, 0.0, 0.0);
	double t1 = Now();
	printf("[gs] no-yield loop: 120 red frames in %.2f ms (%.3f ms/frame); now blocking 1.5 s without yielding\n", t1 - t0, (t1 - t0) / 120);
	printf("SHOT present_noyield_blocked_expect_grey\n");
	emscripten_thread_sleep(1500); // Atomics.wait: the worker's event loop does not run
	printf("[gs] blocking wait over; one setTimeout(0) yield now\n");
	double y = Yield(YIELD_TIMEOUT, 0);
	printf("[gs] yield took %.3f ms\n", y);
	Shot("present_after_one_yield_expect_red");

	// 60 frames paced at 60 Hz: draw, yield once, then block for the rest of the frame (as the
	// emulator would, without returning to the event loop). The page counts the frames it sees.
	for (int kind = YIELD_MICROTASK; kind <= YIELD_RAF; kind++)
	{
		const int frames = 60;
		printf("OBSERVE begin %s\n", s_yield_names[kind]);
		Yield(YIELD_TIMEOUT, 50);
		Stats ys;
		t0 = Now();
		for (int i = 0; i < frames; i++)
		{
			const double frame_start = Now();
			DrawNumberedFrame(i);
			const double d = Yield(static_cast<YieldKind>(kind));
			if (d < 0)
			{
				printf("[gs] %s: not available in this worker\n", s_yield_names[kind]);
				break;
			}
			ys.Add(d);
			const double left = 1000.0 / 60 - (Now() - frame_start);
			if (left > 0)
				emscripten_thread_sleep(left); // blocking, like the rest of an emulated frame
		}
		t1 = Now();
		Yield(YIELD_TIMEOUT, 100);
		printf("OBSERVE end %s\n", s_yield_names[kind]);
		printf("[gs] %s: %d paced frames in %.1f ms; yield %s\n", s_yield_names[kind], frames, t1 - t0, ys.Summary().c_str());
		Yield(YIELD_TIMEOUT, 200);
	}

	// Yield cost alone, no rendering.
	for (int kind = YIELD_MICROTASK; kind <= YIELD_MESSAGE; kind++)
	{
		Stats ys;
		for (int i = 0; i < 300; i++)
			ys.Add(Yield(static_cast<YieldKind>(kind)));
		printf("[gs] bare %s yield: %s\n", s_yield_names[kind], ys.Summary().c_str());
	}
}

// Q3 alternative: frames from a loop that never yields, presented through ImageBitmaps.
static void TestBitmap()
{
	printf("[gs] --- bitmap\n");
	if (!CreateBitmapSurface())
		return;
	printf("OBSERVE begin bitmap-noyield present\n");
	emscripten_thread_sleep(50);
	Stats post, frame;
	const double t0 = Now();
	for (int i = 0; i < 60; i++)
	{
		const double frame_start = Now();
		DrawNumberedFrame(i, g.bitmap_surface);
		post.Add(spike_offscreen_post("#gs-offscreen"));
		frame.Add(Now() - frame_start);
		const double left = 1000.0 / 60 - (Now() - frame_start);
		if (left > 0)
			emscripten_thread_sleep(left); // blocking: this thread never returns to its event loop
	}
	const double t1 = Now();
	emscripten_thread_sleep(100);
	printf("OBSERVE end bitmap-noyield\n");
	printf("[gs] bitmap: 60 paced frames in %.1f ms without any yield; draw+transfer+post %s; transferToImageBitmap+postMessage %s\n",
		t1 - t0, frame.Summary().c_str(), post.Summary().c_str());
	DrawFrame(0.9, 0.9, 0.0, g.bitmap_surface);
	spike_offscreen_post("#gs-offscreen");
	printf("SHOT bitmap_noyield_blocked_expect_yellow_in_present\n");
	emscripten_thread_sleep(1000);
}

// Q3: MTGS sleeps in WaitForWork. A suspending atomic wait lets the worker's event loop run (and
// WebGPU callbacks fire) while it sleeps; a blocking wait does not.
static std::atomic<uint32_t> s_work{0};
static double s_signal_time[256];

static void* Producer(void* arg)
{
	const int count = static_cast<int>(reinterpret_cast<intptr_t>(arg));
	for (int k = 1; k <= count; k++)
	{
		emscripten_thread_sleep(4);
		s_signal_time[k] = Now();
		s_work.store(static_cast<uint32_t>(k));
		emscripten_atomic_notify(&s_work, 1);
	}
	return nullptr;
}

static void TestIdle()
{
	printf("[gs] --- idle\n");
	const int count = 200;
	for (int suspending = 1; suspending >= 0; suspending--)
	{
		// A WebGPU completion that can only be delivered by the event loop.
		static std::atomic<double> done_at{0};
		done_at.store(0);
		DrawFrame(0.0, 0.3, 0.6);
		WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
		cb.mode = WGPUCallbackMode_AllowSpontaneous;
		cb.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void*, void*) { done_at.store(Now()); };
		wgpuQueueOnSubmittedWorkDone(g.queue, cb);

		s_work.store(0);
		pthread_t producer;
		pthread_create(&producer, nullptr, Producer, reinterpret_cast<void*>(static_cast<intptr_t>(count)));
		Stats lat;
		const double t0 = Now();
		for (uint32_t seen = 0; seen < static_cast<uint32_t>(count);)
		{
			uint32_t v = s_work.load();
			if (v == seen)
			{
				if (suspending)
					emscripten_atomic_wait_suspending(&s_work, v, 1000);
				else
					emscripten_atomic_wait_u32(&s_work, v, 1000 * 1000000LL);
				continue;
			}
			const double t = Now();
			for (uint32_t k = seen + 1; k <= v; k++)
				lat.Add(t - s_signal_time[k]);
			seen = v;
		}
		const double t1 = Now();
		pthread_join(producer, nullptr);
		const double cb_at = done_at.load();
		printf("[gs] %s wait: %d wake-ups in %.1f ms, wake latency %s; onSubmittedWorkDone fired %s\n",
			suspending ? "suspending (emscripten_atomic_wait_suspending)" : "blocking (Atomics.wait)", count, t1 - t0,
			lat.Summary().c_str(), cb_at == 0 ? "NOT during the wait" : (cb_at < t1 ? "during the wait" : "after the wait"));
		if (!suspending && cb_at == 0)
		{
			Yield(YIELD_MESSAGE);
			printf("[gs] after one yield: onSubmittedWorkDone %s\n", done_at.load() ? "fired" : "still pending");
		}
	}
}

// Q4: GSDownloadTexture-style synchronous readback.
static void TestReadback()
{
	printf("[gs] --- readback\n");
	for (uint32_t size : {256u, 1024u})
	{
		WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
		td.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
		td.size = {size, size, 1};
		td.format = WGPUTextureFormat_RGBA8Unorm;
		WGPUTexture tex = wgpuDeviceCreateTexture(g.device, &td);
		WGPUTextureView view = wgpuTextureCreateView(tex, nullptr);
		const uint32_t pitch = size * 4; // already a multiple of 256
		const uint64_t bytes = static_cast<uint64_t>(pitch) * size;
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
		bd.size = bytes;
		WGPUBuffer buf = wgpuDeviceCreateBuffer(g.device, &bd);

		Stats lat, spont_yields, spont_lat;
		int bad = 0;
		const int iters = size == 256 ? 100 : 40;
		for (int it = 0; it < iters + 20; it++)
		{
			const bool spontaneous = it >= iters; // last 20: callback + yields instead of WaitAny
			const uint8_t want = static_cast<uint8_t>(it * 37 + 11);
			WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
			ca.view = view;
			ca.loadOp = WGPULoadOp_Clear;
			ca.storeOp = WGPUStoreOp_Store;
			ca.clearValue = {want / 255.0, 0.25, 0.75, 1.0};
			WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
			rp.colorAttachmentCount = 1;
			rp.colorAttachments = &ca;
			WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
			WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
			wgpuRenderPassEncoderEnd(pass);
			WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
			src.texture = tex;
			WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
			dst.buffer = buf;
			dst.layout.bytesPerRow = pitch;
			dst.layout.rowsPerImage = size;
			WGPUExtent3D extent = {size, size, 1};
			wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &extent);
			WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
			wgpuQueueSubmit(g.queue, 1, &cb);
			wgpuCommandBufferRelease(cb);
			wgpuRenderPassEncoderRelease(pass);
			wgpuCommandEncoderRelease(enc);

			static std::atomic<int> map_status{0};
			map_status.store(0);
			WGPUBufferMapCallbackInfo mcb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
			mcb.mode = spontaneous ? WGPUCallbackMode_AllowSpontaneous : WGPUCallbackMode_WaitAnyOnly;
			mcb.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void*, void*) { map_status.store(static_cast<int>(status)); };
			const double t0 = Now();
			WGPUFuture f = wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bytes, mcb);
			if (!spontaneous)
			{
				WGPUWaitStatus ws = Wait(f, 2000);
				if (ws != WGPUWaitStatus_Success)
					printf("[gs] readback %u: WaitAny status %d\n", size, static_cast<int>(ws));
				lat.Add(Now() - t0);
			}
			else
			{
				int yields = 0;
				while (!map_status.load() && yields < 1000)
				{
					Yield(YIELD_MESSAGE);
					yields++;
				}
				spont_lat.Add(Now() - t0);
				spont_yields.Add(yields);
			}
			if (map_status.load() != WGPUMapAsyncStatus_Success)
			{
				printf("[gs] readback %u: map status %d\n", size, map_status.load());
				bad++;
				continue;
			}
			const uint8_t* p = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(buf, 0, bytes));
			if (!p || p[0] != want || p[bytes - 4] != want || p[1] != 64 || p[2] != 191)
				bad++;
			wgpuBufferUnmap(buf);
		}
		printf("[gs] readback %ux%u RGBA8 (%llu KiB): WaitAny %s\n", size, size,
			static_cast<unsigned long long>(bytes / 1024), lat.Summary().c_str());
		printf("[gs] readback %ux%u: AllowSpontaneous + MessageChannel yields: %s; yields per map %s; %d bad results\n",
			size, size, spont_lat.Summary().c_str(), spont_yields.Summary().c_str(), bad);
		wgpuBufferRelease(buf);
		wgpuTextureViewRelease(view);
		wgpuTextureRelease(tex);
	}
}

// Q5: with -sSUPPORT_LONGJMP=wasm, setjmp/longjmp put no JS frame on the stack, so a suspension
// between them is allowed. With -sSUPPORT_LONGJMP=emscripten, the invoke_* trampolines are JS
// frames and JSPI traps.
static jmp_buf s_jmp;
static void __attribute__((noinline)) DeepSuspend(int depth)
{
	if (depth == 0)
	{
		Yield(YIELD_MESSAGE);
		longjmp(s_jmp, 42);
	}
	DeepSuspend(depth - 1);
	printf("[gs] unreachable\n");
}

static void TestLongjmp()
{
	printf("[gs] --- longjmp\n");
	volatile int pass = 0;
	int r = setjmp(s_jmp);
	if (r == 0)
	{
		pass = 1;
		DeepSuspend(8);
	}
	printf("[gs] setjmp, suspend 8 frames deeper, longjmp back: returned %d after pass %d: %s\n", r, pass,
		r == 42 ? "OK" : "WRONG");
}

// Q3/Q4 detail: does work submitted by a thread that does not yield reach the GPU? A compute
// dispatch of known GPU time is submitted, the thread blocks for 100 ms, then waits for it.
static void TestFlush()
{
	printf("[gs] --- flush\n");
	WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
	wgsl.code = {R"(
		override iterations: u32 = 1000000u;
		@group(0) @binding(0) var<storage, read_write> data: array<u32>;
		@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3u) {
			var x = id.x;
			for (var i = 0u; i < iterations; i++) { x = x * 1664525u + 1013904223u; }
			data[id.x] = x;
		})", WGPU_STRLEN};
	WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
	smd.nextInChain = &wgsl.chain;
	WGPUShaderModule sm = wgpuDeviceCreateShaderModule(g.device, &smd);
	WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
	bd.usage = WGPUBufferUsage_Storage;
	bd.size = 64 * 64 * 4;
	WGPUBuffer buf = wgpuDeviceCreateBuffer(g.device, &bd);

	WGPUComputePipeline pipe = nullptr;
	WGPUBindGroup bg = nullptr;
	auto make = [&](double iterations) {
		if (bg)
			wgpuBindGroupRelease(bg);
		if (pipe)
			wgpuComputePipelineRelease(pipe);
		WGPUConstantEntry c = WGPU_CONSTANT_ENTRY_INIT;
		c.key = {"iterations", WGPU_STRLEN};
		c.value = iterations;
		WGPUComputePipelineDescriptor cpd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
		cpd.compute.module = sm;
		cpd.compute.constantCount = 1;
		cpd.compute.constants = &c;
		pipe = wgpuDeviceCreateComputePipeline(g.device, &cpd);
		WGPUBindGroupEntry e = WGPU_BIND_GROUP_ENTRY_INIT;
		e.binding = 0;
		e.buffer = buf;
		e.size = bd.size;
		WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
		WGPUBindGroupLayout bgl = wgpuComputePipelineGetBindGroupLayout(pipe, 0);
		bgd.layout = bgl;
		bgd.entryCount = 1;
		bgd.entries = &e;
		bg = wgpuDeviceCreateBindGroup(g.device, &bgd);
		wgpuBindGroupLayoutRelease(bgl);
	};
	auto submit = [&]() {
		WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
		WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, nullptr);
		wgpuComputePassEncoderSetPipeline(pass, pipe);
		wgpuComputePassEncoderSetBindGroup(pass, 0, bg, 0, nullptr);
		wgpuComputePassEncoderDispatchWorkgroups(pass, 64, 1, 1);
		wgpuComputePassEncoderEnd(pass);
		WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, nullptr);
		wgpuQueueSubmit(g.queue, 1, &cb);
		wgpuCommandBufferRelease(cb);
		wgpuComputePassEncoderRelease(pass);
		wgpuCommandEncoderRelease(enc);
	};
	auto wait_done = [&]() {
		WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
		cb.mode = WGPUCallbackMode_WaitAnyOnly;
		cb.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void*, void*) {};
		const double t0 = Now();
		Wait(wgpuQueueOnSubmittedWorkDone(g.queue, cb), 5000);
		return Now() - t0;
	};

	// Calibrate to about 40 ms of GPU time.
	double iterations = 1000000;
	make(iterations);
	submit();
	wait_done(); // first use compiles the pipeline
	submit();
	double gpu = wait_done();
	iterations = std::min(iterations * 40 / std::max(gpu, 0.5), 2e8);
	make(iterations);
	submit();
	wait_done();
	submit();
	gpu = wait_done();
	printf("[gs] flush: dispatch of %.0f iterations: submit -> done in %.1f ms when waited at once\n", iterations, gpu);

	static const char* const modes[] = {"no yield", "microtask yield", "MessageChannel yield"};
	for (int mode = 0; mode < 3; mode++)
	{
		Stats st;
		for (int rep = 0; rep < 3; rep++)
		{
			submit();
			if (mode == 1)
				Yield(YIELD_MICROTASK);
			else if (mode == 2)
				Yield(YIELD_MESSAGE);
			emscripten_thread_sleep(100);
			st.Add(wait_done());
		}
		printf("[gs] flush: submit, %s, block 100 ms, then wait: %s\n", modes[mode], st.Summary().c_str());
	}
	wgpuBindGroupRelease(bg);
	wgpuComputePipelineRelease(pipe);
	wgpuBufferRelease(buf);
	wgpuShaderModuleRelease(sm);
}

// Draws frames for half a second, yielding after each, so the page holds a settled frame.
static void Settle(double r, double gr, double b)
{
	for (int i = 0; i < 30; i++)
	{
		DrawFrame(r, gr, b);
		Yield(YIELD_MESSAGE);
		emscripten_thread_sleep(16);
	}
}

static bool Recover(const char* what)
{
	const double t0 = Now();
	ReleaseDevice();
	for (int attempt = 1; attempt <= 20; attempt++)
	{
		if (AcquireDevice())
		{
			ConfigureSurface(g.surface);
			printf("[gs] %s: recovered (new adapter + device, surface reconfigured) in %.1f ms, attempt %d\n", what,
				Now() - t0, attempt);
			PrintAdapter();
			return true;
		}
		Yield(YIELD_TIMEOUT, 250);
	}
	printf("[gs] %s: recovery FAILED\n", what);
	return false;
}

// Q6: device loss through destroy() and through a GPU process crash.
static void TestDestroy()
{
	printf("[gs] --- lost (device.destroy)\n");
	wgpuDeviceDestroy(g.device);
	printf("[gs] destroy(): lost=%d before any yield\n", g.lost.load());
	emscripten_thread_sleep(200);
	printf("[gs] after a 200 ms blocking wait: lost=%d\n", g.lost.load());
	DrawFrame(1, 0, 1); // on a destroyed device: must not crash
	Yield(YIELD_MESSAGE);
	printf("[gs] after one yield: lost=%d (2 = Destroyed), uncaptured errors so far %d\n", g.lost.load(), g.errors.load());
	if (Recover("destroy"))
	{
		Settle(0.0, 0.6, 0.0);
		Shot("lost_recovered_expect_green");
	}
}

static void TestCrash()
{
	printf("[gs] --- crash (GPU process)\n");
	const double t0 = Now();
	printf("GPUCRASH\n");
	int frames = 0;
	while (!g.lost.load() && Now() - t0 < 10000)
	{
		DrawNumberedFrame(frames++);
		Yield(YIELD_TIMEOUT, 16);
	}
	if (!g.lost.load())
	{
		printf("[gs] crash: no device loss within 10 s (%d frames)\n", frames);
		return;
	}
	printf("[gs] crash: lost reason=%d, %.1f ms after GPUCRASH, after %d frames\n", g.lost.load(), g.lost_at - t0, frames);
	WGPUSurfaceTexture st = WGPU_SURFACE_TEXTURE_INIT;
	wgpuSurfaceGetCurrentTexture(g.surface, &st);
	printf("[gs] crash: getCurrentTexture on the lost device: status=%d texture=%s\n", static_cast<int>(st.status), st.texture ? "yes" : "no");
	if (st.texture)
		wgpuTextureRelease(st.texture);
	if (Recover("crash"))
	{
		Settle(0.0, 0.0, 0.8);
		// The transferred canvas may stay blank after a GPU process crash; a new OffscreenCanvas
		// presented through ImageBitmaps is the alternative.
		if (CreateBitmapSurface())
		{
			DrawFrame(0.0, 0.8, 0.8, g.bitmap_surface);
			spike_offscreen_post("#gs-offscreen");
		}
		Shot("crash_recovered_expect_blue_and_cyan_in_present");
	}
}
// JSPI can only suspend a stack that entered wasm through a promising export (a pthread's start
// routine). Spontaneous WebGPU callbacks are called straight from the event loop.
static void TestCallbackSuspend()
{
	printf("[gs] --- cbsuspend\n");
	DrawFrame(0.2, 0.2, 0.2);
	WGPUQueueWorkDoneCallbackInfo cb = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
	cb.mode = WGPUCallbackMode_AllowSpontaneous;
	cb.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void*, void*) {
		printf("[gs] in a spontaneous callback (thread suspended elsewhere); suspending from here...\n");
		Yield(YIELD_MICROTASK);
		printf("[gs] ...suspending inside the callback worked\n");
	};
	wgpuQueueOnSubmittedWorkDone(g.queue, cb);
	Yield(YIELD_TIMEOUT, 500);
	printf("[gs] cbsuspend: back on the thread's own stack\n");
}
#endif // SPIKE_SUSPEND

// Q5: a rough look at Asyncify's instrumentation cost on ordinary code (indirect calls, as in
// the interpreters). Only meaningful on an otherwise idle machine.
static int __attribute__((noinline)) AddOne(int x) { return x + 1; }
static int __attribute__((noinline)) AddTwo(int x) { return x + 2; }
static void TestBench()
{
	int (*volatile fns[2])(int) = {AddOne, AddTwo};
	double best = 1e9;
	int acc = 0;
	for (int rep = 0; rep < 3; rep++)
	{
		const double t0 = Now();
		for (int i = 0; i < 20000000; i++)
			acc = fns[i & 1](acc);
		best = std::min(best, Now() - t0);
	}
	printf("[gs] bench: 20M indirect calls in %.1f ms (best of 3, acc=%d)\n", best, acc);
}

// ------------------------------------------------------------------------------------------------
// GS thread
// ------------------------------------------------------------------------------------------------

struct Args
{
	std::string tests;
	bool compat;
};

static bool Wants(const Args& a, const char* name)
{
	return a.tests == "all" || ("," + a.tests + ",").find(std::string(",") + name + ",") != std::string::npos;
}

#if !SPIKE_SUSPEND
// Without JSPI/Asyncify, C++ cannot wait for a promise: a loop that polls never sees it settle,
// and the only way forward is to return to the event loop and continue in callbacks.
static bool s_adapter_done = false;
static double s_cb_t0;

static void RequestAdapterProcessEvents()
{
	s_adapter_done = false;
	WGPURequestAdapterOptions opts = AdapterOptions();
	WGPURequestAdapterCallbackInfo acb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
	acb.mode = WGPUCallbackMode_AllowProcessEvents;
	acb.callback = [](WGPURequestAdapterStatus, WGPUAdapter adapter, WGPUStringView, void*, void*) {
		s_adapter_done = true;
		g.adapter = adapter;
	};
	wgpuInstanceRequestAdapter(g.instance, &opts, acb);
}

static void ContinueInCallbacks()
{
	s_cb_t0 = Now();
	// The adapter request is already pending (AllowProcessEvents): a timer keeps calling
	// ProcessEvents from the event loop, which is how a GPU-owner worker would run.
	emscripten_set_timeout_loop([](double, void*) -> bool {
		wgpuInstanceProcessEvents(g.instance);
		if (!s_adapter_done)
			return true;
		if (!g.adapter)
		{
			printf("[gs] no-suspend: requestAdapter answered null after %.2f ms; asking again\n", Now() - s_cb_t0);
			RequestAdapterProcessEvents();
			return true;
		}
		printf("[gs] no-suspend: adapter after %.2f ms of event-loop turns\n", Now() - s_cb_t0);
		PrintAdapter();
		WGPUDeviceDescriptor dd = DeviceDescriptor();
		WGPURequestDeviceCallbackInfo dcb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
		dcb.mode = WGPUCallbackMode_AllowSpontaneous;
		dcb.callback = [](WGPURequestDeviceStatus, WGPUDevice device, WGPUStringView, void*, void*) {
			printf("[gs] no-suspend: device in a spontaneous callback after %.2f ms\n", Now() - s_cb_t0);
			g.device = device;
			g.queue = wgpuDeviceGetQueue(device);
			ConfigureSurface(g.surface);
			DrawFrame(0.8, 0.4, 0.0);
			printf("[gs] no-suspend: drew orange from a callback; it shows once this task ends\n");
			printf("SHOT nosuspend_callbacks_expect_orange\n");
			emscripten_set_timeout([](void*) { pthread_exit(nullptr); }, 1000, nullptr);
		};
		wgpuAdapterRequestDevice(g.adapter, &dd, dcb);
		return false;
	}, 0, nullptr);
}

static void GsMainNoSuspend()
{
	const double t0 = Now();
	RequestAdapterProcessEvents();
	long polls = 0;
	while (!s_adapter_done && Now() - t0 < 2000)
	{
		wgpuInstanceProcessEvents(g.instance);
		polls++;
	}
	printf("[gs] no-suspend: polled wgpuInstanceProcessEvents %ld times for 2 s without yielding: adapter %s\n", polls,
		s_adapter_done ? "arrived" : "never arrived");
	printf("[gs] no-suspend: returning to the event loop (emscripten_unwind_to_js_event_loop) to continue in callbacks\n");
	ContinueInCallbacks();
	emscripten_unwind_to_js_event_loop();
}
#endif

static void* GsMain(void* p)
{
	const Args& a = *static_cast<Args*>(p);
	g.compat = a.compat;
	WGPUInstanceDescriptor id = WGPU_INSTANCE_DESCRIPTOR_INIT;
#if SPIKE_SUSPEND
	static const WGPUInstanceFeatureName features[] = {WGPUInstanceFeatureName_TimedWaitAny};
	id.requiredFeatureCount = 1;
	id.requiredFeatures = features;
#endif
	g.instance = wgpuCreateInstance(&id);
	printf("[gs] instance %s (TimedWaitAny %s)\n", g.instance ? "ok" : "FAILED", SPIKE_SUSPEND ? "requested" : "not requested");
#if SPIKE_TRANSFER_CANVAS
	if (!g.instance || !(g.surface = CreateSurface("#canvas")))
		return nullptr;
#else
	// Nothing was transferred: "#canvas" tests draw into an OffscreenCanvas nobody shows.
	spike_offscreen_create("#gs-unused", static_cast<int>(g.width), static_cast<int>(g.height));
	if (!g.instance || !(g.surface = CreateSurface("#gs-unused")))
		return nullptr;
#endif

#if SPIKE_SUSPEND
	// A fresh headless Chrome can answer null while its GPU process starts; retry a few times.
	int attempt = 1;
	for (; !AcquireDevice(); attempt++)
	{
		ReleaseDevice();
		if (attempt == 10)
			return nullptr;
		Yield(YIELD_TIMEOUT, 500);
	}
	PrintAdapter();
	ConfigureSurface(g.surface);
	printf("[gs] surface configured: format=%d %ux%u\n", static_cast<int>(g.format), g.width, g.height);
	if (Wants(a, "present"))
		TestPresent();
	if (Wants(a, "bitmap"))
		TestBitmap();
	if (Wants(a, "idle"))
		TestIdle();
	if (Wants(a, "readback"))
		TestReadback();
	if (Wants(a, "flush"))
		TestFlush();
	if (Wants(a, "longjmp"))
		TestLongjmp();
	if (Wants(a, "bench"))
		TestBench();
	if (Wants(a, "lost"))
		TestDestroy();
	if (a.tests.find("crash") != std::string::npos) // never part of "all"
		TestCrash();
	if (a.tests.find("cbsuspend") != std::string::npos) // never part of "all"
		TestCallbackSuspend();
	printf("[gs] uncaptured WebGPU errors: %d\n", g.errors.load());
	return nullptr;
#else
	if (Wants(a, "bench"))
		TestBench();
	GsMainNoSuspend();
	return nullptr;
#endif
}

int main(int argc, char** argv)
{
	static Args a;
	a.tests = argc > 1 ? argv[1] : "all";
	a.compat = argc > 2 && std::strcmp(argv[2], "compat") == 0;
	const int async = emscripten_has_asyncify();
	printf("[main] build: %s, SUPPORT_LONGJMP=%s; tests=%s adapter=%s\n",
		async == 2 ? "JSPI" : async == 1 ? "Asyncify" : "no JSPI/Asyncify", SPIKE_SJLJ, a.tests.c_str(), a.compat ? "compat" : "core");

	pthread_attr_t attr;
	pthread_attr_init(&attr);
#if SPIKE_TRANSFER_CANVAS
	// The page's #canvas reached this thread at startup (OFFSCREENCANVASES_TO_PTHREAD); pass it on.
	emscripten_pthread_attr_settransferredcanvases(&attr, "#canvas");
#endif
	pthread_t gs;
	const double t0 = Now();
	int rc = pthread_create(&gs, &attr, GsMain, &a);
	printf("[main] GS pthread created%s: rc=%d\n", SPIKE_TRANSFER_CANVAS ? " with #canvas transferred" : ", no canvas transferred", rc);
	if (rc == 0)
		pthread_join(gs, nullptr);
	printf("[main] GS thread finished after %.0f ms\n", Now() - t0);
	printf("DONE\n");
	return 0;
}
