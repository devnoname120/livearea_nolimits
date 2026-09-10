#ifndef LIVEAREA_RECOVERY_H
#define LIVEAREA_RECOVERY_H

#include <psp2/kernel/modulemgr.h>
#include <psp2/types.h>
#include <stdint.h>

int recovery_start(SceUID shell_modid, uint32_t shell_nid,
	const SceKernelModuleInfo *shell_info);
int recovery_stop(void);

#endif
