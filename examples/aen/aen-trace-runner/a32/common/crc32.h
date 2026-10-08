/* a32/common/crc32.h -- zlib-compatible CRC32 (reflected, poly 0xEDB88320).
 * Pure C, no libc: built into the A32 stub and into the host tests, so the
 * LAUNCH check and mkpayload.py (zlib.crc32) agree by test, not by argument. */
#ifndef TR_CRC32_H
#define TR_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Continue a CRC: pass 0 to start. tr_crc32(0, buf, n) == zlib.crc32(buf). */
uint32_t tr_crc32(uint32_t crc, const void *buf, size_t n);

#endif /* TR_CRC32_H */
