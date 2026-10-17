#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
# SPDX-License-Identifier: GPL-3.0+
#
# Builds the runtime WebAssembly code generation spike for Node and for the browser.
#   tools/wasm/wasm-jit-spike/build.sh [OUT_DIR]
# OUT_DIR defaults to build-wasm/wasm-jit-spike. Needs Emscripten 6.0.12 (source ~/emsdk/emsdk_env.sh).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="${1:-$ROOT/build-wasm/wasm-jit-spike}"
mkdir -p "$OUT"

# The flag family of pcsx2-wasm and pcsx2-headless (cmake/BuildParameters.cmake,
# pcsx2-wasm/CMakeLists.txt), with a small initial memory so that the spike can test growth.
COMMON=(-O3 -std=c++20 -fno-exceptions -msimd128 -msse4.1 -pthread -sSUPPORT_LONGJMP=wasm)
LINK=(
	-sPROXY_TO_PTHREAD=1 -sPTHREAD_POOL_SIZE=4
	-sSTACK_SIZE=4MB -sDEFAULT_PTHREAD_STACK_SIZE=2MB
	-sINITIAL_MEMORY=64MB -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB
	# Generated functions are put in the function table at run time (addFunction).
	-sALLOW_TABLE_GROWTH=1
	--js-library "$HERE/jit_runtime.js"
	--profiling-funcs
)

build() {
	local name="$1"; shift
	echo "== $name: em++ ${COMMON[*]} ${LINK[*]} $*"
	if em++ "${COMMON[@]}" "${LINK[@]}" "$@" "$HERE/spike.cpp" -o "$OUT/$name.js" 2> "$OUT/$name.log"; then
		echo "   ok: $(stat -c %s "$OUT/$name.wasm") B wasm, $(stat -c %s "$OUT/$name.js") B js"
	else
		echo "   FAILED (see $OUT/$name.log):"; tail -20 "$OUT/$name.log" | sed 's/^/   /'; return 1
	fi
	grep -i 'warning\|signature mismatch' "$OUT/$name.log" | sed 's/^/   /' || true
}

build spike-node -sENVIRONMENT=node -sNODERAWFS=1 -sEXIT_RUNTIME=1
build spike-web -sENVIRONMENT=web,worker -sEXIT_RUNTIME=0
cp "$HERE/index.html" "$OUT/index.html"
