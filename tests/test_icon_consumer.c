/* Share the same firmware/OS stubs and ABI assumptions as the policy tests. */
#define main policy_test_main
#include "test_icon_cache_trial.c"
#undef main

typedef struct {
	int pending_entry;
	int pending_binding;
	int failures;
	PafSurface *artwork;
} TestWidget;

static PafImage *consumer_image;
static unsigned int queued_loads;
static unsigned int getter_calls;
static unsigned int pin_checks;
static int queue_failure;
static PafSurface *completion_surface;
static void *seen_widget;
static void **seen_handle;
static int seen_object;
static int seen_texture;

static unsigned int release_consumer_surface(PafSurface *surface)
{
	assert(surface && surface->references > 0);
	return --surface->references;
}

static void get_consumer_surface(PafSurface **out, PafImage *image)
{
	++getter_calls;
	assert(lock_cache(g_cache_mutex) == 0);
	if (image->cache && image->registered) {
		if (!image->surface && image->load_state == 2 &&
			image->retry_limit && image->retries_left && !queue_failure) {
			image->load_state = 1;
			++queued_loads;
		}
		*out = image->surface;
		if (*out)
			++(*out)->references;
	} else {
		*out = NULL;
	}
	unlock_cache(g_cache_mutex);
}

/* The firmware clears binding bookkeeping on result zero even without artwork. */
static int stock_consumer(void *opaque, void **handle, int object, int texture)
{
	TestWidget *widget = opaque;
	assert(handle && *handle && object == 2 && texture == 1);
	assert(!g_mutex_depth);
	if (consumer_image->load_state != 2)
		return -1;
	PafSurface *surface = NULL;
	get_consumer_surface(&surface, consumer_image);
	int result = 0;
	if (surface) {
		widget->artwork = surface;
	} else {
		++widget->failures;
		result = consumer_image->result;
	}
	if (!result || result == (int32_t)0x80AF5022U)
		widget->pending_binding = 0;
	return result;
}

static void dispatch_consumer(TestWidget *widget, void **handle)
{
	if (!widget->pending_entry || consumer_image->load_state != 2)
		return;
	int result = apply_icon_image(widget, handle, 2, 1);
	if (!result || result == (int32_t)0x80AF5022U)
		widget->pending_entry = 0;
}

static void reset_widget(TestWidget *widget)
{
	if (widget->artwork)
		release_consumer_surface(widget->artwork);
	*widget = (TestWidget){1, 1, 0, NULL};
}

static void complete_load(void)
{
	assert(consumer_image->load_state == 1 && !consumer_image->surface);
	assert(!completion_surface->references);
	completion_surface->references = 1;
	consumer_image->surface = completion_surface;
	consumer_image->load_state = 2;
	consumer_image->result = 0;
}

static void try_evict_pinned_image(void)
{
	assert(consumer_image->surface && consumer_image->surface->references == 2);
	assert(scan_icon_surfaces(consumer_image->cache, original_evict, NULL) == 0);
	++pin_checks;
}

static int delegated_consumer(void *widget, void **handle, int object, int texture)
{
	assert(!g_mutex_depth);
	seen_widget = widget;
	seen_handle = handle;
	seen_object = object;
	seen_texture = texture;
	return -29;
}

static void check_delegated(void *widget, void **handle)
{
	unsigned int calls = getter_calls;
	assert(apply_icon_image(widget, handle, -7, 13) == -29);
	assert(seen_widget == widget && seen_handle == handle && seen_object == -7 && seen_texture == 13);
	assert(getter_calls == calls && !g_mutex_depth);
}

int main(void)
{
	IconPool pool = {0};
	IconPool *pool_pointer = &pool;
	PafCache cache = {0};
	PafSurface surface = {0};
	PafImage image = {0};
	TestWidget widget = {1, 1, 0, NULL};
	pool.surface_pool = &pool;
	cache.surface_pool = &pool;
	image.cache = &cache;
	image.kind = 1;
	image.load_state = 2;
	image.references = 3;
	image.registered = 1;
	image.retries_left = 10;
	image.retry_limit = 10;
	image.surface = &surface;
	surface.references = 1;
	consumer_image = &image;
	completion_surface = &surface;
	g_paf_data = calloc(1, g_paf_data_size);
	assert(g_paf_data);
	g_icon_pool_slot = &pool_pointer;
	g_cache_mutex = g_paf_data + PAF_MUTEX_OFFSET;
	g_cache_clock = (const uint32_t *)(g_paf_data + PAF_CLOCK_OFFSET);
	g_lock_cache = lock_cache;
	g_unlock_cache = unlock_cache;
	g_release_surface = release_consumer_surface;
	g_stock_evict = original_evict;
	g_scan_ref = 123;
	g_consumer_continue = stock_consumer;
	g_multiple_items = &image;
	g_multiple_count = 1;
	static const int canonical_vtable;
	g_apply_ref = 125;
	g_image_handle_vtable = &canonical_vtable;
	g_get_surface = get_consumer_surface;
	image.handle.vtable = &canonical_vtable;
	void *handle = &image.handle;
#if LIVEAREA_DEBUG_LOGGING
	test_debug_capture_reset();
#endif
	/* A negative control preserves the exact failure this wrapper must prevent. */
	assert(scan_icon_surfaces(&cache, original_evict, NULL) == 1);
	int stock_result = stock_consumer(&widget, &handle, 2, 1);
	if (!stock_result)
		widget.pending_entry = 0;
	assert(!stock_result && !widget.pending_binding && !widget.artwork && image.load_state == 1);
	complete_load();
	dispatch_consumer(&widget, &handle);
	assert(!widget.artwork);
	reset_widget(&widget);
	dispatch_consumer(&widget, &handle);
#if LIVEAREA_DEBUG_LOGGING
	assert(test_debug_capture_contains("[cache] apply-enter"));
	assert(test_debug_capture_contains("[cache] apply-exit"));
#endif
	assert(widget.artwork == &surface);
	reset_widget(&widget);
	queued_loads = 0;
	assert(scan_icon_surfaces(&cache, original_evict, NULL) == 1);
	assert(!image.surface && image.result == 0 && image.load_state == 2);
	dispatch_consumer(&widget, &handle);
	assert(queued_loads == 1 && image.load_state == 1);
	assert(widget.pending_entry && widget.pending_binding && !widget.artwork);
	assert(widget.failures == 0);
	surface.references = 1;
	image.surface = &surface;
	image.load_state = 2;
	dispatch_consumer(&widget, &handle);
	assert(!widget.pending_entry && !widget.pending_binding && widget.artwork == &surface);
	assert(surface.references == 2 && image.retries_left == 10 && !g_mutex_depth);
	for (unsigned int i = 0; i < 32; ++i) {
		reset_widget(&widget);
		assert(scan_icon_surfaces(&cache, original_evict, NULL) == 1);
		dispatch_consumer(&widget, &handle);
		assert(widget.pending_entry && widget.pending_binding && !widget.failures);
		unsigned int jobs = queued_loads;
		dispatch_consumer(&widget, &handle);
		assert(queued_loads == jobs);
		complete_load();
		dispatch_consumer(&widget, &handle);
		assert(widget.artwork == &surface && surface.references == 2);
		assert(!widget.pending_entry && !widget.pending_binding && !widget.failures);
		assert(image.result == 0 && image.retries_left == 10 && image.retry_limit == 10);
	}
	reset_widget(&widget);
	assert(scan_icon_surfaces(&cache, original_evict, NULL) == 1);
	g_cache_unlocked = complete_load;
	dispatch_consumer(&widget, &handle);
	assert(image.load_state == 2 && widget.pending_entry && widget.pending_binding);
	assert(!widget.artwork && !widget.failures);
	dispatch_consumer(&widget, &handle);
	assert(widget.artwork == &surface && surface.references == 2);
	reset_widget(&widget);
	g_cache_unlocked = try_evict_pinned_image;
	dispatch_consumer(&widget, &handle);
	assert(pin_checks == 1 && widget.artwork == &surface && surface.references == 2);
	reset_widget(&widget);
	assert(scan_icon_surfaces(&cache, original_evict, NULL) == 1);
	queue_failure = 1;
	dispatch_consumer(&widget, &handle);
	assert(image.load_state == 2 && widget.pending_entry && widget.pending_binding && !widget.failures);
	queue_failure = 0;
	dispatch_consumer(&widget, &handle);
	assert(image.load_state == 1 && widget.pending_entry && widget.pending_binding);
	complete_load();
	widget.pending_entry = widget.pending_binding = 0;
	dispatch_consumer(&widget, &handle);
	assert(!widget.artwork && surface.references == 1);
	reset_widget(&widget);
	dispatch_consumer(&widget, &handle);
	assert(widget.artwork == &surface && surface.references == 2);
	reset_widget(&widget);
	release_consumer_surface(&surface);
	image.surface = NULL;
	image.result = (int32_t)0x80AF5022U;
	image.retries_left = 0;
	unsigned int jobs = queued_loads;
	dispatch_consumer(&widget, &handle);
	assert(!widget.artwork && !widget.pending_entry && !widget.pending_binding && widget.failures == 1);
	assert(image.result == (int32_t)0x80AF5022U && !image.retries_left && jobs == queued_loads);
	reset_widget(&widget);
	image.result = -123;
	image.retries_left = 3;
	dispatch_consumer(&widget, &handle);
	assert(widget.failures == 1 && widget.pending_entry && widget.pending_binding && image.load_state == 1);
	assert(image.result == -123 && image.retries_left == 3);
	complete_load();
	dispatch_consumer(&widget, &handle);
	assert(widget.artwork == &surface && surface.references == 2);
	reset_widget(&widget);
	release_consumer_surface(&surface);
	image.surface = NULL;
	g_consumer_continue = delegated_consumer;
	image.result = -123;
	check_delegated(&widget, &handle);
	image.result = 0;
	image.retries_left = 0;
	check_delegated(&widget, &handle);
	image.retries_left = 10;
	image.retry_limit = 0;
	check_delegated(&widget, &handle);
	image.retry_limit = 10;
	image.registered = 0;
	check_delegated(&widget, &handle);
	image.registered = 1;
	image.kind = 0;
	check_delegated(&widget, &handle);
	image.kind = 1;
	cache.surface_pool = NULL;
	check_delegated(&widget, &handle);
	cache.surface_pool = &pool;
	image.cache = NULL;
	check_delegated(&widget, &handle);
	image.cache = &cache;
	pool_pointer = NULL;
	check_delegated(&widget, &handle);
	pool_pointer = &pool;
	check_delegated(NULL, &handle);
	check_delegated(&widget, NULL);
	void *null_handle = NULL;
	check_delegated(&widget, &null_handle);
	PafImageHandle *other = calloc(1, sizeof(*other));
	assert(other);
	static const int other_vtable = 1;
	other->vtable = &other_vtable;
	void *other_handle = other;
	check_delegated(&widget, &other_handle);
	free(other);
	g_mutex_failure = 1;
	unsigned int calls = getter_calls;
	assert(apply_icon_image(&widget, &handle, 2, 1) == -1);
	assert(getter_calls == calls && !g_mutex_depth && !g_log_writes);
	g_mutex_failure = 0;
	assert(surface.references == 0 && !g_cache_unlocked);
	free(g_paf_data);
	puts("Consumer reload: stale-success control, pending subscription, 32 reload cycles, fast completion, pinned handoff, cancellation, queue failure, real errors and delegation passed");
	return 0;
}
