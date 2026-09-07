#ifndef TEST_TAIHEN_H
#define TEST_TAIHEN_H
#include <psp2/types.h>
typedef struct {
	SceSize size;
	SceUID modid;
	uint32_t module_nid;
} tai_module_info_t;
int taiGetModuleInfo(const char *name, tai_module_info_t *info);
SceUID taiInjectData(SceUID modid, int segment, uint32_t offset,
	const void *data, SceSize size);
int taiInjectRelease(SceUID uid);

typedef uintptr_t tai_hook_ref_t;
#define TAI_ANY_LIBRARY 0xFFFFFFFFU
SceUID taiHookFunctionImport(tai_hook_ref_t *ref, const char *module,
	uint32_t library, uint32_t nid, const void *hook);
int taiGetModuleExportFunc(const char *module, uint32_t library, uint32_t nid,
	uintptr_t *function);
SceUID taiHookFunctionOffset(tai_hook_ref_t *ref, SceUID modid, int segment,
	uint32_t offset, int thumb, const void *hook);
int taiHookRelease(SceUID uid, tai_hook_ref_t ref);
int test_tai_continue(tai_hook_ref_t ref, ...);
#define TAI_CONTINUE(type, ref, ...) ((type)test_tai_continue((ref), ##__VA_ARGS__))
#endif
