#ifndef LIVEAREA_ICON_CACHE_TRIAL_H
#define LIVEAREA_ICON_CACHE_TRIAL_H

#include <psp2/kernel/modulemgr.h>
#include <stdint.h>

int icon_cache_trial_start(SceUID shell_modid, uint32_t shell_nid,
	const SceKernelModuleInfo *shell_info);
void icon_cache_trial_stop(void);

#endif
