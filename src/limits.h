#ifndef LIVEAREA_NOLIMITS_H
#define LIVEAREA_NOLIMITS_H

/*
 * These constants target the supported SceShell implementations. The
 * assembler will reject values that cannot be represented by the original
 * instruction forms.
 */
#define LIVEAREA_PAGE_LIMIT       20
#define LIVEAREA_ICONS_PER_PAGE   10
#define LIVEAREA_TOP_LEVEL_LIMIT  200
#define LIVEAREA_ICON_LIMIT       1000

#if LIVEAREA_PAGE_LIMIT > 255
#error LIVEAREA_PAGE_LIMIT must fit in the original 8-bit Thumb immediates
#endif

#if LIVEAREA_TOP_LEVEL_LIMIT > 255
#error LIVEAREA_TOP_LEVEL_LIMIT must fit in the original 8-bit Thumb immediates
#endif

#endif /* LIVEAREA_NOLIMITS_H */
