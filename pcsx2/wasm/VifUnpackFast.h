// SPDX-FileCopyrightText: 2026 TabStationToo contributors
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <cstdint>
#include <cstring>

// Small, unmasked 32-bit VIF packets are common in geometry-heavy games. The
// WebAssembly build cannot use the desktop VIF dynarec, so replace repeated
// indirect per-vector UNPACK calls with direct copies for S-32, V2-32 and V4-32.
// This is intentionally independent of PCSX2 structs to make the state
// transitions and VU-memory wrapping testable on a native compiler.
namespace VifUnpackFast
{
	inline bool Unpack32(std::uint8_t* mem, std::uint32_t mem_size, const std::uint8_t* data,
		std::uint32_t unpack_type, std::uint8_t cycle_cl, std::uint8_t cycle_wl,
		std::uint32_t& addr, std::uint32_t& num, int& cl)
	{
		if (!num || (unpack_type != 0x00 && unpack_type != 0x04 && unpack_type != 0x0c))
			return false;

		const unsigned wl = cycle_wl ? cycle_wl : 256;
		const bool is_fill = cycle_cl < wl;
		const unsigned v_size = ((unpack_type >> 2) + 1) * 4;
		const int skip_size = (int(cycle_cl) - int(cycle_wl)) * 16;
		const std::uint32_t mem_mask = mem_size - 16;

		do
		{
			std::uint8_t* const dest = mem + (addr & mem_mask);
			if (unpack_type == 0x0c)
			{
				std::memcpy(dest, data, 16);
			}
			else
			{
				std::uint32_t first, second;
				std::memcpy(&first, data, 4);
				if (unpack_type == 0x04)
					std::memcpy(&second, data + 4, 4);
				else
					second = first;
				const std::uint32_t words[4] = {first, second, first, second};
				std::memcpy(dest, words, sizeof(words));
			}

			addr += 16;
			--num;
			++cl;
			if (is_fill)
			{
				if (cl <= cycle_cl)
					data += v_size;
				else if (cl == cycle_wl)
					cl = 0;
			}
			else
			{
				data += v_size;
				if (cl >= cycle_wl)
				{
					addr += skip_size;
					cl = 0;
				}
			}
		} while (num);
		return true;
	}
} // namespace VifUnpackFast
