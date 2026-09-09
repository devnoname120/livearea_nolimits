#include <psp2/kernel/modulemgr.h>
#include <psp2/types.h>
#include <stdint.h>
#include <string.h>
#include <taihen.h>

#include "debug_log.h"
#include "limits.h"
#include "recovery.h"

#define RECOVERY_TEXT_SIZE 0x3E9E8U
#define RECOVERY_DATA_SIZE 0x3094U
#define MODULEMGR_LIBRARY 0xEAED1616U
#define START_MODULE_NID 0x72CD301FU
#define STOP_MODULE_NID 0x086867A8U
#define UNLOAD_MODULE_NID 0x8E4A7716U
#define RECOVERY_ERROR (-1)
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

extern const uint8_t patch_cmp_r0_icon_limit[4];
extern const uint8_t patch_rsbs_r1_r0_icon_limit[4];
extern const uint8_t patch_cmp_r4_page_limit[2];
extern const uint8_t patch_movs_r9_last_page[4];

typedef struct {
	uint32_t offset;
	unsigned int size;
	uint8_t expected[4];
	const uint8_t *replacement;
} RecoveryPatch;

static const RecoveryPatch recovery_patches[] = {
	{0x06280, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x07054, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x0C700, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
	{0x0C776, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
	{0x0C834, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
	{0x06084, 2, {0x0A, 0x2C}, patch_cmp_r4_page_limit},
	{0x060FE, 4, {0x5F, 0xF0, 0x09, 0x09}, patch_movs_r9_last_page},
};

static const struct {
	uint32_t shell_nid;
	uint32_t recovery_nid;
} recovery_profiles[] = {
	{0x0552F692U, 0xC1F30F67U},
	{0x5549BF1FU, 0x3F76E38FU},
	{0xEAB89D5CU, 0xC1F30F67U},
};

typedef struct {
	uint32_t flags;
	const void *option;
	int *result;
	uint32_t reserved;
} ModuleAction;

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(ModuleAction) == 16, "raw module-action ABI must be 16 bytes");
#endif

/* Unrelated-module fast paths can overlap the serialized recovery lifecycle. */
static _Atomic uint32_t expected_recovery_nid;
static _Atomic SceUID recovery_modid = -1;
static int recovery_running;
static volatile int lifecycle_busy;
static SceUID patch_ids[ARRAY_COUNT(recovery_patches)];
static SceUID lifecycle_ids[3] = {-1, -1, -1};
static tai_hook_ref_t lifecycle_refs[3];
static _Atomic int initialized;

static int enter_lifecycle(void)
{
	return __sync_bool_compare_and_swap(&lifecycle_busy, 0, 1);
}

static void leave_lifecycle(void)
{
	__sync_lock_release(&lifecycle_busy);
}

static int verify_recovery(const SceKernelModuleInfo *info)
{
	const uint8_t *text = info->segments[0].vaddr;
	unsigned int i;

	debug_logf("recovery", "validation-begin text=0x%08X text_size=0x%08X data=0x%08X data_size=0x%08X",
		(unsigned int)(uintptr_t)text, (unsigned int)info->segments[0].memsz,
		(unsigned int)(uintptr_t)info->segments[1].vaddr,
		(unsigned int)info->segments[1].memsz);
	if (text == NULL || info->segments[0].memsz != RECOVERY_TEXT_SIZE ||
		info->segments[1].vaddr == NULL || info->segments[1].memsz != RECOVERY_DATA_SIZE) {
		debug_logf("recovery", "validation-segment-mismatch expected_text=0x%08X expected_data=0x%08X",
			RECOVERY_TEXT_SIZE, RECOVERY_DATA_SIZE);
		return -1;
	}
	for (i = 0; i < ARRAY_COUNT(recovery_patches); ++i) {
		const RecoveryPatch *patch = &recovery_patches[i];
		debug_logf("recovery", "patch-verify index=%u offset=0x%08X size=%u",
			i, (unsigned int)patch->offset, patch->size);
		if (memcmp(text + patch->offset, patch->expected, patch->size) != 0) {
			debug_logf("recovery", "patch-mismatch index=%u offset=0x%08X",
				i, (unsigned int)patch->offset);
			debug_log_hex("recovery", "patch-actual", patch->offset,
				text + patch->offset, patch->size);
			debug_log_hex("recovery", "patch-expected", patch->offset,
				patch->expected, patch->size);
			return -1;
		}
		debug_logf("recovery", "patch-verified index=%u", i);
	}

	debug_logf("recovery", "validation-complete patches=%u",
		(unsigned int)ARRAY_COUNT(recovery_patches));
	return 0;
}

static int release_recovery(void)
{
	int i, result;

	debug_logf("recovery", "release-begin modid=%d running=%d",
		recovery_modid, recovery_running);
	if (recovery_modid < 0) {
		debug_logf("recovery", "release-noop");
		return 0;
	}
	if (recovery_running) {
		debug_logf("recovery", "release-blocked-running modid=%d", recovery_modid);
		return RECOVERY_ERROR;
	}
	for (i = (int)ARRAY_COUNT(patch_ids) - 1; i >= 0; --i) {
		if (patch_ids[i] >= 0) {
			result = taiInjectRelease(patch_ids[i]);
			debug_logf("recovery", "patch-release index=%d uid=%d result=%d",
				i, patch_ids[i], result);
			if (result < 0)
				return result;
			patch_ids[i] = -1;
		}
	}
	recovery_modid = -1;
	debug_logf("recovery", "release-complete");
	return 0;
}

static int install_recovery(const tai_module_info_t *module)
{
	SceKernelModuleInfo info = {0};
	unsigned int i;
	int result;
	int verify_result;

	debug_logf("recovery", "install-begin modid=%d nid=0x%08X",
		module->modid, (unsigned int)module->module_nid);
	info.size = sizeof(info);
	result = sceKernelGetModuleInfo(module->modid, &info);
	debug_logf("recovery", "module-info result=%d text=0x%08X text_size=0x%08X data=0x%08X data_size=0x%08X",
		result,
		result < 0 ? 0U : (unsigned int)(uintptr_t)info.segments[0].vaddr,
		result < 0 ? 0U : (unsigned int)info.segments[0].memsz,
		result < 0 ? 0U : (unsigned int)(uintptr_t)info.segments[1].vaddr,
		result < 0 ? 0U : (unsigned int)info.segments[1].memsz);
	if (result < 0)
		return RECOVERY_ERROR;
	verify_result = verify_recovery(&info);
	debug_logf("recovery", "validation-result result=%d", verify_result);
	if (verify_result < 0)
		return RECOVERY_ERROR;

	recovery_modid = module->modid;
	for (i = 0; i < ARRAY_COUNT(recovery_patches); ++i) {
		const RecoveryPatch *patch = &recovery_patches[i];
		debug_logf("recovery", "patch-inject index=%u offset=0x%08X size=%u",
			i, (unsigned int)patch->offset, patch->size);
		patch_ids[i] = taiInjectData(module->modid, 0, patch->offset,
			patch->replacement, patch->size);
		debug_logf("recovery", "patch-injected index=%u uid=%d", i, patch_ids[i]);
		if (patch_ids[i] < 0)
			return patch_ids[i];
	}
	debug_logf("recovery", "install-complete modid=%d", module->modid);
	return 0;
}

static int start_module_hook(SceUID modid, SceSize args, const void *argp,
	ModuleAction *action)
{
	tai_module_info_t module;
	ModuleAction local_action;
	int status = -1;
	int result, installed = 0;
#if LIVEAREA_DEBUG_LOGGING
	int lookup_result;

	debug_logf("recovery", "module-start observed modid=%d args=%u argp=0x%08X action=0x%08X expected_nid=0x%08X",
		modid, (unsigned int)args, (unsigned int)(uintptr_t)argp,
		(unsigned int)(uintptr_t)action, (unsigned int)expected_recovery_nid);
	module.size = sizeof(module);
	if (action == NULL) {
		debug_logf("recovery", "module-start passthrough reason=action-null");
		return TAI_CONTINUE(int, lifecycle_refs[0], modid, args, argp, action);
	}
	lookup_result = taiGetModuleInfo("SceDbRecovery", &module);
	debug_logf("recovery", "module-start lookup result=%d found_modid=%d found_nid=0x%08X",
		lookup_result, lookup_result < 0 ? -1 : module.modid,
		lookup_result < 0 ? 0U : (unsigned int)module.module_nid);
	if (lookup_result < 0 || module.modid != modid ||
		module.module_nid != expected_recovery_nid) {
		debug_logf("recovery", "module-start passthrough reason=identity-mismatch");
		return TAI_CONTINUE(int, lifecycle_refs[0], modid, args, argp, action);
	}
#else
	module.size = sizeof(module);
	if (action == NULL || taiGetModuleInfo("SceDbRecovery", &module) < 0 ||
		module.modid != modid || module.module_nid != expected_recovery_nid)
		return TAI_CONTINUE(int, lifecycle_refs[0], modid, args, argp, action);
#endif
	if (!enter_lifecycle()) {
		debug_logf("recovery", "module-start lifecycle-busy");
		return RECOVERY_ERROR;
	}
	if (recovery_running) {
		debug_logf("recovery", "module-start already-running");
		result = TAI_CONTINUE(int, lifecycle_refs[0], modid, args, argp, action);
		debug_logf("recovery", "module-start already-running result=%d", result);
		leave_lifecycle();
		return result;
	}
	result = release_recovery();
	debug_logf("recovery", "preinstall-release result=%d", result);
	if (result < 0)
		goto done;
	result = install_recovery(&module);
	debug_logf("recovery", "install-result result=%d", result);
	debug_log_flush();
	if (result >= 0) {
		installed = 1;
	} else {
		result = release_recovery();
		debug_logf("recovery", "install-failure-cleanup result=%d", result);
		if (result < 0)
			goto done;
	}

	local_action.flags = action->flags;
	local_action.option = action->option;
	local_action.result = action->result ? action->result : &status;
	local_action.reserved = 0;
	debug_logf("recovery", "native-start begin installed=%d flags=0x%08X option=0x%08X result_ptr=0x%08X",
		installed, (unsigned int)local_action.flags,
		(unsigned int)(uintptr_t)local_action.option,
		(unsigned int)(uintptr_t)local_action.result);
	result = TAI_CONTINUE(int, lifecycle_refs[0], modid, args, argp, &local_action);
	debug_logf("recovery", "native-start result=%d status=%d installed=%d",
		result, *local_action.result, installed);
	debug_log_flush();
	if (installed) {
		if (result >= 0 && *local_action.result == SCE_KERNEL_START_SUCCESS) {
			recovery_running = 1;
			debug_logf("recovery", "native-start active modid=%d", modid);
		} else {
			SceKernelModuleInfo info;
			info.size = sizeof(info);
			int info_result = sceKernelGetModuleInfo(modid, &info);
			debug_logf("recovery", "native-start failure module-info result=%d", info_result);
			if (info_result < 0) {
				/* Never restore bytes into an unexpectedly unmapped module. */
				recovery_running = 1;
				debug_logf("recovery", "native-start failure module-unmapped");
			} else if (release_recovery() < 0) {
				result = RECOVERY_ERROR;
				debug_logf("recovery", "native-start failure cleanup-failed");
			}
		}
	}
done:
	debug_logf("recovery", "module-start return=%d running=%d installed=%d",
		result, recovery_running, installed);
	debug_log_flush();
	leave_lifecycle();
	return result;
}

static int stop_module_hook(SceUID modid, SceSize args, const void *argp,
	ModuleAction *action)
{
	ModuleAction local_action;
	int status = -1;
	int result;

	debug_logf("recovery", "module-stop observed modid=%d args=%u argp=0x%08X action=0x%08X tracked=%d",
		modid, (unsigned int)args, (unsigned int)(uintptr_t)argp,
		(unsigned int)(uintptr_t)action, recovery_modid);
	if (modid != recovery_modid || action == NULL) {
		debug_logf("recovery", "module-stop passthrough");
		return TAI_CONTINUE(int, lifecycle_refs[1], modid, args, argp, action);
	}
	if (!enter_lifecycle()) {
		debug_logf("recovery", "module-stop lifecycle-busy");
		return RECOVERY_ERROR;
	}
	local_action.flags = action->flags;
	local_action.option = action->option;
	local_action.result = action->result ? action->result : &status;
	local_action.reserved = 0;
	result = TAI_CONTINUE(int, lifecycle_refs[1], modid, args, argp, &local_action);
	debug_logf("recovery", "native-stop result=%d status=%d",
		result, *local_action.result);
	if (result >= 0 && *local_action.result == SCE_KERNEL_STOP_SUCCESS) {
		recovery_running = 0;
		int release_result = release_recovery();
		debug_logf("recovery", "native-stop release result=%d", release_result);
		if (release_result < 0)
			result = RECOVERY_ERROR;
	}
	debug_logf("recovery", "module-stop return=%d running=%d", result, recovery_running);
	leave_lifecycle();
	return result;
}

static int unload_module_hook(SceUID modid, int flags, const void *option)
{
	int result;

	debug_logf("recovery", "module-unload observed modid=%d flags=0x%08X option=0x%08X tracked=%d",
		modid, (unsigned int)flags, (unsigned int)(uintptr_t)option, recovery_modid);
	if (modid != recovery_modid) {
		debug_logf("recovery", "module-unload passthrough");
		return TAI_CONTINUE(int, lifecycle_refs[2], modid, flags, option);
	}
	if (!enter_lifecycle()) {
		debug_logf("recovery", "module-unload lifecycle-busy");
		return RECOVERY_ERROR;
	}
	result = release_recovery();
	debug_logf("recovery", "module-unload release result=%d", result);
	if (result >= 0)
		result = TAI_CONTINUE(int, lifecycle_refs[2], modid, flags, option);
	debug_logf("recovery", "module-unload return=%d", result);
	leave_lifecycle();
	return result;
}

int recovery_stop(void)
{
	int i, result;

	debug_logf("recovery", "stop begin initialized=%d running=%d modid=%d",
		initialized, recovery_running, recovery_modid);
	if (!initialized) {
		debug_logf("recovery", "stop noop");
		return 0;
	}
	if (!enter_lifecycle()) {
		debug_logf("recovery", "stop lifecycle-busy");
		return RECOVERY_ERROR;
	}
	result = release_recovery();
	debug_logf("recovery", "stop release result=%d", result);
	if (result < 0)
		goto done;
	for (i = 0; i < 3; ++i) {
		if (lifecycle_ids[i] >= 0) {
			result = taiHookRelease(lifecycle_ids[i], lifecycle_refs[i]);
			debug_logf("recovery", "lifecycle-release slot=%d uid=%d result=%d",
				i, lifecycle_ids[i], result);
			if (result < 0)
				goto done;
			lifecycle_ids[i] = -1;
		}
	}
	initialized = 0;
	expected_recovery_nid = 0;
done:
	debug_logf("recovery", "stop return=%d initialized=%d", result, initialized);
	leave_lifecycle();
	return result;
}

int recovery_start(uint32_t shell_nid)
{
	unsigned int i;
	int slot;
	static const uint32_t nids[] = {START_MODULE_NID, STOP_MODULE_NID, UNLOAD_MODULE_NID};
	const void *hooks[] = {start_module_hook, stop_module_hook, unload_module_hook};
	int result;

	debug_logf("recovery", "start shell_nid=0x%08X initialized=%d",
		(unsigned int)shell_nid, initialized);
	if (initialized) {
		debug_logf("recovery", "start rejected reason=already-initialized");
		return RECOVERY_ERROR;
	}
	expected_recovery_nid = 0;
	for (i = 0; i < ARRAY_COUNT(recovery_profiles); ++i) {
		if (shell_nid == recovery_profiles[i].shell_nid)
			expected_recovery_nid = recovery_profiles[i].recovery_nid;
	}
	if (expected_recovery_nid == 0) {
		debug_logf("recovery", "start rejected reason=profile-missing");
		return RECOVERY_ERROR;
	}
	debug_logf("recovery", "profile-selected shell_nid=0x%08X recovery_nid=0x%08X",
		(unsigned int)shell_nid, (unsigned int)expected_recovery_nid);
	for (i = 0; i < ARRAY_COUNT(patch_ids); ++i)
		patch_ids[i] = -1;
	initialized = 1;
	/* Cleanup interception must exist before a start can install patches. */
	for (slot = 2; slot >= 0; --slot) {
		lifecycle_ids[slot] = taiHookFunctionImport(&lifecycle_refs[slot],
			"SceLibKernel", MODULEMGR_LIBRARY, nids[slot], hooks[slot]);
		debug_logf("recovery", "lifecycle-hook slot=%d nid=0x%08X uid=%d",
			slot, (unsigned int)nids[slot], lifecycle_ids[slot]);
		if (lifecycle_ids[slot] < 0) {
			result = lifecycle_ids[slot];
			debug_logf("recovery", "lifecycle-hook-failed slot=%d result=%d",
				slot, result);
			(void)recovery_stop();
			return result;
		}
	}
	debug_logf("recovery", "start complete recovery_nid=0x%08X",
		(unsigned int)expected_recovery_nid);
	return 0;
}
