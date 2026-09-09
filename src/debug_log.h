#ifndef LIVEAREA_DEBUG_LOG_H
#define LIVEAREA_DEBUG_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifndef LIVEAREA_DEBUG_LOGGING
#define LIVEAREA_DEBUG_LOGGING 0
#endif

#ifndef LIVEAREA_DEBUG_SYNC_INTERVAL
#define LIVEAREA_DEBUG_SYNC_INTERVAL 1
#endif

#if LIVEAREA_DEBUG_SYNC_INTERVAL < 1
#error "LIVEAREA_DEBUG_SYNC_INTERVAL must be at least 1"
#endif

#define LIVEAREA_DEBUG_LOG_PATH "ur0:/data/livearea_nolimits-debug.log"
#define LIVEAREA_DEBUG_LOG_PREVIOUS_PATH "ur0:/data/livearea_nolimits-debug.previous.log"
#define LIVEAREA_DEBUG_LOG_FALLBACK_PATH "ux0:/data/livearea_nolimits-debug.log"
#define LIVEAREA_DEBUG_LOG_FALLBACK_PREVIOUS_PATH "ux0:/data/livearea_nolimits-debug.previous.log"

#if LIVEAREA_DEBUG_LOGGING

void debug_log_open(void);
void debug_log_close(void);
void debug_log_flush(void);
void debug_logf(const char *component, const char *format, ...)
	__attribute__((format(printf, 2, 3)));
void debug_log_hex(const char *component, const char *event, uint32_t offset,
	const void *data, size_t size);

#else

#define debug_log_open() ((void)0)
#define debug_log_close() ((void)0)
#define debug_log_flush() ((void)0)
#define debug_logf(...) ((void)0)
#define debug_log_hex(...) ((void)0)

#endif

#endif
