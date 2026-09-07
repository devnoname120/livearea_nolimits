#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "transform-dis.c"
#include "arm/jump-patch.h"

/* The target address type is 32-bit even when this test runs on a 64-bit host. */
extern bool jump_dis_main(void *code, uint_tptr start, uint_tptr end,
	struct arch_dis_ctx arch);

static uint_tptr parse_offset(const char *text)
{
	char *end;
	errno = 0;
	unsigned long value = strtoul(text, &end, 0);
	assert(!errno && end != text && !*end && value <= UINT32_MAX);
	return (uint_tptr)value;
}

static void check_entry(FILE *file, uint_tptr base, uint_tptr offset,
	int expected_result, int expected_patch_size, int expected_consumed)
{
	unsigned char input[2048], output[1024], patch[MAX_JUMP_PATCH_SIZE];
	int offsets[64];
	void *out = output;
	void *patch_end = patch;
	struct arch_dis_ctx arch;
	uint_tptr start = base + offset;

	assert(fseek(file, (long)offset, SEEK_SET) == 0);
	assert(fread(input, 1, sizeof(input), file) == sizeof(input));
	arch_dis_ctx_init(&arch);
	arch.pc_low_bit = true;
	int size = jump_patch_size(start, 0x81200245, arch, false);
	uint_tptr end = start + size;
	assert(size == expected_patch_size);
	make_jump_patch(&patch_end, start, 0x81200245, arch);
	assert((unsigned char *)patch_end - patch == size);

	int result = transform_dis_main(input, &out, start, &end, 0x81600000,
		&arch, offsets, 0);
	printf("Hook relocation: entry=0x%08x patch=%d result=%d (%s)\n",
		(unsigned int)start, size, result, substitute_strerror(result));
	assert(result == expected_result);
	if (result == SUBSTITUTE_OK) {
		assert(end == start + expected_consumed);
		assert((unsigned char *)out - output == expected_consumed);
		assert(memcmp(input, output, (size_t)expected_consumed) == 0);
		assert(!jump_dis_main(input, start, end, arch));
		puts("  Prologue preserved byte-for-byte; backward-branch check passed");
	}
}

int main(int argc, char **argv)
{
	assert(argc >= 7);
	FILE *file = fopen(argv[1], "rb");
	assert(file);
	uint_tptr rejected = parse_offset(argv[2]);
	uint_tptr accepted = parse_offset(argv[3]);
	const uint_tptr bases[] = {0x81000000, 0x83200E90};

	for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); ++i) {
		check_entry(file, bases[i], rejected, SUBSTITUTE_ERR_FUNC_BAD_INSN_AT_START, 12, 12);
		check_entry(file, bases[i], accepted, SUBSTITUTE_OK, 12, 12);
		check_entry(file, bases[i], parse_offset(argv[6]), SUBSTITUTE_OK, 12, 14);
	}
	assert(fclose(file) == 0);
	file = fopen(argv[4], "rb");
	assert(file);
	uint_tptr initializer = parse_offset(argv[5]);
	for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); ++i)
		check_entry(file, bases[i], initializer, SUBSTITUTE_OK, 8, 10);
	assert(fclose(file) == 0);
	for (int arg = 7; arg < argc; ++arg) {
		file = fopen(argv[arg], "rb");
		assert(file);
		for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); ++i)
			check_entry(file, bases[i], 0x5F50, SUBSTITUTE_OK, 8, 10);
		assert(fclose(file) == 0);
	}
	return 0;
}
