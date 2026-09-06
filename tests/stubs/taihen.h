#ifndef TEST_TAIHEN_H
#define TEST_TAIHEN_H
#include <psp2/types.h>
typedef struct {
	SceSize size;
	SceUID modid;
	uint32_t module_nid;
} tai_module_info_t;
int taiGetModuleInfo(const char *name, tai_module_info_t *info);
SceUID taiInjectData(SceUID modid, int segment, uint32_t offset,
	const void *data, SceSize size);
int taiInjectRelease(SceUID uid);
#endif
