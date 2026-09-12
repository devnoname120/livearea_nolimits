/* Host lifecycle checks. Native PAF/OS services are modeled; the separate ARM
 * harness executes compiled entry/continuation and native firmware code. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/recovery.c"
#if LIVEAREA_DEBUG_LOGGING
#include "debug_capture.h"
#endif

static uint8_t *shell_text,*module_text,*module_data,*paf_text,*paf_data;
static SceKernelModuleInfo shell_info;
static const RecoveryProfile *fixture_profile;
static PafModuleImpl impl;
static PafModule owner;
static struct { uint8_t before_module[48]; PafModule *module; } plugin;
static unsigned int mutex_depth,mutex_created,acquires,releases,starts,stops,finishes,loads,inits;
static int available,ctor_error,info_error,bad_nid,create_error,owner_unloads,finish_reenters;
static int fail_injection,fail_release,attempts;
static uint32_t fake_framework[32];
static void *fake_framework_ptr=fake_framework;
static LoadFinish queued[8];
static unsigned int queued_count;
static struct { int live; uint8_t *destination; uint8_t original[10]; unsigned int size; } injections[32];
static PafLoadParam param;

int sceKernelCreateLwMutex(SceKernelLwMutexWork *w,const char *n,unsigned int a,int count,void *o)
{ assert(w==&lifecycle_mutex && n && !a && !count && !o); if(create_error)return -9;mutex_created=1;return 0; }
int sceKernelDeleteLwMutex(SceKernelLwMutexWork *w)
{ assert(w==&lifecycle_mutex && !mutex_depth);mutex_created=0;return 0; }
int sceKernelLockLwMutex(SceKernelLwMutexWork *w,int count,unsigned int *timeout)
{ assert(w==&lifecycle_mutex && count==1 && !timeout && mutex_created && !mutex_depth);mutex_depth=1;return 0; }
int sceKernelUnlockLwMutex(SceKernelLwMutexWork *w,int count)
{ assert(w==&lifecycle_mutex && count==1 && mutex_depth==1);mutex_depth=0;return 0; }
int shell_detour_encode_call(uint8_t out[4],uintptr_t from,uintptr_t to)
{ assert(from==(uintptr_t)shell_text+SHELL_RECOVERY_LOAD_CALL_OFFSET);assert(to==((uintptr_t)recovery_load_hook|1));memset(out,0xCC,4);return 0; }
int taiGetModuleInfo(const char *name,tai_module_info_t *m)
{
 assert(m->size==sizeof(*m));
 if(!strcmp(name,"ScePaf")) {m->modid=90;m->module_nid=fixture_profile->paf_nid;return 0;}
 assert(!strcmp(name,"SceDbRecovery"));if(!available)return -1;
 m->modid=42;m->module_nid=bad_nid?0:fixture_profile->recovery_nid;return 0;
}
int sceKernelGetModuleInfo(SceUID id,SceKernelModuleInfo *info)
{
 assert(info->size==sizeof(*info));
 if(id==90) {info->segments[0].vaddr=paf_text;info->segments[0].memsz=0x300D00;info->segments[1].vaddr=paf_data;info->segments[1].memsz=0x166D0;return 0;}
 assert(id==42);if(info_error)return -1;
 info->segments[0].vaddr=module_text;info->segments[0].memsz=RECOVERY_TEXT_SIZE;
 info->segments[1].vaddr=module_data;info->segments[1].memsz=RECOVERY_DATA_SIZE;return 0;
}
SceUID taiInjectData(SceUID id,int segment,uint32_t offset,const void *data,SceSize size)
{
 int call=attempts++;assert(call<32 && size<=10);
 if(call==fail_injection)return -100-call;
 uint8_t *destination;
 if(id==7) {assert(segment==0 && offset==SHELL_RECOVERY_LOAD_CALL_OFFSET && size==4);destination=shell_text+offset;}
 else {assert(id==42 && available && mutex_depth==1);destination=(segment?module_data:module_text)+offset;}
 injections[call].live=1;injections[call].destination=destination;injections[call].size=size;
 memcpy(injections[call].original,destination,size);memcpy(destination,data,size);return 100+call;
}
int taiInjectRelease(SceUID id)
{
 assert(id>=100 && id<132);if(id==fail_release)return -9;
 unsigned int i=id-100;assert(injections[i].live);memcpy(injections[i].destination,injections[i].original,injections[i].size);injections[i].live=0;return 0;
}
static int all_capacity_live(void)
{
 for(unsigned int i=0;i<ARRAY_COUNT(patch_ids);++i)if(patch_ids[i]<0)return 0;
 return 1;
}
static int mock_stop(SceSize argc,const void *argv)
{
 assert(!argc && !argv && !mutex_depth && !all_capacity_live() && init_observer_id<0);
 ++stops;available=0;return 0;
}
static PafModule *mock_acquire(PafModule *m,const char *name,const char *function,int option,const void *extra)
{
 assert(!mutex_depth && !strcmp(name,"vs0:vsh/common/dbrecovery_plugin.suprx") && !function && !extra);
 assert(option==(fake_framework[21]==5?3:1));++acquires;
 if(!impl.references) {++starts;available=1;impl.handle=42;impl.result=ctor_error;impl.option=option;}
 ++impl.references;m->impl=&impl;return m;
}
static PafModule *mock_release(PafModule *m)
{
 assert(!mutex_depth && m->impl==&impl && impl.references>0);++releases;
 if(!--impl.references) {
  if(stop_redirect_id>=0) (void)recovery_module_stop_redirect(0,NULL);
  else {++stops;available=0;}
 }
 return m;
}
static void mock_init(void *p)
{
 assert(p==&plugin && !mutex_depth);++inits;
 *(uint32_t *)(module_data+RECOVERY_ACTION_FLAGS_OFFSET)=all_capacity_live()?0x80000:0;
 assert(recovery_stop()<0);
}
static void mock_load(const PafLoadParam *p,LoadFinish finish,int option)
{
 assert(!mutex_depth && p==&param && option==17 && queued_count<8);++loads;queued[queued_count++]=finish;
}
static void mock_finish(void *p)
{
 assert(!mutex_depth);++finishes;assert(recovery_stop()<0);
 if(p && owner_unloads) {assert(owner.impl);mock_release(&owner);owner.impl=NULL;plugin.module=NULL;}
 if(finish_reenters) {finish_reenters=0;recovery_load_hook(&param,mock_finish,17);}
}
static void *mock_interface(PafModule *m,int version)
{assert(m->impl==&impl && version==1 && !mutex_depth);return module_data;}
static void set_native_mocks(void)
{module_acquire=mock_acquire;module_release=mock_release;module_get_interface=mock_interface;original_load=mock_load;original_finish=mock_finish;framework_slot=&fake_framework_ptr;}
static void seed_shell(void)
{
 uint8_t *p=shell_text+SHELL_RECOVERY_LOAD_CALL_OFFSET-12;uintptr_t cb=(uintptr_t)shell_text+SHELL_RECOVERY_READY_OFFSET+1;
 encode_mov(p,0xF240,1,(uint16_t)cb);p[4]=8;p[5]=0xA8;encode_mov(p+6,0xF2C0,1,(uint16_t)(cb>>16));p[10]=0;p[11]=0x22;memcpy(p+12,fixture_profile->load_call,4);
}
static void seed_module(void)
{
 memset(module_text,0,RECOVERY_TEXT_SIZE);memcpy(module_text+RECOVERY_STOP_OFFSET,expected_stop_entry,sizeof(expected_stop_entry));
 for(unsigned int i=0;i<ARRAY_COUNT(recovery_patches);++i)memcpy(module_text+recovery_patches[i].offset,recovery_patches[i].expected,recovery_patches[i].size);
 memset(module_data,0,RECOVERY_DATA_SIZE);uint32_t offsets[]={0x99,0x9B,0x15F,0x167,0x207};
 for(unsigned int i=0;i<5;++i) {uint32_t x=(uint32_t)((uintptr_t)module_text+offsets[i]);memcpy(module_data+i*4,&x,4);}
}
static void fresh(const RecoveryProfile *p)
{
 /* Every case models a new process. Fault cases deliberately retain resources
  * until reboot; this harness reset is not a runtime cleanup API. */
 fixture_profile=p;active_profile=NULL;initialized=0;load_call_id=stop_redirect_id=init_observer_id=recovery_modid=-1;
 recovery_install_complete=0;preload.impl=NULL;preload_phase=PRELOAD_IDLE;pending_loads=0;
 load_in_flight=finish_in_flight=init_in_flight=0;native_stop=NULL;native_init=NULL;action_flags=NULL;clear_patch_ids();
 mutex_depth=mutex_created=acquires=releases=starts=stops=finishes=loads=inits=queued_count=0;
 available=ctor_error=info_error=bad_nid=create_error=owner_unloads=finish_reenters=attempts=0;fail_injection=fail_release=-1;
 memset(injections,0,sizeof(injections));memset(&impl,0,sizeof(impl));memset(&owner,0,sizeof(owner));memset(&plugin,0,sizeof(plugin));memset(fake_framework,0,sizeof(fake_framework));
 memset(&param,0,sizeof(param));param.name=(PafString){"dbrecovery_plugin",17,17};param.module_file=(PafString){"vs0:vsh/common/dbrecovery_plugin.suprx",37,37};param.module_interface_version=param.module_option=1;
 param.name.length=(uint32_t)strlen(param.name.data);param.module_file.length=(uint32_t)strlen(param.module_file.data);
 shell_info=(SceKernelModuleInfo){.size=sizeof(shell_info)};shell_info.segments[0].vaddr=shell_text;shell_info.segments[0].memsz=p->shell_text_size;
 seed_shell();seed_module();
#if LIVEAREA_DEBUG_LOGGING
 test_debug_capture_reset();
#endif
}
static void start(void)
{ assert(recovery_start(7,fixture_profile->shell_nid,&shell_info)==0);set_native_mocks();assert(recovery_stop()<0); }
static int prepare(void)
{
 preload_phase=PRELOAD_PREPARING;int result=prepare_recovery_module(&param);
 if(recovery_install_complete) {native_init=mock_init;native_stop=mock_stop;}
 return result;
}
static void make_plugin(void)
{
 mock_acquire(&owner,param.module_file.data,NULL,param.module_option,NULL);plugin.module=&owner;
#if LIVEAREA_DEBUG_LOGGING
 if(init_observer_id>=0)recovery_init_observer(&plugin);else mock_init(&plugin);
#else
 mock_init(&plugin);
#endif
}
static void check_success(void)
{
 fresh(fixture_profile);start();assert(prepare()==1 && starts==1 && inits==0 && impl.references==1 && all_capacity_live());
 make_plugin();assert(starts==1 && impl.references==2 && inits==1);
 recovery_load_finished(&plugin);assert(finishes==1 && impl.references==1 && !preload.impl && preload_phase==PRELOAD_IDLE && recovery_modid==42);
 mock_release(&owner);assert(stops==1 && recovery_modid<0 && !all_capacity_live() && !mutex_depth);
 assert(!memcmp(module_text+RECOVERY_STOP_OFFSET,expected_stop_entry,sizeof(expected_stop_entry)));
 for(unsigned int i=0;i<ARRAY_COUNT(recovery_patches);++i)assert(!memcmp(module_text+recovery_patches[i].offset,recovery_patches[i].expected,recovery_patches[i].size));
}
static void check_failures(void)
{
 unsigned int install_count=8;
#if LIVEAREA_DEBUG_LOGGING
 ++install_count;
#endif
 for(unsigned int call=1;call<=install_count;++call) {
  fresh(fixture_profile);start();fail_injection=(int)call;assert(prepare()==1 && !recovery_install_complete && recovery_modid<0);
  recovery_load_finished(NULL);assert(stops==1 && !preload.impl && !pending_loads);
  for(unsigned int i=1;i<32;++i)assert(!injections[i].live);
 }
 fresh(fixture_profile);start();fail_injection=3;fail_release=102;
 assert(prepare()==-1 && preload.impl && preload_phase==PRELOAD_BLOCKED && recovery_modid==42);
 recovery_load_hook(&param,mock_finish,17);assert(!loads && !releases && !load_in_flight);
 fresh(fixture_profile);start();assert(prepare()==1);fail_release=102;
 recovery_load_finished(NULL);assert(preload_phase==PRELOAD_BLOCKED && !preload.impl && recovery_modid==42 && !recovery_install_complete);
 recovery_load_hook(&param,mock_finish,17);assert(!loads);
 fresh(fixture_profile);start();ctor_error=-7;assert(prepare()==0 && releases==1 && stops==1 && !preload.impl && !recovery_install_complete);
 fresh(fixture_profile);start();available=1;assert(prepare()==0 && !acquires && !all_capacity_live());
 fresh(fixture_profile);start();bad_nid=1;assert(prepare()==1 && !all_capacity_live());recovery_load_finished(NULL);assert(stops==1);
 fresh(fixture_profile);start();info_error=1;assert(prepare()==1 && !all_capacity_live());recovery_load_finished(NULL);assert(stops==1);
 fresh(fixture_profile);start();module_text[recovery_patches[3].offset]^=1;assert(prepare()==1 && !all_capacity_live());recovery_load_finished(NULL);
}
static void check_all_original_bytes(void)
{
 for(unsigned int patch=0;patch<ARRAY_COUNT(recovery_patches);++patch)
  for(unsigned int byte=0;byte<recovery_patches[patch].size;++byte) {
   fresh(fixture_profile);start();module_text[recovery_patches[patch].offset+byte]^=1;
   assert(prepare()==1 && !recovery_install_complete && attempts==1);recovery_load_finished(NULL);
  }
 for(unsigned int byte=0;byte<sizeof(expected_stop_entry);++byte) {
  fresh(fixture_profile);start();module_text[RECOVERY_STOP_OFFSET+byte]^=1;
  assert(prepare()==1 && !recovery_install_complete && attempts==1);recovery_load_finished(NULL);
 }
 for(unsigned int byte=0;byte<20;++byte) {
  fresh(fixture_profile);start();module_data[byte]^=1;
  assert(prepare()==1 && !recovery_install_complete && attempts==1);recovery_load_finished(NULL);
 }
}

static void check_callbacks(void)
{
 fresh(fixture_profile);start();assert(prepare()==1);make_plugin();owner_unloads=1;
 recovery_load_finished(&plugin);assert(!preload.impl && !impl.references && stops==1 && !pending_loads);
 fresh(fixture_profile);start();assert(prepare()==1);make_plugin();finish_reenters=1;
 recovery_load_finished(&plugin);assert(pending_loads==1 && preload.impl && queued_count==1);
 queued[0](&plugin);assert(!preload.impl && impl.references==1 && !pending_loads && finishes==2);
 mock_release(&owner);assert(stops==1);
 fresh(fixture_profile);start();assert(prepare()==1);PafModule wrong={0};plugin.module=&wrong;
 recovery_load_finished(&plugin);assert(preload_phase==PRELOAD_BLOCKED && preload.impl && releases==0);
 fresh(fixture_profile);start();preload_phase=PRELOAD_PREPARING;recovery_load_hook(&param,mock_finish,17);assert(!loads);
 fresh(fixture_profile);start();param.module_option=0;recovery_load_hook(&param,mock_finish,17);assert(loads==1 && !acquires && queued[0]==mock_finish);
 fresh(fixture_profile);start();assert(prepare()==1);init_in_flight=1;assert(recovery_module_stop_redirect(0,NULL)==SCE_KERNEL_STOP_CANCEL && recovery_modid==42);init_in_flight=0;
}
static void check_startup(void)
{
 fresh(fixture_profile);create_error=1;assert(recovery_start(7,fixture_profile->shell_nid,&shell_info)<0 && !initialized);
 fresh(fixture_profile);fail_injection=0;assert(recovery_start(7,fixture_profile->shell_nid,&shell_info)<0 && !initialized && !mutex_created);
 for(unsigned int i=0;i<16;++i) {fresh(fixture_profile);shell_text[SHELL_RECOVERY_LOAD_CALL_OFFSET-12+i]^=1;assert(recovery_start(7,fixture_profile->shell_nid,&shell_info)<0 && !attempts);}
 fresh(fixture_profile);assert(recovery_start(7,0,&shell_info)<0);
 fresh(fixture_profile);--shell_info.segments[0].memsz;assert(recovery_start(7,fixture_profile->shell_nid,&shell_info)<0);
 fresh(fixture_profile);start();fake_framework[21]=5;assert(prepare()==1 && impl.option==3);recovery_load_finished(NULL);
}
int main(int argc,char **argv)
{
 shell_text=calloc(1,0x550000);module_text=calloc(1,RECOVERY_TEXT_SIZE);module_data=calloc(1,RECOVERY_DATA_SIZE);paf_text=calloc(1,0x300D00);paf_data=calloc(1,0x166D0);
 assert(shell_text && module_text && module_data && paf_text && paf_data);
 for(unsigned int i=0;i<ARRAY_COUNT(recovery_profiles);++i) {
  fixture_profile=&recovery_profiles[i];check_success();check_failures();check_all_original_bytes();check_callbacks();check_startup();
  printf("Recovery preload lifecycle/rollback/ownership/callbacks passed: 0x%08X logging=%d\n",fixture_profile->shell_nid,LIVEAREA_DEBUG_LOGGING);
 }
 assert((argc-1)%3==0);
 for(int i=1;i<argc;i+=3) {
  uint32_t nid=(uint32_t)strtoul(argv[i],NULL,0);const RecoveryProfile *p=NULL;
  for(unsigned int j=0;j<ARRAY_COUNT(recovery_profiles);++j)if(recovery_profiles[j].shell_nid==nid)p=&recovery_profiles[j];
  assert(p);fresh(p);FILE *f=fopen(argv[i+1],"rb");assert(f && fread(shell_text,1,p->shell_text_size,f)==p->shell_text_size);fclose(f);
  /* Validate invariant call-site instructions from the actual shell; adjust
   * only its loader-relocated local callback immediates to the host fixture. */
  uint8_t *ctx=shell_text+SHELL_RECOVERY_LOAD_CALL_OFFSET-12;
  assert(ctx[4]==8 && ctx[5]==0xA8 && ctx[10]==0 && ctx[11]==0x22 && !memcmp(ctx+12,p->load_call,4));seed_shell();
  f=fopen(argv[i+2],"rb");assert(f && fread(module_text,1,RECOVERY_TEXT_SIZE,f)==RECOVERY_TEXT_SIZE);fclose(f);
  start();assert(prepare()==1 && recovery_install_complete);make_plugin();recovery_load_finished(&plugin);mock_release(&owner);
  printf("Real recovery/shell bytes passed: 0x%08X\n",nid);
 }
 free(shell_text);free(module_text);free(module_data);free(paf_text);free(paf_data);return 0;
}
