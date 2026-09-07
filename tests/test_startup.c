#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/main.c"

extern const char *test_patch_name(const uint8_t *data);

static uint8_t *g_text;
static SceSize g_text_size;
static uint32_t g_module_nid;
static int g_info_error;
static int g_null_segment;
static int g_fail_injection;
static unsigned int g_injection_calls;
static unsigned int g_live_injections;
static unsigned int g_release_calls;
static unsigned int g_recovery_calls;
static int g_recovery_live;
static int g_recovery_failure;
static int g_recovery_busy;

int recovery_start(uint32_t shell_nid)
{
	assert(shell_nid == g_module_nid && g_live_injections == ARRAY_SIZE(patches_360));
	++g_recovery_calls;
	if (g_recovery_failure)
		return -1;
	g_recovery_live = 1;
	return 0;
}

int recovery_stop(void)
{
	if (g_recovery_busy)
		return -1;
	if (g_recovery_live)
		assert(g_live_injections == ARRAY_SIZE(patches_360));
	g_recovery_live = 0;
	return 0;
}
#ifdef LIVEAREA_ICON_CACHE_TRIAL
static int g_trial_failure;
static int g_trial_live;
static unsigned int g_trial_calls;

int icon_cache_trial_start(SceUID shell_modid, uint32_t shell_nid,
	const SceKernelModuleInfo *info)
{
	assert(shell_modid == 42);
	assert(shell_nid == g_module_nid && info->segments[0].vaddr == g_text);
	assert(g_live_injections == ARRAY_SIZE(patches_360));
	++g_trial_calls;
	if (g_trial_failure)
		return -1;
	g_trial_live = 1;
	return 0;
}

void icon_cache_trial_stop(void)
{
	/* Remove the PAF hook before rolling back the shell patches. */
	if (g_trial_live)
		assert(g_live_injections == ARRAY_SIZE(patches_360));
	g_trial_live = 0;
}
#endif
static struct {
	uint32_t offset;
	SceSize size;
	uint8_t original[sizeof(((Patch *)0)->expected)];
} g_injections[ARRAY_SIZE(patches_360)];

int taiGetModuleInfo(const char *name, tai_module_info_t *info)
{
	assert(strcmp(name, "SceShell") == 0);
	assert(info->size == sizeof(*info));
	if (g_info_error == 1)
		return -1;
	info->modid = 42;
	info->module_nid = g_module_nid;
	return 0;
}

int sceKernelGetModuleInfo(SceUID modid, SceKernelModuleInfo *info)
{
	assert(modid == 42 && info->size == sizeof(*info));
	if (g_info_error == 2)
		return -1;
	info->segments[0].vaddr = g_null_segment ? NULL : g_text;
	info->segments[0].memsz = g_text_size;
	return 0;
}

SceUID taiInjectData(SceUID modid, int segment, uint32_t offset,
	const void *data, SceSize size)
{
	unsigned int index = g_injection_calls++;
	assert(modid == 42 && segment == 0 && index < ARRAY_SIZE(patches_360));
	assert(size <= sizeof(g_injections[0].original) && offset <= g_text_size - size);
	if ((int)index == g_fail_injection)
		return -1;
	g_injections[index].offset = offset;
	g_injections[index].size = size;
	memcpy(g_injections[index].original, g_text + offset, size);
	memcpy(g_text + offset, data, size);
	++g_live_injections;
	return (SceUID)index + 100;
}

int taiInjectRelease(SceUID uid)
{
	unsigned int index = (unsigned int)(uid - 100);
	assert(g_live_injections && index == g_live_injections - 1);
	memcpy(g_text + g_injections[index].offset,
		g_injections[index].original, g_injections[index].size);
	--g_live_injections;
	++g_release_calls;
	return 0;
}

static void reset_attempt(const PatchProfile *profile)
{
	assert(g_live_injections == 0);
	assert(!g_recovery_live);
	g_recovery_calls = 0;
	g_recovery_failure = 0;
	g_recovery_busy = 0;
#ifdef LIVEAREA_ICON_CACHE_TRIAL
	assert(!g_trial_live);
	g_trial_failure = 0;
	g_trial_calls = 0;
#endif
	g_module_nid = profile->module_nid;
	g_text_size = profile->text_size;
	g_info_error = 0;
	g_null_segment = 0;
	g_fail_injection = -1;
	g_injection_calls = 0;
	g_release_calls = 0;
}

static void expect_rejection(void)
{
	assert(module_start(0, NULL) == SCE_KERNEL_START_FAILED);
	assert(g_injection_calls == 0 && g_live_injections == 0);
	assert(module_stop(0, NULL) == SCE_KERNEL_STOP_SUCCESS);
	assert(g_release_calls == 0);
}

static void test_profile(const PatchProfile *profile)
{
	uint8_t *original = malloc(profile->text_size);
	unsigned int index;
	assert(original && profile->patch_count == ARRAY_SIZE(patches_360));
	memcpy(original, g_text, profile->text_size);
	reset_attempt(profile);
	assert(module_start(0, NULL) == SCE_KERNEL_START_SUCCESS);
	assert(g_injection_calls == ARRAY_SIZE(patches_360) && g_live_injections == ARRAY_SIZE(patches_360));
	assert(g_recovery_calls == 1 && g_recovery_live);
#ifdef LIVEAREA_ICON_CACHE_TRIAL
	assert(g_trial_calls == 1 && g_trial_live);
#endif
	for (index = 0; index < profile->patch_count; ++index) {
		const Patch *patch = &profile->patches[index];
		assert(memcmp(g_text + patch->offset, patch->replacement, patch->size) == 0);
	}
	g_recovery_busy = 1;
	assert(module_stop(0, NULL) != SCE_KERNEL_STOP_SUCCESS);
	assert(g_live_injections == ARRAY_SIZE(patches_360) && g_recovery_live);
	g_recovery_busy = 0;
	assert(module_stop(0, NULL) == SCE_KERNEL_STOP_SUCCESS);
	assert(g_live_injections == 0 && g_release_calls == ARRAY_SIZE(patches_360));
	assert(memcmp(g_text, original, profile->text_size) == 0);
	assert(module_stop(0, NULL) == SCE_KERNEL_STOP_SUCCESS);
	assert(g_release_calls == ARRAY_SIZE(patches_360));
	reset_attempt(profile);
	g_recovery_failure = 1;
	assert(module_start(0, NULL) == SCE_KERNEL_START_SUCCESS);
	assert(g_recovery_calls == 1 && !g_recovery_live);
	assert(g_live_injections == ARRAY_SIZE(patches_360));
	assert(module_stop(0, NULL) == SCE_KERNEL_STOP_SUCCESS);
	assert(memcmp(g_text, original, profile->text_size) == 0);

#ifdef LIVEAREA_ICON_CACHE_TRIAL
	reset_attempt(profile);
	g_trial_failure = 1;
	assert(module_start(0, NULL) == SCE_KERNEL_START_SUCCESS);
	assert(g_trial_calls == 1 && !g_trial_live);
	assert(g_live_injections == ARRAY_SIZE(patches_360) && g_release_calls == 0);
	assert(module_stop(0, NULL) == SCE_KERNEL_STOP_SUCCESS);
	assert(g_live_injections == 0 && g_release_calls == ARRAY_SIZE(patches_360));
	assert(memcmp(g_text, original, profile->text_size) == 0);
#endif

	/* Every original byte is checked before the first write. */
	for (index = 0; index < profile->patch_count; ++index) {
		const Patch *patch = &profile->patches[index];
		unsigned int byte;
		for (byte = 0; byte < patch->size; ++byte) {
			reset_attempt(profile);
			g_text[patch->offset + byte] ^= 1;
			expect_rejection();
			g_text[patch->offset + byte] ^= 1;
		}
	}
	/* Every possible partial installation rolls back in reverse order. */
	for (index = 0; index < profile->patch_count; ++index) {
		reset_attempt(profile);
		g_fail_injection = (int)index;
		assert(module_start(0, NULL) == SCE_KERNEL_START_FAILED);
		assert(g_injection_calls == index + 1 && g_live_injections == 0);
		assert(g_release_calls == index);
		assert(memcmp(g_text, original, profile->text_size) == 0);
	}
	reset_attempt(profile);
	g_module_nid = 0;
	expect_rejection();
	reset_attempt(profile);
	g_module_nid = 0x03650000U; /* A version number is not a module identity. */
	expect_rejection();
	for (index = 0; index < ARRAY_SIZE(patch_profiles); ++index) {
		if (patch_profiles[index].module_nid == profile->module_nid)
			continue;
		reset_attempt(profile);
		g_module_nid = patch_profiles[index].module_nid;
		expect_rejection();
	}
	reset_attempt(profile);
	--g_text_size;
	expect_rejection();
	reset_attempt(profile);
	++g_text_size;
	expect_rejection();
	reset_attempt(profile);
	g_null_segment = 1;
	expect_rejection();
	for (index = 1; index <= 2; ++index) {
		reset_attempt(profile);
		g_info_error = (int)index;
		expect_rejection();
	}
	free(original);
}

static void dump_manifest(void)
{
	printf("{\"pages\":%u,\"top_level\":%u,\"counted\":%u,\"profiles\":[",
		LIVEAREA_PAGE_LIMIT, LIVEAREA_TOP_LEVEL_LIMIT, LIVEAREA_ICON_LIMIT);
	for (unsigned int i = 0; i < ARRAY_SIZE(patch_profiles); ++i) {
		const PatchProfile *profile = &patch_profiles[i];
		printf("%s{\"nid\":%u,\"text_size\":%u,\"patches\":[",
			i ? "," : "", profile->module_nid, (unsigned int)profile->text_size);
		for (unsigned int j = 0; j < profile->patch_count; ++j) {
			const Patch *patch = &profile->patches[j];
			printf("%s{\"offset\":%u,\"size\":%u,\"symbol\":\"%s\",\"expected\":\"",
				j ? "," : "", patch->offset, patch->size, test_patch_name(patch->replacement));
			for (unsigned int k = 0; k < patch->size; ++k)
				printf("%02x", patch->expected[k]);
			printf("\"}");
		}
		printf("]}");
	}
	puts("]}");
}

int main(int argc, char **argv)
{
	unsigned int index;
	int arg;
	if (argc == 2 && strcmp(argv[1], "--manifest") == 0) {
		dump_manifest();
		return 0;
	}
	assert(argc % 2 == 1);
	assert(LIVEAREA_TOP_LEVEL_LIMIT == 500);
	assert(LIVEAREA_PAGE_LIMIT == 50);
	assert(LIVEAREA_ICONS_PER_PAGE == 10 && LIVEAREA_ICON_LIMIT == 1000);
	assert(sizeof(((Patch *)0)->expected) >= 22);
	assert(ARRAY_SIZE(patches_360) == 22);
	for (index = 0; index < ARRAY_SIZE(patch_profiles); ++index) {
		const PatchProfile *profile = &patch_profiles[index];
		unsigned int patch;
		g_text = calloc(1, profile->text_size);
		assert(g_text);
		for (patch = 0; patch < profile->patch_count; ++patch)
			memcpy(g_text + profile->patches[patch].offset,
				profile->patches[patch].expected, profile->patches[patch].size);
		test_profile(profile);
		free(g_text);
		printf("Startup/validation/rollback passed: NID 0x%08X\n", profile->module_nid);
	}
	for (arg = 1; arg < argc; arg += 2) {
		const PatchProfile *profile = find_patch_profile((uint32_t)strtoul(argv[arg], NULL, 0));
		FILE *file = fopen(argv[arg + 1], "rb");
		assert(profile && file);
		g_text = malloc(profile->text_size);
		assert(g_text && fread(g_text, 1, profile->text_size, file) == profile->text_size);
		assert(fgetc(file) == EOF);
		fclose(file);
		test_profile(profile);
		free(g_text);
		printf("Real shell bytes passed all startup/validation/rollback tests: %s\n", argv[arg + 1]);
	}
	return 0;
}
