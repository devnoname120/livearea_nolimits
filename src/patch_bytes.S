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

	.global patch_top_level_admission
	.type patch_top_level_admission, %object
patch_top_level_admission:
	/* Keep the Boolean result and live registers; free space for CMP.W. */
	movs r1, #1
	movs r2, #0
	cmp.w r6, #LIVEAREA_TOP_LEVEL_LIMIT
	it lt
	movlt r2, r1
	movs r3, r2
	ands r3, r5
	strb r3, [r4]
	nop
	nop
	.org patch_top_level_admission + 22
	.size patch_top_level_admission, . - patch_top_level_admission

	.global patch_top_level_message
	.type patch_top_level_message, %object
patch_top_level_message:
.Ltop_level_message:
	/* MOVT preserves comparison flags, so both branches can share CMP.W. */
	cmp.w r0, #LIVEAREA_TOP_LEVEL_LIMIT
	movt r4, #0x4502
	beq.n .Ltop_level_message + 42
	ble.n .Ltop_level_message + 28
	.org patch_top_level_message + 12
	.size patch_top_level_message, . - patch_top_level_message

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

	.global patch_cmp_r4_page_limit
	.type patch_cmp_r4_page_limit, %object
patch_cmp_r4_page_limit:
	cmp r4, #LIVEAREA_PAGE_LIMIT
	.size patch_cmp_r4_page_limit, . - patch_cmp_r4_page_limit

	.global patch_movs_r9_last_page
	.type patch_movs_r9_last_page, %object
patch_movs_r9_last_page:
	movs.w r9, #(LIVEAREA_PAGE_LIMIT - 1)
	.size patch_movs_r9_last_page, . - patch_movs_r9_last_page
