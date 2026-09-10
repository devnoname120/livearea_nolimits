#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr/thread.h>

#include "../src/debug_log.h"

static char output[8192];
static size_t output_size;
static int fail_primary;
static int open_calls;
static int write_calls;
static int sync_calls;
static int close_calls;
static int remove_calls;
static int rename_calls;
static const char *opened_path;
static unsigned int write_chunk;
static int fail_write;

SceUID sceIoOpen(const char *path, int flags, int mode)
{
	assert(flags == (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC));
	assert(mode == 0666);
	++open_calls;
	opened_path = path;
	if (fail_primary && strcmp(path, LIVEAREA_DEBUG_LOG_PATH) == 0)
		return -1;
	return strcmp(path, LIVEAREA_DEBUG_LOG_PATH) == 0 ? 7 : 8;
}

int sceIoWrite(SceUID fd, const void *data, SceSize size)
{
	assert(fd == 7 || fd == 8);
	++write_calls;
	if (fail_write)
		return 0;
	if (write_chunk && size > write_chunk)
		size = write_chunk;
	assert(output_size + size < sizeof(output));
	memcpy(output + output_size, data, size);
	output_size += size;
	output[output_size] = '\0';
	return (int)size;
}

int sceIoSyncByFd(SceUID fd, int flag)
{
	assert((fd == 7 || fd == 8) && flag == 0);
	++sync_calls;
	return 0;
}

int sceIoClose(SceUID fd)
{
	assert(fd == 7 || fd == 8);
	++close_calls;
	return 0;
}

int sceIoRemove(const char *path)
{
	assert(strstr(path, "livearea_nolimits-debug.previous.log"));
	++remove_calls;
	return 0;
}

int sceIoRename(const char *old_path, const char *new_path)
{
	assert(strstr(old_path, "livearea_nolimits-debug.log"));
	assert(strstr(new_path, "livearea_nolimits-debug.previous.log"));
	++rename_calls;
	return 0;
}

int sceClibVsnprintf(char *dst, SceSize size, const char *format, va_list args)
{
	return vsnprintf(dst, size, format, args);
}

int sceClibSnprintf(char *dst, SceSize size, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	int result = vsnprintf(dst, size, format, args);
	va_end(args);
	return result;
}

SceUInt64 sceKernelGetProcessTimeWide(void)
{
	static SceUInt64 now = 1000;
	now += 125;
	return now;
}

int sceKernelGetThreadId(void)
{
	return 0x1234;
}

static void reset_output(void)
{
	memset(output, 0, sizeof(output));
	output_size = 0;
	open_calls = write_calls = sync_calls = close_calls = 0;
	remove_calls = rename_calls = 0;
	opened_path = NULL;
	write_chunk = 0;
	fail_write = 0;
}

int main(void)
{
	static const uint8_t bytes[] = {0x00, 0x7F, 0x80, 0xFF};

	reset_output();
	debug_log_open();
	assert(open_calls == 1 && remove_calls == 1 && rename_calls == 1);
	assert(strcmp(opened_path, LIVEAREA_DEBUG_LOG_PATH) == 0);
	debug_log_flush();
	assert(sync_calls == 1);
	debug_log_flush();
	assert(sync_calls == 1);
	debug_logf("main", "module-start argc=%u result=%d", 3U, -7);
	debug_log_hex("shell", "patch-mismatch", 0x5531EU, bytes, sizeof(bytes));
	debug_log_close();
	assert(write_calls == 4 && close_calls == 1);
#if LIVEAREA_DEBUG_SYNC_INTERVAL == 1
	assert(sync_calls == write_calls);
#else
	assert(LIVEAREA_DEBUG_SYNC_INTERVAL == 8);
	assert(sync_calls == 2);
#endif
	assert(strstr(output, "[debug] session-start build=test-build"));
	assert(strstr(output, "path=ur0:/data/livearea_nolimits-debug.log"));
	assert(strstr(output, "[main] module-start argc=3 result=-7"));
	assert(strstr(output, "[shell] patch-mismatch offset=0x0005531E size=4 data=00 7F 80 FF"));
	assert(strstr(output, "[debug] session-stop"));
	assert(strstr(output, "seq=000001"));
	assert(strstr(output, "time_us=1125"));
	assert(strstr(output, "thread=0x00001234"));

	reset_output();
	fail_primary = 1;
	debug_log_open();
	assert(open_calls == 2 && remove_calls == 2 && rename_calls == 2);
	assert(strcmp(opened_path, LIVEAREA_DEBUG_LOG_FALLBACK_PATH) == 0);
	assert(strstr(output, "path=ux0:/data/livearea_nolimits-debug.log"));
	debug_log_close();
	assert(write_calls == 2 && close_calls == 1);
#if LIVEAREA_DEBUG_SYNC_INTERVAL == 1
	assert(sync_calls == write_calls);
#else
	assert(sync_calls == 1);
#endif

	reset_output();
	fail_primary = 0;
	write_chunk = 7;
	debug_log_open();
	debug_logf("test", "short writes retain the complete line");
	assert(strstr(output, "short writes retain the complete line\n"));
	assert(write_calls > 2);
	fail_write = 1;
	debug_logf("test", "storage unavailable");
	fail_write = 0;
	debug_logf("test", "storage recovered");
	assert(strstr(output, "storage recovered write_failures=1\n"));
	debug_log_close();

	printf("Debug log: rotation, fallback, short writes, failed writes, sequencing, timestamps, formatting, hex dumps and sync interval %u passed\n",
		(unsigned int)LIVEAREA_DEBUG_SYNC_INTERVAL);
	return 0;
}
