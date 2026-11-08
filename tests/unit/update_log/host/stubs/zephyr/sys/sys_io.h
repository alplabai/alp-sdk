/* SPDX-License-Identifier: Apache-2.0 -- host stub: every MHU register reads
 * non-zero (sender ready) and writes are dropped; the doorbell is unused. */
#include <stdint.h>
static inline uint32_t sys_read32(uintptr_t a)
{
	(void)a;
	return 1U;
}
static inline void sys_write32(uint32_t v, uintptr_t a)
{
	(void)v;
	(void)a;
}
