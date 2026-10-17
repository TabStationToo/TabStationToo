// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVD/Emscripten/WebDiscFileReader.h"
#include "CDVD/Emscripten/WebDisc.h"

#include "common/Console.h"
#include "common/Error.h"

#include <algorithm>

// 128 KiB, as FlatFileReader: a chunk from a picked file costs 0.7-0.9 ms. Larger chunks stream faster
// but make every readahead miss cost more (docs/web-port/DISC-DESIGN.md, 5.2).
static constexpr u64 CHUNK_SIZE = 128 * 1024;

// Read errors are logged, but a disc that went away fails every read: only the first few.
static constexpr u32 MAX_ERRORS_LOGGED = 8;

WebDiscFileReader::WebDiscFileReader() = default;

WebDiscFileReader::~WebDiscFileReader() = default;

bool WebDiscFileReader::Open2(std::string filename, Error* error)
{
	WebDisc::Info info;
	if (!WebDisc::Query(&info, error))
		return false;

	if (info.size == 0)
	{
		Error::SetStringView(error, "The disc image is empty.");
		return false;
	}

	m_filename = std::move(filename);
	m_generation = info.generation;
	m_size = info.size;
	m_errors_logged.store(0, std::memory_order_relaxed);
	return true;
}

ThreadedFileReader::Chunk WebDiscFileReader::ChunkForOffset(u64 offset)
{
	Chunk chunk = {};
	if (offset >= m_size)
	{
		chunk.chunkID = -1;
	}
	else
	{
		chunk.chunkID = static_cast<s64>(offset / CHUNK_SIZE);
		chunk.offset = static_cast<u64>(chunk.chunkID) * CHUNK_SIZE;
		chunk.length = static_cast<u32>(std::min<u64>(m_size - chunk.offset, CHUNK_SIZE));
	}
	return chunk;
}

int WebDiscFileReader::ReadChunk(void* dst, s64 chunkID)
{
	if (chunkID < 0)
		return -1;

	const u64 offset = static_cast<u64>(chunkID) * CHUNK_SIZE;
	if (offset >= m_size)
		return -1;

	const u32 length = static_cast<u32>(std::min<u64>(m_size - offset, CHUNK_SIZE));
	const s32 result = WebDisc::Read(m_generation, dst, offset, length);
	if (result < 0)
	{
		// A Stop cancels reads on purpose; anything else (a timeout, or a disc reader that stopped) is worth a
		// line in the log.
		const bool stopping = (result == WebDisc::E_CANCELLED && WebDisc::ReadsAborted());
		if (!stopping && m_errors_logged.fetch_add(1, std::memory_order_relaxed) < MAX_ERRORS_LOGGED)
			Console.ErrorFmt("WebDisc: reading {} bytes at {} failed: {} ({})", length, offset, WebDisc::ResultName(result), result);
		return -1;
	}

	return result;
}

void WebDiscFileReader::Close2()
{
	m_generation = 0;
	m_size = 0;
}

u32 WebDiscFileReader::GetBlockCount() const
{
	return static_cast<u32>(m_size / m_blocksize);
}
