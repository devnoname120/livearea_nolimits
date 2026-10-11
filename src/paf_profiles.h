#ifndef LIVEAREA_PAF_PROFILES_H
#define LIVEAREA_PAF_PROFILES_H
#include "instance_cache_firmware.h"
typedef struct {
	uint32_t text_size,data_size,pool_table,image_table,allocate,surface_release,get_surface,apply_status,apply_status_return,lock_cache,unlock_cache,cache_clock,cache_mutex,surface_mutex,apply_consumer,scan,module_acquire,module_release,module_interface,framework_slot,plugin_initializer_call;
	const uint32_t *pool_values, *image_values;
	const uint8_t *allocate_check, *release_check, *surface_check, *status_check;
} PafLayout;
static const PafLayout paf_layout_0 = {
	0x300D00U,0x166D0U,0x2E3960U,0x2E101CU,0x8DD5AU,0x8997AU,0x13344U,0x15FA08U,0x15FA12U,0x53E90U,0x53EA4U,0xB224U,0xB238U,0x10230U,0x15F9E2U,0x15E6AU,0x4722AU,0x473D4U,0x471D8U,0x194U,0x58E5EU,
	expected_pool_table,expected_image_table,
	expected_pool_allocate,expected_surface_release,expected_get_surface,expected_apply_status_call
};
static const uint32_t paf_pool_values_1[] = {0x0U,0x0U,0x8E791U,0x8E833U,0x8E84BU,0x8E8DFU,0x8E3FDU};
static const uint32_t paf_image_values_1[] = {0x0U,0x0U,0x1DF519U,0x133B5U,0x133F5U,0x134C9U,0x1340DU,0x1349BU,0x1354BU,0x13AEDU,0x131F7U,0x13345U,0x1DF515U,0x1DF51DU,0x1DF525U,0x1DF52DU,0x1DF535U,0x1DF539U,0xFFFFFF9CU,0x0U,0x1468FU,0x14681U,0x14673U,0x1DF549U,0x1DF559U,0x1DF569U,0x1DF579U,0x1DF589U,0x1DF599U,0x1DF5A9U};
static const PafLayout paf_layout_1 = {
	0x3019B8U,0x166D0U,0x2E4618U,0x2E1CD4U,0x8E84AU,0x8A46AU,0x13344U,0x1605BCU,0x1605C6U,0x54980U,0x54994U,0xB224U,0xB238U,0x10230U,0x160596U,0x15E6AU,0x4722AU,0x473D4U,0x471D8U,0x194U,0x5994EU,
	paf_pool_values_1,paf_image_values_1,
	expected_pool_allocate,expected_surface_release,expected_get_surface,expected_apply_status_call
};
typedef struct { uint32_t nid; const PafLayout *layout; } PafIdentity;
static const PafIdentity paf_identities[] = {
	{0xCD679177U,&paf_layout_0},
	{0x73F90499U,&paf_layout_0},
	{0x4AB91355U,&paf_layout_0},
	{0x0305714CU,&paf_layout_0},
	{0xE15A86EDU,&paf_layout_0},
	{0xE7A89CA9U,&paf_layout_0},
	{0x66EC0C5DU,&paf_layout_0},
	{0x52A49770U,&paf_layout_0},
	{0x78DF8738U,&paf_layout_0},
	{0x09111E96U,&paf_layout_0},
	{0xD5A425AFU,&paf_layout_0},
	{0x9CE470B4U,&paf_layout_0},
	{0x0DBFBA9BU,&paf_layout_0},
	{0x0CEAECDDU,&paf_layout_0},
	{0xB8DB18FDU,&paf_layout_0},
	{0xDFE774A1U,&paf_layout_0},
	{0xA1FE073AU,&paf_layout_0},
	{0xE3D2D5CEU,&paf_layout_0},
	{0x2985CD70U,&paf_layout_0},
	{0x534D3F3DU,&paf_layout_0},
	{0xD0380FBCU,&paf_layout_0},
	{0xFA5137D0U,&paf_layout_1},
	{0x43AAED2BU,&paf_layout_1},
	{0xF8E53ADDU,&paf_layout_1},
	{0xD6AB6069U,&paf_layout_1},
	{0x352D98D4U,&paf_layout_1},
	{0x9E5A741CU,&paf_layout_1},
	{0xD1BC32EEU,&paf_layout_1},
	{0xE9488C33U,&paf_layout_1},
	{0x441BBC71U,&paf_layout_1},
	{0x1FE36D93U,&paf_layout_1},
};
static inline const PafLayout *paf_layout_for_nid(uint32_t nid) {
	for (unsigned int i=0;i<sizeof(paf_identities)/sizeof(paf_identities[0]);++i)
		if(paf_identities[i].nid==nid) return paf_identities[i].layout;
	return NULL;
}
#endif
