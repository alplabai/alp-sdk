/* SPDX-License-Identifier: Apache-2.0 -- host stub: no cache on the host. */
#include <stddef.h>
static inline int sys_cache_data_flush_range(void *a, size_t n)
{
	(void)a;
	(void)n;
	return 0;
}
static inline int sys_cache_data_invd_range(void *a, size_t n)
{
	(void)a;
	(void)n;
	return 0;
}
