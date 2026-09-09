#include "debug_log.h"

#if !LIVEAREA_DEBUG_LOGGING
#error "debug_log.c must only be compiled with LIVEAREA_DEBUG_LOGGING enabled"
#endif

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/types.h>
#include <stdarg.h>

#ifndef LIVEAREA_DEBUG_BUILD_ID
#define LIVEAREA_DEBUG_BUILD_ID "unspecified"
#endif

#define DEBUG_LINE_CAPACITY 512
#define DEBUG_HEX_CAPACITY 64

static SceUID log_fd = -1;
static const char *log_path;
static volatile int log_lock;
static volatile unsigned int dropped_lines;
static unsigned int sequence;
static unsigned int unsynced_lines;

static void sync_log_locked(void)
{
	if (log_fd >= 0 && unsynced_lines) {
		(void)sceIoSyncByFd(log_fd, 0);
		unsynced_lines = 0;
	}
}

static SceUID open_log(const char *path, const char *previous)
{
	(void)sceIoRemove(previous);
	(void)sceIoRename(path, previous);
	return sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
}

static size_t clamp_length(int length, size_t capacity)
{
	if (length <= 0)
		return 0;
	if ((size_t)length >= capacity)
		return capacity - 1;
	return (size_t)length;
}

void debug_logf(const char *component, const char *format, ...)
{
	char line[DEBUG_LINE_CAPACITY];
	SceUInt64 now;
	unsigned int dropped;
	size_t length;
	int formatted;
	va_list args;

	if (log_fd < 0)
		return;
	if (!__sync_bool_compare_and_swap(&log_lock, 0, 1)) {
		__sync_fetch_and_add(&dropped_lines, 1);
		return;
	}

	now = sceKernelGetProcessTimeWide();
	dropped = __sync_lock_test_and_set(&dropped_lines, 0);
	formatted = sceClibSnprintf(line, sizeof(line),
		"seq=%06u time_us=%llu thread=0x%08X [%s] ",
		++sequence, (unsigned long long)now,
		(unsigned int)sceKernelGetThreadId(), component ? component : "unknown");
	length = clamp_length(formatted, sizeof(line));

	va_start(args, format);
	formatted = sceClibVsnprintf(line + length, sizeof(line) - length,
		format ? format : "(null)", args);
	va_end(args);
	length += clamp_length(formatted, sizeof(line) - length);

	if (dropped && length < sizeof(line) - 1) {
		formatted = sceClibSnprintf(line + length, sizeof(line) - length,
			" dropped=%u", dropped);
		length += clamp_length(formatted, sizeof(line) - length);
	}
	if (length == 0 || line[length - 1] != '\n') {
		if (length >= sizeof(line) - 1)
			length = sizeof(line) - 2;
		line[length++] = '\n';
	}

	(void)sceIoWrite(log_fd, line, (SceSize)length);
	++unsynced_lines;
	if (unsynced_lines >= LIVEAREA_DEBUG_SYNC_INTERVAL)
		sync_log_locked();
	__sync_lock_release(&log_lock);
}

void debug_log_flush(void)
{
	if (log_fd < 0 || !__sync_bool_compare_and_swap(&log_lock, 0, 1))
		return;
	sync_log_locked();
	__sync_lock_release(&log_lock);
}

void debug_log_hex(const char *component, const char *event, uint32_t offset,
	const void *data, size_t size)
{
	static const char hex[] = "0123456789ABCDEF";
	const uint8_t *bytes = data;
	char encoded[DEBUG_HEX_CAPACITY * 3];
	size_t count = size < DEBUG_HEX_CAPACITY ? size : DEBUG_HEX_CAPACITY;
	size_t cursor = 0;

	for (size_t index = 0; index < count; ++index) {
		if (index)
			encoded[cursor++] = ' ';
		encoded[cursor++] = hex[bytes[index] >> 4];
		encoded[cursor++] = hex[bytes[index] & 15];
	}
	encoded[cursor] = '\0';
	debug_logf(component, "%s offset=0x%08X size=%u data=%s%s",
		event ? event : "bytes", (unsigned int)offset, (unsigned int)size,
		encoded, count < size ? " ..." : "");
}

void debug_log_open(void)
{
	if (log_fd >= 0)
		return;

	sequence = 0;
	log_lock = 0;
	dropped_lines = 0;
	unsynced_lines = 0;
	log_path = LIVEAREA_DEBUG_LOG_PATH;
	log_fd = open_log(LIVEAREA_DEBUG_LOG_PATH, LIVEAREA_DEBUG_LOG_PREVIOUS_PATH);
	if (log_fd < 0) {
		log_path = LIVEAREA_DEBUG_LOG_FALLBACK_PATH;
		log_fd = open_log(LIVEAREA_DEBUG_LOG_FALLBACK_PATH,
			LIVEAREA_DEBUG_LOG_FALLBACK_PREVIOUS_PATH);
	}
	if (log_fd >= 0)
		debug_logf("debug", "session-start build=%s path=%s",
			LIVEAREA_DEBUG_BUILD_ID, log_path);
}

void debug_log_close(void)
{
	SceUID fd;

	if (log_fd < 0)
		return;
	debug_logf("debug", "session-stop");
	if (!__sync_bool_compare_and_swap(&log_lock, 0, 1))
		return;
	fd = log_fd;
	sync_log_locked();
	log_fd = -1;
	(void)sceIoClose(fd);
	__sync_lock_release(&log_lock);
}
