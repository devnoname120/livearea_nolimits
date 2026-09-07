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

project(livearea_nolimits VERSION 1.6.0 LANGUAGES C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

option(LIVEAREA_ICON_CACHE_TRIAL
	"Enable icon-cache eviction and reload correction on supported firmware" ON)
option(LIVEAREA_ICON_CACHE_LOGGING
	"Enable icon-cache startup diagnostics" OFF)

add_executable(livearea_nolimits
	src/main.c
	src/recovery.c
	src/patch_bytes.S
)

if(LIVEAREA_ICON_CACHE_TRIAL)
	target_sources(livearea_nolimits PRIVATE src/icon_cache_trial.c)
	target_compile_definitions(livearea_nolimits PRIVATE LIVEAREA_ICON_CACHE_TRIAL=1)
	if(LIVEAREA_ICON_CACHE_LOGGING)
		target_compile_definitions(livearea_nolimits PRIVATE LIVEAREA_ICON_CACHE_LOGGING=1)
		target_link_libraries(livearea_nolimits SceIofilemgr_stub)
	endif()
endif()

target_compile_options(livearea_nolimits PRIVATE
	-Wall
	-Wextra
	-Werror
	-O2
	-fno-pic
	-fno-PIC
)

target_include_directories(livearea_nolimits PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(livearea_nolimits
	taihen_stub
	SceKernelModulemgr_stub
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
