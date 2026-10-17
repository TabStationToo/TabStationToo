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
# ImGuiManager loads (Roboto is the host-registered text font; no emoji font) and the SDL
# controller database.
set(PCSX2_WASM_RESOURCES
	GameIndex.yaml
	game_controller_db.txt
	fonts/Roboto-Regular.ttf
	fonts/RobotoMono-Medium.ttf
	fonts/fa-solid-900.ttf
	fonts/promptfont.otf
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
	_pcsx2web_key_code
	_pcsx2web_key
	_pcsx2web_frame
)
# Discs, game details and memory cards (Main.cpp).
list(APPEND PCSX2_WASM_EXPORTS
	_pcsx2web_boot_disc
	_pcsx2web_disc_block
	_pcsx2web_game
)
# VU1 JIT testing (the page's ?vu1jit= parameter).
list(APPEND PCSX2_WASM_EXPORTS
	_pcsx2web_vu1jit_enable
	_pcsx2web_vu1jit_fuzz
	_pcsx2web_vu1jit_fuzz_status
	_pcsx2web_vu1jit_stats
)
list(JOIN PCSX2_WASM_EXPORTS "," PCSX2_WASM_EXPORTS)

# main() runs on a worker (PROXY_TO_PTHREAD) so the page's thread never blocks; the pool
# covers the CPU, GS and software renderer threads without waiting on the page to spawn
# workers. Emscripten's 64 KB default stacks are far too small for PCSX2.
target_link_options(pcsx2-wasm PRIVATE
	"-sENVIRONMENT=web,worker"
	"-sPROXY_TO_PTHREAD=1"
	"-sPTHREAD_POOL_SIZE=8"
	"-sEXIT_RUNTIME=0"
	"-sSTACK_SIZE=4MB"
	"-sDEFAULT_PTHREAD_STACK_SIZE=2MB"
	"-sFORCE_FILESYSTEM=1"
	"-sEXPORTED_FUNCTIONS=${PCSX2_WASM_EXPORTS}"
	"-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,HEAPU8,HEAP32,wasmMemory"
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

# The page mounts IndexedDB (IDBFS) on /memcards, so memory cards outlive a reload.
target_link_options(pcsx2-wasm PRIVATE "-lidbfs.js")
