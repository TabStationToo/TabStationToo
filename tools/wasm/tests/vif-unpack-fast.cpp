// SPDX-FileCopyrightText: 2026 TabStationToo contributors
// SPDX-License-Identifier: GPL-3.0+
// Run: c++ -std=c++17 -O2 -Wall -Wextra -I. tools/wasm/tests/vif-unpack-fast.cpp -o /tmp/vif-test && /tmp/vif-test
#include "pcsx2/wasm/VifUnpackFast.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// Reference: the unmasked, mode-0 UNPACK_S/V2/V4 + the original generic
// _nVifUnpackLoop() address, cycle and source progression.
static void Reference(std::uint8_t* mem, unsigned size, const std::uint8_t* data,
	unsigned type, unsigned cycle_cl, unsigned cycle_wl,
	std::uint32_t& addr, std::uint32_t& num, int& cl)
{
	const bool is_fill = cycle_cl < (cycle_wl ? cycle_wl : 256);
	const unsigned v_size = type == 0x0c ? 16 : (type == 0x04 ? 8 : 4);
	const int skip_size = (int(cycle_cl) - int(cycle_wl)) * 16;
	do
	{
		auto* dest = mem + (addr & (size - 16));
		std::uint32_t src[4]{};
		std::memcpy(src, data, v_size);
		std::uint32_t out[4]{};
		if (type == 0x00)
			out[0] = out[1] = out[2] = out[3] = src[0];
		else if (type == 0x04)
		{
			out[0] = out[2] = src[0];
			out[1] = out[3] = src[1];
		}
		else
			std::memcpy(out, src, 16);
		std::memcpy(dest, out, sizeof(out));
		addr += 16;
		--num;
		++cl;
		if (is_fill)
		{
			if (cl <= int(cycle_cl))
				data += v_size;
			else if (cl == int(cycle_wl))
				cl = 0;
		}
		else
		{
			data += v_size;
			if (cl >= int(cycle_wl))
			{
				addr += skip_size;
				cl = 0;
			}
		}
	} while (num);
}

int main()
{
	std::mt19937 rng(0x97399u);
	std::array<std::uint8_t, 8192> input{};
	for (auto& b : input)
		b = std::uint8_t(rng());
	const std::array<unsigned, 13> cycles = {0, 1, 2, 3, 4, 5, 7, 8, 16, 31, 64, 128, 255};
	unsigned checked = 0;
	for (unsigned mem_size : {0x1000u, 0x4000u})
	{
		for (unsigned type : {0x00u, 0x04u, 0x0cu})
		{
			for (unsigned cycle_cl : cycles)
			{
				for (unsigned cycle_wl : cycles)
				{
					for (unsigned sample = 0; sample < 9; ++sample)
					{
						// Exercise the encoded NUM boundaries as well as random counts.
						const unsigned count = sample == 0 ? 256 : (sample == 1 ? 1 : 1 + (rng() % 256));
						std::vector<std::uint8_t> ref(mem_size), fast(mem_size);
						for (auto& b : ref)
							b = std::uint8_t(rng());
						fast = ref;
						std::uint32_t addr_ref = sample == 0 ? 0xfffffff0u : (rng() % mem_size) & ~15u;
						std::uint32_t addr_fast = addr_ref;
						std::uint32_t num_ref = count, num_fast = count;
						int cl_ref = 0, cl_fast = 0;
						const auto* data = input.data() + 4 * (sample % 4);
						Reference(ref.data(), mem_size, data, type, cycle_cl, cycle_wl,
							addr_ref, num_ref, cl_ref);
						const bool ok = VifUnpackFast::Unpack32(fast.data(), mem_size, data, type,
							cycle_cl, cycle_wl, addr_fast, num_fast, cl_fast);
						if (!ok || ref != fast || addr_ref != addr_fast || num_ref != num_fast || cl_ref != cl_fast)
						{
							std::fprintf(stderr, "Mismatch type=%02x cl=%u wl=%u n=%u size=%u\n",
								type, cycle_cl, cycle_wl, count, mem_size);
							return 1;
						}
						++checked;
					}
				}
			}
		}
	}

	// Rejected packets must leave the original loop's inputs untouched.
	std::uint32_t addr = 0x0ff0, num = 0;
	int cl = 0;
	if (VifUnpackFast::Unpack32(nullptr, 0x1000, nullptr, 0x00, 1, 1, addr, num, cl) ||
		addr != 0x0ff0 || num != 0 || cl != 0)
		return 1;
	num = 1;
	if (VifUnpackFast::Unpack32(nullptr, 0x1000, nullptr, 0x08, 1, 1, addr, num, cl) ||
		addr != 0x0ff0 || num != 1 || cl != 0)
		return 1;

	std::printf("VIF 32-bit fast path: %u cases match reference\n", checked);
}
