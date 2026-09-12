#include "shell_detour.h"
#include <taihen.h>

int shell_detour_encode_branch(uint8_t out[4], uintptr_t source, uintptr_t target)
{
	/* Thumb B.W T4. Four bytes suffice within +/-16 MiB, including an entry
	 * at address 2 mod 4. Refuse ARM destinations or an out-of-range target. */
	if ((source & 1U) || !(target & 1U) ||
		source > UINT32_MAX - 4U || target > UINT32_MAX)
		return -1;
	int64_t delta = (int64_t)(target & ~(uintptr_t)1) - ((int64_t)source + 4);
	if (delta < -16777216 || delta > 16777214 || (delta & 1))
		return -1;
	uint32_t bits = (uint32_t)delta;
	uint32_t sign = (bits >> 24) & 1U;
	uint32_t j1 = 1U ^ ((bits >> 23) & 1U) ^ sign;
	uint32_t j2 = 1U ^ ((bits >> 22) & 1U) ^ sign;
	uint16_t first = 0xF000U | (sign << 10) | ((bits >> 12) & 0x3FFU);
	uint16_t second = 0x9000U | (j1 << 13) | (j2 << 11) | ((bits >> 1) & 0x7FFU);
	out[0] = first; out[1] = first >> 8;
	out[2] = second; out[3] = second >> 8;
	return 0;
}

SceUID shell_detour_install(SceUID module, uint32_t offset,
	uintptr_t source, const void *target)
{
	uint8_t branch[4];
	if (shell_detour_encode_branch(branch, source, (uintptr_t)target) < 0)
		return -1;
	return taiInjectData(module, 0, offset, branch, sizeof(branch));
}

int shell_detour_encode_call(uint8_t out[4], uintptr_t source, uintptr_t target)
{
	int result=shell_detour_encode_branch(out,source,target);
	if(result>=0) out[3]|=0x40; /* B.W T4 -> BL T1; same displacement, sets LR. */
	return result;
}
