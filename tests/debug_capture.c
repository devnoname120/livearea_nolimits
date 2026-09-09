#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../src/debug_log.h"
#include "debug_capture.h"

static char capture[2 * 1024 * 1024];
static size_t capture_size;

static void append(const char *component, const char *format, va_list args)
{
	if (capture_size >= sizeof(capture) - 1)
		return;
	int prefix = snprintf(capture + capture_size, sizeof(capture) - capture_size,
		"[%s] ", component ? component : "unknown");
	if (prefix < 0)
		return;
	capture_size += (size_t)prefix < sizeof(capture) - capture_size
		? (size_t)prefix : sizeof(capture) - capture_size - 1;
	if (capture_size >= sizeof(capture) - 1)
		return;
	int body = vsnprintf(capture + capture_size, sizeof(capture) - capture_size,
		format ? format : "(null)", args);
	if (body < 0)
		return;
	capture_size += (size_t)body < sizeof(capture) - capture_size
		? (size_t)body : sizeof(capture) - capture_size - 1;
	if (capture_size < sizeof(capture) - 1)
		capture[capture_size++] = '\n';
	capture[capture_size] = '\0';
}

void test_debug_capture_reset(void)
{
	capture_size = 0;
	capture[0] = '\0';
}

int test_debug_capture_contains(const char *text)
{
	return strstr(capture, text) != NULL;
}

unsigned int test_debug_capture_count(const char *text)
{
	unsigned int count = 0;
	const char *cursor = capture;
	size_t length = strlen(text);
	while ((cursor = strstr(cursor, text)) != NULL) {
		++count;
		cursor += length;
	}
	return count;
}

void debug_log_open(void)
{
	debug_logf("debug", "session-start");
}

void debug_log_close(void)
{
	debug_logf("debug", "session-stop");
}

void debug_log_flush(void)
{
}

void debug_logf(const char *component, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	append(component, format, args);
	va_end(args);
}

void debug_log_hex(const char *component, const char *event, uint32_t offset,
	const void *data, size_t size)
{
	(void)data;
	debug_logf(component, "%s offset=0x%08X size=%u",
		event ? event : "bytes", (unsigned int)offset, (unsigned int)size);
}
