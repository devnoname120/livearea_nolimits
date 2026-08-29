#include "limits.h"

	.syntax unified
	.thumb

	.section .rodata.livearea_patch_bytes, "a", %progbits
	.balign 2

	.global patch_movs_r3_page_limit
	.type patch_movs_r3_page_limit, %object
patch_movs_r3_page_limit:
	movs r3, #LIVEAREA_PAGE_LIMIT
	.size patch_movs_r3_page_limit, . - patch_movs_r3_page_limit

	.global patch_cmp_r1_page_limit
	.type patch_cmp_r1_page_limit, %object
patch_cmp_r1_page_limit:
	cmp r1, #LIVEAREA_PAGE_LIMIT
	.size patch_cmp_r1_page_limit, . - patch_cmp_r1_page_limit

	.global patch_cmp_r6_page_limit
	.type patch_cmp_r6_page_limit, %object
patch_cmp_r6_page_limit:
	cmp r6, #LIVEAREA_PAGE_LIMIT
	.size patch_cmp_r6_page_limit, . - patch_cmp_r6_page_limit

	.global patch_cmp_r0_page_limit
	.type patch_cmp_r0_page_limit, %object
patch_cmp_r0_page_limit:
	cmp r0, #LIVEAREA_PAGE_LIMIT
	.size patch_cmp_r0_page_limit, . - patch_cmp_r0_page_limit

	.global patch_movs_r0_page_limit
	.type patch_movs_r0_page_limit, %object
patch_movs_r0_page_limit:
	movs r0, #LIVEAREA_PAGE_LIMIT
	.size patch_movs_r0_page_limit, . - patch_movs_r0_page_limit

	.global patch_movs_r2_page_limit
	.type patch_movs_r2_page_limit, %object
patch_movs_r2_page_limit:
	movs r2, #LIVEAREA_PAGE_LIMIT
	.size patch_movs_r2_page_limit, . - patch_movs_r2_page_limit

	.global patch_cmp_r6_top_level_limit
	.type patch_cmp_r6_top_level_limit, %object
patch_cmp_r6_top_level_limit:
	cmp r6, #LIVEAREA_TOP_LEVEL_LIMIT
	.size patch_cmp_r6_top_level_limit, . - patch_cmp_r6_top_level_limit

	.global patch_cmp_r0_top_level_limit
	.type patch_cmp_r0_top_level_limit, %object
patch_cmp_r0_top_level_limit:
	cmp r0, #LIVEAREA_TOP_LEVEL_LIMIT
	.size patch_cmp_r0_top_level_limit, . - patch_cmp_r0_top_level_limit

	.global patch_cmp_r7_icon_limit
	.type patch_cmp_r7_icon_limit, %object
patch_cmp_r7_icon_limit:
	cmp.w r7, #LIVEAREA_ICON_LIMIT
	.size patch_cmp_r7_icon_limit, . - patch_cmp_r7_icon_limit

	.global patch_cmp_r0_icon_limit
	.type patch_cmp_r0_icon_limit, %object
patch_cmp_r0_icon_limit:
	cmp.w r0, #LIVEAREA_ICON_LIMIT
	.size patch_cmp_r0_icon_limit, . - patch_cmp_r0_icon_limit

	.global patch_subs_r6_r5_icon_limit
	.type patch_subs_r6_r5_icon_limit, %object
patch_subs_r6_r5_icon_limit:
	subs.w r6, r5, #LIVEAREA_ICON_LIMIT
	.size patch_subs_r6_r5_icon_limit, . - patch_subs_r6_r5_icon_limit

	.global patch_cmp_r5_icon_limit
	.type patch_cmp_r5_icon_limit, %object
patch_cmp_r5_icon_limit:
	cmp.w r5, #LIVEAREA_ICON_LIMIT
	.size patch_cmp_r5_icon_limit, . - patch_cmp_r5_icon_limit

	.global patch_movs_r3_icon_limit
	.type patch_movs_r3_icon_limit, %object
patch_movs_r3_icon_limit:
	movs.w r3, #LIVEAREA_ICON_LIMIT
	.size patch_movs_r3_icon_limit, . - patch_movs_r3_icon_limit

	.global patch_rsbs_r1_r0_icon_limit
	.type patch_rsbs_r1_r0_icon_limit, %object
patch_rsbs_r1_r0_icon_limit:
	rsbs.w r1, r0, #LIVEAREA_ICON_LIMIT
	.size patch_rsbs_r1_r0_icon_limit, . - patch_rsbs_r1_r0_icon_limit
