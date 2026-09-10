#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "transform-dis.c"
#include "arm/jump-patch.h"

extern bool jump_dis_main(void *code, uint_tptr start, uint_tptr end,
	struct arch_dis_ctx arch);

static uint_tptr parse_offset(const char *text)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 0);
	assert(!errno && end != text && !*end && value <= UINT32_MAX);
	return (uint_tptr)value;
}

static void check_entry(const char *path, uint_tptr base, uint_tptr offset)
{
	unsigned char input[2048], output[2048];
	int offsets[64];
	struct arch_dis_ctx arch;
	void *out = output;
	uint_tptr start = base + offset;
	uint_tptr end;
	FILE *file = fopen(path, "rb");
	int patch_size;
	int result;

	assert(file != NULL);
	assert(fseek(file, (long)offset, SEEK_SET) == 0);
	assert(fread(input, 1, sizeof(input), file) == sizeof(input));
	assert(fclose(file) == 0);
	arch_dis_ctx_init(&arch);
	arch.pc_low_bit = true;
	patch_size = jump_patch_size(start, 0x81200245, arch, false);
	assert(patch_size > 0 && patch_size <= MAX_JUMP_PATCH_SIZE);
	end = start + patch_size;
	result = transform_dis_main(input, &out, start, &end, 0x81600000,
		&arch, offsets, 0);
	printf("Recovery callback relocation: %s +0x%X base=0x%08X patch=%d result=%d (%s)\n",
		path, (unsigned int)offset, (unsigned int)base, patch_size,
		result, substitute_strerror(result));
	assert(result == SUBSTITUTE_OK);
	assert(end >= start + (uint_tptr)patch_size);
	assert((unsigned char *)out > output);
	assert(!jump_dis_main(input, start, end, arch));
}

int main(int argc, char **argv)
{
	const uint_tptr shell_bases[] = {0x81000000, 0x83200E90};
	uint_tptr shell_offset;
	unsigned int index;

	assert(argc == 3);
	shell_offset = parse_offset(argv[2]);
	for (index = 0; index < sizeof(shell_bases) / sizeof(shell_bases[0]); ++index)
		check_entry(argv[1], shell_bases[index], shell_offset);
	return 0;
}
