#include "icon_cache_trial.h"

#ifndef LIVEAREA_ICON_CACHE_LOGGING
#define LIVEAREA_ICON_CACHE_LOGGING 0
#endif

#if LIVEAREA_ICON_CACHE_LOGGING
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <taihen.h>

#include "debug_log.h"

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

typedef struct {
	uint32_t shell_nid, paf_nid;
	uint32_t text_size, data_size;
	uint32_t pool_slot, init_offset;
} CacheProfile;

/* Matching PAF code/layout does not imply matching shell globals or imports. */
static const CacheProfile cache_profiles[] = {
	{RETAIL_360_SHELL_NID, PAF_360_NID, 0x541B74, 0x93FAC, ICON_POOL_SLOT, SHELL_POOL_INIT_OFFSET},
	{0x5549BF1FU, 0x73F90499U, 0x5420F4, 0x93FBC, 0x6DFC, 0x2CCC},
	{0xEAB89D5CU, PAF_360_NID, 0x535CF4, 0x92D1C, 0x6BBC, 0x2C74},
};
static const CacheProfile *g_profile;

/* Partial layouts verified against all three supported shell/PAF pairs. */
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
#if LIVEAREA_DEBUG_LOGGING
	unsigned int scanned;
	unsigned int eligible;
#endif
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
#if LIVEAREA_DEBUG_LOGGING
static volatile unsigned int g_debug_scan_calls;
static volatile unsigned int g_debug_apply_calls;
static volatile unsigned int g_debug_evictions;
static volatile unsigned int g_debug_pending;
static volatile unsigned int g_debug_scan_errors;
static volatile unsigned int g_debug_apply_errors;

static int trace_hot_call(unsigned int call)
{
	return call <= 32 || (call & 127U) == 0;
}

static int trace_occurrence(unsigned int occurrence)
{
	return occurrence <= 16 || (occurrence & 31U) == 0;
}
#endif
#if LIVEAREA_ICON_CACHE_LOGGING
static SceUID g_log_fd = -1;

static void trial_log(const char *message, SceSize size)
{
	if (g_log_fd >= 0)
		sceIoWrite(g_log_fd, message, size);
}
#endif

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
#if LIVEAREA_DEBUG_LOGGING
	if (selection)
		++selection->scanned;
#endif
	if (image && selection && image->cache == selection->cache &&
		can_evict_icon_surface(image)) {
		uint32_t age = selection->now - image->last_used;
#if LIVEAREA_DEBUG_LOGGING
		++selection->eligible;
#endif
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
#if LIVEAREA_DEBUG_LOGGING
	unsigned int call = __sync_add_and_fetch(&g_debug_scan_calls, 1);
	int trace = trace_hot_call(call);
	unsigned int occurrence = 0;
	if (trace)
		debug_logf("cache", "scan-enter call=%u cache=0x%08X predicate=0x%08X out=0x%08X",
			call, (unsigned int)(uintptr_t)cache, (unsigned int)(uintptr_t)predicate,
			(unsigned int)(uintptr_t)out_item);
#endif
	if (!g_stock_evict || predicate != g_stock_evict || !cache) {
		result = TAI_CONTINUE(int, g_scan_ref, cache, predicate, out_item);
#if LIVEAREA_DEBUG_LOGGING
		if (trace || result < 0)
			debug_logf("cache", "scan-delegate call=%u result=%d stock=0x%08X",
				call, result, (unsigned int)(uintptr_t)g_stock_evict);
#endif
		return result;
	}
	/* The getter and allocator use this same recursive mutex. Keep selection alive
	 * through release, including the interval after the firmware scan unlocks. */
	if (g_lock_cache(g_cache_mutex) < 0) {
		if (out_item)
			*out_item = NULL;
#if LIVEAREA_DEBUG_LOGGING
		debug_logf("cache", "scan-lock-failed call=%u mutex=0x%08X",
			call, (unsigned int)(uintptr_t)g_cache_mutex);
		__sync_add_and_fetch(&g_debug_scan_errors, 1);
#endif
		return 0;
	}
	if (!is_icon_cache(cache)) {
		g_unlock_cache(g_cache_mutex);
		result = TAI_CONTINUE(int, g_scan_ref, cache, predicate, out_item);
#if LIVEAREA_DEBUG_LOGGING
		if (trace || result < 0)
			debug_logf("cache", "scan-non-icon-cache call=%u result=%d",
				call, result);
#endif
		return result;
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
#if LIVEAREA_DEBUG_LOGGING
	if (result == 1)
		occurrence = __sync_add_and_fetch(&g_debug_evictions, 1);
	else if (result < 0)
		occurrence = __sync_add_and_fetch(&g_debug_scan_errors, 1);
	if (trace || (occurrence && trace_occurrence(occurrence)))
		debug_logf("cache", "scan-exit call=%u result=%d scanned=%u eligible=%u victim=0x%08X age=%u out_item=0x%08X evictions=%u scan_errors=%u",
			call, result, selection.scanned, selection.eligible,
			(unsigned int)(uintptr_t)selection.victim, selection.oldest_age,
			(unsigned int)(uintptr_t)(out_item ? *out_item : NULL),
			(unsigned int)g_debug_evictions, (unsigned int)g_debug_scan_errors);
#endif
	return result;
}

static int apply_icon_image(void *widget, void **handle, int object, int texture)
{
	PafSurface *held_surface = NULL;
	PafImage *image = NULL;
	int pending = 0;
#if LIVEAREA_DEBUG_LOGGING
	unsigned int call = __sync_add_and_fetch(&g_debug_apply_calls, 1);
	int trace = trace_hot_call(call);
	int kind = -1;
	int load_state = -1;
	int image_result = 0;
	int references = 0;
	int registered = 0;
	int retries_left = 0;
	int retry_limit = 0;
	void *surface = NULL;
	unsigned int pending_occurrence = 0;
	if (trace)
		debug_logf("cache", "apply-enter call=%u widget=0x%08X handle_ptr=0x%08X handle=0x%08X object=%d texture=%d",
			call, (unsigned int)(uintptr_t)widget, (unsigned int)(uintptr_t)handle,
			(unsigned int)(uintptr_t)(handle ? *handle : NULL), object, texture);
#endif
	if (widget && handle && *handle && g_image_handle_vtable &&
		((PafImageHandle *)*handle)->vtable == g_image_handle_vtable) {
		/* Other image interfaces need not be embedded in a PAF cache item. */
		image = (PafImage *)((uint8_t *)*handle - offsetof(PafImage, handle));
		if (g_lock_cache(g_cache_mutex) < 0) {
			debug_logf("cache", "apply-lock-failed call=%u image=0x%08X mutex=0x%08X",
				call, (unsigned int)(uintptr_t)image,
				(unsigned int)(uintptr_t)g_cache_mutex);
			return -1;
		}
		if (image->kind == 1 && is_icon_cache(image->cache) &&
			image->registered && image->result == 0 &&
			(image->surface || image->load_state == 1 ||
			(image->load_state == 2 && image->retry_limit && image->retries_left))) {
			g_get_surface(&held_surface, image);
			/* Stale success must not finish a widget request for an absent texture. */
			pending = !held_surface;
		}
#if LIVEAREA_DEBUG_LOGGING
		kind = image->kind;
		load_state = image->load_state;
		image_result = image->result;
		references = image->references;
		registered = image->registered;
		retries_left = image->retries_left;
		retry_limit = image->retry_limit;
		surface = image->surface;
#endif
		g_unlock_cache(g_cache_mutex);
	}
#if LIVEAREA_DEBUG_LOGGING
	if (pending)
		pending_occurrence = __sync_add_and_fetch(&g_debug_pending, 1);
	if (trace || (pending_occurrence && trace_occurrence(pending_occurrence)))
		debug_logf("cache", "apply-state call=%u image=0x%08X kind=%d load=%d result=%d refs=%d registered=%d retries=%d/%d surface=0x%08X held=0x%08X pending=%d",
			call, (unsigned int)(uintptr_t)image, kind, load_state, image_result,
			references, registered, retries_left, retry_limit,
			(unsigned int)(uintptr_t)surface, (unsigned int)(uintptr_t)held_surface,
			pending);
#endif
	if (pending) {
#if LIVEAREA_DEBUG_LOGGING
		if (trace || trace_occurrence(pending_occurrence))
			debug_logf("cache", "apply-exit call=%u result=-1 reason=pending pending_total=%u",
				call, pending_occurrence);
#endif
		return -1;
	}
	/* Pin artwork across the native getter, without holding the cache mutex over
	 * widget callbacks. Otherwise an intervening eviction recreates the same race. */
	int result = TAI_CONTINUE(int, g_apply_ref, widget, handle, object, texture);
	if (held_surface)
		g_release_surface(held_surface);
#if LIVEAREA_DEBUG_LOGGING
	unsigned int error_occurrence = 0;
	if (result != 0)
		error_occurrence = __sync_add_and_fetch(&g_debug_apply_errors, 1);
	if (trace || (error_occurrence && trace_occurrence(error_occurrence)))
		debug_logf("cache", "apply-exit call=%u result=%d held=0x%08X released=%u apply_errors=%u",
			call, result, (unsigned int)(uintptr_t)held_surface,
			held_surface ? 1U : 0U, (unsigned int)g_debug_apply_errors);
#endif
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
	int result = 0;

	debug_logf("cache", "stop begin scan_uid=%d apply_uid=%d init_uid=%d",
		g_scan_hook, g_apply_hook, g_pool_init_hook);
	if (g_scan_hook >= 0) {
		result = taiHookRelease(g_scan_hook, g_scan_ref);
		debug_logf("cache", "scan-hook-release uid=%d result=%d", g_scan_hook, result);
		g_scan_hook = -1;
	}
	if (g_apply_hook >= 0) {
		result = taiHookRelease(g_apply_hook, g_apply_ref);
		debug_logf("cache", "apply-hook-release uid=%d result=%d", g_apply_hook, result);
		g_apply_hook = -1;
	}
	if (g_pool_init_hook >= 0) {
		result = taiHookRelease(g_pool_init_hook, g_pool_init_ref);
		debug_logf("cache", "pool-hook-release uid=%d result=%d", g_pool_init_hook, result);
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
	g_profile = NULL;
	(void)result;
#if LIVEAREA_DEBUG_LOGGING
	debug_logf("cache", "totals scan_calls=%u apply_calls=%u evictions=%u pending=%u scan_errors=%u apply_errors=%u",
		(unsigned int)g_debug_scan_calls, (unsigned int)g_debug_apply_calls,
		(unsigned int)g_debug_evictions, (unsigned int)g_debug_pending,
		(unsigned int)g_debug_scan_errors, (unsigned int)g_debug_apply_errors);
#endif
#if LIVEAREA_ICON_CACHE_LOGGING
	if (g_log_fd >= 0) {
		sceIoClose(g_log_fd);
		g_log_fd = -1;
	}
#endif
	debug_logf("cache", "stop complete");
}

static int trial_failure(const char *stage, uint32_t detail)
{
	debug_logf("cache", "failure stage=%s detail=0x%08X",
		stage ? stage : "unknown", (unsigned int)detail);
#if LIVEAREA_ICON_CACHE_LOGGING
	char message[96];
	/* This plugin does not initialize newlib's application runtime. */
	int length = sceClibSnprintf(message, sizeof(message),
		"icon-cache trial %s failed: 0x%08X\n", stage, (unsigned int)detail);
	if (length > 0)
		trial_log(message, (size_t)length < sizeof(message)
			? (size_t)length : sizeof(message) - 1);
#else
	(void)stage;
	(void)detail;
#endif
	return -1;
}

#if LIVEAREA_ICON_CACHE_LOGGING
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
#endif

static int install_paf_hook(void)
{
	tai_module_info_t paf = {0};
	SceKernelModuleInfo info = {0};
	uint8_t *text;
	int result;
	const char *hook_stage = "consumer hook";

	g_install_attempted = 1;
	debug_logf("cache", "paf-install begin profile=0x%08X expected_paf=0x%08X",
		g_profile ? (unsigned int)g_profile->shell_nid : 0U,
		g_profile ? (unsigned int)g_profile->paf_nid : 0U);
	paf.size = sizeof(paf);
	result = taiGetModuleInfo("ScePaf", &paf);
	debug_logf("cache", "paf-lookup result=%d modid=%d nid=0x%08X",
		result, result < 0 ? -1 : paf.modid,
		result < 0 ? 0U : (unsigned int)paf.module_nid);
	if (result < 0)
		return trial_failure("PAF lookup", (uint32_t)result);
	if (!g_profile || paf.module_nid != g_profile->paf_nid) {
		debug_logf("cache", "paf-identity-mismatch actual=0x%08X expected=0x%08X",
			(unsigned int)paf.module_nid,
			g_profile ? (unsigned int)g_profile->paf_nid : 0U);
		return trial_failure("PAF identity", paf.module_nid);
	}
	info.size = sizeof(info);
	result = sceKernelGetModuleInfo(paf.modid, &info);
	debug_logf("cache", "paf-module-info result=%d text=0x%08X text_size=0x%08X data=0x%08X data_size=0x%08X",
		result,
		result < 0 ? 0U : (unsigned int)(uintptr_t)info.segments[0].vaddr,
		result < 0 ? 0U : (unsigned int)info.segments[0].memsz,
		result < 0 ? 0U : (unsigned int)(uintptr_t)info.segments[1].vaddr,
		result < 0 ? 0U : (unsigned int)info.segments[1].memsz);
	if (result < 0)
		return trial_failure("PAF module info", (uint32_t)result);
	if (!info.segments[0].vaddr || info.segments[0].memsz != PAF_TEXT_SIZE) {
		debug_logf("cache", "paf-text-mismatch actual=0x%08X expected=0x%08X",
			(unsigned int)info.segments[0].memsz, PAF_TEXT_SIZE);
		return trial_failure("PAF segment", info.segments[0].memsz);
	}
	if (!info.segments[1].vaddr || info.segments[1].memsz < PAF_MUTEX_OFFSET + 32) {
		debug_logf("cache", "paf-data-mismatch actual=0x%08X minimum=0x%08X",
			(unsigned int)info.segments[1].memsz, PAF_MUTEX_OFFSET + 32);
		return trial_failure("PAF data segment", info.segments[1].memsz);
	}
	text = info.segments[0].vaddr;
	if (!matches(text + PAF_SCAN_OFFSET, expected_scan_entry, sizeof(expected_scan_entry))) {
		debug_log_hex("cache", "paf-scan-actual", PAF_SCAN_OFFSET,
			text + PAF_SCAN_OFFSET, sizeof(expected_scan_entry));
		debug_log_hex("cache", "paf-scan-expected", PAF_SCAN_OFFSET,
			expected_scan_entry, sizeof(expected_scan_entry));
		return trial_failure("PAF scan bytes", PAF_SCAN_OFFSET);
	}
	debug_logf("cache", "paf-scan-verified offset=0x%08X", PAF_SCAN_OFFSET);
	if (!matches(text + PAF_EVICT_OFFSET, expected_evict, sizeof(expected_evict))) {
		debug_log_hex("cache", "paf-predicate-actual", PAF_EVICT_OFFSET,
			text + PAF_EVICT_OFFSET, sizeof(expected_evict));
		return trial_failure("PAF predicate bytes", PAF_EVICT_OFFSET);
	}
	debug_logf("cache", "paf-predicate-verified offset=0x%08X", PAF_EVICT_OFFSET);
	if (!matches(text + PAF_RELEASE_OFFSET, expected_release, sizeof(expected_release))) {
		debug_log_hex("cache", "paf-release-actual", PAF_RELEASE_OFFSET,
			text + PAF_RELEASE_OFFSET, sizeof(expected_release));
		return trial_failure("PAF release bytes", PAF_RELEASE_OFFSET);
	}
	debug_logf("cache", "paf-release-verified offset=0x%08X", PAF_RELEASE_OFFSET);
	if (!matches(text + PAF_LOCK_OFFSET, expected_mutex_functions, sizeof(expected_mutex_functions))) {
		debug_log_hex("cache", "paf-mutex-actual", PAF_LOCK_OFFSET,
			text + PAF_LOCK_OFFSET, sizeof(expected_mutex_functions));
		return trial_failure("PAF mutex bytes", PAF_LOCK_OFFSET);
	}
	debug_logf("cache", "paf-mutex-verified lock=0x%08X unlock=0x%08X",
		PAF_LOCK_OFFSET, PAF_UNLOCK_OFFSET);
	if (!matches(text + PAF_TIMESTAMP_GET_OFFSET, expected_timestamp_getter,
		sizeof(expected_timestamp_getter))) {
		debug_log_hex("cache", "paf-clock-getter-actual", PAF_TIMESTAMP_GET_OFFSET,
			text + PAF_TIMESTAMP_GET_OFFSET, sizeof(expected_timestamp_getter));
		return trial_failure("PAF timestamp bytes", PAF_TIMESTAMP_GET_OFFSET);
	}
	debug_logf("cache", "paf-clock-getter-verified offset=0x%08X",
		PAF_TIMESTAMP_GET_OFFSET);
	/* Reject a data-layout mismatch before locking or reading an unrelated object. */
	uint32_t data_address = (uint32_t)(uintptr_t)info.segments[1].vaddr;
	if (!matches_mov_address(text + PAF_MUTEX_LOAD_OFFSET,
			text + PAF_MUTEX_LOAD_OFFSET + 6, 5, data_address + PAF_MUTEX_OFFSET) ||
		!matches_mov_address(text + PAF_CLOCK_LOAD_OFFSET,
			text + PAF_CLOCK_LOAD_OFFSET + 4, 1, data_address + PAF_CLOCK_OFFSET)) {
		debug_logf("cache", "paf-data-reference-mismatch data=0x%08X mutex=0x%08X clock=0x%08X",
			data_address, data_address + PAF_MUTEX_OFFSET,
			data_address + PAF_CLOCK_OFFSET);
		debug_log_hex("cache", "paf-mutex-load", PAF_MUTEX_LOAD_OFFSET,
			text + PAF_MUTEX_LOAD_OFFSET, 10);
		debug_log_hex("cache", "paf-clock-load", PAF_CLOCK_LOAD_OFFSET,
			text + PAF_CLOCK_LOAD_OFFSET, 8);
		return trial_failure("PAF cache data references", PAF_MUTEX_OFFSET);
	}
	debug_logf("cache", "paf-data-references-verified mutex=0x%08X clock=0x%08X",
		data_address + PAF_MUTEX_OFFSET, data_address + PAF_CLOCK_OFFSET);
	if (!valid_consumer_code(text, data_address)) {
		debug_logf("cache", "paf-consumer-validation-failed guard=0x%08X",
			(unsigned int)(uintptr_t)&__stack_chk_guard);
		debug_log_hex("cache", "paf-consumer-apply", PAF_APPLY_OFFSET,
			text + PAF_APPLY_OFFSET, 30);
		debug_log_hex("cache", "paf-get-surface", PAF_GET_SURFACE_OFFSET,
			text + PAF_GET_SURFACE_OFFSET, sizeof(expected_get_surface));
		debug_log_hex("cache", "paf-handle-thunk", PAF_HANDLE_GET_OFFSET,
			text + PAF_HANDLE_GET_OFFSET, 14);
		debug_log_hex("cache", "paf-vtable-slot", PAF_HANDLE_VTABLE_OFFSET + 8,
			text + PAF_HANDLE_VTABLE_OFFSET + 8, 4);
#if LIVEAREA_ICON_CACHE_LOGGING
		trial_log_consumer_code(text, data_address);
#endif
		return trial_failure("PAF consumer bytes", PAF_APPLY_OFFSET);
	}
	debug_logf("cache", "paf-consumer-verified apply=0x%08X getter=0x%08X thunk=0x%08X vtable=0x%08X",
		PAF_APPLY_OFFSET, PAF_GET_SURFACE_OFFSET, PAF_HANDLE_GET_OFFSET,
		PAF_HANDLE_VTABLE_OFFSET);
	g_release_surface = (void *)((uintptr_t)text + PAF_RELEASE_OFFSET + 1);
	g_stock_evict = (void *)((uintptr_t)text + PAF_EVICT_OFFSET + 1);
	g_lock_cache = (void *)((uintptr_t)text + PAF_LOCK_OFFSET + 1);
	g_unlock_cache = (void *)((uintptr_t)text + PAF_UNLOCK_OFFSET + 1);
	g_cache_mutex = (uint8_t *)info.segments[1].vaddr + PAF_MUTEX_OFFSET;
	g_cache_clock = (const uint32_t *)((uint8_t *)info.segments[1].vaddr + PAF_CLOCK_OFFSET);
	g_image_handle_vtable = text + PAF_HANDLE_VTABLE_OFFSET;
	g_get_surface = (void *)((uintptr_t)text + PAF_GET_SURFACE_OFFSET + 1);
	debug_logf("cache", "paf-addresses stock_evict=0x%08X release=0x%08X lock=0x%08X unlock=0x%08X mutex=0x%08X clock=0x%08X",
		(unsigned int)(uintptr_t)g_stock_evict,
		(unsigned int)(uintptr_t)g_release_surface,
		(unsigned int)(uintptr_t)g_lock_cache,
		(unsigned int)(uintptr_t)g_unlock_cache,
		(unsigned int)(uintptr_t)g_cache_mutex,
		(unsigned int)(uintptr_t)g_cache_clock);
	/* Do not enable additional evictions without the matching consumer fix. */
	g_apply_hook = taiHookFunctionOffset(&g_apply_ref, paf.modid, 0,
		PAF_APPLY_OFFSET, 1, apply_icon_image);
	debug_logf("cache", "apply-hook result=%d offset=0x%08X",
		g_apply_hook, PAF_APPLY_OFFSET);
	if (g_apply_hook < 0) {
		result = g_apply_hook;
		goto hook_failed;
	}
	g_scan_hook = taiHookFunctionOffset(&g_scan_ref, paf.modid, 0,
		PAF_SCAN_OFFSET, 1, scan_icon_surfaces);
	debug_logf("cache", "scan-hook result=%d offset=0x%08X",
		g_scan_hook, PAF_SCAN_OFFSET);
	if (g_scan_hook < 0) {
		hook_stage = "scan hook";
		result = g_scan_hook;
		int release_result = taiHookRelease(g_apply_hook, g_apply_ref);
		debug_logf("cache", "apply-hook rollback uid=%d result=%d",
			g_apply_hook, release_result);
		(void)release_result;
		g_apply_hook = -1;
		goto hook_failed;
	}
	debug_logf("cache", "hooks-active paf_modid=%d apply_uid=%d scan_uid=%d",
		paf.modid, g_apply_hook, g_scan_hook);
#if LIVEAREA_ICON_CACHE_LOGGING
	static const char active[] = "icon-cache trial active (LRU + consumer reload)\n";
	trial_log(active, sizeof(active) - 1);
#endif
	return 0;

hook_failed:
	debug_logf("cache", "hook-install-failed stage=%s result=%d",
		hook_stage, result);
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
	int result = 0;

	debug_logf("cache", "pool-initializer-enter attempted=%d slot=0x%08X",
		g_install_attempted, (unsigned int)(uintptr_t)g_icon_pool_slot);
	TAI_CONTINUE(void, g_pool_init_ref);
	debug_logf("cache", "pool-initializer-original-complete pool=0x%08X surface_pool=0x%08X",
		(unsigned int)(uintptr_t)(g_icon_pool_slot ? *g_icon_pool_slot : NULL),
		(unsigned int)(uintptr_t)(g_icon_pool_slot && *g_icon_pool_slot
			? (*g_icon_pool_slot)->surface_pool : NULL));
	/* PAF is unavailable at plugin startup; this initializer calls its providers. */
	if (!g_install_attempted) {
		result = install_paf_hook();
		debug_logf("cache", "pool-initializer-install result=%d", result);
	} else {
		debug_logf("cache", "pool-initializer-install skipped attempted=1");
	}
	(void)result;
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
	const uint8_t *entry;
	static const uint8_t pool_store[] = {0x06, 0x60};

	g_install_attempted = 0;
	g_profile = NULL;
#if LIVEAREA_DEBUG_LOGGING
	g_debug_scan_calls = 0;
	g_debug_apply_calls = 0;
	g_debug_evictions = 0;
	g_debug_pending = 0;
	g_debug_scan_errors = 0;
	g_debug_apply_errors = 0;
#endif
	debug_logf("cache", "start shell_modid=%d shell_nid=0x%08X shell_info=0x%08X",
		shell_modid, (unsigned int)shell_nid, (unsigned int)(uintptr_t)shell_info);
	for (size_t i = 0; i < sizeof(cache_profiles) / sizeof(cache_profiles[0]); ++i) {
		if (cache_profiles[i].shell_nid == shell_nid) {
			g_profile = &cache_profiles[i];
			debug_logf("cache", "profile-selected index=%u shell_nid=0x%08X paf_nid=0x%08X shell_text=0x%08X shell_data=0x%08X pool_slot=0x%08X init=0x%08X",
				(unsigned int)i, (unsigned int)g_profile->shell_nid,
				(unsigned int)g_profile->paf_nid,
				(unsigned int)g_profile->text_size,
				(unsigned int)g_profile->data_size,
				(unsigned int)g_profile->pool_slot,
				(unsigned int)g_profile->init_offset);
			break;
		}
	}
#if LIVEAREA_ICON_CACHE_LOGGING
	g_log_fd = sceIoOpen("ur0:/data/livearea_nolimits-icon-cache-trial.log",
		SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
#endif
	if (!g_profile) {
		debug_logf("cache", "profile-missing shell_nid=0x%08X",
			(unsigned int)shell_nid);
		trial_failure("shell identity", shell_nid);
		goto fail;
	}
	debug_logf("cache", "shell-segments text=0x%08X text_size=0x%08X data=0x%08X data_size=0x%08X",
		shell_info ? (unsigned int)(uintptr_t)shell_info->segments[0].vaddr : 0U,
		shell_info ? (unsigned int)shell_info->segments[0].memsz : 0U,
		shell_info ? (unsigned int)(uintptr_t)shell_info->segments[1].vaddr : 0U,
		shell_info ? (unsigned int)shell_info->segments[1].memsz : 0U);
	if (!shell_info || !shell_info->segments[1].vaddr ||
		shell_info->segments[1].memsz != g_profile->data_size) {
		debug_logf("cache", "shell-data-mismatch actual=0x%08X expected=0x%08X",
			shell_info ? (unsigned int)shell_info->segments[1].memsz : 0U,
			(unsigned int)g_profile->data_size);
		trial_failure("shell pool slot", g_profile->pool_slot);
		goto fail;
	}
	if (!shell_info->segments[0].vaddr ||
		shell_info->segments[0].memsz != g_profile->text_size) {
		debug_logf("cache", "shell-text-mismatch actual=0x%08X expected=0x%08X",
			(unsigned int)shell_info->segments[0].memsz,
			(unsigned int)g_profile->text_size);
		trial_failure("shell text segment", g_profile->text_size);
		goto fail;
	}
	entry = (const uint8_t *)shell_info->segments[0].vaddr + g_profile->init_offset;
	/* Check the relocated pool reference even when the pool already exists. */
	if (!valid_pool_init_entry(entry) ||
		!matches_mov_address(entry + 0x1E, entry + 0x22, 0,
			(uint32_t)(uintptr_t)shell_info->segments[1].vaddr + g_profile->pool_slot) ||
		!matches(entry + 0x26, pool_store, sizeof(pool_store))) {
		debug_logf("cache", "shell-initializer-mismatch offset=0x%08X expected_pool=0x%08X",
			(unsigned int)g_profile->init_offset,
			(unsigned int)(uintptr_t)shell_info->segments[1].vaddr + g_profile->pool_slot);
		debug_log_hex("cache", "shell-initializer", g_profile->init_offset,
			entry, 0x28);
		trial_failure("shell initializer bytes", g_profile->init_offset);
		goto fail;
	}
	debug_logf("cache", "shell-initializer-verified offset=0x%08X pool_address=0x%08X",
		(unsigned int)g_profile->init_offset,
		(unsigned int)(uintptr_t)shell_info->segments[1].vaddr + g_profile->pool_slot);
	g_icon_pool_slot = (IconPool *volatile *)
		((uintptr_t)shell_info->segments[1].vaddr + g_profile->pool_slot);
	debug_logf("cache", "pool-slot address=0x%08X pool=0x%08X surface_pool=0x%08X",
		(unsigned int)(uintptr_t)g_icon_pool_slot,
		(unsigned int)(uintptr_t)*g_icon_pool_slot,
		(unsigned int)(uintptr_t)(*g_icon_pool_slot ? (*g_icon_pool_slot)->surface_pool : NULL));
	if (*g_icon_pool_slot && (*g_icon_pool_slot)->surface_pool) {
		debug_logf("cache", "pool-ready immediate-install");
		result = install_paf_hook();
		debug_logf("cache", "immediate-install result=%d", result);
		if (result < 0)
			goto fail;
		return 0;
	}
	g_pool_init_hook = taiHookFunctionOffset(&g_pool_init_ref, shell_modid, 0,
		g_profile->init_offset, 1, initialize_icon_pool);
	debug_logf("cache", "pool-hook result=%d offset=0x%08X",
		g_pool_init_hook, (unsigned int)g_profile->init_offset);
	if (g_pool_init_hook < 0) {
		trial_failure("shell initializer hook", (uint32_t)g_pool_init_hook);
		goto fail;
	}
	debug_logf("cache", "waiting-for-pool hook_uid=%d", g_pool_init_hook);
#if LIVEAREA_ICON_CACHE_LOGGING
	static const char waiting[] = "icon-cache trial waiting for icon pool\n";
	trial_log(waiting, sizeof(waiting) - 1);
#endif
	return 0;

fail:
	debug_logf("cache", "start-failed result=%d", result);
	icon_cache_trial_stop();
	return result;
}
