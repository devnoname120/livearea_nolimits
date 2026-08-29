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

project(livearea_nolimits C ASM)
include("${VITASDK}/share/vita.cmake" REQUIRED)

add_executable(livearea_nolimits
	src/main.c
	src/patch_bytes.S
)

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
	c
	SceLibKernel_stub
	gcc
)

set_target_properties(livearea_nolimits PROPERTIES
	LINK_FLAGS "-nostdlib -Wl,-q"
)

vita_create_self(livearea_nolimits.suprx livearea_nolimits
	UNSAFE
	CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/livearea_nolimits.yml
)
