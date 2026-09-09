#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/recovery.c"

#if LIVEAREA_DEBUG_LOGGING
#include "debug_capture.h"
#endif

static _Alignas(4) uint8_t text[RECOVERY_TEXT_SIZE];
static uint8_t original[RECOVERY_TEXT_SIZE];
static uint8_t data_segment[RECOVERY_DATA_SIZE];
static uint32_t module_nid;
static int module_loaded;
static int module_live;
static int info_failure;
static int bad_segment;
static int import_failure;
static int injection_failure;
static int release_failure;
static int start_error;
static int start_status;
static int stop_error;
static int stop_status;
static int unload_error;
static int recurse_start;
static int import_calls;
static int injection_calls;
static int start_calls;
static int stop_calls;
static int unload_calls;
static int live_imports[3];
static int live_patches[ARRAY_COUNT(recovery_patches)];
static int database_object;
static const void *import_hooks[3];

static void seed_text(void)
{
	unsigned int i;

	memset(text, 0xCD, sizeof(text));
	for (i = 0; i < ARRAY_COUNT(recovery_patches); ++i) {
		const RecoveryPatch *p = &recovery_patches[i];
		memcpy(text + p->offset, p->expected, p->size);
	}
	memcpy(original, text, sizeof(text));
}

static void reset_case(uint32_t nid)
{
	unsigned int i;
	assert(!initialized && !module_live);
	for (i = 0; i < ARRAY_COUNT(live_patches); ++i)
		assert(!live_patches[i]);
	for (i = 0; i < ARRAY_COUNT(live_imports); ++i)
		assert(!live_imports[i]);
	module_nid = nid;
	module_loaded = 1;
	info_failure = bad_segment = 0;
	import_failure = injection_failure = release_failure = -1;
	start_error = stop_error = unload_error = 0;
	start_status = stop_status = 0;
	recurse_start = 0;
	import_calls = injection_calls = start_calls = stop_calls = unload_calls = 0;
#if LIVEAREA_DEBUG_LOGGING
	test_debug_capture_reset();
#endif
	seed_text();
}

#if LIVEAREA_DEBUG_LOGGING
static int call_start(SceUID uid);
static void finish_case(void);

static void test_diagnostic_trace(void)
{
	reset_case(0x3F76E38FU);
	assert(recovery_start(0x5549BF1FU) == 0);
	assert(call_start(71) == 0);
	assert(test_debug_capture_contains("[recovery] start shell_nid=0x5549BF1F"));
	assert(test_debug_capture_contains("[recovery] lifecycle-hook slot=0"));
	assert(test_debug_capture_contains("[recovery] module-start observed"));
	assert(test_debug_capture_contains("[recovery] validation-complete"));
	assert(test_debug_capture_contains("[recovery] patch-injected index=6"));
	assert(test_debug_capture_contains("[recovery] native-start result=0 status=0"));
	finish_case();
}
#endif

int taiGetModuleInfo(const char *name, tai_module_info_t *info)
{
	assert(strcmp(name, "SceDbRecovery") == 0 && info->size == sizeof(*info));
	if (!module_loaded || info_failure == 1)
		return -1;
	info->modid = 71;
	info->module_nid = module_nid;
	return 0;
}

int sceKernelGetModuleInfo(SceUID modid, SceKernelModuleInfo *info)
{
	assert(modid == 71 && info->size == sizeof(*info));
	if (!module_loaded || info_failure == 2)
		return -1;
	info->segments[0].vaddr = bad_segment == 1 ? NULL : text;
	info->segments[0].memsz = sizeof(text) + (bad_segment == 2);
	info->segments[1].vaddr = bad_segment == 3 ? NULL : data_segment;
	info->segments[1].memsz = sizeof(data_segment) + (bad_segment == 4);
	return 0;
}

SceUID taiHookFunctionImport(tai_hook_ref_t *ref, const char *name,
	uint32_t library, uint32_t nid, const void *hook)
{
	static const uint32_t expected[] = {START_MODULE_NID, STOP_MODULE_NID, UNLOAD_MODULE_NID};
	int call = import_calls++;
	int i = 2 - call;
	assert(call < 3 && strcmp(name, "SceLibKernel") == 0 && library == MODULEMGR_LIBRARY);
	assert(nid == expected[i]);
	if (call == import_failure)
		return -1;
	assert(!live_imports[i]);
	live_imports[i] = 1;
	import_hooks[i] = hook;
	*ref = (tai_hook_ref_t)i + 10;
	return i + 10;
}

SceUID taiInjectData(SceUID modid, int segment, uint32_t offset,
	const void *bytes, SceSize size)
{
	int i = injection_calls++;
	const RecoveryPatch *p = &recovery_patches[i];
	assert(module_loaded && !module_live);
	assert(i < (int)ARRAY_COUNT(recovery_patches) && !live_patches[i]);
	assert(modid == 71 && segment == 0 && offset == p->offset && size == p->size);
	assert(memcmp(text + offset, p->expected, size) == 0);
	if (i == injection_failure)
		return -1;
	memcpy(text + offset, bytes, size);
	live_patches[i] = 1;
	return i + 200;
}

int taiInjectRelease(SceUID uid)
{
	int i = uid - 200;
	const RecoveryPatch *p = &recovery_patches[i];
	assert(module_loaded && !module_live);
	assert(i >= 0 && i < (int)ARRAY_COUNT(recovery_patches) && live_patches[i]);
	if (uid == release_failure)
		return -1;
	memcpy(text + p->offset, p->expected, p->size);
	live_patches[i] = 0;
	return 0;
}

int taiHookRelease(SceUID uid, tai_hook_ref_t ref)
{
	assert(uid == (SceUID)ref);
	if (uid == release_failure)
		return -1;
	assert(uid >= 10 && uid < 13 && live_imports[uid - 10]);
	live_imports[uid - 10] = 0;
	return 0;
}

static int patch_value(unsigned int i, int stock, int configured)
{
	const RecoveryPatch *p = &recovery_patches[i];
	if (memcmp(text + p->offset, p->replacement, p->size) == 0)
		return configured;
	assert(memcmp(text + p->offset, p->expected, p->size) == 0);
	return stock;
}

int test_tai_continue(tai_hook_ref_t ref, ...)
{
	va_list ap;
	int result;
	va_start(ap, ref);
	if (ref == 12) {
		SceUID modid = va_arg(ap, SceUID);
		(void)va_arg(ap, int);
		(void)va_arg(ap, const void *);
		++unload_calls;
		if (modid == 71) {
			assert(!module_live);
			if (!unload_error)
				module_loaded = 0;
		}
		result = unload_error;
	} else {
		SceUID modid = va_arg(ap, SceUID);
		(void)va_arg(ap, SceSize);
		(void)va_arg(ap, const void *);
		ModuleAction *action = va_arg(ap, ModuleAction *);
		assert(ref == 10 || ref == 11);
		assert(action && action->flags == 0x41 && action->option == &database_object);
		if (ref == 10) {
			++start_calls;
			if (recurse_start) {
				recurse_start = 0;
				assert(start_module_hook(modid, 0, NULL, action) < 0);
			}
			result = start_error;
			if (result >= 0) {
				if (action->result)
					*action->result = start_status;
				if (modid == 71)
					module_live = start_status == 0;
			}
		} else {
			++stop_calls;
			result = stop_error;
			if (result >= 0) {
				if (action->result)
					*action->result = stop_status;
				if (modid == 71 && stop_status == 0)
					module_live = 0;
			}
		}
	}
	va_end(ap);
	return result;
}

static int call_start(SceUID uid)
{
	ModuleAction action = {0x41, &database_object, NULL, 0};
	return start_module_hook(uid, 0, NULL, &action);
}

static int call_stop(void)
{
	ModuleAction action = {0x41, &database_object, NULL, 0};
	return stop_module_hook(71, 0, NULL, &action);
}

static void finish_case(void)
{
	if (module_live)
		assert(call_stop() == 0);
	assert(recovery_stop() == 0);
	assert(memcmp(text, original, sizeof(text)) == 0);
}

static void test_recovery_limits(void)
{
	unsigned int i;

	assert(LIVEAREA_TOP_LEVEL_LIMIT ==
		LIVEAREA_PAGE_LIMIT * LIVEAREA_ICONS_PER_PAGE);
	reset_case(0xC1F30F67U);
	for (i = 0; i < 5; ++i)
		assert(patch_value(i, 500, LIVEAREA_ICON_LIMIT) == 500);
	assert(patch_value(5, 10, LIVEAREA_PAGE_LIMIT) == 10);
	assert(patch_value(6, 9, LIVEAREA_PAGE_LIMIT - 1) == 9);

	assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
	for (i = 0; i < 5; ++i)
		assert(patch_value(i, 500, LIVEAREA_ICON_LIMIT) == LIVEAREA_ICON_LIMIT);
	assert(patch_value(5, 10, LIVEAREA_PAGE_LIMIT) == LIVEAREA_PAGE_LIMIT);
	assert(patch_value(6, 9, LIVEAREA_PAGE_LIMIT - 1) == LIVEAREA_PAGE_LIMIT - 1);
	assert(injection_calls == 7);

	assert(call_stop() == 0);
	injection_calls = 0;
	assert(call_start(71) == 0 && injection_calls == 7);
	finish_case();
	puts("Recovery limits: counted capacity, page capacity, repeat start and rollback passed");
}

static void test_allocator_entry_is_not_a_dependency(void)
{
	reset_case(0x3F76E38FU);
	text[0x5F50] ^= 1;
	memcpy(original, text, sizeof(text));
	assert(recovery_start(0x5549BF1FU) == 0 && call_start(71) == 0);
	assert(injection_calls == 7 && recovery_running);
	finish_case();
	puts("Recovery direct patches do not inspect or hook the relocating allocator entry");
}

static void test_profiles_and_validation(void)
{
	unsigned int p, i, j;
	for (p = 0; p < ARRAY_COUNT(recovery_profiles); ++p) {
		reset_case(recovery_profiles[p].recovery_nid);
		assert(recovery_start(recovery_profiles[p].shell_nid) == 0);
		assert(import_hooks[0] == (const void *)start_module_hook);
		assert(import_hooks[1] == (const void *)stop_module_hook);
		assert(import_hooks[2] == (const void *)unload_module_hook);
		assert(call_start(71) == 0 && injection_calls == 7);
		assert(recovery_stop() < 0);
		assert(unload_module_hook(71, 0, NULL) < 0 && unload_calls == 0);
		finish_case();
	}
	reset_case(0xC1F30F67U);
	assert(recovery_start(0) < 0 && import_calls == 0);
	for (i = 0; i < ARRAY_COUNT(recovery_patches); ++i) {
		const RecoveryPatch *patch = &recovery_patches[i];
		for (j = 0; j < patch->size; ++j) {
			reset_case(0xC1F30F67U);
			text[patch->offset + j] ^= 1;
			memcpy(original, text, sizeof(text));
			assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
			assert(injection_calls == 0);
			finish_case();
		}
	}
	for (i = 1; i <= 4; ++i) {
		reset_case(0xC1F30F67U);
		bad_segment = (int)i;
		assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
		assert(injection_calls == 0);
		finish_case();
	}
	for (i = 0; i < 3; ++i) {
		reset_case(i == 0 ? 0xDEADBEEFU : 0xC1F30F67U);
		info_failure = (int)i;
		assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
		assert(injection_calls == 0);
		info_failure = 0;
		finish_case();
	}
	reset_case(0xC1F30F67U);
	assert(recovery_start(0x5549BF1FU) == 0 && call_start(71) == 0);
	assert(injection_calls == 0);
	finish_case();
}

static void test_failures_and_lifecycle(void)
{
	unsigned int i;
	for (i = 0; i < 3; ++i) {
		reset_case(0xC1F30F67U);
		import_failure = (int)i;
		assert(recovery_start(0x0552F692U) < 0 && !initialized);
		finish_case();
	}
	for (i = 0; i < ARRAY_COUNT(recovery_patches); ++i) {
		reset_case(0xC1F30F67U);
		injection_failure = (int)i;
		assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
		assert(injection_calls == (int)i + 1);
		finish_case();
	}
	reset_case(0xC1F30F67U);
	assert(recovery_start(0x0552F692U) == 0);
	start_error = -55;
	assert(call_start(71) == -55);
	start_error = 0;
	start_status = SCE_KERNEL_START_FAILED;
	injection_calls = 0;
	assert(call_start(71) == 0);
	start_status = 0;
	injection_calls = 0;
	recurse_start = 1;
	assert(call_start(71) == 0 && recovery_running);
	start_error = -66;
	assert(call_start(71) == -66 && injection_calls == 7);
	start_error = 0;
	stop_error = -77;
	assert(call_stop() == -77);
	stop_error = 0;
	stop_status = 1;
	assert(call_stop() == 0 && recovery_running);
	stop_status = 0;
	assert(call_stop() == 0);
	unload_error = -88;
	assert(unload_module_hook(71, 0, NULL) == -88 && module_loaded);
	unload_error = 0;
	injection_calls = 0;
	assert(call_start(71) == 0 && recovery_running);
	finish_case();
	for (i = 0; i < ARRAY_COUNT(recovery_patches); ++i) {
		reset_case(0xC1F30F67U);
		assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
		release_failure = 200 + (int)i;
		assert(call_stop() < 0 && recovery_modid == 71 && !module_live);
		assert(recovery_stop() < 0);
		assert(unload_module_hook(71, 0, NULL) < 0 && unload_calls == 0);
		release_failure = -1;
		assert(unload_module_hook(71, 0, NULL) == 0 && !module_loaded);
		finish_case();
	}
	reset_case(0xC1F30F67U);
	assert(recovery_start(0x0552F692U) == 0);
	assert(call_start(99) == 0 && injection_calls == 0);
	assert(recovery_start(0x0552F692U) < 0);
	finish_case();
	for (i = 10; i < 13; ++i) {
		reset_case(0xC1F30F67U);
		assert(recovery_start(0x0552F692U) == 0);
		release_failure = (int)i;
		assert(recovery_stop() < 0 && initialized);
		assert(recovery_start(0x0552F692U) < 0);
		release_failure = -1;
		finish_case();
	}
	puts("Recovery lifecycle: validated identities/bytes, all partial installations, start/stop/unload failures, re-entry, reload and cleanup failures passed");
}

static void test_firmware(uint32_t shell_nid, const char *path)
{
	FILE *file;
	unsigned int i;
	uint32_t nid;
	for (i = 0; i < ARRAY_COUNT(recovery_profiles); ++i)
		if (shell_nid == recovery_profiles[i].shell_nid)
			break;
	assert(i < ARRAY_COUNT(recovery_profiles));
	reset_case(recovery_profiles[i].recovery_nid);
	file = fopen(path, "rb");
	assert(file && fread(text, 1, sizeof(text), file) == sizeof(text));
	assert(fgetc(file) == EOF);
	fclose(file);
	memcpy(&nid, text + 0x2C294U + 52, sizeof(nid));
	assert(nid == module_nid);
	memcpy(original, text, sizeof(text));
	assert(recovery_start(shell_nid) == 0 && call_start(71) == 0);
	assert(injection_calls == 7);
	finish_case();
	printf("Real recovery bytes: identity, seven direct patches and rollback passed: %s\n", path);
}

int main(int argc, char **argv)
{
	int arg;
	assert(argc % 2 == 1);
#if LIVEAREA_DEBUG_LOGGING
	test_diagnostic_trace();
#endif
	test_recovery_limits();
	test_allocator_entry_is_not_a_dependency();
	test_profiles_and_validation();
	test_failures_and_lifecycle();
	for (arg = 1; arg < argc; arg += 2)
		test_firmware((uint32_t)strtoul(argv[arg], NULL, 0), argv[arg + 1]);
	return 0;
}
