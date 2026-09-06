#include <psp2/kernel/modulemgr.h>
#include <psp2/types.h>
#include <stdint.h>
#include <taihen.h>

#include "limits.h"

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define SCE_SHELL_TEXT_SEGMENT 0

extern const uint8_t patch_movs_r3_page_limit[2];
extern const uint8_t patch_cmp_r1_page_limit[2];
extern const uint8_t patch_cmp_r6_page_limit[2];
extern const uint8_t patch_cmp_r0_page_limit[2];
extern const uint8_t patch_movs_r0_page_limit[2];
extern const uint8_t patch_movs_r2_page_limit[2];
extern const uint8_t patch_cmp_r6_top_level_limit[2];
extern const uint8_t patch_cmp_r0_top_level_limit[2];
extern const uint8_t patch_cmp_r7_icon_limit[4];
extern const uint8_t patch_cmp_r0_icon_limit[4];
extern const uint8_t patch_subs_r6_r5_icon_limit[4];
extern const uint8_t patch_cmp_r5_icon_limit[4];
extern const uint8_t patch_movs_r3_icon_limit[4];
extern const uint8_t patch_rsbs_r1_r0_icon_limit[4];

typedef struct Patch {
	uint32_t offset;
	uint8_t size;
	uint8_t expected[4];
	const uint8_t *replacement;
} Patch;

typedef struct PatchProfile {
	uint32_t module_nid;
	SceSize text_size;
	const Patch *patches;
	unsigned int patch_count;
} PatchProfile;

/*
 * Offsets are relative to PTEL (testkit) FW 3.60 SceShell segment 0.
 * The reference dump has module NID 0xEAB89D5C, not the retail 3.60 NID.
 */
static const Patch patches_ptel_360[] = {
	/* Raise the top-menu page count from 10 to LIVEAREA_PAGE_LIMIT. */
	{0x0A6298, 2, {0x0A, 0x23}, patch_movs_r3_page_limit},
	{0x0A6E0A, 2, {0x0A, 0x29}, patch_cmp_r1_page_limit},
	{0x0AB528, 2, {0x0A, 0x2E}, patch_cmp_r6_page_limit},
	{0x0AC78E, 2, {0x0A, 0x28}, patch_cmp_r0_page_limit},
	{0x0AC792, 2, {0x0A, 0x20}, patch_movs_r0_page_limit},
	{0x0B43AE, 2, {0x0A, 0x2E}, patch_cmp_r6_page_limit},
	{0x0B5C9A, 2, {0x0A, 0x29}, patch_cmp_r1_page_limit},
	{0x0C218E, 2, {0x0A, 0x29}, patch_cmp_r1_page_limit},
	{0x0C3006, 2, {0x0A, 0x28}, patch_cmp_r0_page_limit},
	{0x0D1654, 2, {0x0A, 0x28}, patch_cmp_r0_page_limit},

	/* Count all configured pages and allow ten top-level icons per page. */
	{0x054E70, 2, {0x0A, 0x22}, patch_movs_r2_page_limit},
	{0x054E8A, 2, {0x64, 0x2E}, patch_cmp_r6_top_level_limit},
	{0x06364E, 2, {0x64, 0x28}, patch_cmp_r0_top_level_limit},
	{0x063656, 2, {0x64, 0x28}, patch_cmp_r0_top_level_limit},

	/* Raise every confirmed PTEL FW 3.60 counted-icon limit from 500. */
	{0x026894, 4, {0xB7, 0xF5, 0xFA, 0x7F}, patch_cmp_r7_icon_limit},
	{0x054E64, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x06365A, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x06366A, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x0A787C, 4, {0xB5, 0xF5, 0xFA, 0x76}, patch_subs_r6_r5_icon_limit},
	{0x0A78DA, 4, {0xB5, 0xF5, 0xFA, 0x7F}, patch_cmp_r5_icon_limit},
	{0x0A7906, 4, {0x5F, 0xF4, 0xFA, 0x73}, patch_movs_r3_icon_limit},
	{0x0AB3EA, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x35709E, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
};

/*
 * Offsets are relative to retail FW 3.60 SceShell segment 0 (NID 0x0552F692).
 * This was mislabeled as FW 3.65 in v1.0/v1.1 because the running system
 * spoofed the firmware version returned by sceKernelGetSystemSwVersion.
 */
static const Patch patches_360[] = {
	/* Raise the top-menu page count from 10 to LIVEAREA_PAGE_LIMIT. */
	{0x0A8C70, 2, {0x0A, 0x23}, patch_movs_r3_page_limit},
	{0x0A97E2, 2, {0x0A, 0x29}, patch_cmp_r1_page_limit},
	{0x0ADF00, 2, {0x0A, 0x2E}, patch_cmp_r6_page_limit},
	{0x0AF166, 2, {0x0A, 0x28}, patch_cmp_r0_page_limit},
	{0x0AF16A, 2, {0x0A, 0x20}, patch_movs_r0_page_limit},
	{0x0B6D86, 2, {0x0A, 0x2E}, patch_cmp_r6_page_limit},
	{0x0B8672, 2, {0x0A, 0x29}, patch_cmp_r1_page_limit},
	{0x0C4B66, 2, {0x0A, 0x29}, patch_cmp_r1_page_limit},
	{0x0C59DE, 2, {0x0A, 0x28}, patch_cmp_r0_page_limit},
	{0x0D4044, 2, {0x0A, 0x28}, patch_cmp_r0_page_limit},

	/* Count all configured pages and allow ten top-level icons per page. */
	{0x0552B0, 2, {0x0A, 0x22}, patch_movs_r2_page_limit},
	{0x0552CA, 2, {0x64, 0x2E}, patch_cmp_r6_top_level_limit},
	{0x063A8E, 2, {0x64, 0x28}, patch_cmp_r0_top_level_limit},
	{0x063A96, 2, {0x64, 0x28}, patch_cmp_r0_top_level_limit},

	/* Raise every confirmed retail FW 3.60 counted-icon limit from 500. */
	{0x026728, 4, {0xB7, 0xF5, 0xFA, 0x7F}, patch_cmp_r7_icon_limit},
	{0x0552A4, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x063A9A, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x063AAA, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x0AA254, 4, {0xB5, 0xF5, 0xFA, 0x76}, patch_subs_r6_r5_icon_limit},
	{0x0AA2B2, 4, {0xB5, 0xF5, 0xFA, 0x7F}, patch_cmp_r5_icon_limit},
	{0x0AA2DE, 4, {0x5F, 0xF4, 0xFA, 0x73}, patch_movs_r3_icon_limit},
	{0x0ADDC2, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x360656, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
};

/* Retail FW 3.65 SceShell (NID 0x5549BF1F), verified against its update image. */
static const Patch patches_365[] = {
	/* Raise the top-menu page count from 10 to LIVEAREA_PAGE_LIMIT. */
	{0x0A8CC8, 2, {0x0A, 0x23}, patch_movs_r3_page_limit},
	{0x0A983A, 2, {0x0A, 0x29}, patch_cmp_r1_page_limit},
	{0x0ADF58, 2, {0x0A, 0x2E}, patch_cmp_r6_page_limit},
	{0x0AF1BE, 2, {0x0A, 0x28}, patch_cmp_r0_page_limit},
	{0x0AF1C2, 2, {0x0A, 0x20}, patch_movs_r0_page_limit},
	{0x0B6DDE, 2, {0x0A, 0x2E}, patch_cmp_r6_page_limit},
	{0x0B86CA, 2, {0x0A, 0x29}, patch_cmp_r1_page_limit},
	{0x0C4BBE, 2, {0x0A, 0x29}, patch_cmp_r1_page_limit},
	{0x0C5A36, 2, {0x0A, 0x28}, patch_cmp_r0_page_limit},
	{0x0D409C, 2, {0x0A, 0x28}, patch_cmp_r0_page_limit},

	/* Count all configured pages and allow ten top-level icons per page. */
	{0x055308, 2, {0x0A, 0x22}, patch_movs_r2_page_limit},
	{0x055322, 2, {0x64, 0x2E}, patch_cmp_r6_top_level_limit},
	{0x063AE6, 2, {0x64, 0x28}, patch_cmp_r0_top_level_limit},
	{0x063AEE, 2, {0x64, 0x28}, patch_cmp_r0_top_level_limit},

	/* Raise every confirmed retail FW 3.65 counted-icon limit from 500. */
	{0x026780, 4, {0xB7, 0xF5, 0xFA, 0x7F}, patch_cmp_r7_icon_limit},
	{0x0552FC, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x063AF2, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x063B02, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x0AA2AC, 4, {0xB5, 0xF5, 0xFA, 0x76}, patch_subs_r6_r5_icon_limit},
	{0x0AA30A, 4, {0xB5, 0xF5, 0xFA, 0x7F}, patch_cmp_r5_icon_limit},
	{0x0AA336, 4, {0x5F, 0xF4, 0xFA, 0x73}, patch_movs_r3_icon_limit},
	{0x0ADE1A, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x360A9A, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
};

/* Select the actual shell binary; HENkaku can spoof the system-version API. */
static const PatchProfile patch_profiles[] = {
	{0x0552F692U, 0x541B74, patches_360, ARRAY_SIZE(patches_360)},
	{0x5549BF1FU, 0x5420F4, patches_365, ARRAY_SIZE(patches_365)},
	{0xEAB89D5CU, 0x535CF4, patches_ptel_360, ARRAY_SIZE(patches_ptel_360)},
};

_Static_assert(ARRAY_SIZE(patches_ptel_360) == ARRAY_SIZE(patches_360) &&
	ARRAY_SIZE(patches_360) == ARRAY_SIZE(patches_365),
	"all firmware profiles must contain the same number of patches");

static SceUID patch_uids[ARRAY_SIZE(patches_ptel_360)];
static SceUID shell_module_id = -1;

_Static_assert(LIVEAREA_TOP_LEVEL_LIMIT <=
	LIVEAREA_PAGE_LIMIT * LIVEAREA_ICONS_PER_PAGE,
	"page capacity must cover the top-level icon limit");
_Static_assert(LIVEAREA_TOP_LEVEL_LIMIT >
	(LIVEAREA_PAGE_LIMIT - 1) * LIVEAREA_ICONS_PER_PAGE,
	"page capacity must be the minimum needed for the top-level icon limit");

static const PatchProfile *find_patch_profile(uint32_t module_nid)
{
	unsigned int index;

	for (index = 0; index < ARRAY_SIZE(patch_profiles); ++index) {
		if (patch_profiles[index].module_nid == module_nid)
			return &patch_profiles[index];
	}

	return NULL;
}

static int bytes_equal(const volatile uint8_t *actual,
	const uint8_t *expected, uint8_t size)
{
	uint8_t index;

	for (index = 0; index < size; ++index) {
		if (actual[index] != expected[index])
			return 0;
	}

	return 1;
}

static void release_patches(void)
{
	int index;

	for (index = (int)ARRAY_SIZE(patch_uids) - 1; index >= 0; --index) {
		if (patch_uids[index] >= 0) {
			taiInjectRelease(patch_uids[index]);
			patch_uids[index] = -1;
		}
	}

	shell_module_id = -1;
}

static int verify_patches(const SceKernelSegmentInfo *text_segment,
	const PatchProfile *profile)
{
	uintptr_t text_start = (uintptr_t)text_segment->vaddr;
	SceSize text_size = text_segment->memsz;
	unsigned int index;

	if (text_size != profile->text_size)
		return -1;

	for (index = 0; index < profile->patch_count; ++index) {
		const Patch *patch = &profile->patches[index];

		if (patch->offset > text_size ||
			patch->size > text_size - patch->offset)
			return -1;

		if (!bytes_equal(
			(const volatile uint8_t *)(text_start + patch->offset),
			patch->expected, patch->size))
			return -1;
	}

	return 0;
}

static int install_patches(void)
{
	const PatchProfile *profile;
	tai_module_info_t tai_info;
	SceKernelModuleInfo module_info;
	unsigned int index;
	int result;

	tai_info.size = sizeof(tai_info);
	result = taiGetModuleInfo("SceShell", &tai_info);
	if (result < 0)
		return result;

	profile = find_patch_profile(tai_info.module_nid);
	if (profile == NULL)
		return -1;

	module_info.size = sizeof(module_info);
	result = sceKernelGetModuleInfo(tai_info.modid, &module_info);
	if (result < 0)
		return result;

	if (module_info.segments[SCE_SHELL_TEXT_SEGMENT].vaddr == NULL)
		return -1;

	result = verify_patches(&module_info.segments[SCE_SHELL_TEXT_SEGMENT],
		profile);
	if (result < 0)
		return result;

	shell_module_id = tai_info.modid;
	for (index = 0; index < profile->patch_count; ++index) {
		const Patch *patch = &profile->patches[index];

		patch_uids[index] = taiInjectData(shell_module_id,
			SCE_SHELL_TEXT_SEGMENT, patch->offset,
			patch->replacement, patch->size);
		if (patch_uids[index] < 0) {
			result = patch_uids[index];
			release_patches();
			return result;
		}
	}

	return 0;
}

int module_start(SceSize argc, const void *args)
{
	unsigned int index;
	int result;

	(void)argc;
	(void)args;

	for (index = 0; index < ARRAY_SIZE(patch_uids); ++index)
		patch_uids[index] = -1;

	result = install_patches();
	if (result < 0) {
		release_patches();
		return SCE_KERNEL_START_FAILED;
	}

	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc;
	(void)args;

	release_patches();
	return SCE_KERNEL_STOP_SUCCESS;
}
