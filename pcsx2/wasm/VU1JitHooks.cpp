// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "VU1JitHooks.h"

#include "Gif_Unit.h"

namespace VU1JitHooks
{
	Path1Fn path1 = nullptr;
	InterruptFn interrupt = nullptr;
	bool mtvu_sync = false;
	ExecuteFn execute = nullptr;

	void Path1ToGif(u8* data, u32 size, bool complete)
	{
		// The two calls of _vuXGKICKTransfer's MTVU path (VUops.cpp:1875-1885), which the hook replaces.
		if (!complete)
			gifUnit.gifPath[GIF_PATH_1].CopyGSPacketData(data, size, true);
		else
			gifUnit.TransferGSPacketData(GIF_TRANS_XGKICK, data, size, true);
	}
} // namespace VU1JitHooks
