#ifndef LIVEAREA_NOLIMITS_H
#define LIVEAREA_NOLIMITS_H

/*
 * These constants target the supported SceShell implementations. The
 * assembler rejects limits that cannot be encoded by the replacement blocks.
 */
#define LIVEAREA_PAGE_LIMIT       50
#define LIVEAREA_ICONS_PER_PAGE   10
#define LIVEAREA_TOP_LEVEL_LIMIT  500
#define LIVEAREA_ICON_LIMIT       4000

#if LIVEAREA_PAGE_LIMIT < 1 || LIVEAREA_PAGE_LIMIT > 255
#error LIVEAREA_PAGE_LIMIT must fit in the original 8-bit Thumb immediates
#endif

/* Recovery cannot safely hook its pre-start allocator to impose a lower limit. */
#if LIVEAREA_TOP_LEVEL_LIMIT != LIVEAREA_PAGE_LIMIT * LIVEAREA_ICONS_PER_PAGE
#error LIVEAREA_TOP_LEVEL_LIMIT must equal the configured page capacity
#endif

#if LIVEAREA_TOP_LEVEL_LIMIT > LIVEAREA_ICON_LIMIT || LIVEAREA_ICON_LIMIT > 0x7FFFFFFF
#error LiveArea limits must preserve the signed counted-icon capacity
#endif

#if LIVEAREA_ICONS_PER_PAGE != 10
#error Firmware icon-slot and appearance-table bounds must remain unchanged
#endif

#endif /* LIVEAREA_NOLIMITS_H */
