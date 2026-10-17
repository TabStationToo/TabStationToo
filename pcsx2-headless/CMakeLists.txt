add_executable(pcsx2-headless)

target_sources(pcsx2-headless PRIVATE
	Main.cpp
	WavOutput.cpp
	WavOutput.h
)

target_include_directories(pcsx2-headless PRIVATE
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
)

target_link_libraries(pcsx2-headless PRIVATE
	PCSX2_FLAGS
	PCSX2
)

if(EMSCRIPTEN)
	# Runs under Node with the real filesystem. main() runs on a worker so the main
	# JS thread stays free to start the emulator's threads; Emscripten's 64 KB
	# default stacks are far too small for PCSX2.
	target_link_options(pcsx2-headless PRIVATE
		"-sENVIRONMENT=node"
		"-sNODERAWFS=1"
		"-sPROXY_TO_PTHREAD=1"
		"-sEXIT_RUNTIME=1"
		"-sSTACK_SIZE=4MB"
		"-sDEFAULT_PTHREAD_STACK_SIZE=2MB"
		# Keep function names so traps give readable stacks.
		"--profiling-funcs"
	)
endif()
