#ifndef TEST_MODULEMGR_H
#define TEST_MODULEMGR_H
#include <psp2/types.h>
#define SCE_KERNEL_STOP_CANCEL 1
typedef struct { void *vaddr; SceSize memsz; } SceKernelSegmentInfo;
typedef struct {
	SceSize size;
	SceKernelSegmentInfo segments[4];
} SceKernelModuleInfo;
int sceKernelGetModuleInfo(SceUID modid, SceKernelModuleInfo *info);
#endif
