#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+
#
# Builds the WebGPU runtime spike in several flag variants, one emcc at a time.
#   tools/wasm/webgpu-spike/build.sh [OUT_DIR] [VARIANT...]
# OUT_DIR defaults to build-wasm/webgpu-spike; variants: jspi jspi-bitmap asyncify none jspi-sjlj-js
# (default: all).
# Needs Emscripten 6.0.12 (source ~/emsdk/emsdk_env.sh). The emdawnwebgpu port is downloaded on
# first use.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="${1:-$ROOT/build-wasm/webgpu-spike}"
shift || true
VARIANTS=("$@")
[ ${#VARIANTS[@]} -eq 0 ] && VARIANTS=(jspi jspi-bitmap asyncify none jspi-sjlj-js)
mkdir -p "$OUT"

# The flag family of pcsx2-wasm (pcsx2-wasm/CMakeLists.txt, cmake/BuildParameters.cmake).
COMMON=(-O2 -std=c++20 -fno-exceptions -msimd128 -pthread --use-port=emdawnwebgpu)
LINK=(
	-sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=4 -sEXIT_RUNTIME=0
	-sSTACK_SIZE=4MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB
	-sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB
	-sENVIRONMENT=web,worker
)
# The page's canvas reaches the CPU thread at startup (OFFSCREENCANVASES_TO_PTHREAD="#canvas"),
# which hands it to the GS thread. (Emscripten keeps it in GL.offscreenCanvases; emdawnwebgpu's
# canvas lookup already links $GL, so no -sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE is needed.)
TRANSFER=(-sOFFSCREENCANVAS_SUPPORT=1)

for v in "${VARIANTS[@]}"; do
	case "$v" in
		jspi) FLAGS=(-DSPIKE_SUSPEND=1 -sSUPPORT_LONGJMP=wasm -sJSPI=1 "${TRANSFER[@]}") ;;
		jspi-bitmap) FLAGS=(-DSPIKE_SUSPEND=1 -DSPIKE_TRANSFER_CANVAS=0 -sSUPPORT_LONGJMP=wasm -sJSPI=1) ;;
		asyncify) FLAGS=(-DSPIKE_SUSPEND=1 -sSUPPORT_LONGJMP=wasm -sASYNCIFY=1 -sASYNCIFY_STACK_SIZE=65536 "${TRANSFER[@]}") ;;
		none) FLAGS=(-DSPIKE_SUSPEND=0 -sSUPPORT_LONGJMP=wasm "${TRANSFER[@]}") ;;
		jspi-sjlj-js) FLAGS=(-DSPIKE_SUSPEND=1 '-DSPIKE_SJLJ="emscripten"' -sSUPPORT_LONGJMP=emscripten -sJSPI=1 "${TRANSFER[@]}") ;;
		*) echo "unknown variant $v" >&2; exit 1 ;;
	esac
	echo "== $v: em++ ${COMMON[*]} ${FLAGS[*]} ${LINK[*]}"
	if em++ "${COMMON[@]}" "${FLAGS[@]}" "${LINK[@]}" "$HERE/spike.cpp" -o "$OUT/spike-$v.js" 2> "$OUT/spike-$v.log"; then
		echo "   ok: $(stat -c %s "$OUT/spike-$v.wasm") B wasm, $(stat -c %s "$OUT/spike-$v.js") B js"
	else
		echo "   FAILED (see $OUT/spike-$v.log):"; tail -5 "$OUT/spike-$v.log" | sed 's/^/   /'
	fi
	grep -i 'warning' "$OUT/spike-$v.log" | sed 's/^/   /' || true
done
cp "$HERE/index.html" "$OUT/index.html"
