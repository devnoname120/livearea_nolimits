#ifndef TEST_MODULEMGR_H
#define TEST_MODULEMGR_H
#include <psp2/types.h>
typedef struct { void *vaddr; SceSize memsz; } SceKernelSegmentInfo;
typedef struct {
	SceSize size;
	SceKernelSegmentInfo segments[4];
} SceKernelModuleInfo;
int sceKernelGetModuleInfo(SceUID modid, SceKernelModuleInfo *info);
#endif
