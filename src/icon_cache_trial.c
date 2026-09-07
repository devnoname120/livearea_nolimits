#include "icon_cache_trial.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <stddef.h>
#include <stdint.h>
#include <taihen.h>

/* PAF and this plugin import the same loader-resolved SceLibKernel variable. */
extern uint32_t __stack_chk_guard;

#define RETAIL_360_SHELL_NID 0x0552F692U
#define PAF_360_NID         0xCD679177U
#define PAF_TEXT_SIZE       0x300D00U
#define PAF_EVICT_OFFSET    0x013EF6U
#define PAF_SCAN_OFFSET     0x015E6AU
#define PAF_RELEASE_OFFSET  0x08997AU
#define PAF_APPLY_OFFSET    0x15F9E2U
#define PAF_GET_SURFACE_OFFSET 0x013344U
#define PAF_HANDLE_VTABLE_OFFSET 0x2E106CU
#define PAF_HANDLE_GET_OFFSET 0x014672U
#define ICON_POOL_SLOT      0x006DECU /* SceShell segment 1. */
#define SHELL_POOL_INIT_OFFSET 0x002C74U
#define PAF_LOCK_OFFSET (0x83254D20U - 0x83200E90U)
#define PAF_UNLOCK_OFFSET (0x83254D34U - 0x83200E90U)
#define PAF_TIMESTAMP_GET_OFFSET (0x83213F70U - 0x83200E90U)
#define PAF_CLOCK_OFFSET (0x8116BA04U - 0x811607E0U)
#define PAF_MUTEX_OFFSET (0x8116BA18U - 0x811607E0U)
#define PAF_MUTEX_LOAD_OFFSET (0x83216D0EU - 0x83200E90U)
#define PAF_CLOCK_LOAD_OFFSET (0x83216D86U - 0x83200E90U)

/* Partial layouts verified against the 3.60 PAF providers and shell callers. */
typedef struct {
	uint8_t reserved[24];
	int32_t references;
} PafSurface;

typedef struct {
	uint8_t reserved[84];
	void *surface_pool;
} PafCache;

typedef struct {
	const void *vtable;
} PafImageHandle;

typedef struct {
	void *vtable;
	PafCache *cache;
	uint8_t kind;
	uint8_t flags;
	uint8_t load_state;
	uint8_t reserved;
	int32_t result;
	int32_t references;
	uint8_t reserved_to_timestamp[56];
	uint32_t last_used;
	uint8_t reserved_at_80;
	uint8_t registered;
	uint8_t reserved_at_82[2];
	int32_t retries_left;
	int32_t retry_limit;
	uint8_t reserved_to_handle[8];
	PafImageHandle handle;
	uint8_t reserved_to_surface[52];
	PafSurface *surface;
} PafImage;

typedef struct {
	void *memory;
	void *surface_pool;
} IconPool;

/* Host logic tests use native pointers; the actual ARM build checks the ABI. */
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(offsetof(PafImage, cache) == 4, "PAF cache pointer offset");
_Static_assert(offsetof(PafImage, kind) == 8, "PAF image kind offset");
_Static_assert(offsetof(PafImage, load_state) == 10, "PAF load state offset");
_Static_assert(offsetof(PafImage, result) == 12, "PAF load result offset");
_Static_assert(offsetof(PafImage, references) == 16, "PAF item references offset");
_Static_assert(offsetof(PafImage, last_used) == 76, "PAF timestamp offset");
_Static_assert(offsetof(PafImage, registered) == 81, "PAF registration offset");
_Static_assert(offsetof(PafImage, retries_left) == 84, "PAF remaining retries offset");
_Static_assert(offsetof(PafImage, retry_limit) == 88, "PAF retry limit offset");
_Static_assert(offsetof(PafImage, handle) == 100, "PAF image interface offset");
_Static_assert(offsetof(PafImage, surface) == 156, "PAF surface offset");
_Static_assert(offsetof(PafCache, surface_pool) == 84, "PAF pool offset");
_Static_assert(offsetof(IconPool, surface_pool) == 4, "shell pool offset");
#endif
_Static_assert(offsetof(PafSurface, references) == 24, "PAF surface references offset");

/* These routines contain no relocated absolute addresses. Check every byte. */
static const uint8_t expected_evict[] = {
	0x10,0xB5,0x04,0x1C,0x01,0xD1,0x00,0x20,0x1E,0xE0,0x20,0x7A,
	0x01,0x28,0x01,0xD0,0x00,0x20,0x19,0xE0,0x21,0x68,0x20,0x1C,
	0x89,0x6B,0x88,0x47,0x01,0x28,0x01,0xDD,0x00,0x20,0x11,0xE0,
	0xD4,0xF8,0x9C,0x00,0x08,0xB9,0x01,0x20,0x0C,0xE0,0x81,0x69,
	0x01,0x29,0x01,0xDD,0x00,0x20,0x07,0xE0,0x75,0xF0,0x24,0xFD,
	0x00,0x20,0xC4,0xF8,0x9C,0x00,0x01,0x20,0x00,0xE0,0xFE,0xE7,
	0x10,0xBD
};
static const uint8_t expected_release[] = {
	0x10,0xB5,0xBF,0xF3,0x5F,0x8F,0x10,0xF1,0x18,0x02,0x52,0xE8,
	0x00,0x1F,0x4B,0x1E,0x42,0xE8,0x00,0x34,0x00,0x2C,0xF8,0xD1,
	0xBF,0xF3,0x5F,0x8F,0x01,0x29,0x05,0xD1,0x10,0xB1,0x01,0x68,
	0x49,0x68,0x88,0x47,0x00,0x20,0x00,0xE0,0x80,0x69,0x10,0xBD
};

/* The first branch is outside the 12-byte taiHEN patch, unlike the predicate. */
static const uint8_t expected_scan_entry[] = {
	0x2D,0xE9,0xF0,0x4F,0x85,0xB0,0x5F,0xEA,0x01,0x0B,
	0x01,0x90,0x14,0x46,0x01,0xD1,0x00,0x20,0x90,0xE0
};

static const uint8_t expected_pool_init_prefix[] = {
	0x2D,0xE9,0xF0,0x41,0x8A,0xB0
};

static const uint8_t expected_mutex_functions[] = {
	0x10,0xB5,0x01,0x21,0x00,0x22,0x0E,0xF2,0xC0,0xEA,
	0x00,0x28,0x01,0xD0,0xAD,0xF7,0x9B,0xFA,0x10,0xBD,
	0x10,0xB5,0x01,0x21,0x0E,0xF2,0x1E,0xEB,
	0x00,0x28,0x01,0xD0,0xAD,0xF7,0x92,0xFA,0x10,0xBD
};
static const uint8_t expected_timestamp_getter[] = {0xC0,0x6C,0x70,0x47};

static const uint8_t expected_apply_prefix[] = {0x2D,0xE9,0xF0,0x4F,0xA5,0xB0};
static const uint8_t expected_apply_suffix[] = {
	0x24,0x68,0x23,0x94,0x80,0x46,0xD8,0xF8,0x64,0x01,
	0x9B,0x46,0x92,0x46,0x89,0x46
};
static const uint8_t expected_get_surface[] = {
	0x70,0xB5,0x4B,0xF6,0x18,0x26,0xC8,0xF2,0x16,0x16,
	0x05,0x1C,0x0C,0x1C,0x30,0x1C,0x40,0xF0,0x9C,0xFD,
	0x60,0x68,0x28,0xB9,0x00,0x20,0x28,0x60,0x30,0x1C,
	0x40,0xF0,0x9F,0xFD,0x20,0xE0,0x94,0xF8,0x51,0x00,
	0x28,0xB9,0x00,0x20,0x28,0x60,0x30,0x1C,0x40,0xF0,
	0x96,0xFD,0x17,0xE0,0x21,0x68,0x20,0x1C,0x49,0x69,
	0x88,0x47,0x00,0x28,0x05,0xDA,0x00,0x20,0x28,0x60,
	0x30,0x1C,0x40,0xF0,0x8A,0xFD,0x0B,0xE0,0x20,0x1C,
	0xFF,0xF7,0xC6,0xF8,0xD4,0xF8,0x9C,0x00,0x28,0x60,
	0x08,0xB1,0x76,0xF0,0xDC,0xFA,0x30,0x1C,0x40,0xF0,
	0x7D,0xFD,0x70,0xBD
};
static const uint8_t expected_handle_get_prefix[] = {0xA1,0xF1,0x64,0x01};
static const uint8_t expected_handle_get_suffix[] = {0x60,0x47};

typedef struct {
	PafCache *cache;
	PafImage *victim;
	uint32_t now;
	uint32_t oldest_age;
} IconSelection;

static SceUID g_scan_hook = -1;
static tai_hook_ref_t g_scan_ref;
static SceUID g_apply_hook = -1;
static tai_hook_ref_t g_apply_ref;
static const void *g_image_handle_vtable;
static void (*g_get_surface)(PafSurface **out, PafImage *image);
static SceUID g_pool_init_hook = -1;
static tai_hook_ref_t g_pool_init_ref;
static int g_install_attempted;
static int (*g_stock_evict)(void *image);
static IconPool *volatile *g_icon_pool_slot;
static unsigned int (*g_release_surface)(PafSurface *surface);
static int (*g_lock_cache)(void *mutex);
static int (*g_unlock_cache)(void *mutex);
static void *g_cache_mutex;
static const uint32_t *g_cache_clock;
static IconSelection *g_selection;
static SceUID g_log_fd = -1;

static void trial_log(const char *message, SceSize size)
{
	if (g_log_fd >= 0)
		sceIoWrite(g_log_fd, message, size);
}

static int is_icon_cache(const PafCache *cache)
{
	IconPool *pool = g_icon_pool_slot ? *g_icon_pool_slot : NULL;
	return cache && pool && pool->surface_pool &&
		cache->surface_pool == pool->surface_pool;
}

static int can_evict_icon_surface(const PafImage *image)
{
	/* Non-image entries do not have the extended surface field. */
	if (!image || image->kind != 1 || image->load_state != 2 ||
		image->references < 1)
		return 0;
	/* A retained handle without retry eligibility cannot recreate its texture. */
	if (image->references > 1 && (!image->registered ||
		image->retries_left <= 0 || image->retry_limit <= 0))
		return 0;
	return image->surface && image->surface->references == 1;
}

static int select_icon_surface(void *item)
{
	PafImage *image = item;
	IconSelection *selection = g_selection;
	if (image && selection && image->cache == selection->cache &&
		can_evict_icon_surface(image)) {
		uint32_t age = selection->now - image->last_used;
		if (!selection->victim || age > selection->oldest_age) {
			selection->victim = image;
			selection->oldest_age = age;
		}
	}
	/* The firmware's signed, strictly-positive age selector must not veto progress. */
	return 0;
}

static int scan_icon_surfaces(void *cache, int (*predicate)(void *),
	void **out_item)
{
	IconSelection selection = {0};
	IconSelection *previous;
	int result;
	if (!g_stock_evict || predicate != g_stock_evict || !cache)
		return TAI_CONTINUE(int, g_scan_ref, cache, predicate, out_item);
	/* The getter and allocator use this same recursive mutex. Keep selection alive
	 * through release, including the interval after the firmware scan unlocks. */
	if (g_lock_cache(g_cache_mutex) < 0) {
		if (out_item)
			*out_item = NULL;
		return 0;
	}
	if (!is_icon_cache(cache)) {
		g_unlock_cache(g_cache_mutex);
		return TAI_CONTINUE(int, g_scan_ref, cache, predicate, out_item);
	}
	selection.cache = cache;
	selection.now = *g_cache_clock;
	previous = g_selection;
	g_selection = &selection;
	result = TAI_CONTINUE(int, g_scan_ref, cache, select_icon_surface, (void **)NULL);
	g_selection = previous;
	if (out_item)
		*out_item = NULL;
	if (result >= 0 && selection.victim &&
		can_evict_icon_surface(selection.victim)) {
		PafImage *image = selection.victim;
		g_release_surface(image->surface);
		image->surface = NULL;
		if (out_item)
			*out_item = image;
		result = 1;
	} else {
		result = 0;
	}
	g_unlock_cache(g_cache_mutex);
	return result;
}

static int apply_icon_image(void *widget, void **handle, int object, int texture)
{
	PafSurface *held_surface = NULL;
	int pending = 0;
	if (widget && handle && *handle && g_image_handle_vtable &&
		((PafImageHandle *)*handle)->vtable == g_image_handle_vtable) {
		/* Other image interfaces need not be embedded in a PAF cache item. */
		PafImage *image = (PafImage *)((uint8_t *)*handle - offsetof(PafImage, handle));
		if (g_lock_cache(g_cache_mutex) < 0)
			return -1;
		if (image->kind == 1 && is_icon_cache(image->cache) &&
			image->registered && image->result == 0 &&
			(image->surface || image->load_state == 1 ||
			(image->load_state == 2 && image->retry_limit && image->retries_left))) {
			g_get_surface(&held_surface, image);
			/* Stale success must not finish a widget request for an absent texture. */
			pending = !held_surface;
		}
		g_unlock_cache(g_cache_mutex);
	}
	if (pending)
		return -1;
	/* Pin artwork across the native getter, without holding the cache mutex over
	 * widget callbacks. Otherwise an intervening eviction recreates the same race. */
	int result = TAI_CONTINUE(int, g_apply_ref, widget, handle, object, texture);
	if (held_surface)
		g_release_surface(held_surface);
	return result;
}

static int matches(const uint8_t *actual, const uint8_t *expected, size_t size)
{
	size_t index;
	for (index = 0; index < size; ++index) {
		if (actual[index] != expected[index])
			return 0;
	}
	return 1;
}

static int matches_mov_half(const uint8_t *code, uint16_t opcode,
	unsigned int reg, uint16_t value)
{
	uint16_t first = (uint16_t)(opcode | (value >> 12) | ((value >> 1) & 0x0400U));
	uint16_t second = (uint16_t)(((value << 4) & 0x7000U) | (reg << 8) | (value & 0xFFU));
	return (code[0] | code[1] << 8) == first &&
		(code[2] | code[3] << 8) == second;
}

static int matches_mov_address(const uint8_t *low, const uint8_t *high,
	unsigned int reg, uint32_t address)
{
	return matches_mov_half(low, 0xF240U, reg, (uint16_t)address) &&
		matches_mov_half(high, 0xF2C0U, reg, (uint16_t)(address >> 16));
}

static int valid_consumer_code(const uint8_t *text, uint32_t data_address)
{
	uint32_t text_address = (uint32_t)(uintptr_t)text;
	const uint8_t *apply = text + PAF_APPLY_OFFSET;
	const uint8_t *get = text + PAF_GET_SURFACE_OFFSET;
	const uint8_t *thunk = text + PAF_HANDLE_GET_OFFSET;
	const uint8_t *slot = text + PAF_HANDLE_VTABLE_OFFSET + 8;
	uint32_t getter = (uint32_t)slot[0] | (uint32_t)slot[1] << 8 |
		(uint32_t)slot[2] << 16 | (uint32_t)slot[3] << 24;
	return matches(apply, expected_apply_prefix, sizeof(expected_apply_prefix)) &&
		matches_mov_address(apply + 6, apply + 10, 4,
			(uint32_t)(uintptr_t)&__stack_chk_guard) &&
		matches(apply + 14, expected_apply_suffix, sizeof(expected_apply_suffix)) &&
		matches(get, expected_get_surface, 2) &&
		matches_mov_address(get + 2, get + 6, 6, data_address + PAF_MUTEX_OFFSET) &&
		matches(get + 10, expected_get_surface + 10, sizeof(expected_get_surface) - 10) &&
		getter == text_address + PAF_HANDLE_GET_OFFSET + 1 &&
		matches(thunk, expected_handle_get_prefix, sizeof(expected_handle_get_prefix)) &&
		matches_mov_address(thunk + 4, thunk + 8, 12,
			text_address + PAF_GET_SURFACE_OFFSET + 1) &&
		matches(thunk + 12, expected_handle_get_suffix, sizeof(expected_handle_get_suffix));
}

void icon_cache_trial_stop(void)
{
	if (g_scan_hook >= 0) {
		taiHookRelease(g_scan_hook, g_scan_ref);
		g_scan_hook = -1;
	}
	if (g_apply_hook >= 0) {
		taiHookRelease(g_apply_hook, g_apply_ref);
		g_apply_hook = -1;
	}
	if (g_pool_init_hook >= 0) {
		taiHookRelease(g_pool_init_hook, g_pool_init_ref);
		g_pool_init_hook = -1;
	}
	g_install_attempted = 0;
	g_stock_evict = NULL;
	g_icon_pool_slot = NULL;
	g_release_surface = NULL;
	g_lock_cache = NULL;
	g_unlock_cache = NULL;
	g_cache_mutex = NULL;
	g_cache_clock = NULL;
	g_selection = NULL;
	g_image_handle_vtable = NULL;
	g_get_surface = NULL;
	if (g_log_fd >= 0) {
		sceIoClose(g_log_fd);
		g_log_fd = -1;
	}
}

static int trial_failure(const char *stage, uint32_t detail)
{
	char message[96];
	/* This plugin does not initialize newlib's application runtime. */
	int length = sceClibSnprintf(message, sizeof(message),
		"icon-cache trial %s failed: 0x%08X\n", stage, (unsigned int)detail);
	if (length > 0)
		trial_log(message, (size_t)length < sizeof(message)
			? (size_t)length : sizeof(message) - 1);
	return -1;
}

static void trial_log_bytes(const uint8_t *text, uint32_t offset, size_t size)
{
	static const char hex[] = "0123456789ABCDEF";
	for (size_t index = 0; index < size; index += 16) {
		char message[96];
		size_t count = size - index < 16 ? size - index : 16;
		int length = sceClibSnprintf(message, sizeof(message),
			"icon-cache bytes +%08X:", (unsigned int)(offset + index));
		if (length < 0 || (size_t)length + count * 3 + 1 > sizeof(message))
			return;
		for (size_t byte = 0; byte < count; ++byte) {
			uint8_t value = text[offset + index + byte];
			message[length++] = ' ';
			message[length++] = hex[value >> 4];
			message[length++] = hex[value & 15];
		}
		message[length++] = '\n';
		trial_log(message, (SceSize)length);
	}
}

static void trial_log_consumer_code(const uint8_t *text, uint32_t data_address)
{
	char message[96];
	if (g_log_fd < 0)
		return;
	int length = sceClibSnprintf(message, sizeof(message),
		"icon-cache PAF text=0x%08X data=0x%08X\n",
		(unsigned int)(uintptr_t)text, (unsigned int)data_address);
	if (length > 0 && (size_t)length < sizeof(message))
		trial_log(message, (SceSize)length);
	trial_log_bytes(text, PAF_APPLY_OFFSET, 30);
	trial_log_bytes(text, PAF_GET_SURFACE_OFFSET, sizeof(expected_get_surface));
	trial_log_bytes(text, PAF_HANDLE_GET_OFFSET, 14);
	trial_log_bytes(text, PAF_HANDLE_VTABLE_OFFSET + 8, 4);
}

static int install_paf_hook(void)
{
	tai_module_info_t paf = {0};
	SceKernelModuleInfo info = {0};
	uint8_t *text;
	int result;
	const char *hook_stage = "consumer hook";
	static const char active[] = "icon-cache trial active (LRU + consumer reload)\n";

	g_install_attempted = 1;
	paf.size = sizeof(paf);
	result = taiGetModuleInfo("ScePaf", &paf);
	if (result < 0)
		return trial_failure("PAF lookup", (uint32_t)result);
	if (paf.module_nid != PAF_360_NID)
		return trial_failure("PAF identity", paf.module_nid);
	info.size = sizeof(info);
	result = sceKernelGetModuleInfo(paf.modid, &info);
	if (result < 0)
		return trial_failure("PAF module info", (uint32_t)result);
	if (!info.segments[0].vaddr || info.segments[0].memsz != PAF_TEXT_SIZE)
		return trial_failure("PAF segment", info.segments[0].memsz);
	if (!info.segments[1].vaddr || info.segments[1].memsz < PAF_MUTEX_OFFSET + 32)
		return trial_failure("PAF data segment", info.segments[1].memsz);
	text = info.segments[0].vaddr;
	if (!matches(text + PAF_SCAN_OFFSET, expected_scan_entry, sizeof(expected_scan_entry)))
		return trial_failure("PAF scan bytes", PAF_SCAN_OFFSET);
	if (!matches(text + PAF_EVICT_OFFSET, expected_evict, sizeof(expected_evict)))
		return trial_failure("PAF predicate bytes", PAF_EVICT_OFFSET);
	if (!matches(text + PAF_RELEASE_OFFSET, expected_release, sizeof(expected_release)))
		return trial_failure("PAF release bytes", PAF_RELEASE_OFFSET);
	if (!matches(text + PAF_LOCK_OFFSET, expected_mutex_functions, sizeof(expected_mutex_functions)))
		return trial_failure("PAF mutex bytes", PAF_LOCK_OFFSET);
	if (!matches(text + PAF_TIMESTAMP_GET_OFFSET, expected_timestamp_getter,
		sizeof(expected_timestamp_getter)))
		return trial_failure("PAF timestamp bytes", PAF_TIMESTAMP_GET_OFFSET);
	/* Reject a data-layout mismatch before locking or reading an unrelated object. */
	uint32_t data_address = (uint32_t)(uintptr_t)info.segments[1].vaddr;
	if (!matches_mov_address(text + PAF_MUTEX_LOAD_OFFSET,
		text + PAF_MUTEX_LOAD_OFFSET + 6, 5, data_address + PAF_MUTEX_OFFSET) ||
		!matches_mov_address(text + PAF_CLOCK_LOAD_OFFSET,
		text + PAF_CLOCK_LOAD_OFFSET + 4, 1, data_address + PAF_CLOCK_OFFSET))
		return trial_failure("PAF cache data references", PAF_MUTEX_OFFSET);
	if (!valid_consumer_code(text, data_address)) {
		trial_log_consumer_code(text, data_address);
		return trial_failure("PAF consumer bytes", PAF_APPLY_OFFSET);
	}
	g_release_surface = (void *)((uintptr_t)text + PAF_RELEASE_OFFSET + 1);
	g_stock_evict = (void *)((uintptr_t)text + PAF_EVICT_OFFSET + 1);
	g_lock_cache = (void *)((uintptr_t)text + PAF_LOCK_OFFSET + 1);
	g_unlock_cache = (void *)((uintptr_t)text + PAF_UNLOCK_OFFSET + 1);
	g_cache_mutex = (uint8_t *)info.segments[1].vaddr + PAF_MUTEX_OFFSET;
	g_cache_clock = (const uint32_t *)((uint8_t *)info.segments[1].vaddr + PAF_CLOCK_OFFSET);
	g_image_handle_vtable = text + PAF_HANDLE_VTABLE_OFFSET;
	g_get_surface = (void *)((uintptr_t)text + PAF_GET_SURFACE_OFFSET + 1);
	/* Do not enable additional evictions without the matching consumer fix. */
	g_apply_hook = taiHookFunctionOffset(&g_apply_ref, paf.modid, 0,
		PAF_APPLY_OFFSET, 1, apply_icon_image);
	if (g_apply_hook < 0) {
		result = g_apply_hook;
		goto hook_failed;
	}
	g_scan_hook = taiHookFunctionOffset(&g_scan_ref, paf.modid, 0,
		PAF_SCAN_OFFSET, 1, scan_icon_surfaces);
	if (g_scan_hook < 0) {
		hook_stage = "scan hook";
		result = g_scan_hook;
		taiHookRelease(g_apply_hook, g_apply_ref);
		g_apply_hook = -1;
		goto hook_failed;
	}
	trial_log(active, sizeof(active) - 1);
	return 0;

hook_failed:
	g_stock_evict = NULL;
	g_release_surface = NULL;
	g_lock_cache = NULL;
	g_unlock_cache = NULL;
	g_cache_mutex = NULL;
	g_cache_clock = NULL;
	g_image_handle_vtable = NULL;
	g_get_surface = NULL;
	return trial_failure(hook_stage, (uint32_t)result);
}

static void initialize_icon_pool(void)
{
	TAI_CONTINUE(void, g_pool_init_ref);
	/* PAF is unavailable at plugin startup; this initializer calls its providers. */
	if (!g_install_attempted)
		(void)install_paf_hook();
}

static int valid_pool_init_entry(const uint8_t *entry)
{
	uint16_t first = (uint16_t)(entry[6] | entry[7] << 8);
	uint16_t second = (uint16_t)(entry[8] | entry[9] << 8);
	/* The MOVW immediate contains a relocated address; its opcode/register do not. */
	return matches(entry, expected_pool_init_prefix, sizeof(expected_pool_init_prefix)) &&
		(first & 0xFBF0U) == 0xF240U && (second & 0x8F00U) == 0x0800U;
}

int icon_cache_trial_start(SceUID shell_modid, uint32_t shell_nid,
	const SceKernelModuleInfo *shell_info)
{
	int result = -1;
	static const char waiting[] = "icon-cache trial waiting for icon pool\n";

	g_install_attempted = 0;
	g_log_fd = sceIoOpen("ur0:/data/livearea_nolimits-icon-cache-trial.log",
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (shell_nid != RETAIL_360_SHELL_NID) {
		trial_failure("shell identity", shell_nid);
		goto fail;
	}
	if (!shell_info || !shell_info->segments[1].vaddr ||
		shell_info->segments[1].memsz < ICON_POOL_SLOT + sizeof(IconPool *)) {
		trial_failure("shell pool slot", ICON_POOL_SLOT);
		goto fail;
	}
	g_icon_pool_slot = (IconPool *volatile *)
		((uintptr_t)shell_info->segments[1].vaddr + ICON_POOL_SLOT);
	if (*g_icon_pool_slot && (*g_icon_pool_slot)->surface_pool) {
		result = install_paf_hook();
		if (result < 0)
			goto fail;
		return 0;
	}
	if (!shell_info->segments[0].vaddr ||
		shell_info->segments[0].memsz < SHELL_POOL_INIT_OFFSET + 10 ||
		!valid_pool_init_entry((const uint8_t *)shell_info->segments[0].vaddr +
			SHELL_POOL_INIT_OFFSET)) {
		trial_failure("shell initializer bytes", SHELL_POOL_INIT_OFFSET);
		goto fail;
	}
	g_pool_init_hook = taiHookFunctionOffset(&g_pool_init_ref, shell_modid, 0,
		SHELL_POOL_INIT_OFFSET, 1, initialize_icon_pool);
	if (g_pool_init_hook < 0) {
		trial_failure("shell initializer hook", (uint32_t)g_pool_init_hook);
		goto fail;
	}
	trial_log(waiting, sizeof(waiting) - 1);
	return 0;

fail:
	icon_cache_trial_stop();
	return result;
}
