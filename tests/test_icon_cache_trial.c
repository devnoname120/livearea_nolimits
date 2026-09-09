#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>

#ifndef LIVEAREA_ICON_CACHE_LOGGING
#define LIVEAREA_ICON_CACHE_LOGGING 0
#endif

#include "../src/icon_cache_trial.c"

#if LIVEAREA_DEBUG_LOGGING
#include "debug_capture.h"
#endif

uint32_t __stack_chk_guard;

static const struct TestCacheProfile {
	uint32_t shell_nid, paf_nid;
	uint32_t text_size, data_size, text_base, data_base;
	uint32_t pool_slot, init_offset;
	uint32_t paf_text_base, paf_data_base;
} test_profiles[] = {
	{0x0552F692, 0xCD679177, 0x541B74, 0x93FAC, 0x81000000, 0x81542000,
		0x6DEC, 0x2C74, 0x81000000, 0x81301000},
	{0x5549BF1F, 0x73F90499, 0x5420F4, 0x93FBC, 0x81000000, 0x81543000,
		0x6DFC, 0x2CCC, 0x81000000, 0x81301000},
	{0xEAB89D5C, 0xCD679177, 0x535CF4, 0x92D1C, 0x83200DC0, 0x83780FB0,
		0x6BBC, 0x2C74, 0x83200E90, 0x811607E0},
};
static const struct TestCacheProfile *test_profile = &test_profiles[0];

static uint8_t *g_paf_text;
static uint8_t *g_paf_data;
static SceSize g_paf_data_size = PAF_MUTEX_OFFSET + 32;
static uint32_t g_paf_nid = PAF_360_NID;
static SceSize g_paf_size = PAF_TEXT_SIZE;
static int g_query_error;
static int g_hook_failure;
static int g_hook_calls;
static int g_hook_releases;
static int g_surface_releases;
static int g_original_calls;
static int g_log_writes;
static int g_log_open;
static int g_log_failure;
static char g_log_contents[4096];
static size_t g_log_size;
static void *g_expected_cache;
static void **g_expected_out;
static int (*g_expected_predicate)(void *);
static void *g_scan_item;
static int g_scan_result;
static PafImage *g_multiple_items;
static size_t g_multiple_count;
static int g_init_hook_failure;
static int g_init_hook_releases;
static int g_original_init_calls;
static int g_paf_queries;
static IconPool *g_initialized_pool;
static unsigned int g_mutex_depth;
static int g_mutex_failure;
static int g_scan_failure;
static int g_nested_scan;
static int (*g_consumer_continue)(void *, void **, int, int);
static int g_consumer_hook_failure;
static int g_consumer_hook_calls;
static int g_consumer_hook_releases;
static void (*g_cache_unlocked)(void);

static void check_log(const char *message)
{
#if LIVEAREA_ICON_CACHE_LOGGING
	assert(strstr(g_log_contents, message));
#else
	(void)message;
	assert(!g_log_open && !g_log_writes && !g_log_size && !g_log_contents[0]);
#endif
}

static int lock_cache(void *mutex)
{
	assert(mutex == g_paf_data + PAF_MUTEX_OFFSET);
	if (g_mutex_failure)
		return -1;
	++g_mutex_depth;
	return 0;
}

static int unlock_cache(void *mutex)
{
	assert(mutex == g_paf_data + PAF_MUTEX_OFFSET && g_mutex_depth);
	--g_mutex_depth;
	if (!g_mutex_depth && g_cache_unlocked) {
		void (*callback)(void) = g_cache_unlocked;
		g_cache_unlocked = NULL;
		callback();
	}
	return 0;
}

static void relocate_mov_half(uint8_t *code, uint16_t opcode,
	unsigned int reg, uint16_t value)
{
	uint16_t first = (uint16_t)(opcode | (value >> 12) | ((value >> 1) & 0x0400U));
	uint16_t second = (uint16_t)(((value << 4) & 0x7000U) | (reg << 8) | (value & 0xFFU));
	code[0] = (uint8_t)first;
	code[1] = (uint8_t)(first >> 8);
	code[2] = (uint8_t)second;
	code[3] = (uint8_t)(second >> 8);
}

int taiGetModuleInfo(const char *name, tai_module_info_t *info)
{
	assert(strcmp(name, "ScePaf") == 0 && info->size == sizeof(*info));
	++g_paf_queries;
	info->modid = 77;
	info->module_nid = g_paf_nid;
	return g_query_error == 1 ? -1 : 0;
}

int sceKernelGetModuleInfo(SceUID modid, SceKernelModuleInfo *info)
{
	assert(modid == 77 && info->size == sizeof(*info));
	info->segments[0].vaddr = g_paf_text;
	info->segments[0].memsz = g_paf_size;
	info->segments[1].vaddr = g_paf_data;
	info->segments[1].memsz = g_paf_data_size;
	return g_query_error == 2 ? -1 : 0;
}

SceUID taiHookFunctionOffset(tai_hook_ref_t *ref, SceUID modid, int segment,
	uint32_t offset, int thumb, const void *hook)
{
	assert(segment == 0 && thumb == 1);
	if (modid == 42) {
		assert(offset == test_profile->init_offset && hook == initialize_icon_pool);
		*ref = 124;
		return g_init_hook_failure ? -1 : 457;
	}
	assert(modid == 77);
	if (offset == PAF_APPLY_OFFSET) {
		assert(hook == apply_icon_image && g_scan_hook < 0);
		++g_consumer_hook_calls;
		*ref = 125;
		return g_consumer_hook_failure ? -1 : 458;
	}
	++g_hook_calls;
	*ref = 123;
	if (offset == 0x013EF6U)
		return -2;
	assert(offset == PAF_SCAN_OFFSET && thumb == 1 && hook == scan_icon_surfaces);
	return g_hook_failure ? -1 : 456;
}

int taiHookRelease(SceUID uid, tai_hook_ref_t ref)
{
	if (uid == 458) {
		assert(ref == 125 && g_scan_hook < 0);
		++g_consumer_hook_releases;
		return 0;
	}
	if (uid == 457) {
		assert(ref == 124 && g_scan_hook < 0 && g_apply_hook < 0);
		++g_init_hook_releases;
		return 0;
	}
	assert(uid == 456 && ref == 123);
	++g_hook_releases;
	return 0;
}

static int original_evict(void *image)
{
	(void)image;
	++g_original_calls;
	return 7; /* Distinguishes delegation from a trial eviction. */
}

static int unrelated_predicate(void *image)
{
	(void)image;
	return 0;
}

int test_tai_continue(tai_hook_ref_t ref, ...)
{
	if (ref == 125) {
		va_list args;
		va_start(args, ref);
		void *widget = va_arg(args, void *);
		void **handle = va_arg(args, void **);
		int object = va_arg(args, int);
		int texture = va_arg(args, int);
		va_end(args);
		assert(g_consumer_continue);
		return g_consumer_continue(widget, handle, object, texture);
	}
	if (ref == 124) {
		++g_original_init_calls;
		*g_icon_pool_slot = g_initialized_pool;
		return 0;
	}
	assert(ref == 123);
	va_list args;
	va_start(args, ref);
	void *cache = va_arg(args, void *);
	int (*predicate)(void *) = va_arg(args, int (*)(void *));
	void **out_item = va_arg(args, void **);
	va_end(args);
	if (g_multiple_items) {
		assert(g_mutex_depth >= 1 && predicate == select_icon_surface);
		assert(cache == g_multiple_items[0].cache);
		if (g_scan_failure)
			return -1;
		lock_cache(g_cache_mutex);
		if (out_item)
			*out_item = NULL;
		int found = 0;
		for (size_t i = 0; i < g_multiple_count; ++i) {
			int before = g_surface_releases;
			if (predicate(&g_multiple_items[i])) {
				found = 1;
				if (out_item)
					*out_item = &g_multiple_items[i];
			}
			assert(g_surface_releases == before);
			if (g_nested_scan) {
				g_nested_scan = 0;
				IconSelection *outer = g_selection;
				size_t count = g_multiple_count;
				g_multiple_count = 0;
				assert(scan_icon_surfaces(cache, g_stock_evict, NULL) == 0);
				assert(g_selection == outer);
				g_multiple_count = count;
			}
		}
		unlock_cache(g_cache_mutex);
		return g_multiple_count ? found : 1;
	}
	assert(cache == g_expected_cache && predicate == g_expected_predicate);
	assert(out_item == g_expected_out);
	if (g_scan_item)
		assert(predicate(g_scan_item) == 1);
	if (out_item)
		*out_item = g_scan_item;
	return g_scan_result;
}

SceUID sceIoOpen(const char *path, int flags, int mode)
{
	assert(LIVEAREA_ICON_CACHE_LOGGING && "logless builds must not open files");
	assert(strstr(path, "icon-cache-trial.log"));
	assert(flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC) && mode == 0666);
	assert(!g_log_open);
	g_log_size = 0;
	g_log_contents[0] = '\0';
	if (g_log_failure)
		return -1;
	g_log_open = 1;
	return 8;
}

int sceClibSnprintf(char *dst, SceSize size, const char *format, ...)
{
	assert(LIVEAREA_ICON_CACHE_LOGGING && "logless builds must not format diagnostics");
	va_list args;
	va_start(args, format);
	int length = vsnprintf(dst, size, format, args);
	va_end(args);
	return length;
}

int sceIoWrite(SceUID fd, const void *data, SceSize size)
{
	assert(LIVEAREA_ICON_CACHE_LOGGING && "logless builds must not write files");
	assert(fd == 8 && g_log_open && data && size);
	assert(!g_mutex_depth);
	assert(g_log_size + size < sizeof(g_log_contents));
	memcpy(g_log_contents + g_log_size, data, size);
	g_log_size += size;
	g_log_contents[g_log_size] = '\0';
	++g_log_writes;
	return (int)size;
}

int sceIoClose(SceUID fd)
{
	assert(LIVEAREA_ICON_CACHE_LOGGING && "logless builds must not close files");
	assert(fd == 8 && g_log_open);
	g_log_open = 0;
	return 0;
}

static unsigned int release_surface(PafSurface *surface)
{
	assert(surface && surface->references == 1);
	assert(g_mutex_depth >= 1);
	--surface->references;
	++g_surface_releases;
	return 0;
}

static void rejected(uint32_t shell_nid, const SceKernelModuleInfo *shell)
{
	int before = g_hook_calls;
	assert(icon_cache_trial_start(42, shell_nid, shell) < 0);
	assert(g_hook_calls == before && g_scan_hook == -1 && !g_log_open);
	assert(g_pool_init_hook == -1);
	assert(g_apply_hook == -1 && !g_image_handle_vtable && !g_get_surface);
	assert(!g_icon_pool_slot && !g_release_surface && !g_stock_evict);
	assert(!g_profile);
	check_log("failed: 0x");
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--profiles") == 0) {
		putchar('[');
		for (size_t i = 0; i < sizeof(cache_profiles) / sizeof(cache_profiles[0]); ++i)
			printf("%s%u", i ? "," : "", cache_profiles[i].shell_nid);
		puts("]");
		return 0;
	}
	int arg = 1;
	if (arg < argc && strncmp(argv[arg], "0x", 2) == 0) {
		uint32_t nid = (uint32_t)strtoul(argv[arg++], NULL, 0);
		unsigned int index;
		for (index = 0; index < sizeof(test_profiles) / sizeof(test_profiles[0]); ++index)
			if (test_profiles[index].shell_nid == nid)
				break;
		assert(index < sizeof(test_profiles) / sizeof(test_profiles[0]));
		test_profile = &test_profiles[index];
	}
	const char *paf_path = arg < argc ? argv[arg++] : NULL;
	const char *shell_path = arg < argc ? argv[arg++] : NULL;
	assert(arg == argc);
	g_paf_nid = test_profile->paf_nid;
	SceKernelModuleInfo shell = {0};
	uint8_t *shell_data = calloc(1, test_profile->data_size + sizeof(void *));
	uint8_t *shell_text = calloc(1, test_profile->text_size);
	IconPool pool = {0};
	PafCache cache = {0};
	PafSurface surface = {0};
	PafImage image = {0};
	PafImage before;
	void *selected = NULL;
	size_t i;
	int releases;
	assert(shell_data && shell_text);
	g_paf_text = calloc(1, PAF_TEXT_SIZE);
	g_paf_data = calloc(1, g_paf_data_size);
	assert(g_paf_text && g_paf_data);
	if (paf_path) {
		FILE *file = fopen(paf_path, "rb");
		assert(file && fread(g_paf_text, 1, PAF_TEXT_SIZE, file) == PAF_TEXT_SIZE);
		assert(fgetc(file) == EOF);
		fclose(file);
		uint32_t input_nid;
		memcpy(&input_nid, g_paf_text + 0x262AF8 + 52, sizeof(input_nid));
		assert(input_nid == test_profile->paf_nid);
	} else {
		memcpy(g_paf_text + PAF_SCAN_OFFSET, expected_scan_entry, sizeof(expected_scan_entry));
		memcpy(g_paf_text + PAF_EVICT_OFFSET, expected_evict, sizeof(expected_evict));
		memcpy(g_paf_text + PAF_RELEASE_OFFSET, expected_release, sizeof(expected_release));
		memcpy(g_paf_text + PAF_LOCK_OFFSET, expected_mutex_functions, sizeof(expected_mutex_functions));
		memcpy(g_paf_text + PAF_TIMESTAMP_GET_OFFSET, expected_timestamp_getter,
			sizeof(expected_timestamp_getter));
		memcpy(g_paf_text + PAF_APPLY_OFFSET, expected_apply_prefix, sizeof(expected_apply_prefix));
		memcpy(g_paf_text + PAF_APPLY_OFFSET + 14, expected_apply_suffix, sizeof(expected_apply_suffix));
		memcpy(g_paf_text + PAF_GET_SURFACE_OFFSET, expected_get_surface, sizeof(expected_get_surface));
		memcpy(g_paf_text + PAF_HANDLE_GET_OFFSET, expected_handle_get_prefix, sizeof(expected_handle_get_prefix));
		memcpy(g_paf_text + PAF_HANDLE_GET_OFFSET + 12, expected_handle_get_suffix, sizeof(expected_handle_get_suffix));
	}
	const uint8_t reference_mutex[] = {0x4B,0xF6,0x18,0x25,0x02,0x94,0xC8,0xF2,0x16,0x15};
	const uint8_t reference_clock[] = {0x4B,0xF6,0x04,0x21,0xC8,0xF2,0x16,0x11};
	assert(matches_mov_address(reference_mutex, reference_mutex + 6, 5, 0x8116BA18U));
	assert(matches_mov_address(reference_clock, reference_clock + 4, 1, 0x8116BA04U));
	uint32_t data_address = (uint32_t)(uintptr_t)g_paf_data;
	uint8_t *mutex_load = g_paf_text + PAF_MUTEX_LOAD_OFFSET;
	uint8_t *clock_load = g_paf_text + PAF_CLOCK_LOAD_OFFSET;
	/* Only absolute-address immediates differ in the host's emulated relocation. */
	if (paf_path) {
		assert(matches_mov_address(mutex_load, mutex_load + 6, 5,
			test_profile->paf_data_base + PAF_MUTEX_OFFSET));
		assert(matches_mov_address(clock_load, clock_load + 4, 1,
			test_profile->paf_data_base + PAF_CLOCK_OFFSET));
		const uint8_t *get = g_paf_text + PAF_GET_SURFACE_OFFSET;
		const uint8_t *thunk = g_paf_text + PAF_HANDLE_GET_OFFSET;
		assert(matches_mov_address(get + 2, get + 6, 6,
			test_profile->paf_data_base + PAF_MUTEX_OFFSET));
		assert(matches_mov_address(thunk + 4, thunk + 8, 12,
			test_profile->paf_text_base + PAF_GET_SURFACE_OFFSET + 1));
		uint32_t getter;
		memcpy(&getter, g_paf_text + PAF_HANDLE_VTABLE_OFFSET + 8, sizeof(getter));
		assert(getter == test_profile->paf_text_base + PAF_HANDLE_GET_OFFSET + 1);
		assert(((mutex_load[0] | mutex_load[1] << 8) & 0xFBF0U) == 0xF240U);
		assert(((mutex_load[2] | mutex_load[3] << 8) & 0x8F00U) == 0x0500U);
		assert(((mutex_load[6] | mutex_load[7] << 8) & 0xFBF0U) == 0xF2C0U);
		assert(((mutex_load[8] | mutex_load[9] << 8) & 0x8F00U) == 0x0500U);
		assert(((clock_load[0] | clock_load[1] << 8) & 0xFBF0U) == 0xF240U);
		assert(((clock_load[2] | clock_load[3] << 8) & 0x8F00U) == 0x0100U);
		assert(((clock_load[4] | clock_load[5] << 8) & 0xFBF0U) == 0xF2C0U);
		assert(((clock_load[6] | clock_load[7] << 8) & 0x8F00U) == 0x0100U);
	}
	relocate_mov_half(mutex_load, 0xF240U, 5, (uint16_t)(data_address + PAF_MUTEX_OFFSET));
	relocate_mov_half(mutex_load + 6, 0xF2C0U, 5, (uint16_t)((data_address + PAF_MUTEX_OFFSET) >> 16));
	relocate_mov_half(clock_load, 0xF240U, 1, (uint16_t)(data_address + PAF_CLOCK_OFFSET));
	relocate_mov_half(clock_load + 4, 0xF2C0U, 1, (uint16_t)((data_address + PAF_CLOCK_OFFSET) >> 16));
	uint32_t text_address = (uint32_t)(uintptr_t)g_paf_text;
	uint32_t guard_address = (uint32_t)(uintptr_t)&__stack_chk_guard;
	assert(guard_address != text_address + 0x26FCDCU);
	const uint8_t captured_guard_load[] = {0x4E,0xF6,0xF4,0x14,0xCE,0xF2,0x01,0x04};
	assert(matches_mov_address(captured_guard_load, captured_guard_load + 4, 4, 0xE001E9F4U));
	assert(!matches_mov_address(captured_guard_load, captured_guard_load + 4, 4,
		0xE020B300U + 0x26FCDCU));
	const struct {
		uint32_t offset;
		unsigned int reg;
		uint32_t address;
	} consumer_relocations[] = {
		{PAF_APPLY_OFFSET + 6, 4, guard_address},
		{PAF_GET_SURFACE_OFFSET + 2, 6, data_address + PAF_MUTEX_OFFSET},
		{PAF_HANDLE_GET_OFFSET + 4, 12, text_address + PAF_GET_SURFACE_OFFSET + 1}
	};
	for (i = 0; i < sizeof(consumer_relocations) / sizeof(consumer_relocations[0]); ++i) {
		uint8_t *code = g_paf_text + consumer_relocations[i].offset;
		unsigned int reg = consumer_relocations[i].reg;
		uint32_t address = consumer_relocations[i].address;
		if (paf_path) {
			assert(((code[0] | code[1] << 8) & 0xFBF0U) == 0xF240U);
			assert(((code[2] | code[3] << 8) & 0x8F00U) == reg << 8);
			assert(((code[4] | code[5] << 8) & 0xFBF0U) == 0xF2C0U);
			assert(((code[6] | code[7] << 8) & 0x8F00U) == reg << 8);
		}
		relocate_mov_half(code, 0xF240U, reg, (uint16_t)address);
		relocate_mov_half(code + 4, 0xF2C0U, reg, (uint16_t)(address >> 16));
	}
	uint32_t handle_get = text_address + PAF_HANDLE_GET_OFFSET + 1;
	for (i = 0; i < 4; ++i)
		g_paf_text[PAF_HANDLE_VTABLE_OFFSET + 8 + i] = (uint8_t)(handle_get >> (8 * i));
	assert(valid_consumer_code(g_paf_text, data_address));
	uint8_t *guard_load = g_paf_text + PAF_APPLY_OFFSET + 6;
	relocate_mov_half(guard_load, 0xF240U, 4, (uint16_t)(text_address + 0x26FCDCU));
	relocate_mov_half(guard_load + 4, 0xF2C0U, 4, (uint16_t)((text_address + 0x26FCDCU) >> 16));
	assert(!valid_consumer_code(g_paf_text, data_address));
	relocate_mov_half(guard_load, 0xF240U, 4, (uint16_t)guard_address);
	relocate_mov_half(guard_load + 4, 0xF2C0U, 4, (uint16_t)(guard_address >> 16));
	/* Keep the emulated pointer slot aligned on both 32- and 64-bit hosts. */
	shell.segments[1].vaddr = shell_data +
		((sizeof(void *) - test_profile->pool_slot % sizeof(void *)) % sizeof(void *));
	shell.segments[1].memsz = test_profile->data_size;
	shell.segments[0].vaddr = shell_text;
	shell.segments[0].memsz = test_profile->text_size;
	uint8_t *init_entry = shell_text + test_profile->init_offset;
	const uint8_t entry[] = {0x2D,0xE9,0xF0,0x41,0x8A,0xB0,0x47,0xF2,0xCC,0x58};
	const uint8_t checked_bits[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xF0,0xFB,0,0x8F};
	if (shell_path) {
		FILE *file = fopen(shell_path, "rb");
		assert(file && fread(shell_text, 1, test_profile->text_size, file) == test_profile->text_size);
		assert(fgetc(file) == EOF);
		fclose(file);
		assert(matches_mov_address(init_entry + 0x1E, init_entry + 0x22, 0,
			test_profile->data_base + test_profile->pool_slot));
	} else {
		memcpy(init_entry, entry, sizeof(entry));
		init_entry[0x26] = 0x06;
		init_entry[0x27] = 0x60;
	}
	uint32_t pool_address = (uint32_t)(uintptr_t)shell.segments[1].vaddr + test_profile->pool_slot;
	relocate_mov_half(init_entry + 0x1E, 0xF240U, 0, (uint16_t)pool_address);
	relocate_mov_half(init_entry + 0x22, 0xF2C0U, 0, (uint16_t)(pool_address >> 16));
	for (i = 0; i < sizeof(entry); ++i) {
		for (unsigned int bit = 0; bit < 8; ++bit) {
			init_entry[i] ^= 1U << bit;
			assert(valid_pool_init_entry(init_entry) == !(checked_bits[i] & (1U << bit)));
			init_entry[i] ^= 1U << bit;
		}
	}
	pool.surface_pool = &pool;
	g_initialized_pool = &pool;
	cache.surface_pool = &pool;
	*(IconPool **)((uint8_t *)shell.segments[1].vaddr + test_profile->pool_slot) = &pool;

	g_query_error = 1;
	*(IconPool **)((uint8_t *)shell.segments[1].vaddr + test_profile->pool_slot) = NULL;
	int queries = g_paf_queries;
		assert(icon_cache_trial_start(42, test_profile->shell_nid, &shell) == 0);
#if LIVEAREA_DEBUG_LOGGING
		assert(test_debug_capture_contains("[cache] start shell_modid=42"));
		assert(test_debug_capture_contains("[cache] profile-selected"));
		assert(test_debug_capture_contains("[cache] waiting-for-pool"));
#endif
	assert(g_pool_init_hook == 457 && g_scan_hook < 0 && g_paf_queries == queries);
	check_log("waiting for icon pool");
	g_query_error = 0;
		initialize_icon_pool();
#if LIVEAREA_DEBUG_LOGGING
		assert(test_debug_capture_contains("[cache] pool-initializer-enter"));
		assert(test_debug_capture_contains("[cache] paf-lookup result=0"));
		assert(test_debug_capture_contains("[cache] hooks-active"));
#endif
	assert(g_original_init_calls == 1 && g_scan_hook == 456 && g_paf_queries == queries + 1);
	check_log("active (LRU + consumer reload)");
	assert(g_apply_hook == 458);
	initialize_icon_pool();
	assert(g_original_init_calls == 2 && g_paf_queries == queries + 1);
	icon_cache_trial_stop();
	assert(g_init_hook_releases == 1 && g_hook_releases == 1);
	g_hook_calls = 0;
	g_hook_releases = 0;
	g_query_error = 1;
	*(IconPool **)((uint8_t *)shell.segments[1].vaddr + test_profile->pool_slot) = NULL;
	assert(icon_cache_trial_start(42, test_profile->shell_nid, &shell) == 0);
	initialize_icon_pool();
	assert(g_scan_hook < 0 && g_pool_init_hook == 457);
	assert(g_log_open == LIVEAREA_ICON_CACHE_LOGGING);
	assert(g_init_hook_releases == 1);
	check_log("PAF lookup failed");
	queries = g_paf_queries;
	initialize_icon_pool();
	assert(g_paf_queries == queries);
	icon_cache_trial_stop();
	g_query_error = 0;
	*(IconPool **)((uint8_t *)shell.segments[1].vaddr + test_profile->pool_slot) = NULL;
	init_entry[0] ^= 1;
	rejected(test_profile->shell_nid, &shell);
	init_entry[0] ^= 1;
	g_init_hook_failure = 1;
	rejected(test_profile->shell_nid, &shell);
	check_log("shell initializer hook failed");
	g_init_hook_failure = 0;
	*(IconPool **)((uint8_t *)shell.segments[1].vaddr + test_profile->pool_slot) = &pool;

	for (i = 0; i < sizeof(test_profiles) / sizeof(test_profiles[0]); ++i) {
		if (test_profiles[i].shell_nid != test_profile->shell_nid)
			rejected(test_profiles[i].shell_nid, &shell);
		if (test_profiles[i].paf_nid != test_profile->paf_nid) {
			g_paf_nid = test_profiles[i].paf_nid;
			rejected(test_profile->shell_nid, &shell);
			g_paf_nid = test_profile->paf_nid;
		}
	}
	for (int ready = 0; ready <= 1; ++ready) {
		*(IconPool **)((uint8_t *)shell.segments[1].vaddr + test_profile->pool_slot) = ready ? &pool : NULL;
		for (i = 0x1E; i < 0x28; ++i) {
			for (unsigned int bit = 0; bit < 8; ++bit) {
				init_entry[i] ^= 1U << bit;
				rejected(test_profile->shell_nid, &shell);
				init_entry[i] ^= 1U << bit;
			}
		}
	}
	--shell.segments[0].memsz;
	rejected(test_profile->shell_nid, &shell);
	shell.segments[0].memsz += 2;
	rejected(test_profile->shell_nid, &shell);
	--shell.segments[0].memsz;
	shell.segments[0].vaddr = NULL;
	rejected(test_profile->shell_nid, &shell);
	shell.segments[0].vaddr = shell_text;
	++shell.segments[1].memsz;
	rejected(test_profile->shell_nid, &shell);
	--shell.segments[1].memsz;
	rejected(0x03650000, &shell);
	check_log("shell identity failed: 0x03650000");
	rejected(test_profile->shell_nid, NULL);
	check_log("shell pool slot");
	shell.segments[1].memsz = test_profile->pool_slot + sizeof(IconPool *) - 1;
	rejected(test_profile->shell_nid, &shell);
	shell.segments[1].memsz = test_profile->data_size;
	g_paf_nid ^= 1;
	rejected(test_profile->shell_nid, &shell);
	g_paf_nid ^= 1;
	--g_paf_size;
	rejected(test_profile->shell_nid, &shell);
	++g_paf_size;
	g_paf_data_size = PAF_MUTEX_OFFSET + 31;
	rejected(test_profile->shell_nid, &shell);
	++g_paf_data_size;
	for (g_query_error = 1; g_query_error <= 2; ++g_query_error)
		rejected(test_profile->shell_nid, &shell);
	g_query_error = 0;
	for (i = 0; i < sizeof(expected_scan_entry); ++i) {
		g_paf_text[PAF_SCAN_OFFSET + i] ^= 1;
		rejected(test_profile->shell_nid, &shell);
		check_log("PAF scan bytes failed: 0x00015E6A");
		g_paf_text[PAF_SCAN_OFFSET + i] ^= 1;
	}
	for (i = 0; i < sizeof(expected_evict) + sizeof(expected_release); ++i) {
		uint8_t *byte = i < sizeof(expected_evict)
			? g_paf_text + PAF_EVICT_OFFSET + i
			: g_paf_text + PAF_RELEASE_OFFSET + i - sizeof(expected_evict);
		*byte ^= 1;
		rejected(test_profile->shell_nid, &shell);
		*byte ^= 1;
	}
	for (i = 0; i < sizeof(expected_mutex_functions) + sizeof(expected_timestamp_getter); ++i) {
		uint8_t *byte = i < sizeof(expected_mutex_functions)
			? g_paf_text + PAF_LOCK_OFFSET + i
			: g_paf_text + PAF_TIMESTAMP_GET_OFFSET + i - sizeof(expected_mutex_functions);
		*byte ^= 1;
		rejected(test_profile->shell_nid, &shell);
		*byte ^= 1;
	}
	for (i = 0; i < 16; ++i) {
		uint8_t *byte = i < 4 ? mutex_load + i : i < 8
			? mutex_load + i + 2 : clock_load + i - 8;
		for (unsigned int bit = 0; bit < 8; ++bit) {
			*byte ^= 1U << bit;
			rejected(test_profile->shell_nid, &shell);
			check_log("PAF cache data references");
			*byte ^= 1U << bit;
		}
	}
	const struct { uint32_t offset; size_t size; } consumer_regions[] = {
		{PAF_APPLY_OFFSET, 30}, {PAF_GET_SURFACE_OFFSET, sizeof(expected_get_surface)},
		{PAF_HANDLE_GET_OFFSET, 14}, {PAF_HANDLE_VTABLE_OFFSET + 8, 4}
	};
	for (i = 0; i < sizeof(consumer_regions) / sizeof(consumer_regions[0]); ++i) {
		for (size_t j = 0; j < consumer_regions[i].size; ++j) {
			uint8_t *byte = g_paf_text + consumer_regions[i].offset + j;
			for (unsigned int bit = 0; bit < 8; ++bit) {
				*byte ^= 1U << bit;
				rejected(test_profile->shell_nid, &shell);
				check_log("PAF consumer bytes");
				check_log("icon-cache PAF text=0x");
				check_log("icon-cache bytes +0015F9E2:");
				check_log("icon-cache bytes +00013344:");
				check_log("icon-cache bytes +00014672:");
				check_log("icon-cache bytes +002E1074:");
				*byte ^= 1U << bit;
			}
		}
	}
	g_consumer_hook_failure = 1;
	rejected(test_profile->shell_nid, &shell);
	check_log("consumer hook failed");
	g_consumer_hook_failure = 0;
	int consumer_releases = g_consumer_hook_releases;
	g_hook_failure = 1;
	assert(icon_cache_trial_start(42, test_profile->shell_nid, &shell) < 0);
	assert(!g_icon_pool_slot && !g_release_surface && !g_log_open && !g_stock_evict);
	check_log("scan hook failed: 0xFFFFFFFF");
	assert(g_hook_releases == 0);
	assert(g_apply_hook < 0 && g_consumer_hook_releases == consumer_releases + 1);
	g_hook_failure = 0;
	assert(icon_cache_trial_start(42, test_profile->shell_nid, &shell) == 0);
	check_log("active (LRU + consumer reload)");
	assert(!strstr(g_log_contents, "icon-cache bytes"));
	assert(g_apply_hook == 458 && g_image_handle_vtable == g_paf_text + PAF_HANDLE_VTABLE_OFFSET);
	assert((uintptr_t)g_get_surface == (uintptr_t)g_paf_text + PAF_GET_SURFACE_OFFSET + 1);
	assert((uintptr_t)g_stock_evict == (uintptr_t)g_paf_text + PAF_EVICT_OFFSET + 1);
	assert((uintptr_t)g_lock_cache == (uintptr_t)g_paf_text + PAF_LOCK_OFFSET + 1);
	assert((uintptr_t)g_unlock_cache == (uintptr_t)g_paf_text + PAF_UNLOCK_OFFSET + 1);
	g_stock_evict = original_evict;
	g_release_surface = release_surface;
	g_lock_cache = lock_cache;
	g_unlock_cache = unlock_cache;
	image.cache = &cache;
	image.kind = 1;
	image.load_state = 2;
	image.references = 3;
	image.registered = 1;
	image.retries_left = 10;
	image.retry_limit = 10;
	image.surface = &surface;
	surface.references = 1;
	before = image;
	PafImage candidates[3] = {image, image, image};
	PafSurface candidate_surfaces[3] = {{0}, {0}, {0}};
	for (i = 0; i < 3; ++i) {
		candidate_surfaces[i].references = 1;
		candidates[i].surface = &candidate_surfaces[i];
	}
	g_multiple_items = candidates;
	g_multiple_count = 3;
	uint32_t *clock = (uint32_t *)(g_paf_data + PAF_CLOCK_OFFSET);
	*clock = 100;
	candidates[0].last_used = 80;
	candidates[1].last_used = 10;
		candidates[2].last_used = 50;
		int freed_before = g_surface_releases;
		releases = g_log_writes;
#if LIVEAREA_DEBUG_LOGGING
		test_debug_capture_reset();
#endif
		assert(scan_icon_surfaces(&cache, original_evict, &selected) == 1);
#if LIVEAREA_DEBUG_LOGGING
		assert(test_debug_capture_contains("[cache] scan-enter"));
		assert(test_debug_capture_contains("[cache] scan-exit"));
#endif
		assert(g_surface_releases - freed_before == 1);
	assert(selected == &candidates[1] && !candidates[1].surface);
	assert(candidates[0].surface && candidates[2].surface);
	assert(scan_icon_surfaces(&cache, original_evict, &selected) == 1);
	assert(selected == &candidates[2]);
	assert(scan_icon_surfaces(&cache, original_evict, &selected) == 1);
	assert(selected == &candidates[0]);
	assert(scan_icon_surfaces(&cache, original_evict, &selected) == 0 && !selected);
	assert(g_log_writes == releases && !g_mutex_depth && !g_selection);
	for (i = 0; i < 3; ++i) {
		candidate_surfaces[i].references = 1;
		candidates[i].surface = &candidate_surfaces[i];
		candidates[i].last_used = *clock;
	}
	g_nested_scan = 1;
	assert(scan_icon_surfaces(&cache, original_evict, &selected) == 1);
	assert(selected == &candidates[0]);
	assert(!g_selection && !g_mutex_depth);
	assert(scan_icon_surfaces(&cache, original_evict, NULL) == 1);
	assert(!candidates[1].surface && candidates[2].surface);
	g_scan_failure = 1;
	assert(scan_icon_surfaces(&cache, original_evict, &selected) == 0 && !selected);
	assert(candidates[2].surface && !g_selection && !g_mutex_depth);
	g_scan_failure = 0;
	g_mutex_failure = 1;
	assert(scan_icon_surfaces(&cache, original_evict, &selected) == 0 && !selected);
	assert(candidates[2].surface);
	g_mutex_failure = 0;
	*clock = 5;
	for (i = 0; i < 3; ++i) {
		candidate_surfaces[i].references = 1;
		candidates[i].surface = &candidate_surfaces[i];
	}
	candidates[0].last_used = 3;
	candidates[1].last_used = UINT32_MAX - 5;
	candidates[2].last_used = 0;
	assert(scan_icon_surfaces(&cache, original_evict, &selected) == 1);
	assert(selected == &candidates[1]);
	candidates[0].surface->references = 2;
	assert(scan_icon_surfaces(&cache, original_evict, &selected) == 1);
	assert(selected == &candidates[2]);
	assert(scan_icon_surfaces(&cache, original_evict, &selected) == 0 && !selected);
	assert(candidates[0].surface && candidates[0].surface->references == 2);
	g_multiple_items = NULL;
	g_expected_cache = &cache;
	g_expected_out = &selected;
	g_scan_item = NULL;
	g_expected_predicate = unrelated_predicate;
	g_scan_result = -29;
	assert(scan_icon_surfaces(&cache, unrelated_predicate, &selected) == -29);
	assert(!selected);
	g_expected_predicate = NULL;
	g_expected_out = NULL;
	assert(scan_icon_surfaces(&cache, NULL, NULL) == -29);
	g_expected_predicate = original_evict;
	cache.surface_pool = NULL;
	assert(scan_icon_surfaces(&cache, original_evict, NULL) == -29);
	cache.surface_pool = &pool;
	image.surface = NULL;
	assert(can_evict_icon_surface(&image) == 0);
	image.surface = &surface;
	surface.references = 2;
	assert(can_evict_icon_surface(&image) == 0 && image.surface == &surface);
	surface.references = 1;
	image.load_state = 1;
	assert(can_evict_icon_surface(&image) == 0);
	image.load_state = 0;
	assert(can_evict_icon_surface(&image) == 0);
	image.load_state = 2;
	image.kind = 0;
	assert(can_evict_icon_surface(&image) == 0);
	PafImage *non_image = calloc(1, offsetof(PafImage, reserved_to_timestamp));
	assert(non_image);
	non_image->cache = &cache;
	non_image->references = 3;
	assert(can_evict_icon_surface(non_image) == 0);
	free(non_image);
	image.kind = 1;
	image.retries_left = 0;
	assert(can_evict_icon_surface(&image) == 0);
	image.retries_left = -1;
	assert(can_evict_icon_surface(&image) == 0);
	image.retries_left = 10;
	image.retry_limit = 0;
	assert(can_evict_icon_surface(&image) == 0);
	image.retry_limit = 10;
	image.registered = 0;
	assert(can_evict_icon_surface(&image) == 0);
	image.registered = 1;
	image.references = 0;
	assert(can_evict_icon_surface(&image) == 0);
	image.references = 1;
	assert(can_evict_icon_surface(&image) == 1);
	assert(can_evict_icon_surface(NULL) == 0);
	image.references = 3;
	releases = g_log_writes;
	g_multiple_items = &image;
	g_multiple_count = 1;
	for (i = 0; i < 32; ++i) {
		surface.references = 1;
			image.surface = &surface;
			before = image;
			assert(scan_icon_surfaces(&cache, original_evict, &selected) == 1);
			before.surface = NULL;
		assert(memcmp(&image, &before, sizeof(image)) == 0);
	}
	g_multiple_items = NULL;
	assert(g_log_writes == releases && !g_mutex_depth && !g_selection);
	icon_cache_trial_stop();
	icon_cache_trial_stop();
	assert(g_hook_releases == 1 && !g_log_open && !g_icon_pool_slot);
	assert(!g_stock_evict && !g_release_surface);
	assert(!g_cache_mutex && !g_cache_clock && !g_lock_cache && !g_unlock_cache);
	assert(g_apply_hook < 0 && !g_get_surface && !g_image_handle_vtable);
	assert(!g_profile);
	g_log_failure = 1;
	assert(icon_cache_trial_start(42, test_profile->shell_nid, &shell) == 0);
	assert(!g_log_open);
	icon_cache_trial_stop();
	assert(g_hook_releases == 2);
	free(g_paf_text);
	free(g_paf_data);
	free(shell_data);
	free(shell_text);
	puts(LIVEAREA_ICON_CACHE_LOGGING
		? "Logging enabled: startup and failure diagnostics verified"
		: "Logging disabled: no file operations or diagnostic formatting on any tested path");
	puts("Icon cache trial: deferred startup, LRU ordering/ties/wrap, one victim, protected surfaces, reloadability, nested locking, metadata, no hot-path I/O, validation and cleanup passed");
	printf("Cache profile passed: shell 0x%08X / PAF 0x%08X\n", test_profile->shell_nid, test_profile->paf_nid);
	return 0;
}
