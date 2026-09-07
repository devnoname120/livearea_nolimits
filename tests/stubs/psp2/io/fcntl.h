#ifndef TEST_IO_FCNTL_H
#define TEST_IO_FCNTL_H
#include <psp2/types.h>
#define SCE_O_WRONLY 0x0002
#define SCE_O_CREAT  0x0200
#define SCE_O_TRUNC  0x0400
SceUID sceIoOpen(const char *path, int flags, int mode);
int sceIoWrite(SceUID fd, const void *data, SceSize size);
int sceIoClose(SceUID fd);
#endif
