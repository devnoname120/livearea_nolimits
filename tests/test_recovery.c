#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/recovery.c"

uint32_t __stack_chk_guard;

static _Alignas(4) uint8_t text[RECOVERY_TEXT_SIZE];
static uint8_t original[RECOVERY_TEXT_SIZE];
static uint8_t data_segment[RECOVERY_DATA_SIZE];
static uint8_t hook_original[14];
static uint32_t module_nid;
static int module_loaded;
static int module_live;
static int info_failure;
static int bad_segment;
static int export_failure;
static int import_failure;
static int offset_failure;
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
static int allocator_calls;
static int query_error;
static int query_count;
static unsigned int pages_used;
static unsigned int slots[LIVEAREA_PAGE_LIMIT];
static int live_imports[3];
static int live_patches[ARRAY_COUNT(recovery_patches)];
static int live_allocator;
static int database_object;
static const void *import_hooks[3];
static const void *allocator_callback;
static uint8_t context[12 + sizeof(void *)];

static void encode_mov(uint8_t *out, uint16_t opcode, uint16_t immediate)
{
	uint16_t first = opcode | (immediate >> 12) | ((immediate & 0x800U) >> 1);
	uint16_t second = 0x0300U | ((immediate & 0x700U) << 4) | (immediate & 255U);
	out[0] = (uint8_t)first;
	out[1] = (uint8_t)(first >> 8);
	out[2] = (uint8_t)second;
	out[3] = (uint8_t)(second >> 8);
}

static void seed_text(void)
{
	static const uint8_t prologue[] = {
		0x2D, 0xE9, 0xF0, 0x4F, 0x8D, 0xB0,
		0, 0, 0, 0, 0, 0, 0, 0,
		0x1B, 0x68, 0x0B, 0x93, 0x80, 0x46, 0xD8, 0xF8, 0x0C, 0x00,
	};
	uint32_t guard = (uint32_t)(uintptr_t)&__stack_chk_guard;
	unsigned int i;

	memset(text, 0xCD, sizeof(text));
	for (i = 0; i < ARRAY_COUNT(recovery_patches); ++i) {
		const RecoveryPatch *p = &recovery_patches[i];
		memcpy(text + p->offset, p->expected, p->size);
	}
	memcpy(text + RECOVERY_ALLOCATE_OFFSET, prologue, sizeof(prologue));
	encode_mov(text + RECOVERY_ALLOCATE_OFFSET + 6, 0xF240U, (uint16_t)guard);
	encode_mov(text + RECOVERY_ALLOCATE_OFFSET + 10, 0xF2C0U, (uint16_t)(guard >> 16));
	memcpy(original, text, sizeof(text));
}

static void reset_case(uint32_t nid)
{
	unsigned int i;
	const void *database = &database_object;
	assert(!initialized && !module_live && !live_allocator);
	for (i = 0; i < ARRAY_COUNT(live_patches); ++i)
		assert(!live_patches[i]);
	for (i = 0; i < ARRAY_COUNT(live_imports); ++i)
		assert(!live_imports[i]);
	module_nid = nid;
	module_loaded = 1;
	info_failure = bad_segment = export_failure = offset_failure = 0;
	import_failure = injection_failure = release_failure = -1;
	start_error = stop_error = unload_error = 0;
	start_status = stop_status = 0;
	recurse_start = 0;
	import_calls = injection_calls = start_calls = stop_calls = unload_calls = 0;
	allocator_calls = query_error = query_count = 0;
	pages_used = 1;
	memset(slots, 0, sizeof(slots));
	memcpy(context + 12, &database, sizeof(database));
	seed_text();
}

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

static int count_icons(const void *database, int first, unsigned int count)
{
	assert(database == &database_object && first == 0 && count == LIVEAREA_PAGE_LIMIT);
	return query_error ? query_error : query_count;
}

int taiGetModuleExportFunc(const char *name, uint32_t library, uint32_t nid,
	uintptr_t *function)
{
	assert(strcmp(name, "SceLsdb") == 0 && library == TAI_ANY_LIBRARY);
	assert(nid == ICON_COUNT_NID);
	if (export_failure == 1)
		return -1;
	*function = export_failure == 2 ? 0 : (uintptr_t)count_icons;
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

SceUID taiHookFunctionOffset(tai_hook_ref_t *ref, SceUID modid, int segment,
	uint32_t offset, int thumb, const void *hook)
{
	assert(module_loaded && !module_live && !live_allocator);
	assert(modid == 71 && segment == 0 && offset == RECOVERY_ALLOCATE_OFFSET && thumb == 1);
	if (offset_failure)
		return -1;
	memcpy(hook_original, text + offset, sizeof(hook_original));
	memset(text + offset, 0xAA, sizeof(hook_original));
	live_allocator = 1;
	allocator_callback = hook;
	*ref = 100;
	return 100;
}

SceUID taiInjectData(SceUID modid, int segment, uint32_t offset,
	const void *bytes, SceSize size)
{
	int i = injection_calls++;
	const RecoveryPatch *p = &recovery_patches[i];
	assert(module_loaded && !module_live && live_allocator);
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
	assert(module_loaded && !module_live && !live_allocator);
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
	if (uid == 100) {
		assert(module_loaded && !module_live && live_allocator);
		memcpy(text + RECOVERY_ALLOCATE_OFFSET, hook_original, sizeof(hook_original));
		live_allocator = 0;
	} else {
		assert(uid >= 10 && uid < 13 && live_imports[uid - 10]);
		live_imports[uid - 10] = 0;
	}
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

static int native_allocate(void *state, int *page, int *position)
{
	unsigned int p;
	unsigned int page_limit = (unsigned int)patch_value(5, 10, LIVEAREA_PAGE_LIMIT);
	assert(state == context);
	++allocator_calls;
	for (p = 0; p < pages_used; ++p) {
		if (slots[p] < LIVEAREA_ICONS_PER_PAGE) {
			*page = (int)p;
			*position = (int)slots[p];
			return 0;
		}
	}
	if (pages_used < page_limit) {
		*page = (int)pages_used++;
		*position = 0;
	} else {
		*page = RECOVERY_HIDDEN_PAGE;
		*position = 0;
	}
	return 0;
}

int test_tai_continue(tai_hook_ref_t ref, ...)
{
	va_list ap;
	int result;
	va_start(ap, ref);
	if (ref == 100) {
		void *state = va_arg(ap, void *);
		int *page = va_arg(ap, int *);
		int *position = va_arg(ap, int *);
		result = native_allocate(state, page, position);
	} else if (ref == 12) {
		SceUID modid = va_arg(ap, SceUID);
		(void)va_arg(ap, int);
		(void)va_arg(ap, const void *);
		++unload_calls;
		if (modid == 71) {
			assert(!live_allocator && !module_live);
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

static void test_recovery_model(void)
{
	int displayed = 500, hidden = 2, restored = 0, budget, page, position;
	unsigned int i;
	reset_case(0xC1F30F67U);
	query_count = 100;
	pages_used = 10;
	for (i = 0; i < pages_used; ++i)
		slots[i] = 10;
	budget = patch_value(2, 500, LIVEAREA_ICON_LIMIT) - displayed;
	assert(budget == 0 && hidden == 2);
	assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
	assert(allocator_callback == (const void *)allocate_page_hook);
	for (i = 2; i < 5; ++i)
		assert(patch_value(i, 500, LIVEAREA_ICON_LIMIT) - displayed == 500);
	budget = hidden;
	while (budget-- && displayed < patch_value(0, 500, LIVEAREA_ICON_LIMIT)) {
		assert(allocate_page_hook(context, &page, &position) == 0);
		assert(page == 10 && position == restored);
		++slots[page];
		++query_count;
		++displayed;
		--hidden;
		++restored;
	}
	assert(displayed == 502 && hidden == 0 && restored == 2);
	for (i = 0; i < 10; ++i)
		assert(slots[i] == 10);
	assert(call_stop() == 0);
	injection_calls = 0;
	assert(call_start(71) == 0 && hidden == 0 && query_count == 102);
	assert(patch_value(0, 500, LIVEAREA_ICON_LIMIT) > 501);
	assert(patch_value(1, 500, LIVEAREA_ICON_LIMIT) == LIVEAREA_ICON_LIMIT);
	assert(patch_value(0, 500, LIVEAREA_ICON_LIMIT) == 1000);
	assert(1000 >= patch_value(0, 500, LIVEAREA_ICON_LIMIT));
	query_count = LIVEAREA_TOP_LEVEL_LIMIT;
	i = (unsigned int)allocator_calls;
	assert(allocate_page_hook(context, &page, &position) == 0);
	assert(page == RECOVERY_HIDDEN_PAGE && allocator_calls == (int)i);
	query_error = -87;
	page = position = -6;
	assert(allocate_page_hook(context, &page, &position) == -87);
	assert(page == -6 && position == -6 && allocator_calls == (int)i);
	query_error = 0;
	query_count = LIVEAREA_TOP_LEVEL_LIMIT - 1;
	pages_used = LIVEAREA_PAGE_LIMIT;
	for (i = 0; i < pages_used; ++i)
		slots[i] = 10;
	slots[LIVEAREA_PAGE_LIMIT - 1] = 9;
	assert(allocate_page_hook(context, &page, &position) == 0);
	assert(page == LIVEAREA_PAGE_LIMIT - 1 && position == 9);
	query_count = LIVEAREA_TOP_LEVEL_LIMIT;
	slots[LIVEAREA_PAGE_LIMIT - 1] = 10;
	assert(allocate_page_hook(context, &page, &position) == 0 && page == RECOVERY_HIDDEN_PAGE);
	finish_case();
	puts("Recovery model: 500 visible + 2 hidden, existing full pages preserved, repeat boot, total and top-level limits passed");
}

static void test_wide_top_level_recovery(void)
{
	int page, position;
	unsigned int saved_slots[50];
	assert(LIVEAREA_TOP_LEVEL_LIMIT == 500 && LIVEAREA_PAGE_LIMIT == 50);
	reset_case(0xC1F30F67U);
	assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
	pages_used = 26;
	query_count = 255;
	for (unsigned int i = 0; i < 25; ++i)
		slots[i] = 10;
	slots[25] = 5;
	for (int count = 255; count < 500; ++count) {
		assert(query_count == count);
		assert(allocate_page_hook(context, &page, &position) == 0);
		assert(page == count / 10 && position == count % 10);
		++slots[page];
		++query_count;
	}
	assert(pages_used == 50);
	for (unsigned int i = 0; i < 50; ++i)
		assert(slots[i] == 10);
	assert(allocate_page_hook(context, &page, &position) == 0);
	assert(page == RECOVERY_HIDDEN_PAGE);
	memcpy(saved_slots, slots, sizeof(slots));
	assert(call_stop() == 0);
	injection_calls = 0;
	assert(call_start(71) == 0);
	assert(allocate_page_hook(context, &page, &position) == 0);
	assert(page == RECOVERY_HIDDEN_PAGE && memcmp(saved_slots, slots, sizeof(slots)) == 0);
	--query_count;
	--slots[49];
	assert(allocate_page_hook(context, &page, &position) == 0 && page == 49 && position == 9);
	finish_case();
	puts("Recovery top-level boundary: 255 through 500, page 49, full rejection, reboot and freed-slot reuse passed");
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
		assert(call_start(71) == 0 && live_allocator && injection_calls == 7);
		assert(recovery_stop() < 0 && live_allocator);
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
			assert(!live_allocator && injection_calls == 0);
			finish_case();
		}
	}
	for (i = 0; i < 24; ++i) {
		if (i >= 6 && i < 14)
			continue;
		reset_case(0xC1F30F67U);
		text[RECOVERY_ALLOCATE_OFFSET + i] ^= 1;
		memcpy(original, text, sizeof(text));
		assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
		assert(!live_allocator && injection_calls == 0);
		finish_case();
	}
	for (i = 1; i <= 4; ++i) {
		reset_case(0xC1F30F67U);
		bad_segment = (int)i;
		assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
		assert(!live_allocator && injection_calls == 0);
		finish_case();
	}
	for (i = 0; i < 3; ++i) {
		reset_case(i == 0 ? 0xDEADBEEFU : 0xC1F30F67U);
		info_failure = (int)i;
		assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
		assert(!live_allocator && injection_calls == 0);
		info_failure = 0;
		finish_case();
	}
	for (i = 1; i <= 2; ++i) {
		reset_case(0xC1F30F67U);
		export_failure = (int)i;
		assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
		assert(!live_allocator && injection_calls == 0);
		finish_case();
	}
	for (i = 0; i < 8; ++i) {
		for (j = 0; j < 8; ++j) {
			reset_case(0xC1F30F67U);
			text[RECOVERY_ALLOCATE_OFFSET + 6 + i] ^= (uint8_t)(1U << j);
			memcpy(original, text, sizeof(text));
			assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
			assert(!live_allocator && injection_calls == 0);
			finish_case();
		}
	}
	for (i = 0; i < 4; ++i) {
		uint32_t guard;
		reset_case(0xC1F30F67U);
		guard = i == 0 ? (uint32_t)(uintptr_t)text + 0x2DA40U :
			i == 1 ? (uint32_t)(uintptr_t)text + 0x2DA58U :
			(uint32_t)(uintptr_t)&__stack_chk_guard + (i == 2 ? 4U : 2U);
		encode_mov(text + RECOVERY_ALLOCATE_OFFSET + 6, 0xF240U, (uint16_t)guard);
		encode_mov(text + RECOVERY_ALLOCATE_OFFSET + 10, 0xF2C0U, (uint16_t)(guard >> 16));
		memcpy(original, text, sizeof(text));
		assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
		assert(!live_allocator && injection_calls == 0);
		finish_case();
	}
	reset_case(0xC1F30F67U);
	assert(recovery_start(0x5549BF1FU) == 0 && call_start(71) == 0);
	assert(!live_allocator && injection_calls == 0);
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
		assert(!live_allocator && injection_calls == (int)i + 1);
		finish_case();
	}
	reset_case(0xC1F30F67U);
	offset_failure = 1;
	assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
	assert(injection_calls == 0);
	finish_case();
	reset_case(0xC1F30F67U);
	assert(recovery_start(0x0552F692U) == 0);
	start_error = -55;
	assert(call_start(71) == -55 && !live_allocator);
	start_error = 0;
	start_status = SCE_KERNEL_START_FAILED;
	injection_calls = 0;
	assert(call_start(71) == 0 && !live_allocator);
	start_status = 0;
	injection_calls = 0;
	recurse_start = 1;
	assert(call_start(71) == 0 && live_allocator);
	start_error = -66;
	assert(call_start(71) == -66 && live_allocator && injection_calls == 7);
	start_error = 0;
	stop_error = -77;
	assert(call_stop() == -77 && live_allocator);
	stop_error = 0;
	stop_status = 1;
	assert(call_stop() == 0 && live_allocator && recovery_running);
	stop_status = 0;
	assert(call_stop() == 0 && !live_allocator);
	unload_error = -88;
	assert(unload_module_hook(71, 0, NULL) == -88 && module_loaded);
	unload_error = 0;
	injection_calls = 0;
	assert(call_start(71) == 0 && live_allocator);
	finish_case();
	for (i = 0; i < ARRAY_COUNT(recovery_patches) + 1; ++i) {
		reset_case(0xC1F30F67U);
		assert(recovery_start(0x0552F692U) == 0 && call_start(71) == 0);
		release_failure = i == ARRAY_COUNT(recovery_patches) ? 100 : 200 + (int)i;
		assert(call_stop() < 0 && recovery_modid == 71 && !module_live);
		assert(recovery_stop() < 0);
		assert(unload_module_hook(71, 0, NULL) < 0 && unload_calls == 0);
		release_failure = -1;
		assert(unload_module_hook(71, 0, NULL) == 0 && !module_loaded);
		finish_case();
	}
	reset_case(0xC1F30F67U);
	assert(recovery_start(0x0552F692U) == 0);
	assert(call_start(99) == 0 && !live_allocator && injection_calls == 0);
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
	uint32_t low, high, guard, relative, nid;
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
	assert(read_mov_imm(text + RECOVERY_ALLOCATE_OFFSET + 6, 0xF240U, &low) == 0);
	assert(read_mov_imm(text + RECOVERY_ALLOCATE_OFFSET + 10, 0xF2C0U, &high) == 0);
	guard = low | high << 16;
	relative = guard - 0x81000000U;
	assert(relative <= sizeof(text) - 4 && (relative & 3U) == 0);
	assert(relative == 0x2DA40U);
	guard = (uint32_t)(uintptr_t)&__stack_chk_guard;
	encode_mov(text + RECOVERY_ALLOCATE_OFFSET + 6, 0xF240U, (uint16_t)guard);
	encode_mov(text + RECOVERY_ALLOCATE_OFFSET + 10, 0xF2C0U, (uint16_t)(guard >> 16));
	memcpy(original, text, sizeof(text));
	assert(recovery_start(shell_nid) == 0 && call_start(71) == 0);
	assert(injection_calls == 7 && live_allocator);
	finish_case();
	printf("Real recovery bytes: identity, seven patches, relocated prologue and rollback passed: %s\n", path);
}

static void test_imported_stack_guard(void)
{
	uint32_t guard = (uint32_t)(uintptr_t)&__stack_chk_guard;
	assert((uint32_t)(guard - (uint32_t)(uintptr_t)text) >= sizeof(text));
	reset_case(0x3F76E38FU);
	encode_mov(text + RECOVERY_ALLOCATE_OFFSET + 6, 0xF240U, (uint16_t)guard);
	encode_mov(text + RECOVERY_ALLOCATE_OFFSET + 10, 0xF2C0U, (uint16_t)(guard >> 16));
	memcpy(original, text, sizeof(text));
	assert(recovery_start(0x5549BF1FU) == 0 && call_start(71) == 0);
	assert(live_allocator && injection_calls == 7);
	finish_case();
}

int main(int argc, char **argv)
{
	int arg;
	assert(argc % 2 == 1);
	test_imported_stack_guard();
	test_recovery_model();
	test_wide_top_level_recovery();
	test_profiles_and_validation();
	test_failures_and_lifecycle();
	for (arg = 1; arg < argc; arg += 2)
		test_firmware((uint32_t)strtoul(argv[arg], NULL, 0), argv[arg + 1]);
	return 0;
}
