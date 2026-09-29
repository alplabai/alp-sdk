/* a32/common/crc32.c -- see crc32.h. */
#include "crc32.h"

/* ponytail: nibble table (64 B, no init) at two lookups per byte; a 1 KiB
 * byte table halves the work if LAUNCH latency on a 512 KiB image matters. */
static const uint32_t tr_crc32_nibble[16] = {
	0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
	0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
	0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t tr_crc32(uint32_t crc, const void *buf, size_t n)
{
	const uint8_t *p = buf;

	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		crc = (crc >> 4) ^ tr_crc32_nibble[crc & 15u];
		crc = (crc >> 4) ^ tr_crc32_nibble[crc & 15u];
	}
	return ~crc;
}
