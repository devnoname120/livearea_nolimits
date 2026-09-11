#ifndef INSTANCE_CACHE_H
#define INSTANCE_CACHE_H
#include <psp2/kernel/modulemgr.h>
#include <stdint.h>
int instance_cache_start(SceUID module, uint32_t nid, const SceKernelModuleInfo *info);
int instance_cache_can_unload(void);
#endif
