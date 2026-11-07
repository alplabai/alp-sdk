/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chunking policy of the GD32 TRNG entropy driver, kept free of devicetree and
 * of the supervisor singleton so it is host-testable (tests/unit/
 * entropy_alp_gd32_trng_fill).
 */

#ifndef ALP_ENTROPY_GD32_TRNG_FILL_H_
#define ALP_ENTROPY_GD32_TRNG_FILL_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* One bridge TRNG_READ: fill buf[0..len), len <= max_chunk.  0 or -errno. */
typedef int (*gd32_trng_read_fn)(void *ctx, uint8_t *buf, size_t len);

/*
 * Fill all `len` bytes in <= max_chunk pieces.  All-or-nothing: Zephyr's
 * entropy_get_entropy() contract is a full buffer or an error, so a partial
 * fill is an error, never a short success.
 */
static inline int
gd32_trng_fill(gd32_trng_read_fn read, void *ctx, uint8_t *buf, size_t len, size_t max_chunk)
{
	size_t done = 0u;

	if (read == NULL || buf == NULL || max_chunk == 0u) {
		return -EINVAL;
	}
	while (done < len) {
		size_t left  = len - done;
		size_t chunk = left > max_chunk ? max_chunk : left;
		int    rc    = read(ctx, buf + done, chunk);

		if (rc != 0) {
			return rc < 0 ? rc : -EIO;
		}
		done += chunk;
	}
	return 0;
}

#endif /* ALP_ENTROPY_GD32_TRNG_FILL_H_ */
