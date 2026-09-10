#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/modulemgr.h>
#include <psp2/types.h>
#include <taihen.h>

#if LIVEAREA_DEBUG_LOGGING
#include "debug_capture.h"
#endif

extern const uint8_t patch_cmp_r0_icon_limit[4];
extern const uint8_t patch_rsbs_r1_r0_icon_limit[4];
extern const uint8_t patch_cmp_r4_page_limit[2];
extern const uint8_t patch_movs_r9_last_page[4];

enum Event {
	EVENT_HOOK_INSTALL,
	EVENT_INJECT,
	EVENT_CONTINUE,
	EVENT_INJECT_RELEASE,
	EVENT_NATIVE_STOP,
	EVENT_HOOK_RELEASE,
};

typedef struct {
	SceUID uid;
	tai_hook_ref_t ref;
	SceUID modid;
	uint32_t offset;
	const void *function;
	int live;
} HookRecord;

typedef struct {
	SceUID uid;
	uint32_t offset;
	SceSize size;
	uint8_t original[10];
	int live;
} InjectionRecord;

static enum Event events[128];
static unsigned int event_count;
static HookRecord hooks[16];
static unsigned int hook_count;
static unsigned int hook_install_calls;
static int hook_install_failure_call;
static SceUID hook_release_failure_uid;
static unsigned int hook_release_calls;
static SceUID hook_release_order[16];
static unsigned int import_hook_calls;
static InjectionRecord injections[16];
static unsigned int injection_count;
static unsigned int injection_attempts;
static int injection_failure_call;
static SceUID injection_release_failure_uid;
static unsigned int injection_release_calls;
static SceUID injection_release_order[16];
static unsigned int continue_calls;
static tai_hook_ref_t continue_order[16];
static int ready_original_result;
static int unload_during_ready;
static unsigned int native_stop_calls;
static int native_stop_result;
static SceSize native_stop_args;
static const void *native_stop_argp;
static int recovery_available;
static int module_info_failure;
static SceUID reported_recovery_modid;
static uint32_t reported_recovery_nid;
static int recovery_text_null;
static int recovery_data_null;
static int recovery_text_size_delta;
static int recovery_data_size_delta;
static uint8_t *shell_text;
static uint8_t *recovery_text;
static uint8_t *recovery_original;
static uint8_t *recovery_data;
static SceKernelModuleInfo shell_info;

#include "../src/recovery.c"

static void record_event(enum Event event)
{
	assert(event_count < sizeof(events) / sizeof(events[0]));
	events[event_count++] = event;
}

int taiGetModuleInfo(const char *name, tai_module_info_t *info)
{
	assert(strcmp(name, "SceDbRecovery") == 0);
	assert(info->size == sizeof(*info));
	if (!recovery_available)
		return -1;
	info->modid = reported_recovery_modid;
	info->module_nid = reported_recovery_nid;
	return 0;
}

SceUID taiInjectData(SceUID modid, int segment, uint32_t offset,
	const void *data, SceSize size)
{
	unsigned int attempt = injection_attempts++;
	InjectionRecord *record;

	assert(modid == reported_recovery_modid && segment == 0);
	assert(attempt <= ARRAY_COUNT(recovery_patches));
	if (attempt == 0) {
		uint32_t target;

		assert(offset == RECOVERY_STOP_OFFSET);
		assert(size == sizeof(expected_stop_entry));
		assert(memcmp(data, stop_redirect_prefix,
			sizeof(stop_redirect_prefix)) == 0);
		memcpy(&target, (const uint8_t *)data +
			sizeof(stop_redirect_prefix), sizeof(target));
		assert(target == ((uint32_t)(uintptr_t)
			recovery_module_stop_redirect | 1U));
		assert(memcmp(recovery_text + offset, expected_stop_entry,
			sizeof(expected_stop_entry)) == 0);
	} else {
		const RecoveryPatch *patch = &recovery_patches[attempt - 1];

		assert(offset == patch->offset && size == patch->size);
		assert(data == patch->replacement);
		assert(memcmp(recovery_text + offset, patch->expected, size) == 0);
	}
	if ((int)attempt == injection_failure_call)
		return -100 - (int)attempt;
	assert(injection_count < ARRAY_COUNT(injections));
	record = &injections[injection_count++];
	record->uid = 500 + (SceUID)attempt;
	record->offset = offset;
	record->size = size;
	memcpy(record->original, recovery_text + offset, size);
	memcpy(recovery_text + offset, data, size);
	record->live = 1;
	record_event(EVENT_INJECT);
	return record->uid;
}

int taiInjectRelease(SceUID uid)
{
	unsigned int index;

	assert(injection_release_calls < ARRAY_COUNT(injection_release_order));
	injection_release_order[injection_release_calls++] = uid;
	record_event(EVENT_INJECT_RELEASE);
	if (uid == injection_release_failure_uid)
		return -200;
	for (index = 0; index < injection_count; ++index) {
		InjectionRecord *record = &injections[index];

		if (record->uid == uid) {
			assert(record->live);
			memcpy(recovery_text + record->offset, record->original,
				record->size);
			record->live = 0;
			return 0;
		}
	}
	assert(0);
	return -1;
}

SceUID taiHookFunctionImport(tai_hook_ref_t *ref, const char *module,
	uint32_t library, uint32_t nid, const void *hook)
{
	(void)ref;
	(void)module;
	(void)library;
	(void)nid;
	(void)hook;
	++import_hook_calls;
	return -1;
}

SceUID taiHookFunctionOffset(tai_hook_ref_t *ref, SceUID modid, int segment,
	uint32_t offset, int thumb, const void *hook)
{
	unsigned int call = hook_install_calls++;
	HookRecord *record;

	assert(segment == 0 && thumb == 1);
	if ((int)call == hook_install_failure_call)
		return -300 - (int)call;
	assert(hook_count < ARRAY_COUNT(hooks));
	record = &hooks[hook_count];
	record->uid = 300 + (SceUID)hook_count;
	record->ref = 0x1000U + hook_count;
	record->modid = modid;
	record->offset = offset;
	record->function = hook;
	record->live = 1;
	*ref = record->ref;
	++hook_count;
	record_event(EVENT_HOOK_INSTALL);
	return record->uid;
}

int taiHookRelease(SceUID uid, tai_hook_ref_t ref)
{
	unsigned int index;

	assert(hook_release_calls < ARRAY_COUNT(hook_release_order));
	hook_release_order[hook_release_calls++] = uid;
	record_event(EVENT_HOOK_RELEASE);
	if (uid == hook_release_failure_uid)
		return -400;
	for (index = 0; index < hook_count; ++index) {
		HookRecord *record = &hooks[index];

		if (record->uid == uid) {
			assert(record->live && record->ref == ref);
			record->live = 0;
			return 0;
		}
	}
	assert(0);
	return -1;
}

int test_tai_continue(tai_hook_ref_t ref, ...)
{
	unsigned int index;
	int result = 0;

	assert(continue_calls < ARRAY_COUNT(continue_order));
	continue_order[continue_calls++] = ref;
	record_event(EVENT_CONTINUE);
	for (index = 0; index < hook_count; ++index) {
		if (hooks[index].ref != ref)
			continue;
		assert(hooks[index].offset == SHELL_RECOVERY_READY_OFFSET);
		if (unload_during_ready) {
			/* Even an unpatched fallback still executes the plugin trampoline. */
			assert(recovery_modid < 0);
			assert(recovery_stop() < 0);
			assert(hooks[index].live);
		}
		result = ready_original_result;
		return result;
	}
	assert(0);
	return -1;
}

static int fake_native_stop(SceSize args, const void *argp)
{
	++native_stop_calls;
	native_stop_args = args;
	native_stop_argp = argp;
	record_event(EVENT_NATIVE_STOP);
	return native_stop_result;
}

int sceKernelGetModuleInfo(SceUID modid, SceKernelModuleInfo *info)
{
	assert(modid == reported_recovery_modid);
	assert(info->size == sizeof(*info));
	if (module_info_failure)
		return -1;
	info->segments[0].vaddr = recovery_text_null ? NULL : recovery_text;
	info->segments[0].memsz = RECOVERY_TEXT_SIZE + recovery_text_size_delta;
	info->segments[1].vaddr = recovery_data_null ? NULL : recovery_data;
	info->segments[1].memsz = RECOVERY_DATA_SIZE + recovery_data_size_delta;
	return 0;
}

static HookRecord *find_hook(uint32_t offset, int live)
{
	unsigned int index;

	for (index = 0; index < hook_count; ++index) {
		if (hooks[index].offset == offset && hooks[index].live == live)
			return &hooks[index];
	}
	return NULL;
}

static int live_injection_count(void)
{
	unsigned int index;
	int count = 0;

	for (index = 0; index < injection_count; ++index)
		count += injections[index].live != 0;
	return count;
}

static void initialize_recovery_image(void)
{
	unsigned int index;

	memset(recovery_text, 0, RECOVERY_TEXT_SIZE);
	memcpy(recovery_text + RECOVERY_STOP_OFFSET, expected_stop_entry,
		sizeof(expected_stop_entry));
	for (index = 0; index < ARRAY_COUNT(recovery_patches); ++index) {
		const RecoveryPatch *patch = &recovery_patches[index];

		memcpy(recovery_text + patch->offset, patch->expected, patch->size);
	}
	memcpy(recovery_original, recovery_text, RECOVERY_TEXT_SIZE);
}

static void reset_source_state(void)
{
	active_profile = NULL;
	ready_hook_id = -1;
	ready_hook_ref = 0;
	stop_redirect_id = -1;
	recovery_modid = -1;
	native_stop = NULL;
	recovery_install_complete = 0;
	clear_patch_ids();
	lifecycle_busy = 0;
	initialized = 0;
	ready_in_flight = 0;
}

static void reset_case(const RecoveryProfile *profile)
{
	free(shell_text);
	free(recovery_text);
	free(recovery_original);
	free(recovery_data);
	shell_text = calloc(1, profile->shell_text_size);
	recovery_text = calloc(1, RECOVERY_TEXT_SIZE);
	recovery_original = malloc(RECOVERY_TEXT_SIZE);
	recovery_data = calloc(1, RECOVERY_DATA_SIZE);
	assert(shell_text && recovery_text && recovery_original && recovery_data);
	memcpy(shell_text + SHELL_RECOVERY_READY_OFFSET, profile->ready_prefix,
		sizeof(profile->ready_prefix));
	initialize_recovery_image();
	memset(&shell_info, 0, sizeof(shell_info));
	shell_info.size = sizeof(shell_info);
	shell_info.segments[0].vaddr = shell_text;
	shell_info.segments[0].memsz = profile->shell_text_size;
	memset(events, 0, sizeof(events));
	event_count = 0;
	memset(hooks, 0, sizeof(hooks));
	hook_count = 0;
	hook_install_calls = 0;
	hook_install_failure_call = -1;
	hook_release_failure_uid = -1;
	hook_release_calls = 0;
	memset(hook_release_order, 0, sizeof(hook_release_order));
	import_hook_calls = 0;
	memset(injections, 0, sizeof(injections));
	injection_count = 0;
	injection_attempts = 0;
	injection_failure_call = -1;
	injection_release_failure_uid = -1;
	injection_release_calls = 0;
	memset(injection_release_order, 0, sizeof(injection_release_order));
	continue_calls = 0;
	memset(continue_order, 0, sizeof(continue_order));
	ready_original_result = 77;
	unload_during_ready = 0;
	native_stop_calls = 0;
	native_stop_result = SCE_KERNEL_STOP_SUCCESS;
	native_stop_args = 0;
	native_stop_argp = NULL;
	recovery_available = 1;
	module_info_failure = 0;
	reported_recovery_modid = 71;
	reported_recovery_nid = profile->recovery_nid;
	recovery_text_null = 0;
	recovery_data_null = 0;
	recovery_text_size_delta = 0;
	recovery_data_size_delta = 0;
	reset_source_state();
#if LIVEAREA_DEBUG_LOGGING
	test_debug_capture_reset();
#endif
}

static int call_ready(void)
{
	HookRecord *hook = find_hook(SHELL_RECOVERY_READY_OFFSET, 1);
	int (*function)(void *);

	assert(hook != NULL);
	function = (int (*)(void *))hook->function;
	return function((void *)0x12345678);
}

static int call_recovery_stop(void)
{
	if (native_stop != fake_native_stop) {
		assert((uintptr_t)native_stop ==
			(uintptr_t)recovery_text + RECOVERY_STOP_OFFSET + 1U);
		native_stop = fake_native_stop;
	}
	return recovery_module_stop_redirect(0x42U, (const void *)0x1234U);
}

static void start_case(const RecoveryProfile *profile)
{
	assert(recovery_start(42, profile->shell_nid, &shell_info) == 0);
	assert(import_hook_calls == 0);
	assert(hook_count == 1 && hook_install_calls == 1);
	assert(hooks[0].modid == 42);
	assert(hooks[0].offset == SHELL_RECOVERY_READY_OFFSET);
	assert(hooks[0].live);
}

static void expect_clean(void)
{
	unsigned int index;

	assert(recovery_modid < 0 && stop_redirect_id < 0);
	assert(native_stop == NULL);
	assert(!recovery_install_complete);
	assert(live_injection_count() == 0);
	assert(memcmp(recovery_text, recovery_original, RECOVERY_TEXT_SIZE) == 0);
	for (index = 0; index < ARRAY_COUNT(patch_ids); ++index)
		assert(patch_ids[index] < 0);
}

static void finish_case(void)
{
	if (stop_redirect_id >= 0) {
		injection_release_failure_uid = -1;
		assert(call_recovery_stop() == SCE_KERNEL_STOP_SUCCESS);
	}
	expect_clean();
	if (initialized) {
		hook_release_failure_uid = -1;
		assert(recovery_stop() == 0);
	}
	assert(!initialized);
	assert(find_hook(SHELL_RECOVERY_READY_OFFSET, 1) == NULL);
}

static void test_happy_path(void)
{
	unsigned int profile_index;
	unsigned int index;

	for (profile_index = 0; profile_index < ARRAY_COUNT(recovery_profiles);
		++profile_index) {
		const RecoveryProfile *profile = &recovery_profiles[profile_index];

		reset_case(profile);
		start_case(profile);
		event_count = 0;
		assert(call_ready() == ready_original_result);
		assert(hook_count == 1);
		assert(injection_attempts == ARRAY_COUNT(recovery_patches) + 1);
		assert(live_injection_count() ==
			(int)ARRAY_COUNT(recovery_patches) + 1);
		assert(recovery_install_complete);
		assert(event_count == ARRAY_COUNT(recovery_patches) + 2);
		for (index = 0; index <= ARRAY_COUNT(recovery_patches); ++index)
			assert(events[index] == EVENT_INJECT);
		assert(events[ARRAY_COUNT(recovery_patches) + 1] == EVENT_CONTINUE);
		assert(memcmp(recovery_text + RECOVERY_STOP_OFFSET,
			stop_redirect_prefix, sizeof(stop_redirect_prefix)) == 0);
		assert(recovery_stop() < 0);

		event_count = 0;
		assert(call_recovery_stop() == SCE_KERNEL_STOP_SUCCESS);
		assert(event_count == ARRAY_COUNT(recovery_patches) + 2);
		for (index = 0; index <= ARRAY_COUNT(recovery_patches); ++index)
			assert(events[index] == EVENT_INJECT_RELEASE);
		assert(events[ARRAY_COUNT(recovery_patches) + 1] == EVENT_NATIVE_STOP);
		assert(native_stop_calls == 1);
		assert(native_stop_args == 0x42U);
		assert(native_stop_argp == (const void *)0x1234U);
		for (index = 0; index <= ARRAY_COUNT(recovery_patches); ++index)
			assert(injection_release_order[index] ==
				500 + (SceUID)(ARRAY_COUNT(recovery_patches) - index));
		expect_clean();
		assert(recovery_stop() == 0);
		assert(!initialized && !hooks[0].live);
	}
	puts("Recovery process scope: three shell profiles, patching and cleanup passed");
}

static void test_shell_validation(void)
{
	const RecoveryProfile *profile = &recovery_profiles[0];
	unsigned int index;

	reset_case(profile);
	assert(recovery_start(42, 0xDEADBEEFU, &shell_info) < 0);
	assert(hook_install_calls == 0 && !initialized);

	reset_case(profile);
	shell_info.segments[0].vaddr = NULL;
	assert(recovery_start(42, profile->shell_nid, &shell_info) < 0);

	reset_case(profile);
	--shell_info.segments[0].memsz;
	assert(recovery_start(42, profile->shell_nid, &shell_info) < 0);

	for (index = 0; index < sizeof(profile->ready_prefix); ++index) {
		reset_case(profile);
		shell_text[SHELL_RECOVERY_READY_OFFSET + index] ^= 1;
		assert(recovery_start(42, profile->shell_nid, &shell_info) < 0);
		assert(hook_install_calls == 0 && !initialized);
	}

	reset_case(profile);
	hook_install_failure_call = 0;
	assert(recovery_start(42, profile->shell_nid, &shell_info) < 0);
	assert(!initialized && hook_count == 0);

	reset_case(profile);
	start_case(profile);
	assert(recovery_start(42, profile->shell_nid, &shell_info) < 0);
	finish_case();
	puts("Recovery shell validation: identity, size, bytes, hook failure and re-entry passed");
}

static void expect_ready_fallback(const RecoveryProfile *profile)
{
	memcpy(recovery_original, recovery_text, RECOVERY_TEXT_SIZE);
	start_case(profile);
	assert(call_ready() == ready_original_result);
	assert(hook_count == 1);
	assert(injection_attempts == 0 && live_injection_count() == 0);
	assert(recovery_modid < 0 && stop_redirect_id < 0);
	assert(!recovery_install_complete);
	finish_case();
}

static void test_recovery_validation(void)
{
	const RecoveryProfile *profile = &recovery_profiles[0];
	unsigned int patch_index;
	unsigned int byte_index;

	reset_case(profile);
	recovery_available = 0;
	expect_ready_fallback(profile);

	reset_case(profile);
	reported_recovery_nid ^= 1;
	expect_ready_fallback(profile);

	reset_case(profile);
	module_info_failure = 1;
	expect_ready_fallback(profile);

	reset_case(profile);
	recovery_text_null = 1;
	expect_ready_fallback(profile);

	reset_case(profile);
	recovery_data_null = 1;
	expect_ready_fallback(profile);

	reset_case(profile);
	recovery_text_size_delta = -1;
	expect_ready_fallback(profile);

	reset_case(profile);
	recovery_data_size_delta = 1;
	expect_ready_fallback(profile);

	for (byte_index = 0; byte_index < sizeof(expected_stop_entry); ++byte_index) {
		reset_case(profile);
		recovery_text[RECOVERY_STOP_OFFSET + byte_index] ^= 1;
		expect_ready_fallback(profile);
	}
	for (patch_index = 0; patch_index < ARRAY_COUNT(recovery_patches);
		++patch_index) {
		const RecoveryPatch *patch = &recovery_patches[patch_index];

		for (byte_index = 0; byte_index < patch->size; ++byte_index) {
			reset_case(profile);
			recovery_text[patch->offset + byte_index] ^= 1;
			expect_ready_fallback(profile);
		}
	}
	puts("Recovery module validation: identity, segments, stop entry and patch bytes passed");
}

static void test_install_failures(void)
{
	const RecoveryProfile *profile = &recovery_profiles[0];
	unsigned int failure;
	unsigned int index;

	reset_case(profile);
	start_case(profile);
	injection_failure_call = 0;
	assert(call_ready() == ready_original_result);
	assert(injection_attempts == 1 && injection_count == 0);
	expect_clean();
	finish_case();

	for (failure = 1; failure <= ARRAY_COUNT(recovery_patches); ++failure) {
		reset_case(profile);
		start_case(profile);
		injection_failure_call = (int)failure;
		assert(call_ready() == ready_original_result);
		assert(injection_attempts == failure + 1);
		assert(injection_release_calls == failure);
		for (index = 0; index + 1 < failure; ++index)
			assert(injection_release_order[index] ==
				500 + (SceUID)(failure - 1 - index));
		assert(injection_release_order[failure - 1] == 500);
		expect_clean();
		finish_case();
	}

	reset_case(profile);
	start_case(profile);
	injection_failure_call = 4;
	injection_release_failure_uid = 502;
	assert(call_ready() == RECOVERY_ERROR);
	assert(continue_calls == 0);
	assert(recovery_modid == reported_recovery_modid);
	assert(stop_redirect_id == 500);
	assert(live_injection_count() == 2);
	assert(!recovery_install_complete);
	injection_release_failure_uid = -1;
	assert(call_recovery_stop() == SCE_KERNEL_STOP_SUCCESS);
	finish_case();

	reset_case(profile);
	start_case(profile);
	injection_failure_call = 1;
	injection_release_failure_uid = 500;
	assert(call_ready() == RECOVERY_ERROR);
	assert(continue_calls == 0);
	assert(recovery_modid == reported_recovery_modid);
	assert(stop_redirect_id == 500 && live_injection_count() == 1);
	assert(!recovery_install_complete);
	injection_release_failure_uid = -1;
	assert(call_recovery_stop() == SCE_KERNEL_STOP_SUCCESS);
	finish_case();
	puts("Recovery install failures: redirect, every patch and rollback retention passed");
}

static void install_active_recovery(const RecoveryProfile *profile)
{
	start_case(profile);
	assert(call_ready() == ready_original_result);
	assert(recovery_modid == reported_recovery_modid);
	assert(live_injection_count() ==
		(int)ARRAY_COUNT(recovery_patches) + 1);
	assert(recovery_install_complete);
}

static void test_stop_failures_and_repeat(void)
{
	const RecoveryProfile *profile = &recovery_profiles[0];
	unsigned int attempts;

	reset_case(profile);
	install_active_recovery(profile);
	native_stop_result = -123;
	assert(call_recovery_stop() == -123);
	assert(native_stop_calls == 1);
	finish_case();

	reset_case(profile);
	install_active_recovery(profile);
	attempts = injection_attempts;
	assert(call_ready() == ready_original_result);
	assert(injection_attempts == attempts);
	injection_release_failure_uid = 504;
	assert(call_recovery_stop() == SCE_KERNEL_STOP_CANCEL);
	assert(recovery_modid == reported_recovery_modid);
	assert(stop_redirect_id == 500 && live_injection_count() == 2);
	assert(native_stop_calls == 0);
	injection_release_failure_uid = -1;
	assert(call_recovery_stop() == SCE_KERNEL_STOP_SUCCESS);
	assert(native_stop_calls == 1);
	finish_case();

	reset_case(profile);
	install_active_recovery(profile);
	injection_release_failure_uid = 500;
	assert(call_recovery_stop() == SCE_KERNEL_STOP_CANCEL);
	assert(recovery_modid == reported_recovery_modid);
	assert(stop_redirect_id == 500 && live_injection_count() == 1);
	assert(native_stop_calls == 0);
	injection_release_failure_uid = -1;
	assert(call_recovery_stop() == SCE_KERNEL_STOP_SUCCESS);
	assert(native_stop_calls == 1);
	expect_clean();

	reported_recovery_modid = 72;
	injection_attempts = 0;
	injection_count = 0;
	memset(injections, 0, sizeof(injections));
	injection_release_calls = 0;
	assert(call_ready() == ready_original_result);
	assert(recovery_modid == 72 && live_injection_count() == 8);
	assert(call_recovery_stop() == SCE_KERNEL_STOP_SUCCESS);
	finish_case();
	puts("Recovery stop failures: retry-safe cleanup and repeated module cycles passed");
}

static void test_busy_paths(void)
{
	const RecoveryProfile *profile = &recovery_profiles[0];

	reset_case(profile);
	start_case(profile);
	lifecycle_busy = 1;
	assert(call_ready() == RECOVERY_ERROR);
	assert(continue_calls == 0);
	assert(recovery_modid < 0 && hook_count == 1);
	lifecycle_busy = 0;
	assert(call_ready() == ready_original_result);
	assert(recovery_modid == reported_recovery_modid);
	assert(recovery_stop() < 0);
	assert(recovery_modid == reported_recovery_modid);
	lifecycle_busy = 1;
	assert(call_recovery_stop() == SCE_KERNEL_STOP_CANCEL);
	assert(native_stop_calls == 0);
	lifecycle_busy = 0;
	assert(call_recovery_stop() == SCE_KERNEL_STOP_SUCCESS);
	finish_case();
	puts("Recovery serialization: busy callback and plugin-stop protection passed");

	reset_case(profile);
	start_case(profile);
	recovery_available = 0;
	unload_during_ready = 1;
	assert(call_ready() == ready_original_result);
	assert(ready_in_flight == 0);
	assert(recovery_stop() == 0);
	finish_case();
	puts("Recovery callback lifetime: plugin unload rejected during native fallback and allowed afterward");
}

static void load_exact(const char *path, uint8_t *target, size_t size)
{
	FILE *file = fopen(path, "rb");

	assert(file != NULL);
	assert(fread(target, 1, size, file) == size);
	assert(fgetc(file) == EOF);
	assert(fclose(file) == 0);
}

static void test_firmware(uint32_t shell_nid, const char *shell_path,
	const char *recovery_path)
{
	const RecoveryProfile *profile = NULL;
	uint32_t recovery_nid;
	unsigned int index;

	for (index = 0; index < ARRAY_COUNT(recovery_profiles); ++index) {
		if (recovery_profiles[index].shell_nid == shell_nid) {
			profile = &recovery_profiles[index];
			break;
		}
	}
	assert(profile != NULL);
	reset_case(profile);
	load_exact(shell_path, shell_text, profile->shell_text_size);
	load_exact(recovery_path, recovery_text, RECOVERY_TEXT_SIZE);
	memcpy(&recovery_nid, recovery_text + 0x2C294U + 52, sizeof(recovery_nid));
	assert(recovery_nid == profile->recovery_nid);
	memcpy(recovery_original, recovery_text, RECOVERY_TEXT_SIZE);
	start_case(profile);
	assert(call_ready() == ready_original_result);
	assert(live_injection_count() ==
		(int)ARRAY_COUNT(recovery_patches) + 1);
	assert(call_recovery_stop() == SCE_KERNEL_STOP_SUCCESS);
	finish_case();
	printf("Host recovery lifecycle with firmware bytes passed: shell 0x%08X, %s\n",
		(unsigned int)shell_nid, recovery_path);
}

#if LIVEAREA_DEBUG_LOGGING
static void test_diagnostics(void)
{
	const RecoveryProfile *profile = &recovery_profiles[1];

	reset_case(profile);
	start_case(profile);
	assert(test_debug_capture_contains("[recovery] ready-hook"));
	assert(call_ready() == ready_original_result);
	assert(test_debug_capture_contains("[recovery] module-patched"));
	assert(test_debug_capture_contains("[recovery] ready-callback"));
	assert(test_debug_capture_contains("[recovery] ready-native-enter"));
	assert(test_debug_capture_contains("[recovery] ready-native-return result=77"));
	assert(call_recovery_stop() == SCE_KERNEL_STOP_SUCCESS);
	assert(test_debug_capture_contains("[recovery] module-stop native-return"));
	assert(test_debug_capture_contains("[recovery] stop-redirect-release"));
	finish_case();
	reset_case(profile);
	shell_text[SHELL_RECOVERY_READY_OFFSET] ^= 1;
	assert(recovery_start(42, profile->shell_nid, &shell_info) < 0);
	assert(test_debug_capture_contains("start-rejected reason=ready-bytes"));
	assert(test_debug_capture_contains("ready-actual"));
	assert(test_debug_capture_contains("ready-expected"));
	assert(test_debug_capture_contains("start-failed baseline-capacity-retained"));
	assert(!test_debug_capture_contains("ready-native-enter"));

	reset_case(profile);
	start_case(profile);
	reported_recovery_nid ^= 1;
	assert(call_ready() == ready_original_result);
	assert(test_debug_capture_contains("install-rejected reason=module-identity"));
	assert(test_debug_capture_contains("run_native=1 patched=0"));
	assert(!test_debug_capture_contains("module-patched"));
	finish_case();

	reset_case(profile);
	start_case(profile);
	recovery_text[recovery_patches[2].offset] ^= 1;
	memcpy(recovery_original, recovery_text, RECOVERY_TEXT_SIZE);
	assert(call_ready() == ready_original_result);
	assert(test_debug_capture_contains("validation-failed reason=patch index=2"));
	assert(test_debug_capture_contains("patch-actual"));
	assert(test_debug_capture_contains("patch-expected"));
	finish_case();

	reset_case(profile);
	start_case(profile);
	injection_failure_call = 4;
	injection_release_failure_uid = 502;
	assert(call_ready() < 0);
	assert(test_debug_capture_contains("install-rollback injection_result=-104 cleanup_result=-200"));
	assert(test_debug_capture_contains("run_native=0 patched=0"));
	assert(!test_debug_capture_contains("ready-native-enter"));
	assert(call_recovery_stop() == SCE_KERNEL_STOP_CANCEL);
	assert(test_debug_capture_contains("module-stop cancelled reason=cleanup-failed"));
	finish_case();

	reset_case(profile);
	start_case(profile);
	lifecycle_busy = 1;
	assert(call_ready() < 0);
	assert(test_debug_capture_contains("ready-blocked reason=busy"));
	lifecycle_busy = 0;
	finish_case();
	puts("Recovery diagnostics: success, unpatched fallback, byte mismatch, rollback failure and busy paths passed");
}
#endif

int main(int argc, char **argv)
{
	int argument;

	assert(argc % 3 == 1);
	test_happy_path();
	test_shell_validation();
	test_recovery_validation();
	test_install_failures();
	test_stop_failures_and_repeat();
	test_busy_paths();
#if LIVEAREA_DEBUG_LOGGING
	test_diagnostics();
#endif
	for (argument = 1; argument < argc; argument += 3) {
		test_firmware((uint32_t)strtoul(argv[argument], NULL, 0),
			argv[argument + 1], argv[argument + 2]);
	}
	free(shell_text);
	free(recovery_text);
	free(recovery_original);
	free(recovery_data);
	return 0;
}
