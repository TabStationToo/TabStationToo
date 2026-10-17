#!/bin/sh
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+
#
# Builds requester.wasm (the C requester the disc harness can use instead of its JS one) with the
# emsdk's clang. Usage: tools/wasm/disc-harness/build-requester.sh   (after source ~/emsdk/emsdk_env.sh)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
clang=${EMSDK:?source ~/emsdk/emsdk_env.sh first}/upstream/bin/clang
"$clang" --target=wasm32 -O2 -matomics -mbulk-memory -nostdlib \
	-Wl,--no-entry -Wl,--import-memory -Wl,--shared-memory -Wl,--max-memory=2147483648 \
	-Wl,--export=disc_open -Wl,--export=disc_read \
	-o "$here/requester.wasm" "$here/requester.c"
echo "wrote $here/requester.wasm"
