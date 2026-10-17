// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <webgpu/webgpu.h>

#include <memory>

// A GPU buffer and a CPU shadow of the same size (docs/web-port/WEBGPU-DESIGN.md §6.6). Data is
// written into the shadow; Flush() uploads what was used with a single queue.writeBuffer() just
// before the device submits, and starts over at offset 0. That is safe because writeBuffer() is
// ordered after earlier submits on the queue timeline, so only the encoder still being recorded
// reads the buffer. A submit that needs more than the whole buffer must submit early (the device's
// "ring full" path).
class WebGPUStreamBuffer
{
public:
	WebGPUStreamBuffer();
	~WebGPUStreamBuffer();

	bool Create(WGPUDevice device, WGPUBufferUsage usage, u32 size, const char* label);
	void Destroy();

	__fi WGPUBuffer GetBuffer() const { return m_buffer; }
	__fi u32 GetSize() const { return m_size; }
	__fi u32 GetCurrentOffset() const { return m_position; }
	__fi u8* GetCurrentHostPointer() const { return m_shadow.get() + m_position; }

	/// Moves to an offset aligned to align with size bytes free after it. False if this submit has
	/// used too much of the buffer: the caller must submit, then try again.
	bool ReserveMemory(u32 size, u32 align);
	void CommitMemory(u32 size);

	/// Uploads the bytes used since the last flush and starts over. Call right before a submit.
	void Flush(WGPUQueue queue);

private:
	WGPUBuffer m_buffer = nullptr;
	std::unique_ptr<u8[]> m_shadow;
	u32 m_size = 0;
	u32 m_position = 0;
};
