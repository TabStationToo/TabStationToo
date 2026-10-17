// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Renderers/WebGPU/WebGPUStreamBuffer.h"

#include "common/Assertions.h"
#include "common/Console.h"

WebGPUStreamBuffer::WebGPUStreamBuffer() = default;

WebGPUStreamBuffer::~WebGPUStreamBuffer()
{
	Destroy();
}

bool WebGPUStreamBuffer::Create(WGPUDevice device, WGPUBufferUsage usage, u32 size, const char* label)
{
	Destroy();

	WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
	desc.label = {label, WGPU_STRLEN};
	desc.usage = usage | WGPUBufferUsage_CopyDst;
	desc.size = size;
	m_buffer = wgpuDeviceCreateBuffer(device, &desc);
	if (!m_buffer)
	{
		Console.ErrorFmt("WebGPU: failed to create the {} ({} bytes).", label, size);
		return false;
	}

	m_shadow = std::make_unique<u8[]>(size);
	m_size = size;
	m_position = 0;
	return true;
}

void WebGPUStreamBuffer::Destroy()
{
	if (m_buffer)
	{
		wgpuBufferDestroy(m_buffer);
		wgpuBufferRelease(m_buffer);
		m_buffer = nullptr;
	}

	m_shadow.reset();
	m_size = 0;
	m_position = 0;
}

bool WebGPUStreamBuffer::ReserveMemory(u32 size, u32 align)
{
	pxAssert(align > 0);
	const u32 aligned = ((m_position + align - 1) / align) * align;
	if (aligned > m_size || size > m_size - aligned)
		return false;

	m_position = aligned;
	return true;
}

void WebGPUStreamBuffer::CommitMemory(u32 size)
{
	pxAssert(m_position + size <= m_size);
	m_position += size;
}

void WebGPUStreamBuffer::Flush(WGPUQueue queue)
{
	// writeBuffer sizes must be multiples of 4; the padding bytes are never read.
	const u32 size = std::min((m_position + 3u) & ~3u, m_size);
	if (size > 0)
		wgpuQueueWriteBuffer(queue, m_buffer, 0, m_shadow.get(), size);
	m_position = 0;
}
