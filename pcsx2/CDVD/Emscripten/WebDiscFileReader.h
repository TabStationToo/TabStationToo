// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "CDVD/ThreadedFileReader.h"

/// A flat disc image (ISO or raw BIN/IMG) that the page reads from the user's file or a URL through
/// its disc worker (see WebDisc.h). Picked by GetFileReader() for "/disc/<name>" paths.
class WebDiscFileReader final : public ThreadedFileReader
{
	DeclareNoncopyableObject(WebDiscFileReader);

	u32 m_generation = 0;
	u64 m_size = 0;
	std::atomic<u32> m_errors_logged{0}; // the read thread and the CPU thread both read

public:
	WebDiscFileReader();
	~WebDiscFileReader() override;

	bool Open2(std::string filename, Error* error) override;

	Chunk ChunkForOffset(u64 offset) override;
	int ReadChunk(void* dst, s64 chunkID) override;

	// The page owns the file, so there is nothing to close. Precaching keeps the base class's "not
	// supported": a disc does not fit in the wasm heap.
	void Close2() override;

	u32 GetBlockCount() const override;
};
