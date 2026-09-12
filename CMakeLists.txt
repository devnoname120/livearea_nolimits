cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
	if(DEFINED ENV{VITASDK})
		set(CMAKE_TOOLCHAIN_FILE
			"$ENV{VITASDK}/share/vita.toolchain.cmake"
			CACHE PATH "VitaSDK toolchain file")
	else()
		message(FATAL_ERROR "Set VITASDK or CMAKE_TOOLCHAIN_FILE")
	endif()
endif()

project(livearea_nolimits VERSION 1.9.1 LANGUAGES C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

option(LIVEAREA_ICON_CACHE_TRIAL
	"Enable per-instance icon-cache eviction and reload correction" ON)
option(LIVEAREA_ICON_CACHE_LOGGING
	"Enable icon-cache startup diagnostics" OFF)
option(LIVEAREA_DEBUG_LOGGING
	"Enable comprehensive crash-resilient diagnostics" OFF)
set(LIVEAREA_DEBUG_BUILD_ID "local" CACHE STRING
	"Identifier written to comprehensive diagnostic logs")
set(LIVEAREA_DEBUG_SYNC_INTERVAL "1" CACHE STRING
	"Number of diagnostic log lines written between storage synchronizations")
if(LIVEAREA_ICON_CACHE_LOGGING)
	# Preserve the old logging switch as an alias for the unified logger.
	set(LIVEAREA_DEBUG_LOGGING ON)
endif()

add_executable(livearea_nolimits
	src/main.c
	src/recovery.c
	src/patch_bytes.S
	src/shell_detour.c
)

if(LIVEAREA_ICON_CACHE_TRIAL)
	target_sources(livearea_nolimits PRIVATE src/instance_cache.c
		src/instance_cache_entry.S)
	target_compile_definitions(livearea_nolimits PRIVATE LIVEAREA_ICON_CACHE_TRIAL=1)
endif()

if(LIVEAREA_DEBUG_LOGGING)
	if(NOT LIVEAREA_DEBUG_SYNC_INTERVAL MATCHES "^[1-9][0-9]*$")
		message(FATAL_ERROR "LIVEAREA_DEBUG_SYNC_INTERVAL must be a positive integer")
	endif()
	target_sources(livearea_nolimits PRIVATE src/debug_log.c)
	target_compile_definitions(livearea_nolimits PRIVATE
		LIVEAREA_DEBUG_LOGGING=1
		LIVEAREA_DEBUG_BUILD_ID="${LIVEAREA_DEBUG_BUILD_ID}"
		LIVEAREA_DEBUG_SYNC_INTERVAL=${LIVEAREA_DEBUG_SYNC_INTERVAL}
	)
endif()

if(LIVEAREA_ICON_CACHE_LOGGING OR LIVEAREA_DEBUG_LOGGING)
	target_link_libraries(livearea_nolimits SceIofilemgr_stub)
endif()

target_compile_options(livearea_nolimits PRIVATE
	-Wall
	-Wextra
	-Werror
	-O2
	-fno-pic
	-fno-PIC
	# vita-elf-create derives module metadata from the ELF, including debug data.
	$<$<CONFIG:RelWithDebInfo>:-fdebug-prefix-map=${CMAKE_BINARY_DIR}=.>
)

target_include_directories(livearea_nolimits PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(livearea_nolimits
	taihen_stub
	SceKernelModulemgr_stub
	SceKernelThreadMgr_stub
	# System imports must precede newlib's application-runtime definitions.
	SceLibKernel_stub
	c
	# Newlib's selected wrappers also depend on system imports.
	SceLibKernel_stub
	gcc
)

set_target_properties(livearea_nolimits PROPERTIES
	C_STANDARD 11
	C_STANDARD_REQUIRED YES
	LINK_FLAGS "-nostdlib -Wl,-q"
)

vita_create_self(livearea_nolimits.suprx livearea_nolimits
	UNSAFE
	CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/livearea_nolimits.yml
)
