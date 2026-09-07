#include <psp2/kernel/modulemgr.h>
#include <psp2/types.h>
#include <stdint.h>
#include <string.h>
#include <taihen.h>

#include "limits.h"
#include "recovery.h"

/* The loader binds both modules to the same SceLibKernel variable. */
extern uint32_t __stack_chk_guard;

#define RECOVERY_TEXT_SIZE 0x3E9E8U
#define RECOVERY_DATA_SIZE 0x3094U
#define RECOVERY_ALLOCATE_OFFSET 0x5F50U
#define RECOVERY_HIDDEN_PAGE (-100000000)
#define MODULEMGR_LIBRARY 0xEAED1616U
#define START_MODULE_NID 0x72CD301FU
#define STOP_MODULE_NID 0x086867A8U
#define UNLOAD_MODULE_NID 0x8E4A7716U
#define ICON_COUNT_NID 0xCF4B3684U
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

typedef int (*IconCountFn)(const void *, int, unsigned int);

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
static IconCountFn icon_count;
static SceUID patch_ids[ARRAY_COUNT(recovery_patches)];
static SceUID allocate_hook_id = -1;
static tai_hook_ref_t allocate_ref;
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

static int allocate_page_hook(void *context, int *page, int *position)
{
	const void *database;
	int count;

	memcpy(&database, (const uint8_t *)context + 12, sizeof(database));
	count = icon_count(database, 0, LIVEAREA_PAGE_LIMIT);
	if (count < 0)
		return count;
	if (count >= LIVEAREA_TOP_LEVEL_LIMIT) {
		*page = RECOVERY_HIDDEN_PAGE;
		*position = 0;
		return 0;
	}

	return TAI_CONTINUE(int, allocate_ref, context, page, position);
}

static uint16_t read_u16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
}

static int read_mov_imm(const uint8_t *bytes, uint16_t opcode, uint32_t *value)
{
	uint16_t first = read_u16(bytes);
	uint16_t second = read_u16(bytes + 2);

	if ((first & 0xFBF0U) != opcode || (second & 0x8F00U) != 0x0300U)
		return -1;
	*value = ((uint32_t)(first & 15U) << 12) |
		((uint32_t)(first & 0x400U) << 1) |
		((uint32_t)(second & 0x7000U) >> 4) | (second & 255U);
	return 0;
}

static int verify_recovery(const SceKernelModuleInfo *info)
{
	const uint8_t *text = info->segments[0].vaddr;
	static const uint8_t entry_begin[] = {0x2D, 0xE9, 0xF0, 0x4F, 0x8D, 0xB0};
	static const uint8_t entry_end[] = {0x1B, 0x68, 0x0B, 0x93, 0x80, 0x46,
		0xD8, 0xF8, 0x0C, 0x00};
	const uint8_t *entry;
	uint32_t low, high, guard;
	unsigned int i;

	if (text == NULL || info->segments[0].memsz != RECOVERY_TEXT_SIZE ||
		info->segments[1].vaddr == NULL || info->segments[1].memsz != RECOVERY_DATA_SIZE)
		return -1;
	for (i = 0; i < ARRAY_COUNT(recovery_patches); ++i) {
		const RecoveryPatch *patch = &recovery_patches[i];
		if (memcmp(text + patch->offset, patch->expected, patch->size) != 0)
			return -1;
	}

	entry = text + RECOVERY_ALLOCATE_OFFSET;
	if (memcmp(entry, entry_begin, sizeof(entry_begin)) != 0 ||
		memcmp(entry + 14, entry_end, sizeof(entry_end)) != 0 ||
		read_mov_imm(entry + 6, 0xF240U, &low) < 0 ||
		read_mov_imm(entry + 10, 0xF2C0U, &high) < 0)
		return -1;
	guard = low | high << 16;
	if (guard != (uint32_t)(uintptr_t)&__stack_chk_guard)
		return -1;
	return 0;
}

static int release_recovery(void)
{
	int i, result;

	if (recovery_modid < 0)
		return 0;
	if (recovery_running)
		return RECOVERY_ERROR;
	if (allocate_hook_id >= 0) {
		result = taiHookRelease(allocate_hook_id, allocate_ref);
		if (result < 0)
			return result;
		allocate_hook_id = -1;
	}
	for (i = (int)ARRAY_COUNT(patch_ids) - 1; i >= 0; --i) {
		if (patch_ids[i] >= 0) {
			result = taiInjectRelease(patch_ids[i]);
			if (result < 0)
				return result;
			patch_ids[i] = -1;
		}
	}
	recovery_modid = -1;
	icon_count = NULL;
	return 0;
}

static int install_recovery(const tai_module_info_t *module)
{
	SceKernelModuleInfo info = {0};
	uintptr_t count_address = 0;
	unsigned int i;
	int result;

	info.size = sizeof(info);
	result = sceKernelGetModuleInfo(module->modid, &info);
	if (result < 0 || verify_recovery(&info) < 0)
		return RECOVERY_ERROR;
	result = taiGetModuleExportFunc("SceLsdb", TAI_ANY_LIBRARY,
		ICON_COUNT_NID, &count_address);
	if (result < 0 || count_address == 0)
		return RECOVERY_ERROR;

	recovery_modid = module->modid;
	icon_count = (IconCountFn)count_address;
	/* No recovery code runs until the original module-start call below. */
	allocate_hook_id = taiHookFunctionOffset(&allocate_ref, module->modid, 0,
		RECOVERY_ALLOCATE_OFFSET, 1, allocate_page_hook);
	if (allocate_hook_id < 0)
		return allocate_hook_id;
	for (i = 0; i < ARRAY_COUNT(recovery_patches); ++i) {
		const RecoveryPatch *patch = &recovery_patches[i];
		patch_ids[i] = taiInjectData(module->modid, 0, patch->offset,
			patch->replacement, patch->size);
		if (patch_ids[i] < 0)
			return patch_ids[i];
	}
	return 0;
}

static int start_module_hook(SceUID modid, SceSize args, const void *argp,
	ModuleAction *action)
{
	tai_module_info_t module;
	ModuleAction local_action;
	int status = -1;
	int result, installed = 0;

	module.size = sizeof(module);
	if (action == NULL || taiGetModuleInfo("SceDbRecovery", &module) < 0 ||
		module.modid != modid || module.module_nid != expected_recovery_nid)
		return TAI_CONTINUE(int, lifecycle_refs[0], modid, args, argp, action);
	if (!enter_lifecycle())
		return RECOVERY_ERROR;
	if (recovery_running) {
		result = TAI_CONTINUE(int, lifecycle_refs[0], modid, args, argp, action);
		leave_lifecycle();
		return result;
	}
	result = release_recovery();
	if (result < 0)
		goto done;
	result = install_recovery(&module);
	if (result >= 0) {
		installed = 1;
	} else {
		result = release_recovery();
		if (result < 0)
			goto done;
	}

	local_action.flags = action->flags;
	local_action.option = action->option;
	local_action.result = action->result ? action->result : &status;
	local_action.reserved = 0;
	result = TAI_CONTINUE(int, lifecycle_refs[0], modid, args, argp, &local_action);
	if (installed) {
		if (result >= 0 && *local_action.result == SCE_KERNEL_START_SUCCESS) {
			recovery_running = 1;
		} else {
			SceKernelModuleInfo info;
			info.size = sizeof(info);
			if (sceKernelGetModuleInfo(modid, &info) < 0) {
				/* Never restore bytes into an unexpectedly unmapped module. */
				recovery_running = 1;
			} else if (release_recovery() < 0) {
				result = RECOVERY_ERROR;
			}
		}
	}
done:
	leave_lifecycle();
	return result;
}

static int stop_module_hook(SceUID modid, SceSize args, const void *argp,
	ModuleAction *action)
{
	ModuleAction local_action;
	int status = -1;
	int result;

	if (modid != recovery_modid || action == NULL)
		return TAI_CONTINUE(int, lifecycle_refs[1], modid, args, argp, action);
	if (!enter_lifecycle())
		return RECOVERY_ERROR;
	local_action.flags = action->flags;
	local_action.option = action->option;
	local_action.result = action->result ? action->result : &status;
	local_action.reserved = 0;
	result = TAI_CONTINUE(int, lifecycle_refs[1], modid, args, argp, &local_action);
	if (result >= 0 && *local_action.result == SCE_KERNEL_STOP_SUCCESS) {
		recovery_running = 0;
		if (release_recovery() < 0)
			result = RECOVERY_ERROR;
	}
	leave_lifecycle();
	return result;
}

static int unload_module_hook(SceUID modid, int flags, const void *option)
{
	int result;

	if (modid != recovery_modid)
		return TAI_CONTINUE(int, lifecycle_refs[2], modid, flags, option);
	if (!enter_lifecycle())
		return RECOVERY_ERROR;
	result = release_recovery();
	if (result >= 0)
		result = TAI_CONTINUE(int, lifecycle_refs[2], modid, flags, option);
	leave_lifecycle();
	return result;
}

int recovery_stop(void)
{
	int i, result;

	if (!initialized)
		return 0;
	if (!enter_lifecycle())
		return RECOVERY_ERROR;
	result = release_recovery();
	if (result < 0)
		goto done;
	for (i = 0; i < 3; ++i) {
		if (lifecycle_ids[i] >= 0) {
			result = taiHookRelease(lifecycle_ids[i], lifecycle_refs[i]);
			if (result < 0)
				goto done;
			lifecycle_ids[i] = -1;
		}
	}
	initialized = 0;
	expected_recovery_nid = 0;
done:
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

	if (initialized)
		return RECOVERY_ERROR;
	expected_recovery_nid = 0;
	for (i = 0; i < ARRAY_COUNT(recovery_profiles); ++i) {
		if (shell_nid == recovery_profiles[i].shell_nid)
			expected_recovery_nid = recovery_profiles[i].recovery_nid;
	}
	if (expected_recovery_nid == 0)
		return RECOVERY_ERROR;
	for (i = 0; i < ARRAY_COUNT(patch_ids); ++i)
		patch_ids[i] = -1;
	initialized = 1;
	/* Cleanup interception must exist before a start can install patches. */
	for (slot = 2; slot >= 0; --slot) {
		lifecycle_ids[slot] = taiHookFunctionImport(&lifecycle_refs[slot],
			"SceLibKernel", MODULEMGR_LIBRARY, nids[slot], hooks[slot]);
		if (lifecycle_ids[slot] < 0) {
			result = lifecycle_ids[slot];
			(void)recovery_stop();
			return result;
		}
	}
	return 0;
}
