#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/instance_cache.c"
#include "debug_capture.h"

uint32_t __stack_chk_guard;
static uintptr_t stock_pool[7],stock_images[30];
static IconPool shell_pool;
static Pool object,foreign_pool;
static Cache cache,other_cache;
static SceKernelLwMutexWork mutex,global_mutex;
static uint32_t clock_value;
static SceUID current_thread,mutex_owner;
static unsigned int mutex_depth,free_bytes,native_allocations,releases,getters,destructions;
static int mutex_contended,recursive_probe,recursive_result;
static Image a,b,c,foreign_image;
static struct { Surface s; unsigned int bytes; } surfaces[4];

int sceKernelGetThreadId(void) { return current_thread; }
int sceKernelTryLockLwMutex(SceKernelLwMutexWork *which,int count)
{
	assert(which==&mutex || which==&global_mutex); assert(count==1);
	if(mutex_contended) return -1;
	assert(!mutex_owner || mutex_owner==current_thread);
	mutex_owner=current_thread; ++mutex_depth; return 0;
}
int sceKernelUnlockLwMutex(SceKernelLwMutexWork *which,int count)
{
	assert(which==&mutex || which==&global_mutex);assert(count==1 && mutex_depth && mutex_owner==current_thread);
	if(!--mutex_depth) mutex_owner=0;return 0;
}
static int lock(void *which) { assert(!mutex_contended);return sceKernelTryLockLwMutex(which,1); }
static int unlock(void *which) { return sceKernelUnlockLwMutex(which,1); }
static int surface_release(Surface *surface)
{
	assert(surface->references>0);++releases;
	if(!--surface->references) {
		for(unsigned int i=0;i<4;++i)if(surface==&surfaces[i].s) {free_bytes+=surfaces[i].bytes;return 0;}
		assert(0);
	}
	return surface->references;
}
static int raw_getter(Surface **out,Image *image)
{
	assert(mutex_depth && mutex_owner==current_thread);++getters;
	if(recursive_probe) {
		recursive_probe=0;
		recursive_result=status_for_caller(&b.handle,apply_status_return);
	}
	*out=image->surface;
	if(*out)++(*out)->references;
	else image->state=1;
	return 0;
}
static int handle_getter(Surface **out,Handle *handle)
{
	lock(&mutex);int r=raw_getter(out,from_handle(handle));unlock(&mutex);return r;
}
static int handle_status(Handle *handle) { return from_handle(handle)->state; }
static uintptr_t native_destroy(Image *image)
{
	assert(image->vtable==original_primary && image->handle.vtable==original_handle);
	assert(mutex_depth);++destructions;return 0x1234;
}
static uintptr_t native_handle_destroy(Handle *h) { return native_destroy(from_handle(h)); }
static void *native_allocate(Pool *self,unsigned int alignment,unsigned int bytes)
{
	assert(self==&object || self==&foreign_pool);(void)alignment;++native_allocations;
	if(free_bytes<bytes)return NULL;
	free_bytes-=bytes;return (void *)(uintptr_t)0x7770;
}
static uintptr_t native_pool_destroy(Pool *p) { assert(p->vtable==original_pool);return 0x6789; }

static void reset(void)
{
	memset(images,0,sizeof(images));memset(pins,0,sizeof(pins));
	memset(stock_pool,0,sizeof(stock_pool));memset(stock_images,0,sizeof(stock_images));
	memset(surfaces,0,sizeof(surfaces));
	original_pool=stock_pool+2;original_primary=stock_images+2;original_handle=stock_images+20;
	stock_images[0]=0;stock_images[18]=(uintptr_t)0xFFFFFF9C;
	stock_images[3]=(uintptr_t)native_destroy;stock_images[4]=(uintptr_t)native_destroy;stock_images[5]=(uintptr_t)native_destroy;
	stock_images[20]=(uintptr_t)native_handle_destroy;stock_images[21]=(uintptr_t)native_handle_destroy;
	stock_images[22]=(uintptr_t)handle_getter;stock_images[24]=(uintptr_t)handle_status;
	stock_pool[2]=(uintptr_t)native_pool_destroy;stock_pool[3]=(uintptr_t)native_pool_destroy;stock_pool[4]=(uintptr_t)native_allocate;
	memcpy(pool_table,stock_pool,sizeof(pool_table));memcpy(image_table,stock_images,sizeof(image_table));
	pool_table[4]=(uintptr_t)pool_allocate;
	image_table[3]=(uintptr_t)image_dtor1;image_table[4]=(uintptr_t)image_dtor2;image_table[5]=(uintptr_t)image_dtor3;
	image_table[20]=(uintptr_t)handle_dtor0;image_table[21]=(uintptr_t)handle_dtor1;
	image_table[22]=(uintptr_t)image_get_surface;image_table[24]=(uintptr_t)image_status;
	object=(Pool){.vtable=pool_table+2,.bytes=2097152}; foreign_pool=(Pool){.vtable=original_pool,.bytes=2097152};
	shell_pool.surface_pool=&object; static IconPool *pointer;pointer=&shell_pool;icon_pool_slot=&pointer;pool=&object;
	cache.surface_pool=&object;other_cache.surface_pool=&foreign_pool;
	cache_mutex=&mutex;surface_mutex=&global_mutex;lock_cache=lock;unlock_cache=unlock;
	get_surface=raw_getter;release_surface=surface_release;cache_clock=&clock_value;clock_value=100;
	current_thread=1001;mutex_owner=0;mutex_depth=0;mutex_contended=0;
	image_count=registered_total=allocation_failures=evictions=retry_success=0;
	pending=transfers=lock_skips=stale_pins=rejected=0;
	ready=1;allocation_busy=0;apply_status_return=0xE015FA12;
	free_bytes=native_allocations=releases=getters=destructions=0;recursive_probe=recursive_result=0;
	test_debug_capture_reset();
}
static void make_image(Image *image,unsigned int index,unsigned int timestamp)
{
	memset(image,0,sizeof(*image));image->vtable=original_primary;image->handle.vtable=original_handle;
	image->cache=&cache;image->kind=1;image->state=2;image->result=0;image->references=2;
	image->registered=1;image->retries_left=image->retry_limit=10;image->last_used=timestamp;
	image->surface=&surfaces[index].s;surfaces[index].s.references=1;surfaces[index].bytes=64;
	register_image(&image->handle);
}
static void check_lru(void)
{
	reset();make_image(&a,0,10);make_image(&b,1,20);
	assert(a.references==2 && image_count==2);
	assert(pool_allocate(&object,16,32));
	assert(!a.surface && b.surface && evictions==1 && retry_success==1 && native_allocations==2 && free_bytes==32);
	assert(a.state==2 && a.registered && a.handle.vtable==image_table+20);
	assert(!mutex_depth);
	reset();make_image(&a,0,10);make_image(&b,1,20);make_image(&c,2,30);
	surfaces[0].bytes=40;surfaces[1].bytes=80;
	assert(pool_allocate(&object,16,100));assert(!a.surface&&!b.surface&&c.surface&&evictions==2&&free_bytes==20);
	reset();make_image(&a,0,0);make_image(&b,1,0);assert(pool_allocate(&object,16,1));assert(!a.surface&&b.surface);
	reset();clock_value=5;make_image(&a,0,0xFFFFFFF0);make_image(&b,1,2);
	assert(pool_allocate(&object,16,1));assert(!a.surface&&b.surface);
}
static void check_protection(void)
{
	for(unsigned reason=0;reason<7;++reason) {
		reset();make_image(&a,0,0);
		if(reason==0)a.surface->references=2;
		if(reason==1)a.state=1;
		if(reason==2)a.registered=0;
		if(reason==3)a.retry_limit=0;
		if(reason==4)a.cache=&other_cache;
		if(reason==5)mutex_contended=1;
		if(reason==6)allocation_busy=1;
		assert(!pool_allocate(&object,16,1));assert(a.surface&&evictions==0&&!mutex_depth);
	}
	reset();make_image(&a,0,0);
	assert(!pool_allocate(&foreign_pool,16,1));assert(!evictions);
	assert(!pool_allocate(&object,3,1));assert(!evictions);
	assert(!pool_allocate(&object,16,2097153));assert(!evictions);
	uintptr_t before[30];memcpy(before,stock_images,sizeof(before));
	foreign_image=a;foreign_image.vtable=original_primary;foreign_image.handle.vtable=original_handle;foreign_image.cache=&other_cache;
	register_image(&foreign_image.handle);assert(foreign_image.handle.vtable==original_handle);
	assert(!memcmp(before,stock_images,sizeof(before)));
}
static void check_consumer(void)
{
	reset();make_image(&a,0,0);
	assert(status_for_caller(&a.handle,123)==2&&getters==0&&a.surface->references==1);
	assert(pool_allocate(&object,16,1));assert(a.surface==NULL);
	assert(status_for_caller(&a.handle,apply_status_return)==1&&a.state==1&&pending==1);
	assert(!thread_pin(current_thread)->surface);
	/* Native async completion installs a surface, preserving the same handle. */
	a.surface=&surfaces[0].s;a.surface->references=1;a.state=2;
	assert(status_for_caller(&a.handle,apply_status_return)==2&&a.surface->references==2);
	free_bytes=0;assert(!pool_allocate(&object,16,1));assert(a.surface&&evictions==1);
	Surface *held=NULL;assert(!image_get_surface(&held,&a.handle));
	assert(held==a.surface&&held->references==2&&transfers==1);
	surface_release(held);assert(a.surface->references==1);
	assert(pool_allocate(&object,16,1));assert(!a.surface);
	/* Exhausted/real errors retain the native result path. */
	a.result=-7;a.state=2;assert(status_for_caller(&a.handle,apply_status_return)==2);
	assert(pending==1);
}
static void check_pin_lifetime(void)
{
	reset();make_image(&a,0,0);make_image(&b,1,10);
	recursive_probe=1;assert(status_for_caller(&a.handle,apply_status_return)==2);
	assert(recursive_result==1&&a.surface->references==2&&b.surface->references==1);
	current_thread=2002;assert(status_for_caller(&b.handle,apply_status_return)==2);
	Surface *held=NULL;image_get_surface(&held,&a.handle);assert(held==a.surface&&a.surface->references==3);surface_release(held);
	image_get_surface(&held,&b.handle);assert(held==b.surface&&transfers==1);surface_release(held);
	current_thread=1001;image_get_surface(&held,&a.handle);surface_release(held);
	assert(a.surface->references==1&&b.surface->references==1&&!mutex_depth);
	assert(status_for_caller(&a.handle,apply_status_return)==2);
	assert(status_for_caller(&b.handle,apply_status_return)==2&&stale_pins==1&&a.surface->references==1);
	assert(image_dtor3(&b)==0x1234);assert(image_count==1&&surfaces[1].s.references==1);
	for(unsigned i=0;i<MAX_PINS;++i)assert(!pins[i].surface);
}
static void check_destruction(void)
{
	for(unsigned which=0;which<5;++which) {
		reset();make_image(&a,0,0);make_image(&b,1,10);
		assert(status_for_caller(&a.handle,apply_status_return)==2);
		uintptr_t ret=which==0?image_dtor1(&a):which==1?image_dtor2(&a):which==2?image_dtor3(&a):which==3?handle_dtor0(&a.handle):handle_dtor1(&a.handle);
		assert(ret==0x1234&&destructions==1&&image_count==1&&images[0]==&b);
		assert(a.vtable==original_primary&&a.handle.vtable==original_handle);
		assert(surfaces[0].s.references==1&&!mutex_depth);
		make_image(&a,0,0);assert(image_count==2); /* Address reuse. */
	}
	reset();assert(pool_dtor0(&object)==0x6789&&!ready&&!pool);
	reset();assert(pool_dtor1(&object)==0x6789&&!ready&&!pool);
}

/* Unused startup dependencies; firmware/startup ABI is tested with ARM code. */
int taiGetModuleInfo(const char *name,tai_module_info_t *m) { (void)name;(void)m;return -1; }
int sceKernelGetModuleInfo(SceUID m,SceKernelModuleInfo *i) { (void)m;(void)i;return -1; }
SceUID shell_detour_install(SceUID m,uint32_t o,uintptr_t s,const void *f) { (void)m;(void)o;(void)s;(void)f;return -1; }
int taiInjectRelease(SceUID id) { (void)id;return 0; }
void instance_pool_original(void) {}
int instance_request_original(void *a,void **b,void *c,unsigned int d,unsigned int e) { (void)a;(void)b;(void)c;(void)d;(void)e;return -1; }

int main(void)
{
	check_lru();check_protection();check_consumer();check_pin_lifetime();check_destruction();
#if !LIVEAREA_DEBUG_LOGGING
	assert(test_debug_capture_count("[instance-cache]")==0);
#endif
	puts("Instance cache: LRU/ties/wrap, retries, protected/foreign images, contention, pending reload, pin transfer, nesting, cross-thread ownership, cancellation/destruction and address reuse passed");
}
