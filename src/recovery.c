#include <psp2/kernel/modulemgr.h>
#include <psp2/types.h>
#include <stdint.h>
#include <string.h>
#include <taihen.h>

#include "debug_log.h"
#include "recovery.h"

#define RECOVERY_TEXT_SIZE 0x3E9E8U
#define RECOVERY_DATA_SIZE 0x3094U
#define RECOVERY_STOP_OFFSET 0x1EU
#define RECOVERY_ERROR (-1)
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define SHELL_RECOVERY_READY_OFFSET 0x1BFEU

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

typedef struct {
	uint32_t shell_nid;
	uint32_t recovery_nid;
	uint32_t shell_text_size;
	uint8_t ready_prefix[12];
} RecoveryProfile;

typedef int (*RecoveryStop)(SceSize args, const void *argp);

static const RecoveryPatch recovery_patches[] = {
	{0x06280, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x07054, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x0C700, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
	{0x0C776, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
	{0x0C834, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
	{0x06084, 2, {0x0A, 0x2C}, patch_cmp_r4_page_limit},
	{0x060FE, 4, {0x5F, 0xF0, 0x09, 0x09}, patch_movs_r9_last_page},
};

static const RecoveryProfile recovery_profiles[] = {
	{
		0x0552F692U, 0xC1F30F67U, 0x541B74U,
		{0x10, 0xB5, 0x01, 0x21, 0x5A, 0xF0, 0x1E, 0xE6,
		 0x04, 0x1C, 0x0D, 0xD0},
	},
	{
		0x5549BF1FU, 0x3F76E38FU, 0x5420F4U,
		{0x10, 0xB5, 0x01, 0x21, 0x5B, 0xF0, 0x42, 0xE0,
		 0x04, 0x1C, 0x0D, 0xD0},
	},
	{
		0xEAB89D5CU, 0xC1F30F67U, 0x535CF4U,
		{0x10, 0xB5, 0x01, 0x21, 0x50, 0xF0, 0xFA, 0xE6,
		 0x04, 0x1C, 0x0D, 0xD0},
	},
};

static const uint8_t expected_stop_entry[] = {
	0x10, 0xB5, 0x24, 0xF0, 0xC9, 0xFB, 0x00, 0x20, 0x10, 0xBD,
};
static const uint8_t stop_redirect_prefix[] = {
	0xDF, 0xF8, 0x04, 0xF0, 0x00, 0xBF,
};

static const RecoveryProfile *active_profile;
static SceUID ready_hook_id = -1;
static tai_hook_ref_t ready_hook_ref;
static SceUID stop_redirect_id = -1;
static SceUID recovery_modid = -1;
static RecoveryStop native_stop;
static SceUID patch_ids[ARRAY_COUNT(recovery_patches)];
static volatile int lifecycle_busy;
static int recovery_install_complete;
static int initialized;
static volatile unsigned int ready_in_flight;

_Static_assert((RECOVERY_STOP_OFFSET & 3U) == 2U,
	"stop redirect literal requires a halfword-aligned entry");
_Static_assert(sizeof(stop_redirect_prefix) + sizeof(uint32_t) ==
	sizeof(expected_stop_entry), "stop redirect must replace the complete entry");

static int enter_lifecycle(void)
{
	return __sync_bool_compare_and_swap(&lifecycle_busy, 0, 1);
}

static void leave_lifecycle(void)
{
	__sync_lock_release(&lifecycle_busy);
}

static void clear_patch_ids(void)
{
	unsigned int index;

	for (index = 0; index < ARRAY_COUNT(patch_ids); ++index)
		patch_ids[index] = -1;
}

static int verify_recovery(const SceKernelModuleInfo *info)
{
	const uint8_t *text;
	unsigned int index;

	if (info == NULL || info->segments[0].vaddr == NULL ||
		info->segments[0].memsz != RECOVERY_TEXT_SIZE ||
		info->segments[1].vaddr == NULL ||
		info->segments[1].memsz != RECOVERY_DATA_SIZE) {
		debug_logf("recovery", "validation-failed reason=segments expected_text=0x%08X expected_data=0x%08X",
			RECOVERY_TEXT_SIZE, RECOVERY_DATA_SIZE);
		return RECOVERY_ERROR;
	}
	text = info->segments[0].vaddr;
	if (memcmp(text + RECOVERY_STOP_OFFSET, expected_stop_entry,
			sizeof(expected_stop_entry)) != 0) {
		debug_logf("recovery", "validation-failed reason=stop-entry");
		debug_log_hex("recovery", "stop-actual", RECOVERY_STOP_OFFSET,
			text + RECOVERY_STOP_OFFSET, sizeof(expected_stop_entry));
		debug_log_hex("recovery", "stop-expected", RECOVERY_STOP_OFFSET,
			expected_stop_entry, sizeof(expected_stop_entry));
		return RECOVERY_ERROR;
	}
	for (index = 0; index < ARRAY_COUNT(recovery_patches); ++index) {
		const RecoveryPatch *patch = &recovery_patches[index];

		if (memcmp(text + patch->offset, patch->expected, patch->size) != 0) {
			debug_logf("recovery", "validation-failed reason=patch index=%u offset=0x%08X",
				index, (unsigned int)patch->offset);
			debug_log_hex("recovery", "patch-actual", patch->offset,
				text + patch->offset, patch->size);
			debug_log_hex("recovery", "patch-expected", patch->offset,
				patch->expected, patch->size);
			return RECOVERY_ERROR;
		}
	}
	debug_logf("recovery", "validation-complete patches=%u stop_offset=0x%08X",
		(unsigned int)ARRAY_COUNT(recovery_patches), RECOVERY_STOP_OFFSET);
	return 0;
}

static int release_recovery_patches(void)
{
	int first_error = 0;
	int index;

	for (index = (int)ARRAY_COUNT(patch_ids) - 1; index >= 0; --index) {
		if (patch_ids[index] >= 0) {
			int result = taiInjectRelease(patch_ids[index]);

			debug_logf("recovery", "patch-release index=%d uid=%d result=%d",
				index, patch_ids[index], result);
			if (result >= 0)
				patch_ids[index] = -1;
			else if (first_error == 0)
				first_error = result;
		}
	}
	return first_error;
}

static int release_stop_redirect(void)
{
	int result;

	if (stop_redirect_id < 0)
		return 0;
	result = taiInjectRelease(stop_redirect_id);
	debug_logf("recovery", "stop-redirect-release uid=%d result=%d",
		stop_redirect_id, result);
	if (result >= 0)
		stop_redirect_id = -1;
	return result;
}

static void clear_recovery_state(void)
{
	recovery_modid = -1;
	native_stop = NULL;
	recovery_install_complete = 0;
}

static int recovery_module_stop_redirect(SceSize args, const void *argp)
{
	RecoveryStop stop;
	int result;

	debug_logf("recovery", "module-stop enter modid=%d args=%u argp=0x%08X native=0x%08X complete=%d",
		recovery_modid, (unsigned int)args, (unsigned int)(uintptr_t)argp,
		(unsigned int)(uintptr_t)native_stop, recovery_install_complete);
	debug_log_flush();
	if (!enter_lifecycle()) {
		debug_logf("recovery", "module-stop cancelled reason=busy");
		debug_log_flush();
		return SCE_KERNEL_STOP_CANCEL;
	}
	if (native_stop == NULL) {
		debug_logf("recovery", "module-stop cancelled reason=native-stop-null");
		debug_log_flush();
		leave_lifecycle();
		return SCE_KERNEL_STOP_CANCEL;
	}
	if (release_recovery_patches() < 0 || release_stop_redirect() < 0) {
		debug_logf("recovery", "module-stop cancelled reason=cleanup-failed modid=%d redirect=%d",
			recovery_modid, stop_redirect_id);
		debug_log_flush();
		leave_lifecycle();
		return SCE_KERNEL_STOP_CANCEL;
	}
	stop = native_stop;
	clear_recovery_state();
	debug_logf("recovery", "module-stop native-enter address=0x%08X",
		(unsigned int)(uintptr_t)stop);
	debug_log_flush();
	result = stop(args, argp);
	debug_logf("recovery", "module-stop native-return result=%d", result);
	debug_log_flush();
	leave_lifecycle();
	return result;
}

static int install_recovery(void)
{
	tai_module_info_t module = {0};
	SceKernelModuleInfo info = {0};
	uint8_t stop_redirect[sizeof(expected_stop_entry)];
	uint32_t target;
	unsigned int index;
	int result;

	if (active_profile == NULL || recovery_modid >= 0) {
		debug_logf("recovery", "install-rejected reason=state profile=%u modid=%d",
			active_profile != NULL, recovery_modid);
		return RECOVERY_ERROR;
	}
	module.size = sizeof(module);
	result = taiGetModuleInfo("SceDbRecovery", &module);
	debug_logf("recovery", "module-lookup result=%d modid=%d nid=0x%08X expected=0x%08X",
		result, result < 0 ? -1 : module.modid,
		result < 0 ? 0U : (unsigned int)module.module_nid,
		(unsigned int)active_profile->recovery_nid);
	if (result < 0 || module.module_nid != active_profile->recovery_nid) {
		debug_logf("recovery", "install-rejected reason=module-identity");
		return RECOVERY_ERROR;
	}
	info.size = sizeof(info);
	result = sceKernelGetModuleInfo(module.modid, &info);
	debug_logf("recovery", "module-info result=%d text=0x%08X text_size=0x%08X data=0x%08X data_size=0x%08X",
		result, (unsigned int)(uintptr_t)info.segments[0].vaddr,
		(unsigned int)info.segments[0].memsz,
		(unsigned int)(uintptr_t)info.segments[1].vaddr,
		(unsigned int)info.segments[1].memsz);
	if (result < 0 || verify_recovery(&info) < 0)
		return RECOVERY_ERROR;

	memcpy(stop_redirect, stop_redirect_prefix, sizeof(stop_redirect_prefix));
	target = (uint32_t)(uintptr_t)recovery_module_stop_redirect | 1U;
	memcpy(stop_redirect + sizeof(stop_redirect_prefix), &target,
		sizeof(target));
	stop_redirect_id = taiInjectData(module.modid, 0, RECOVERY_STOP_OFFSET,
		stop_redirect, sizeof(stop_redirect));
	debug_logf("recovery", "stop-redirect-injected uid=%d offset=0x%08X target=0x%08X",
		stop_redirect_id, RECOVERY_STOP_OFFSET, (unsigned int)target);
	if (stop_redirect_id < 0)
		return stop_redirect_id;
	recovery_modid = module.modid;
	native_stop = (RecoveryStop)((uintptr_t)info.segments[0].vaddr +
		RECOVERY_STOP_OFFSET + 1U);
	clear_patch_ids();
	for (index = 0; index < ARRAY_COUNT(recovery_patches); ++index) {
		const RecoveryPatch *patch = &recovery_patches[index];

		patch_ids[index] = taiInjectData(module.modid, 0, patch->offset,
			patch->replacement, patch->size);
		debug_logf("recovery", "patch-injected index=%u offset=0x%08X size=%u uid=%d",
			index, (unsigned int)patch->offset, patch->size, patch_ids[index]);
		if (patch_ids[index] < 0) {
			int release_result;

			result = patch_ids[index];
			patch_ids[index] = -1;
			release_result = release_recovery_patches();
			if (release_result >= 0)
				release_result = release_stop_redirect();
			if (release_result >= 0)
				clear_recovery_state();
			debug_logf("recovery", "install-rollback injection_result=%d cleanup_result=%d retained_modid=%d",
				result, release_result, recovery_modid);
			return result;
		}
	}
	recovery_install_complete = 1;
	debug_logf("recovery", "module-patched modid=%d stop-redirect=%d",
		recovery_modid, stop_redirect_id);
	debug_log_flush();
	return 0;
}

static int recovery_ready_hook(void *plugin)
{
	int install_result = RECOVERY_ERROR;
	int run_recovery = 0;
	int result;

	__sync_add_and_fetch(&ready_in_flight, 1);
	debug_logf("recovery", "ready-enter plugin=0x%08X modid=%d complete=%d",
		(unsigned int)(uintptr_t)plugin, recovery_modid,
		recovery_install_complete);
	debug_log_flush();

	if (enter_lifecycle()) {
		if (recovery_modid < 0) {
			install_result = install_recovery();
		} else if (recovery_install_complete) {
			install_result = 0;
		}
		run_recovery = recovery_install_complete || recovery_modid < 0;
		leave_lifecycle();
	} else {
		debug_logf("recovery", "ready-blocked reason=busy");
	}
	debug_logf("recovery", "ready-callback install-result=%d modid=%d run_native=%d patched=%d",
		install_result, recovery_modid, run_recovery, recovery_install_complete);
	debug_log_flush();
	(void)install_result;
	if (!run_recovery) {
		debug_logf("recovery", "ready-blocked reason=incomplete-or-busy");
		debug_log_flush();
		__sync_sub_and_fetch(&ready_in_flight, 1);
		return RECOVERY_ERROR;
	}
	debug_logf("recovery", "ready-native-enter");
	debug_log_flush();
	result = TAI_CONTINUE(int, ready_hook_ref, plugin);
	debug_logf("recovery", "ready-native-return result=%d", result);
	debug_log_flush();
	__sync_sub_and_fetch(&ready_in_flight, 1);
	return result;
}

int recovery_stop(void)
{
	int result = 0;

	debug_logf("recovery", "plugin-stop initialized=%d modid=%d redirect=%d ready_hook=%d",
		initialized, recovery_modid, stop_redirect_id, ready_hook_id);
	if (!initialized)
		return 0;
	if (!enter_lifecycle()) {
		debug_logf("recovery", "plugin-stop blocked reason=busy");
		return RECOVERY_ERROR;
	}
	if (recovery_modid >= 0 || stop_redirect_id >= 0 || ready_in_flight != 0) {
		debug_logf("recovery", "plugin-stop blocked reason=module-or-callback-active callbacks=%u",
			(unsigned int)ready_in_flight);
		leave_lifecycle();
		return RECOVERY_ERROR;
	}
	if (ready_hook_id >= 0) {
		result = taiHookRelease(ready_hook_id, ready_hook_ref);
		debug_logf("recovery", "ready-hook-release uid=%d result=%d", ready_hook_id, result);
		if (result >= 0)
			ready_hook_id = -1;
	}
	if (result >= 0) {
		active_profile = NULL;
		initialized = 0;
	}
	leave_lifecycle();
	debug_logf("recovery", "plugin-stop return=%d initialized=%d", result, initialized);
	debug_log_flush();
	return result;
}

int recovery_start(SceUID shell_modid, uint32_t shell_nid,
	const SceKernelModuleInfo *shell_info)
{
	const uint8_t *text;
	unsigned int index;

	debug_logf("recovery", "start shell_modid=%d shell_nid=0x%08X initialized=%d hook=0x%08X stop_redirect=0x%08X",
		shell_modid, (unsigned int)shell_nid, initialized,
		(unsigned int)(uintptr_t)recovery_ready_hook,
		(unsigned int)(uintptr_t)recovery_module_stop_redirect);
	if (initialized) {
		debug_logf("recovery", "start-rejected reason=already-initialized");
		return RECOVERY_ERROR;
	}
	active_profile = NULL;
	for (index = 0; index < ARRAY_COUNT(recovery_profiles); ++index) {
		if (recovery_profiles[index].shell_nid == shell_nid) {
			active_profile = &recovery_profiles[index];
			break;
		}
	}
	if (active_profile == NULL) {
		debug_logf("recovery", "start-rejected reason=profile-missing");
		goto fail;
	}
	debug_logf("recovery", "profile-selected recovery_nid=0x%08X expected_text=0x%08X",
		(unsigned int)active_profile->recovery_nid,
		(unsigned int)active_profile->shell_text_size);
	if (shell_info == NULL ||
		shell_info->segments[0].vaddr == NULL ||
		shell_info->segments[0].memsz != active_profile->shell_text_size) {
		debug_logf("recovery", "start-rejected reason=shell-segment");
		goto fail;
	}
	text = shell_info->segments[0].vaddr;
	if (memcmp(text + SHELL_RECOVERY_READY_OFFSET,
		active_profile->ready_prefix, sizeof(active_profile->ready_prefix)) != 0) {
		debug_logf("recovery", "start-rejected reason=ready-bytes");
		debug_log_hex("recovery", "ready-actual", SHELL_RECOVERY_READY_OFFSET,
			text + SHELL_RECOVERY_READY_OFFSET, sizeof(active_profile->ready_prefix));
		debug_log_hex("recovery", "ready-expected", SHELL_RECOVERY_READY_OFFSET,
			active_profile->ready_prefix, sizeof(active_profile->ready_prefix));
		goto fail;
	}

	/* SceShell code is process-local; shared-library lifecycle hooks are not. */
	ready_hook_id = taiHookFunctionOffset(&ready_hook_ref, shell_modid, 0,
		SHELL_RECOVERY_READY_OFFSET, 1, recovery_ready_hook);
	debug_logf("recovery", "ready-hook-install result=%d offset=0x%08X",
		ready_hook_id, SHELL_RECOVERY_READY_OFFSET);
	if (ready_hook_id < 0)
		goto fail;
	clear_patch_ids();
	stop_redirect_id = -1;
	clear_recovery_state();
	initialized = 1;
	debug_logf("recovery", "ready-hook uid=%d offset=0x%08X",
		ready_hook_id, SHELL_RECOVERY_READY_OFFSET);
	debug_log_flush();
	return 0;

fail:
	debug_logf("recovery", "start-failed baseline-capacity-retained");
	debug_log_flush();
	active_profile = NULL;
	ready_hook_id = -1;
	return RECOVERY_ERROR;
}
