/* tests/host/test_crc32.c -- a32/common/crc32.c (the stub's LAUNCH check)
 * against values computed by Python's zlib.crc32 on the same buffers.
 * mkpayload.py --selftest re-runs this comparison live against zlib. */
#include <assert.h>
#include <stdio.h>

#include "../../a32/common/crc32.c"

int main(void)
{
	static uint8_t ramp[1024], mix[4097];

	for (unsigned i = 0; i < sizeof(ramp); i++) ramp[i] = (uint8_t)i;
	for (unsigned i = 0; i < sizeof(mix); i++) mix[i] = (uint8_t)(i * 37u + 11u);

	assert(tr_crc32(0, "", 0) == 0x00000000u);
	assert(tr_crc32(0, "a", 1) == 0xE8B7BE43u);
	assert(tr_crc32(0, "123456789", 9) == 0xCBF43926u);
	assert(tr_crc32(0, ramp, sizeof(ramp)) == 0xB70B4C26u);
	assert(tr_crc32(0, mix, sizeof(mix)) == 0x424089AEu);
	/* chaining == one pass, as zlib.crc32(b, zlib.crc32(a)) */
	assert(tr_crc32(tr_crc32(0, "123", 3), "456789", 6) == 0xCBF43926u);
	puts("test_crc32: ok");
	return 0;
}
