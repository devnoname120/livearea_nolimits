#ifndef TEST_KERNEL_CLIB_H
#define TEST_KERNEL_CLIB_H
#include <psp2/types.h>
#include <stdarg.h>
int sceClibSnprintf(char *dst, SceSize size, const char *format, ...);
int sceClibVsnprintf(char *dst, SceSize size, const char *format, va_list args);
#endif
