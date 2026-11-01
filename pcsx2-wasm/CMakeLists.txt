# Browser frontend: <build>/bin/ becomes the web root (pcsx2-wasm.js/.wasm/.data plus the
# page from web/). Serve it with tools/wasm/serve.py, which sets the COOP/COEP headers.
if(NOT EMSCRIPTEN)
	return()
endif()

add_executable(pcsx2-wasm)

target_sources(pcsx2-wasm PRIVATE
	Main.cpp
)

target_include_directories(pcsx2-wasm PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

target_link_libraries(pcsx2-wasm PRIVATE
	PCSX2_FLAGS
	PCSX2
)

# Only what the core reads at runtime, mounted at /resources: the game database, the fonts
# ImGuiManager loads (Roboto is the host-registered text font; no emoji font), the SDL
# controller database and the WebGPU device's shaders.
set(PCSX2_WASM_RESOURCES
	GameIndex.yaml
	game_controller_db.txt
	fonts/Roboto-Regular.ttf
	fonts/RobotoMono-Medium.ttf
	fonts/fa-solid-900.ttf
	fonts/promptfont.otf
	shaders/webgpu/convert.wgsl
	shaders/webgpu/imgui.wgsl
	shaders/webgpu/interlace.wgsl
	shaders/webgpu/merge.wgsl
	shaders/webgpu/present.wgsl
	shaders/webgpu/shadeboost.wgsl
	shaders/webgpu/tfx.wgsl
)
set(PCSX2_WASM_PRELOAD_OPTIONS)
set(PCSX2_WASM_PRELOAD_FILES)
foreach(res IN LISTS PCSX2_WASM_RESOURCES)
	set(src "${CMAKE_SOURCE_DIR}/bin/resources/${res}")
	# The --opt=value form is one argument per file: CMake neither splits it at spaces nor
	# de-duplicates it. '@' separates source and destination, so escape it in the path.
	string(REPLACE "@" "@@" src_escaped "${src}")
	list(APPEND PCSX2_WASM_PRELOAD_OPTIONS "--preload-file=${src_escaped}@/resources/${res}")
	list(APPEND PCSX2_WASM_PRELOAD_FILES "${src}")
endforeach()
set_property(TARGET pcsx2-wasm APPEND PROPERTY LINK_DEPENDS ${PCSX2_WASM_PRELOAD_FILES}
	"${CMAKE_SOURCE_DIR}/pcsx2/wasm/WasmJitRuntime.js")

set(PCSX2_WASM_EXPORTS
	_main
	_pcsx2web_boot
	_pcsx2web_stop
	_pcsx2web_state
	_pcsx2web_speed
	_pcsx2web_vps
	_pcsx2web_thread_usage
	_pcsx2web_perf_ring
	_pcsx2web_perf_written
	_pcsx2web_key_code
	_pcsx2web_key
	_pcsx2web_pad_input
	_pcsx2web_pad
	_pcsx2web_test_pad_state
	_pcsx2web_frame
	_pcsx2web_surface_size
	_pcsx2web_set_crisp
	_pcsx2web_set_tv_shader
	_pcsx2web_set_ee_cycle_rate
	_pcsx2web_vsync_count
	_pcsx2web_test_snapshot
	_pcsx2web_test_lose_device
	_pcsx2web_test_capture
)
# Discs, game details and memory cards (Main.cpp).
list(APPEND PCSX2_WASM_EXPORTS
	_pcsx2web_boot_disc
	_pcsx2web_boot_elf
	_pcsx2web_disc_block
	_pcsx2web_game
	_pcsx2web_memcard
	_pcsx2web_memcard_status
	_pcsx2web_set_boot_state
	_pcsx2web_save_state
	_pcsx2web_load_state
	_pcsx2web_state_result
	_pcsx2web_change_disc
	_pcsx2web_disc_change_result
	_pcsx2web_pad_vibration
)
# The racing wheels on the two USB ports (Main.cpp, "Racing wheels").
list(APPEND PCSX2_WASM_EXPORTS
	_pcsx2web_set_wheel
	_pcsx2web_wheel
	_pcsx2web_ffb_ring
)
# Per-game TFX pipeline lists (Main.cpp, WebGPUHost.h).
list(APPEND PCSX2_WASM_EXPORTS
	_pcsx2web_tfx_log
	_pcsx2web_tfx_prewarm
)
# Per-game EE JIT code lists (Main.cpp, EEJit.h).
list(APPEND PCSX2_WASM_EXPORTS
	_pcsx2web_ee_code_log
	_pcsx2web_ee_code_list
)
# Audio (Main.cpp, "Audio").
list(APPEND PCSX2_WASM_EXPORTS
	_pcsx2web_audio_start
	_pcsx2web_audio
)
# VU1 JIT testing (the page's ?vu1jit= parameter).
list(APPEND PCSX2_WASM_EXPORTS
	_pcsx2web_vu1jit_enable
	_pcsx2web_vu1jit_fuzz
	_pcsx2web_vu1jit_fuzz_status
	_pcsx2web_vu1jit_stats
)
list(JOIN PCSX2_WASM_EXPORTS "," PCSX2_WASM_EXPORTS)
set_property(TARGET pcsx2-wasm APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/jspi-fallback.js"
	"${CMAKE_SOURCE_DIR}/pcsx2/wasm/WasmJitRuntime.js")

# main() runs on a worker (PROXY_TO_PTHREAD) so the page's thread never blocks; the pool
# covers the CPU, GS and software renderer threads without waiting on the page to spawn
# workers (up to 6 rasterizer threads, Main.cpp). Emscripten's 64 KB default stacks are far too small for PCSX2.
target_link_options(pcsx2-wasm PRIVATE
	"-sENVIRONMENT=web,worker"
	"-sPROXY_TO_PTHREAD=1"
	"-sPTHREAD_POOL_SIZE=12"
	"-sEXIT_RUNTIME=0"
	"-sSTACK_SIZE=4MB"
	"-sDEFAULT_PTHREAD_STACK_SIZE=2MB"
	"-sFORCE_FILESYSTEM=1"
	"-sEXPORTED_FUNCTIONS=${PCSX2_WASM_EXPORTS}"
	"-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,HEAPU8,HEAP32,wasmMemory"
	# The GS thread waits for WebGPU (adapter, device, readbacks) by suspending its stack with JSPI
	# (docs/web-port/WEBGPU-DESIGN.md §3.2). It only wraps main() and thread entry points; the
	# pcsx2web_* exports stay synchronous. jspi-fallback.js lets the module start in browsers
	# without JSPI, which then use the 2D path.
	"-sJSPI"
	"--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/jspi-fallback.js"
	# Keep function names so traps give readable stacks.
	"--profiling-funcs"
	${PCSX2_WASM_PRELOAD_OPTIONS}
)

# Copy the page next to the build output on every build of the target (a POST_BUILD step
# would miss edits to web/ that do not relink). The directory is spelled out rather than
# $<TARGET_FILE_DIR:pcsx2-wasm>, which before CMake 3.19 would make this target depend on
# pcsx2-wasm and form a cycle.
if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
	set(PCSX2_WASM_WEB_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
else()
	set(PCSX2_WASM_WEB_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()
add_custom_target(pcsx2-wasm-web
	COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/web" "${PCSX2_WASM_WEB_DIR}"
	COMMENT "Copying the pcsx2-wasm web page"
	VERBATIM
)
add_dependencies(pcsx2-wasm pcsx2-wasm-web)

# The disc library page (web/library.html) names discs from their serials before they boot, with this
# index of the game database.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
	OUTPUT "${PCSX2_WASM_WEB_DIR}/game-titles.tsv"
	COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/tools/wasm/make-game-titles.py"
		"${CMAKE_SOURCE_DIR}/bin/resources/GameIndex.yaml" "${PCSX2_WASM_WEB_DIR}/game-titles.tsv"
	DEPENDS "${CMAKE_SOURCE_DIR}/tools/wasm/make-game-titles.py" "${CMAKE_SOURCE_DIR}/bin/resources/GameIndex.yaml"
	COMMENT "Indexing game titles for the disc library page"
	VERBATIM
)
add_custom_target(pcsx2-wasm-titles DEPENDS "${PCSX2_WASM_WEB_DIR}/game-titles.tsv")
add_dependencies(pcsx2-wasm pcsx2-wasm-titles)

# Audio (Main.cpp, "Audio"; docs/web-port/AUDIO-DESIGN.md): an Emscripten Wasm Audio Worklet runs
# AudioStream::ReadFrames() on the browser's audio rendering thread. The module's JS is evaluated in
# the AudioWorkletGlobalScope too, so the glue must never touch window or document at load.
target_link_options(pcsx2-wasm PRIVATE "-sAUDIO_WORKLET=1" "-sWASM_WORKERS=1")
