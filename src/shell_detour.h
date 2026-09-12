#ifndef LIVEAREA_SHELL_DETOUR_H
#define LIVEAREA_SHELL_DETOUR_H
#include <stdint.h>
#include <psp2/types.h>
int shell_detour_encode_branch(uint8_t out[4], uintptr_t source, uintptr_t target);
int shell_detour_encode_call(uint8_t out[4], uintptr_t source, uintptr_t target);
SceUID shell_detour_install(SceUID module, uint32_t offset,
    uintptr_t source, const void *target);
#endif
