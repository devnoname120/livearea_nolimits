#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr/lw_mutex.h>
#include <stddef.h>
#include <stdatomic.h>
#include <psp2/types.h>
#include <stdint.h>
#include <string.h>
#include <taihen.h>

#include "debug_log.h"
#include "recovery.h"
#include "shell_detour.h"

#define RECOVERY_TEXT_SIZE 0x3E9E8U
#define RECOVERY_DATA_SIZE 0x3094U
#define RECOVERY_STOP_OFFSET 0x1EU
#define RECOVERY_ERROR (-1)
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define SHELL_RECOVERY_READY_OFFSET 0x1BFEU
#define SHELL_RECOVERY_LOAD_CALL_OFFSET 0xC1EU
#define RECOVERY_INITIALIZER_OFFSET 0x9AU
#define RECOVERY_ACTION_FLAGS_OFFSET 0x1100U

extern const uint8_t patch_cmp_r0_icon_limit[4];
extern const uint8_t patch_rsbs_r1_r0_icon_limit[4];
extern const uint8_t patch_cmp_r4_page_limit[2];
extern const uint8_t patch_movs_r9_last_page[4];

typedef struct {
	uint32_t offset;
	unsigned int size;
	uint8_t expected[4];
	const uint8_t *replacement;
} RecoveryPatch;

typedef struct {
	uint32_t shell_nid, recovery_nid, shell_text_size, paf_nid, load_import;
	uint8_t load_call[4];
} RecoveryProfile;

/* PAF's native Module wrapper is a single owning ModuleImpl pointer. */
typedef struct {
	uint32_t name[3];
	SceUID handle;
	uint32_t interfaces[3];
	int32_t references, option, result;
} PafModuleImpl;
typedef struct { PafModuleImpl *impl; } PafModule;
typedef struct { const char *data; uint32_t length, capacity; } PafString;
typedef struct {
	PafString name, caller;
	uintptr_t functions[5];
	uint8_t middle[60];
	PafString module_file;
	int32_t module_interface_version, module_option;
} PafLoadParam;
typedef void (*LoadFinish)(void *plugin);
typedef void (*LoadAsync)(const PafLoadParam *, LoadFinish, int);
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(PafModule)==4 && sizeof(PafString)==12, "PAF wrapper ABI");
_Static_assert(offsetof(PafModuleImpl,handle)==12 && offsetof(PafModuleImpl,references)==28 &&
	offsetof(PafModuleImpl,result)==36, "PAF ModuleImpl ABI");
_Static_assert(offsetof(PafLoadParam,module_file)==104 &&
	offsetof(PafLoadParam,module_option)==120, "PAF InitParam ABI");
#endif

typedef int (*RecoveryStop)(SceSize args, const void *argp);

static const RecoveryPatch recovery_patches[] = {
	{0x06280, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x07054, 4, {0xB0, 0xF5, 0xFA, 0x7F}, patch_cmp_r0_icon_limit},
	{0x0C700, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
	{0x0C776, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
	{0x0C834, 4, {0xD0, 0xF5, 0xFA, 0x71}, patch_rsbs_r1_r0_icon_limit},
	{0x06084, 2, {0x0A, 0x2C}, patch_cmp_r4_page_limit},
	{0x060FE, 4, {0x5F, 0xF0, 0x09, 0x09}, patch_movs_r9_last_page},
};

static const RecoveryProfile recovery_profiles[] = {
	{0x0552F692U,0xC1F30F67U,0x541B74U,0xCD679177U,0x45CA50U,{0x5B,0xF0,0x18,0xE7}},
	{0x5549BF1FU,0x3F76E38FU,0x5420F4U,0x73F90499U,0x45CE98U,{0x5C,0xF0,0x3C,0xE1}},
	{0xEAB89D5CU,0xC1F30F67U,0x535CF4U,0xCD679177U,0x452C08U,{0x51,0xF0,0xF4,0xE7}},
};

static const uint8_t expected_stop_entry[] = {
	0x10, 0xB5, 0x24, 0xF0, 0xC9, 0xFB, 0x00, 0x20, 0x10, 0xBD,
};
static const uint8_t stop_redirect_prefix[] = {
	0xDF, 0xF8, 0x04, 0xF0, 0x00, 0xBF,
};

static const RecoveryProfile *active_profile;
static SceUID load_call_id = -1, stop_redirect_id = -1, init_observer_id = -1;
static atomic_int recovery_modid = -1;
static RecoveryStop native_stop;
static void (*native_init)(void *);
static const uint32_t *action_flags;
static SceUID patch_ids[ARRAY_COUNT(recovery_patches)];
static int recovery_install_complete, initialized;
static SceKernelLwMutexWork lifecycle_mutex;
static atomic_uint load_in_flight, finish_in_flight, init_in_flight, stop_in_flight;
static unsigned int pending_loads;
enum { PRELOAD_IDLE, PRELOAD_PREPARING, PRELOAD_WAITING,
	PRELOAD_RELEASING, PRELOAD_BLOCKED };
static atomic_int preload_phase;
static PafModule preload;
static PafModule *(*module_acquire)(PafModule *, const char *, const char *, int, const void *);
static PafModule *(*module_release)(PafModule *);
static void *(*module_get_interface)(PafModule *,int);
static void **framework_slot;
static LoadAsync original_load;
static LoadFinish original_finish;

_Static_assert((RECOVERY_STOP_OFFSET & 3U) == 2U,
	"stop redirect literal requires a halfword-aligned entry");
_Static_assert(sizeof(stop_redirect_prefix) + sizeof(uint32_t) ==
	sizeof(expected_stop_entry), "stop redirect must replace the complete entry");

/* Never hold this mutex across a PAF constructor/destructor, load request or
 * client callback. These may run callbacks or wait for another thread. */
static int enter_lifecycle(void)
{
	return sceKernelLockLwMutex(&lifecycle_mutex,1,NULL)>=0;
}
static void leave_lifecycle(void)
{
	sceKernelUnlockLwMutex(&lifecycle_mutex,1);
}

static void clear_patch_ids(void)
{
	unsigned int index;

	for (index = 0; index < ARRAY_COUNT(patch_ids); ++index)
		patch_ids[index] = -1;
}

static int verify_recovery(const SceKernelModuleInfo *info)
{
	const uint8_t *text;
	unsigned int index;

	if (info == NULL || info->segments[0].vaddr == NULL ||
		info->segments[0].memsz != RECOVERY_TEXT_SIZE ||
		info->segments[1].vaddr == NULL ||
		info->segments[1].memsz != RECOVERY_DATA_SIZE) {
		debug_logf("recovery", "validation-failed reason=segments expected_text=0x%08X expected_data=0x%08X",
			RECOVERY_TEXT_SIZE, RECOVERY_DATA_SIZE);
		return RECOVERY_ERROR;
	}
#if UINTPTR_MAX == UINT32_MAX
	if((uintptr_t)info->segments[0].vaddr>=0xE0000000U ||
		(uintptr_t)info->segments[1].vaddr>=0xE0000000U) {
		debug_logf("recovery","validation-failed reason=shared-range-module");
		return RECOVERY_ERROR;
	}
#endif
	text = info->segments[0].vaddr;
	static const uint32_t methods[] = {0x99,0x9B,0x15F,0x167,0x207};
	for (unsigned int i=0;i<ARRAY_COUNT(methods);++i) {
		uint32_t pointer;
		memcpy(&pointer,(uint8_t *)info->segments[1].vaddr+i*4,4);
		if (pointer!=(uint32_t)((uintptr_t)text+methods[i])) {
			debug_logf("recovery","validation-failed reason=module-interface slot=%u",i);
			return RECOVERY_ERROR;
		}
	}
	if (memcmp(text + RECOVERY_STOP_OFFSET, expected_stop_entry,
			sizeof(expected_stop_entry)) != 0) {
		debug_logf("recovery", "validation-failed reason=stop-entry");
		debug_log_hex("recovery", "stop-actual", RECOVERY_STOP_OFFSET,
			text + RECOVERY_STOP_OFFSET, sizeof(expected_stop_entry));
		debug_log_hex("recovery", "stop-expected", RECOVERY_STOP_OFFSET,
			expected_stop_entry, sizeof(expected_stop_entry));
		return RECOVERY_ERROR;
	}
	for (index = 0; index < ARRAY_COUNT(recovery_patches); ++index) {
		const RecoveryPatch *patch = &recovery_patches[index];

		if (memcmp(text + patch->offset, patch->expected, patch->size) != 0) {
			debug_logf("recovery", "validation-failed reason=patch index=%u offset=0x%08X",
				index, (unsigned int)patch->offset);
			debug_log_hex("recovery", "patch-actual", patch->offset,
				text + patch->offset, patch->size);
			debug_log_hex("recovery", "patch-expected", patch->offset,
				patch->expected, patch->size);
			return RECOVERY_ERROR;
		}
	}
	debug_logf("recovery", "validation-complete patches=%u stop_offset=0x%08X",
		(unsigned int)ARRAY_COUNT(recovery_patches), RECOVERY_STOP_OFFSET);
	return 0;
}

static int release_recovery_patches(void)
{
	int first_error = 0;
	int index;
	if (init_observer_id>=0) {
		int result=taiInjectRelease(init_observer_id);
		debug_logf("recovery","init-observer-release uid=%d result=%d",init_observer_id,result);
		if (result>=0) init_observer_id=-1;
		else first_error=result;
	}

	for (index = (int)ARRAY_COUNT(patch_ids) - 1; index >= 0; --index) {
		if (patch_ids[index] >= 0) {
			int result = taiInjectRelease(patch_ids[index]);

			debug_logf("recovery", "patch-release index=%d uid=%d result=%d",
				index, patch_ids[index], result);
			if (result >= 0)
				patch_ids[index] = -1;
			else if (first_error == 0)
				first_error = result;
		}
	}
	return first_error;
}

static int release_stop_redirect(void)
{
	int result;

	if (stop_redirect_id < 0)
		return 0;
	result = taiInjectRelease(stop_redirect_id);
	debug_logf("recovery", "stop-redirect-release uid=%d result=%d",
		stop_redirect_id, result);
	if (result >= 0)
		stop_redirect_id = -1;
	return result;
}

static void clear_recovery_state(void)
{
	recovery_modid = -1;
	native_stop = NULL;
	native_init = NULL;
	action_flags = NULL;
	recovery_install_complete = 0;
}

static int recovery_module_stop_redirect(SceSize args,const void *argp)
{
	RecoveryStop stop;
	int result=SCE_KERNEL_STOP_CANCEL;
	atomic_fetch_add(&stop_in_flight,1);
	debug_logf("recovery","module-stop enter modid=%d args=%u argp=0x%08X",
		recovery_modid,(unsigned int)args,(unsigned int)(uintptr_t)argp);
	debug_log_flush();
	if(!enter_lifecycle()) goto done;
	if(!native_stop || init_in_flight || stop_in_flight>1) {
		debug_logf("recovery","module-stop cancelled reason=callback-active-or-native-null");
		leave_lifecycle();goto done;
	}
	if(release_recovery_patches()<0 || release_stop_redirect()<0) {
		debug_logf("recovery","module-stop cancelled reason=cleanup-failed");
		recovery_install_complete=0;preload_phase=PRELOAD_BLOCKED;
		leave_lifecycle();goto done;
	}
	stop=native_stop;clear_recovery_state();leave_lifecycle();
	debug_logf("recovery","module-stop native-enter address=0x%08X",(unsigned int)(uintptr_t)stop);
	debug_log_flush();
	result=stop(args,argp);
	debug_logf("recovery","module-stop native-return result=%d",result);
done:
	debug_log_flush();atomic_fetch_sub(&stop_in_flight,1);return result;
}

#if LIVEAREA_DEBUG_LOGGING
static void recovery_init_observer(void *plugin);
#endif

static int install_recovery(SceUID expected_modid,const void *module_interface)
{
	tai_module_info_t module = {0};
	SceKernelModuleInfo info = {0};
	uint8_t stop_redirect[sizeof(expected_stop_entry)];
	uint32_t target;
	unsigned int index;
	int result;

	if (active_profile == NULL || recovery_modid >= 0) {
		debug_logf("recovery", "install-rejected reason=state profile=%u modid=%d",
			active_profile != NULL, recovery_modid);
		return RECOVERY_ERROR;
	}
	module.size = sizeof(module);
	result = taiGetModuleInfo("SceDbRecovery", &module);
	debug_logf("recovery", "module-lookup result=%d modid=%d nid=0x%08X expected=0x%08X",
		result, result < 0 ? -1 : module.modid,
		result < 0 ? 0U : (unsigned int)module.module_nid,
		(unsigned int)active_profile->recovery_nid);
	if (result < 0 || module.modid != expected_modid ||
		module.module_nid != active_profile->recovery_nid) {
		debug_logf("recovery", "install-rejected reason=module-identity");
		return RECOVERY_ERROR;
	}
	info.size = sizeof(info);
	result = sceKernelGetModuleInfo(module.modid, &info);
	debug_logf("recovery", "module-info result=%d text=0x%08X text_size=0x%08X data=0x%08X data_size=0x%08X",
		result, (unsigned int)(uintptr_t)info.segments[0].vaddr,
		(unsigned int)info.segments[0].memsz,
		(unsigned int)(uintptr_t)info.segments[1].vaddr,
		(unsigned int)info.segments[1].memsz);
	if (result < 0 || module_interface!=info.segments[1].vaddr || verify_recovery(&info) < 0)
		return RECOVERY_ERROR;

	memcpy(stop_redirect, stop_redirect_prefix, sizeof(stop_redirect_prefix));
	target = (uint32_t)(uintptr_t)recovery_module_stop_redirect | 1U;
	memcpy(stop_redirect + sizeof(stop_redirect_prefix), &target,
		sizeof(target));
	stop_redirect_id = taiInjectData(module.modid, 0, RECOVERY_STOP_OFFSET,
		stop_redirect, sizeof(stop_redirect));
	debug_logf("recovery", "stop-redirect-injected uid=%d offset=0x%08X target=0x%08X",
		stop_redirect_id, RECOVERY_STOP_OFFSET, (unsigned int)target);
	if (stop_redirect_id < 0)
		return stop_redirect_id;
	recovery_modid = module.modid;
	native_stop = (RecoveryStop)((uintptr_t)info.segments[0].vaddr +
		RECOVERY_STOP_OFFSET + 1U);
	native_init=(void *)((uintptr_t)info.segments[0].vaddr+RECOVERY_INITIALIZER_OFFSET+1U);
	action_flags=(void *)((uint8_t *)info.segments[1].vaddr+RECOVERY_ACTION_FLAGS_OFFSET);
	clear_patch_ids();
	for (index = 0; index < ARRAY_COUNT(recovery_patches); ++index) {
		const RecoveryPatch *patch = &recovery_patches[index];

		patch_ids[index] = taiInjectData(module.modid, 0, patch->offset,
			patch->replacement, patch->size);
		debug_logf("recovery", "patch-injected index=%u offset=0x%08X size=%u uid=%d",
			index, (unsigned int)patch->offset, patch->size, patch_ids[index]);
		if (patch_ids[index] < 0) {
			result=patch_ids[index];patch_ids[index]=-1;goto rollback;
		}
	}
#if LIVEAREA_DEBUG_LOGGING
	/* Observe the private module's initializer through its existing interface.
	 * PAF copies this pointer before calling init. No entry trampoline is used. */
	target=(uint32_t)(uintptr_t)recovery_init_observer|1U;
	init_observer_id=taiInjectData(module.modid,1,4,&target,4);
	debug_logf("recovery","init-observer-injected uid=%d",init_observer_id);
	if (init_observer_id<0) {result=init_observer_id;goto rollback;}
#endif

	recovery_install_complete = 1;
	debug_logf("recovery", "module-patched modid=%d stop-redirect=%d",
		recovery_modid, stop_redirect_id);
	debug_log_flush();
	return 0;
rollback:
	{
		int cleanup=release_recovery_patches();
		if (cleanup>=0) cleanup=release_stop_redirect();
		if (cleanup>=0) clear_recovery_state();
		debug_logf("recovery","install-rollback result=%d cleanup=%d retained_modid=%d",
			result,cleanup,recovery_modid);
		debug_log_flush();return result;
	}
}


#if LIVEAREA_DEBUG_LOGGING
static uint32_t read_action_flags(void)
{
	uint32_t flags=0;
	if(enter_lifecycle()) {if(action_flags) flags=*action_flags;leave_lifecycle();}
	return flags;
}

static void recovery_init_observer(void *plugin)
{
	atomic_fetch_add(&init_in_flight,1);
	debug_logf("recovery","init-enter plugin=0x%08X modid=%d patched=%d flags=0x%08X",
		(unsigned int)(uintptr_t)plugin,recovery_modid,recovery_install_complete,
		read_action_flags());
	debug_log_flush();
	native_init(plugin);
	debug_logf("recovery","init-return flags=0x%08X",read_action_flags());
	debug_log_flush();
	atomic_fetch_sub(&init_in_flight,1);
}
#endif

static int resolve_paf(void)
{
	tai_module_info_t module={0};
	SceKernelModuleInfo info={0};
	module.size=sizeof(module);info.size=sizeof(info);
	if(taiGetModuleInfo("ScePaf",&module)<0 || module.module_nid!=active_profile->paf_nid ||
		sceKernelGetModuleInfo(module.modid,&info)<0 ||
		!info.segments[0].vaddr || info.segments[0].memsz!=0x300D00U ||
		!info.segments[1].vaddr || info.segments[1].memsz<0xE000U) return RECOVERY_ERROR;
	const uint8_t *text=info.segments[0].vaddr;
	static const struct { uint32_t offset; unsigned int size; uint8_t bytes[12]; } checks[]={
		{0x4722A,6,{0x2D,0xE9,0xF0,0x4F,0x8D,0xB0}},
		{0x473D4,2,{0x70,0xB5}},
		{0x472D0,10,{0xC8,0xF8,0x00,0x00,0xC2,0x69,0x01,0x32,0xC2,0x61}},
		{0x473E6,8,{0x20,0x68,0xC1,0x69,0x01,0x39,0xC1,0x61}},
		{0x471D8,10,{0x02,0x68,0x12,0x69,0x0A,0xB9,0x00,0x20,0x13,0xE0}},
		{0x47218,6,{0x00,0x68,0x40,0x6A,0x70,0x47}},
		{0x54580,4,{0x40,0x6D,0x70,0x47}},
		{0x58E5E,10,{0x31,0x6A,0x09,0xB1,0x30,0x1C,0x88,0x47,0x01,0x20}},
	};
	for(unsigned int i=0;i<ARRAY_COUNT(checks);++i)
		if(memcmp(text+checks[i].offset,checks[i].bytes,checks[i].size)) return RECOVERY_ERROR;
	framework_slot=(void *)((uint8_t *)info.segments[1].vaddr+0x194U);
	if(!*framework_slot) return RECOVERY_ERROR;
	module_acquire=(void *)((uintptr_t)text+0x4722BU);
	module_release=(void *)((uintptr_t)text+0x473D5U);
	module_get_interface=(void *)((uintptr_t)text+0x471D9U);
	debug_logf("recovery","paf-validated nid=0x%08X text=0x%08X module-acquire=0x%08X",
		module.module_nid,(unsigned int)(uintptr_t)text,(unsigned int)(uintptr_t)module_acquire);
	return 0;
}

static int matches_string(const PafString *str,const char *expected,unsigned int length)
{
	return str->data && str->length==length && !memcmp(str->data,expected,length+1);
}
static int matches_request(const PafLoadParam *param,LoadFinish finish)
{
	static const char name[]="dbrecovery_plugin";
	static const char file[]="vs0:vsh/common/dbrecovery_plugin.suprx";
	return param && finish==original_finish && param->module_interface_version==1 &&
		param->module_option==1 && matches_string(&param->name,name,sizeof(name)-1) &&
		matches_string(&param->module_file,file,sizeof(file)-1);
}

static void recovery_load_finished(void *plugin)
{
	int release=0;
	atomic_fetch_add(&finish_in_flight,1);
	debug_logf("recovery","load-finished plugin=0x%08X modid=%d flags=0x%08X",
		(unsigned int)(uintptr_t)plugin,recovery_modid,read_action_flags());
	/* The callback owns a valid Plugin until the client callback is invoked.
	 * Check ModuleImpl reuse now; the client may itself unload the Plugin. */
	if(enter_lifecycle()) {
		if(plugin && preload.impl) {
			PafModule *owner;
			memcpy(&owner,(uint8_t *)plugin+48,sizeof(owner));
			if(!owner || owner->impl!=preload.impl) {
				preload_phase=PRELOAD_BLOCKED;
				debug_logf("recovery","preload-owner-mismatch reference-retained=1");
			}
		}
		leave_lifecycle();
	}
	debug_logf("recovery","finish-native-enter callback=0x%08X",
		(unsigned int)(uintptr_t)original_finish);
	debug_log_flush();
	original_finish(plugin);
	debug_logf("recovery","finish-native-return");
	debug_log_flush();
	if(enter_lifecycle()) {
		if(pending_loads) --pending_loads;
		else debug_logf("recovery","finish-unbalanced reference-retained=1");
		if(!pending_loads && preload_phase==PRELOAD_WAITING && preload.impl) {
			preload_phase=PRELOAD_RELEASING;release=1;
		}
		leave_lifecycle();
	}
	if(release) {
		/* May stop/unload the module on a failed Plugin load. Keep all locks
		 * released so the private stop redirect can restore its patches. */
		debug_logf("recovery","preload-release modid=%d",preload.impl->handle);
		debug_log_flush();
		module_release(&preload);
		preload.impl=NULL; /* RELEASING excludes another writer. */
		if(enter_lifecycle()) {
			if(preload_phase!=PRELOAD_BLOCKED) preload_phase=PRELOAD_IDLE;
			leave_lifecycle();
		}
		debug_logf("recovery","preload-released");
	}
	debug_log_flush();
	atomic_fetch_sub(&finish_in_flight,1);
}

/* Called only after reserving PREPARING and validating the PAF helpers.
 * Returns 1 with a retained preload, 0 for native fallback, -1 when blocked. */
static int prepare_recovery_module(const PafLoadParam *param)
{
	int installed=RECOVERY_ERROR,mode;
	if(recovery_modid<0) {
		tai_module_info_t existing={0};existing.size=sizeof(existing);
		if(taiGetModuleInfo("SceDbRecovery",&existing)>=0) {
			debug_logf("recovery","preload-skipped reason=untracked-existing-module modid=%d",existing.modid);
			return 0;
		}
	}
	memcpy(&mode,(uint8_t *)*framework_slot+84,sizeof(mode));
	int module_option=param->module_option|(mode==5?2:0);
	debug_logf("recovery","preload-enter module-option=%d",module_option);
	debug_log_flush();
	module_acquire(&preload,param->module_file.data,NULL,module_option,NULL);
	debug_logf("recovery","preload-return impl=0x%08X modid=%d result=%d",
		(unsigned int)(uintptr_t)preload.impl,preload.impl?preload.impl->handle:-1,
		preload.impl?preload.impl->result:RECOVERY_ERROR);
	debug_log_flush();
	if(!preload.impl || preload.impl->result || preload.impl->handle<=0) {
		/* A retained failed ModuleImpl would poison native LoadAsync retries. */
		if(preload.impl) module_release(&preload);
		preload.impl=NULL;return 0;
	}
	const void *module_interface=module_get_interface(&preload,param->module_interface_version);
	if(!module_interface) {
		debug_logf("recovery","preload-skipped reason=missing-module-interface");
		module_release(&preload);preload.impl=NULL;return 0;
	}
	if(!enter_lifecycle()) return -1;
	if(recovery_modid<0) installed=install_recovery(preload.impl->handle,module_interface);
	else if(recovery_modid==preload.impl->handle && recovery_install_complete) installed=0;
	if(installed<0 && recovery_modid>=0) {
		preload_phase=PRELOAD_BLOCKED;leave_lifecycle();return -1;
	}
	pending_loads=1;preload_phase=PRELOAD_WAITING;
	leave_lifecycle();
	debug_logf("recovery","preload-prepared patched=%u before-initializer=1",installed==0);
	debug_log_flush();return 1;
}

static void recovery_load_hook(const PafLoadParam *param,LoadFinish finish,int option)
{
	atomic_fetch_add(&load_in_flight,1);
	if(!matches_request(param,finish)) {
		debug_logf("recovery","load-passthrough reason=request-mismatch");
		original_load(param,finish,option);goto done;
	}
	if(!enter_lifecycle()) goto blocked;
	if(stop_in_flight) {leave_lifecycle();goto blocked;}
	if(preload_phase==PRELOAD_WAITING) {
		++pending_loads;leave_lifecycle();
		original_load(param,recovery_load_finished,option);goto done;
	}
	if(preload_phase!=PRELOAD_IDLE || stop_in_flight) {leave_lifecycle();goto blocked;}
	preload_phase=PRELOAD_PREPARING;leave_lifecycle();
	if(resolve_paf()<0) {
		debug_logf("recovery","preload-skipped reason=paf-validation");goto fallback;
	}
	int prepared=prepare_recovery_module(param);
	if(prepared<0) goto blocked;
	if(!prepared) goto fallback;
	debug_logf("recovery","load-native-enter");debug_log_flush();
	original_load(param,recovery_load_finished,option);
	debug_logf("recovery","load-native-return");goto done;
fallback:
	if(enter_lifecycle()) {
		preload_phase=PRELOAD_IDLE;leave_lifecycle();
		original_load(param,finish,option);goto done;
	}
blocked:
	debug_logf("recovery","load-blocked phase=%d retained-modid=%d",preload_phase,recovery_modid);
	debug_log_flush();
done:
	atomic_fetch_sub(&load_in_flight,1);
}

static void encode_mov(uint8_t *p,unsigned int op,unsigned int reg,uint16_t value)
{
	uint16_t a=op|(value>>12)|((value>>1)&0x400), b=((value<<4)&0x7000)|(reg<<8)|(value&255);
	p[0]=a;p[1]=a>>8;p[2]=b;p[3]=b>>8;
}

int recovery_start(SceUID shell_modid,uint32_t shell_nid,const SceKernelModuleInfo *info)
{
	if(initialized) return RECOVERY_ERROR;
	active_profile=NULL;
	for(unsigned int i=0;i<ARRAY_COUNT(recovery_profiles);++i)
		if(recovery_profiles[i].shell_nid==shell_nid) active_profile=&recovery_profiles[i];
	if(!active_profile || !info || !info->segments[0].vaddr ||
		info->segments[0].memsz!=active_profile->shell_text_size) return RECOVERY_ERROR;
	uintptr_t base=(uintptr_t)info->segments[0].vaddr;
	if(base&3U) return RECOVERY_ERROR;
	uint8_t expected[16]={0,0,0,0,0x08,0xA8,0,0,0,0,0x00,0x22,0,0,0,0},call[4];
	uintptr_t callback=base+SHELL_RECOVERY_READY_OFFSET+1U;
	encode_mov(expected,0xF240,1,(uint16_t)callback);
	encode_mov(expected+6,0xF2C0,1,(uint16_t)(callback>>16));
	memcpy(expected+12,active_profile->load_call,4);
	if(memcmp((void *)(base+SHELL_RECOVERY_LOAD_CALL_OFFSET-12),expected,sizeof(expected)) ||
		shell_detour_encode_call(call,base+SHELL_RECOVERY_LOAD_CALL_OFFSET,(uintptr_t)recovery_load_hook|1U)<0) {
		debug_logf("recovery","start-rejected reason=load-call-validation");return RECOVERY_ERROR;
	}
	int result=sceKernelCreateLwMutex(&lifecycle_mutex,"LiveAreaNoLimitsRecovery",0,0,NULL);
	if(result<0) return result;
	initialized=1;clear_patch_ids();
	original_load=(void *)(base+active_profile->load_import);
	original_finish=(void *)callback;
	load_call_id=taiInjectData(shell_modid,0,SHELL_RECOVERY_LOAD_CALL_OFFSET,call,sizeof(call));
	debug_logf("recovery","load-call-installed uid=%d offset=0x%08X original=0x%08X ready-entry-unmodified=1",
		load_call_id,SHELL_RECOVERY_LOAD_CALL_OFFSET,(unsigned int)(uintptr_t)original_load);
	debug_log_flush();
	if(load_call_id<0) {
		result=load_call_id;sceKernelDeleteLwMutex(&lifecycle_mutex);initialized=0;
		return result;
	}
	return 0;
}

int recovery_stop(void)
{
	if(!initialized) return 0;
	/* As with the per-instance cache, a live SceShell call site can dispatch
	 * callbacks later. Switch builds by reboot, never by hot-unloading it. */
	if(load_call_id>=0 || load_in_flight || finish_in_flight || init_in_flight || stop_in_flight ||
		preload.impl || recovery_modid>=0 || stop_redirect_id>=0) {
		debug_logf("recovery","plugin-stop cancelled reason=live-call-site-or-module");
		return RECOVERY_ERROR;
	}
	int result=sceKernelDeleteLwMutex(&lifecycle_mutex);
	if(result>=0) initialized=0;
	return result;
}
