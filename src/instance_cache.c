#include "instance_cache.h"
#include "shell_detour.h"
#include "debug_log.h"
#include <psp2/kernel/threadmgr/lw_mutex.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdatomic.h>
#include <taihen.h>

extern uint32_t __stack_chk_guard;
uintptr_t g_instance_pool_resume, g_instance_request_resume;
extern void instance_pool_original(void);
extern int instance_request_original(void *, void **, void *, unsigned int, unsigned int);

typedef struct { uint8_t reserved[24]; int32_t references; } Surface;
typedef struct { uint8_t reserved[84]; void *surface_pool; } Cache;
typedef struct { const uintptr_t *vtable; } Handle;
typedef struct Image {
	const uintptr_t *vtable;
	Cache *cache;
	uint8_t kind, flags, state, reserved;
	int32_t result, references;
	uint8_t before_time[56];
	uint32_t last_used;
	uint8_t reserved80, registered, reserved82[2];
	int32_t retries_left, retry_limit;
	uint8_t before_handle[8];
	Handle handle;
	uint8_t before_surface[52];
	Surface *surface;
} Image;
typedef struct { void *memory, *surface_pool; } IconPool;
typedef struct { const uintptr_t *vtable; void *allocator, *memory; uint32_t bytes; } Pool;
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(offsetof(Image, handle) == 100 && offsetof(Image, surface) == 156, "image ABI");
_Static_assert(offsetof(Image, references) == 16 && offsetof(Image, registered) == 81, "image lifetime ABI");
_Static_assert(offsetof(Cache, surface_pool) == 84 && offsetof(Pool, bytes) == 12, "pool ABI");
#endif
_Static_assert(offsetof(Surface, references) == 24, "surface ABI");

typedef struct { uint32_t shell, paf, text, data, pool_slot, initialize, request; } Profile;
static const Profile profiles[] = {
	{0x0552F692,0xCD679177,0x541B74,0x93FAC,0x6DEC,0x2C74,0xBD34E},
	{0x5549BF1F,0x73F90499,0x5420F4,0x93FBC,0x6DFC,0x2CCC,0xBD3A6},
	{0xEAB89D5C,0xCD679177,0x535CF4,0x92D1C,0x6BBC,0x2C74,0xBA976}
};
#include "instance_cache_firmware.h"

#define MAX_IMAGES 8192U
#define MAX_PINS 32U
typedef struct { SceUID thread; Image *image; Surface *surface; unsigned int busy; } Pin;
static const Profile *profile;
static IconPool *volatile *icon_pool_slot;
static _Atomic(Pool *) pool;
static const uintptr_t *original_pool, *original_primary, *original_handle;
static uintptr_t pool_table[7], image_table[30];
static Image *images[MAX_IMAGES];
static atomic_uint image_count;
static Pin pins[MAX_PINS];
static SceKernelLwMutexWork *cache_mutex, *surface_mutex;
static int (*lock_cache)(void *), (*unlock_cache)(void *);
static int (*get_surface)(Surface **, Image *);
static int (*release_surface)(Surface *);
static const uint32_t *cache_clock;
static uintptr_t apply_status_return;
static SceUID entry_ids[2] = {-1,-1};
static atomic_int ready;
static atomic_flag setup_busy = ATOMIC_FLAG_INIT;
static int setup_attempts, allocation_busy;
static unsigned int registered_total, evictions, retry_success;
static atomic_uint allocation_failures, lock_skips;
static unsigned int pending, transfers, stale_pins, rejected;

static int sample(unsigned int n) { return n <= 16 || !(n & 63U); }
static Image *from_handle(Handle *handle) { return (Image *)((uint8_t *)handle - offsetof(Image, handle)); }
static int owned(const Image *image)
{
	return ready && pool && icon_pool_slot && *icon_pool_slot &&
		(*icon_pool_slot)->surface_pool == pool && image && image->kind == 1 &&
		image->cache && image->cache->surface_pool == pool;
}
static void clear_pin(Pin *pin)
{
	Surface *surface = pin->surface;
	memset(pin, 0, sizeof(*pin));
	if (surface) release_surface(surface);
}
static Pin *thread_pin(SceUID thread)
{
	Pin *empty = NULL;
	for (unsigned int i=0;i<MAX_PINS;++i) {
		if (pins[i].thread == thread) return &pins[i];
		if (!pins[i].thread && !empty) empty = &pins[i];
	}
	return empty;
}

/* Only the native consumer's inner status check obtains a transient surface
 * pin. Returning pending there preserves its existing queue subscription. */
static int status_for_caller(Handle *handle, uintptr_t caller)
{
	int result = ((int (*)(Handle *))original_handle[4])(handle);
	if (!ready || caller != apply_status_return || result != 2) return result;
	Image *image = from_handle(handle);
	if (lock_cache(cache_mutex) < 0) return 1;
	if (!owned(image) || !image->registered || image->result ||
		(!image->surface && (!image->retry_limit || !image->retries_left))) {
		unlock_cache(cache_mutex); return result;
	}
	SceUID thread = sceKernelGetThreadId();
	Pin *pin = thread_pin(thread);
	if (!pin) { ++rejected; unlock_cache(cache_mutex); return 1; }
	if (pin->busy) { unlock_cache(cache_mutex); return 1; }
	if (pin->surface) { ++stale_pins; clear_pin(pin); }
	pin->thread = thread; pin->image = image; pin->busy = 1;
	Surface *held = NULL;
	get_surface(&held, image);
	pin->busy = 0;
	if (held) {
		pin->surface = held;
	} else {
		memset(pin, 0, sizeof(*pin));
		result = 1;
		++pending;
		if (sample(pending)) debug_logf("instance-cache", "pending count=%u image=0x%08X state=%u result=%d",
			pending,(unsigned int)(uintptr_t)image,image->state,image->result);
	}
	unlock_cache(cache_mutex);
	return result;
}
static int image_status(Handle *handle)
{
	return status_for_caller(handle, (uintptr_t)__builtin_return_address(0) & ~(uintptr_t)1);
}
static int image_get_surface(Surface **out, Handle *handle)
{
	Image *image = from_handle(handle);
	if (lock_cache(cache_mutex) >= 0) {
		Pin *pin = thread_pin(sceKernelGetThreadId());
		if (pin && pin->image == image && pin->surface) {
			/* Transfer the reference to native ApplyImageHandleToWidget's local
			 * SurfacePtr, which releases it after binding/callbacks. */
			*out = pin->surface; memset(pin, 0, sizeof(*pin)); ++transfers;
			unlock_cache(cache_mutex); return 0;
		}
		unlock_cache(cache_mutex);
	}
	return ((int (*)(Surface **, Handle *))original_handle[2])(out, handle);
}

/* All native final-release routes consult either primary or interface virtual
 * destructors. Remove weak records and restore both tables before delegation. */
static uintptr_t destroy_image(Image *image, unsigned int slot, int interface)
{
	lock_cache(cache_mutex);
	for (unsigned int i=0;i<MAX_PINS;++i)
		if (pins[i].image == image) clear_pin(&pins[i]);
	for (unsigned int i=0;i<image_count;++i) if (images[i] == image) {
		images[i] = images[--image_count]; images[image_count] = NULL; break;
	}
	__atomic_store_n(&image->handle.vtable, original_handle, __ATOMIC_RELEASE);
	__atomic_store_n(&image->vtable, original_primary, __ATOMIC_RELEASE);
	uintptr_t result;
	if (interface) result = ((uintptr_t (*)(Handle *))original_handle[slot])(&image->handle);
	else result = ((uintptr_t (*)(Image *))original_primary[slot])(image);
	unlock_cache(cache_mutex);
	return result;
}
static uintptr_t image_dtor1(Image *p) { return destroy_image(p,1,0); }
static uintptr_t image_dtor2(Image *p) { return destroy_image(p,2,0); }
static uintptr_t image_dtor3(Image *p) { return destroy_image(p,3,0); }
static uintptr_t handle_dtor0(Handle *p) { return destroy_image(from_handle(p),0,1); }
static uintptr_t handle_dtor1(Handle *p) { return destroy_image(from_handle(p),1,1); }

static void register_image(Handle *handle)
{
	if (!ready || !handle || handle->vtable == &image_table[20]) return;
	if (handle->vtable != original_handle) return;
	Image *image = from_handle(handle);
#if UINTPTR_MAX == UINT32_MAX
	if ((uintptr_t)image >= 0xE0000000U) return;
#endif
	if (lock_cache(cache_mutex) < 0) return;
	if (handle->vtable == original_handle && image->vtable == original_primary && owned(image)) {
		if (image_count < MAX_IMAGES) {
			images[image_count++] = image;
			__atomic_store_n(&image->vtable, &image_table[2], __ATOMIC_RELEASE);
			__atomic_store_n(&image->handle.vtable, &image_table[20], __ATOMIC_RELEASE);
			++registered_total;
			if (sample(registered_total)) debug_logf("instance-cache", "image-registered total=%u live=%u image=0x%08X",
				registered_total,image_count,(unsigned int)(uintptr_t)image);
		} else ++rejected;
	}
	unlock_cache(cache_mutex);
}

static int eligible(const Image *image)
{
	return owned(image) && image->vtable == &image_table[2] &&
		image->handle.vtable == &image_table[20] && image->state == 2 && image->references >= 1 &&
		(image->references == 1 || (image->registered && image->retries_left > 0 && image->retry_limit > 0)) &&
		image->surface && image->surface->references == 1;
}
static void *pool_allocate(Pool *self, unsigned int alignment, unsigned int size)
{
	void *(*native)(Pool *,unsigned int,unsigned int) = (void *)original_pool[2];
	void *result = native(self,alignment,size);
	if (result || !ready || self != pool || !size || size > self->bytes ||
		(alignment && (alignment & (alignment-1)))) return result;
	++allocation_failures;
	/* Caller may hold PAF's surface-global mutex. Never wait for the cache
	 * mutex in reverse order: recursive acquisition or immediate failure only. */
	if (sceKernelTryLockLwMutex(cache_mutex,1) < 0) {
		unsigned int skipped = ++lock_skips;
		if (sample(skipped)) debug_logf("instance-cache", "allocation-lock-skipped count=%u bytes=%u",skipped,size);
		return NULL;
	}
	if (allocation_busy) { unlock_cache(cache_mutex); return NULL; }
	allocation_busy = 1;
	unsigned int budget = image_count, freed = 0;
	while (!result && budget--) {
		Image *victim = NULL; uint32_t oldest = 0, now = *cache_clock;
		unsigned int count = image_count;
		for (unsigned int i=0;i<count;++i) if (eligible(images[i])) {
			uint32_t age = now-images[i]->last_used;
			if (!victim || age > oldest) { victim=images[i]; oldest=age; }
		}
		if (!victim || !eligible(victim)) break;
		Surface *surface = victim->surface;
		victim->surface = NULL;
		release_surface(surface); ++evictions; ++freed;
		result = native(self,alignment,size);
	}
	allocation_busy = 0;
	if (result) ++retry_success;
	(void)freed; /* Only formatted by diagnostic builds. */
	if (sample(allocation_failures) || !result) debug_logf("instance-cache",
		"allocation-retry count=%u bytes=%u freed=%u success=%u evictions=%u tracked=%u pins_transferred=%u pending=%u lock_skips=%u stale_pins=%u",
		allocation_failures,size,freed,result!=NULL,evictions,image_count,transfers,pending,lock_skips,stale_pins);
	unlock_cache(cache_mutex);
	return result;
}
static uintptr_t destroy_pool(Pool *self, unsigned int slot)
{
	/* Native pool destruction requires all users to have finished. Stop new
	 * policy work; image destructor callbacks remain valid until full reboot. */
	ready = 0; pool = NULL;
	__atomic_store_n(&self->vtable, original_pool, __ATOMIC_RELEASE);
	debug_logf("instance-cache", "pool-destroy slot=%u tracked=%u",slot,image_count);
	debug_log_flush();
	return ((uintptr_t (*)(Pool *))original_pool[slot])(self);
}
static uintptr_t pool_dtor0(Pool *p) { return destroy_pool(p,0); }
static uintptr_t pool_dtor1(Pool *p) { return destroy_pool(p,1); }

static void encode_mov(uint8_t *p,unsigned op,unsigned reg,uint16_t value)
{
	uint16_t a=op|(value>>12)|((value>>1)&0x400), b=((value<<4)&0x7000)|(reg<<8)|(value&255);
	p[0]=a;p[1]=a>>8;p[2]=b;p[3]=b>>8;
}
static int recursive_mutex(SceKernelLwMutexWork *mutex)
{
	if (sceKernelTryLockLwMutex(mutex,1) < 0) return 0;
	int result=sceKernelTryLockLwMutex(mutex,1);
	if (result>=0) sceKernelUnlockLwMutex(mutex,1);
	sceKernelUnlockLwMutex(mutex,1);
	return result>=0;
}
static int table_matches(const uintptr_t *table,const uint32_t *expected,unsigned count,uintptr_t text)
{
	for (unsigned i=0;i<count;++i) {
		uintptr_t value=expected[i];
		if (value && value<0x300D00U) value+=text;
		if (table[i]!=value) return 0;
	}
	return 1;
}
#if LIVEAREA_DEBUG_LOGGING
static uint32_t hash_bytes(const uint8_t *p,unsigned count)
{
	uint32_t hash=2166136261U;
	for(unsigned i=0;i<count;++i) hash=(hash^p[i])*16777619U;
	return hash;
}
#endif
static int setup_instances_inner(void)
{
	if (ready) return 0;
	if (!icon_pool_slot || !*icon_pool_slot || !(*icon_pool_slot)->surface_pool) return -1;
	if (++setup_attempts>8) return -1;
	tai_module_info_t module={0}; SceKernelModuleInfo info={0};
	module.size=sizeof(module);info.size=sizeof(info);
	if(taiGetModuleInfo("ScePaf",&module)<0 || module.module_nid!=profile->paf ||
		sceKernelGetModuleInfo(module.modid,&info)<0 || info.segments[0].memsz!=0x300D00 ||
		!info.segments[0].vaddr || !info.segments[1].vaddr || info.segments[1].memsz<0x10250) goto bad;
	uint8_t *text=info.segments[0].vaddr;uintptr_t base=(uintptr_t)text;
	Pool *candidate=(*icon_pool_slot)->surface_pool;
#if UINTPTR_MAX == UINT32_MAX
	if ((uintptr_t)candidate>=0xE0000000U) goto bad;
#endif
	const uintptr_t *pt=(void *)(text+0x2E3960),*it=(void *)(text+0x2E101C);
	if(candidate->bytes!=2097152 || candidate->vtable!=pt+2 ||
		!table_matches(pt,expected_pool_table,7,base) || !table_matches(it,expected_image_table,30,base) ||
		memcmp(text+0x8DD5A,expected_pool_allocate,sizeof(expected_pool_allocate)) ||
		memcmp(text+0x8997A,expected_surface_release,sizeof(expected_surface_release)) ||
		memcmp(text+0x13344,expected_get_surface,sizeof(expected_get_surface)) ||
		memcmp(text+0x15FA08,expected_apply_status_call,sizeof(expected_apply_status_call))) goto bad;
	cache_mutex=(void *)((uint8_t *)info.segments[1].vaddr+0xB238);
	surface_mutex=(void *)((uint8_t *)info.segments[1].vaddr+0x10230);
	if(!recursive_mutex(cache_mutex)||!recursive_mutex(surface_mutex)) {
		debug_logf("instance-cache","mutex-check-deferred attempt=%d",setup_attempts);return -1;
	}
	lock_cache=(void *)(base+0x53E91);unlock_cache=(void *)(base+0x53EA5);
	get_surface=(void *)(base+0x13345);release_surface=(void *)(base+0x8997B);
	cache_clock=(void *)((uint8_t *)info.segments[1].vaddr+0xB224);
	apply_status_return=base+0x15FA12;
	original_pool=pt+2;original_primary=it+2;original_handle=it+20;
	memcpy(pool_table,pt,sizeof(pool_table));memcpy(image_table,it,sizeof(image_table));
	pool_table[2]=(uintptr_t)pool_dtor0;pool_table[3]=(uintptr_t)pool_dtor1;pool_table[4]=(uintptr_t)pool_allocate;
	image_table[3]=(uintptr_t)image_dtor1;image_table[4]=(uintptr_t)image_dtor2;image_table[5]=(uintptr_t)image_dtor3;
	image_table[20]=(uintptr_t)handle_dtor0;image_table[21]=(uintptr_t)handle_dtor1;
	image_table[22]=(uintptr_t)image_get_surface;image_table[24]=(uintptr_t)image_status;
#if LIVEAREA_DEBUG_LOGGING
	uint32_t scan_hash=hash_bytes(text+0x15E6A,32),apply_hash=hash_bytes(text+0x15F9E2,32);
#endif
	pool=candidate;ready=1;
	__atomic_store_n(&pool->vtable,&pool_table[2],__ATOMIC_RELEASE);
	debug_logf("instance-cache","active pool=0x%08X private_vtable=0x%08X paf_hooks=0 pool_bytes=%u recursive_mutexes=1 scan_hash=0x%08X apply_hash=0x%08X text_unchanged=%u",
		(unsigned)(uintptr_t)pool,(unsigned)(uintptr_t)pool->vtable,pool->bytes,scan_hash,apply_hash,
		scan_hash==hash_bytes(text+0x15E6A,32)&&apply_hash==hash_bytes(text+0x15F9E2,32));
	debug_log_flush();return 0;
bad:
	setup_attempts=9;debug_logf("instance-cache","PAF-or-pool-validation-failed");debug_log_flush();return -1;
}
static int setup_instances(void)
{
	if (atomic_flag_test_and_set_explicit(&setup_busy,memory_order_acquire)) return -1;
	int result=setup_instances_inner();
	atomic_flag_clear_explicit(&setup_busy,memory_order_release);
	return result;
}
static void initialize_pool(void)
{
	instance_pool_original();
	setup_instances();
}
static int request_image(void *path,void **out,void *options,unsigned int arg4,unsigned int arg5)
{
	if(!ready) setup_instances();
	int result=instance_request_original(path,out,options,arg4,arg5);
	if(!ready) setup_instances();
	if(!result && out && *out) register_image(*out);
	return result;
}
int instance_cache_can_unload(void)
{
	return entry_ids[0]<0 && entry_ids[1]<0 && !image_count && !pool;
}
int instance_cache_start(SceUID module,uint32_t nid,const SceKernelModuleInfo *info)
{
	if(!instance_cache_can_unload()) return -1;
	profile = NULL;
	for(unsigned i=0;i<sizeof(profiles)/sizeof(profiles[0]);++i) if(profiles[i].shell==nid) profile=&profiles[i];
	if(!profile||!info||!info->segments[0].vaddr||!info->segments[1].vaddr||
		info->segments[0].memsz!=profile->text||info->segments[1].memsz!=profile->data) return -1;
	uintptr_t base=(uintptr_t)info->segments[0].vaddr;
	uint8_t init[16]={0x2d,0xe9,0xf0,0x41,0x8a,0xb0,0,0,0,0,0,0,0,0,0xd8,0xf8};
	uint8_t request[16]={0x2d,0xe9,0xf0,0x43,0x99,0xb0,0x0f,0x1c,0,0,0,0,0,0,0,0};
	uintptr_t guard=(uintptr_t)&__stack_chk_guard;
	encode_mov(init+6,0xf240,8,guard);encode_mov(init+10,0xf2c0,8,guard>>16);
	encode_mov(request+8,0xf240,9,guard);encode_mov(request+12,0xf2c0,9,guard>>16);
	if(memcmp((void *)(base+profile->initialize),init,16)||memcmp((void *)(base+profile->request),request,16)) {
		debug_logf("instance-cache","shell-prefix-validation-failed");return -1;
	}
	icon_pool_slot=(void *)((uint8_t *)info->segments[1].vaddr+profile->pool_slot);
	g_instance_pool_resume=(base+profile->initialize+4)|1;
	g_instance_request_resume=(base+profile->request+4)|1;
	entry_ids[0]=shell_detour_install(module,profile->initialize,base+profile->initialize,initialize_pool);
	if(entry_ids[0]<0) return -1;
	entry_ids[1]=shell_detour_install(module,profile->request,base+profile->request,request_image);
	if(entry_ids[1]<0) {
		if(taiInjectRelease(entry_ids[0])>=0) entry_ids[0]=-1;
		return -1;
	}
	debug_logf("instance-cache","shell-detours-installed init=0x%08X request=0x%08X paf_hooks=0",profile->initialize,profile->request);
	if(*icon_pool_slot && (*icon_pool_slot)->surface_pool) setup_instances();
	return 0;
}
