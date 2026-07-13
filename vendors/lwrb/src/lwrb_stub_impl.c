/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stub implementation of the non-inline LwRB API surface declared in
 * vendors/lwrb/include/lwrb/lwrb.h.  Single-producer / single-consumer
 * safe via the canonical empty/full disambiguation (one slot reserved
 * so r == w always means empty and (w + 1) % size == r always means
 * full).
 *
 * Replaced wholesale by the upstream `MaJerle/lwrb` sources at
 * `modules/lib/lwrb/src/` once the `extras-lwrb-nanopb` group is
 * enabled (`west update --group-filter +extras-lwrb-nanopb`) and
 * the Zephyr build picks up the upstream module via
 * EXTRA_ZEPHYR_MODULES.  Interim/deferred as of v0.9 -- no
 * committed release date for the swap.  Both impls share the
 * lwrb_t ABI and the function signatures here, so SDK consumers do
 * not change.
 *
 * Concurrency contract:
 *   - lwrb_write / lwrb_advance / lwrb_get_free MAY be called from
 *     the producer context only (typically a UART or DMA ISR).
 *   - lwrb_read / lwrb_peek / lwrb_skip / lwrb_get_full MAY be called
 *     from the consumer context only (typically a worker thread).
 *   - lwrb_is_ready / lwrb_init / lwrb_free / lwrb_reset are setup /
 *     teardown helpers; the caller is responsible for serialising
 *     them against producer + consumer activity.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lwrb/lwrb.h"

static size_t advance(size_t idx, size_t step, size_t cap)
{
    idx += step;
    if (idx >= cap) {
        idx -= cap;
    }
    return idx;
}

size_t lwrb_get_full(const lwrb_t *b)
{
    if (b == NULL || b->buff == NULL) {
        return 0;
    }
    size_t r = b->r;
    size_t w = b->w;
    if (w >= r) {
        return w - r;
    }
    return b->size - r + w;
}

size_t lwrb_get_free(const lwrb_t *b)
{
    if (b == NULL || b->buff == NULL || b->size < 2u) {
        return 0;
    }
    /* One slot is reserved to disambiguate empty (r==w) from full. */
    return b->size - 1u - lwrb_get_full(b);
}

size_t lwrb_write(lwrb_t *b, const void *data, size_t len)
{
    if (b == NULL || b->buff == NULL || data == NULL || len == 0u) {
        return 0;
    }
    size_t free_bytes = lwrb_get_free(b);
    if (len > free_bytes) {
        len = free_bytes;
    }
    if (len == 0u) {
        return 0;
    }
    const uint8_t *src = (const uint8_t *)data;
    size_t         w   = b->w;
    /* Two-segment copy to handle wrap. */
    size_t first = b->size - w;
    if (first > len) {
        first = len;
    }
    memcpy(b->buff + w, src, first);
    if (len > first) {
        memcpy(b->buff, src + first, len - first);
    }
    b->w = advance(w, len, b->size);
    return len;
}

size_t lwrb_read(lwrb_t *b, void *data, size_t len)
{
    if (b == NULL || b->buff == NULL || data == NULL || len == 0u) {
        return 0;
    }
    size_t full = lwrb_get_full(b);
    if (len > full) {
        len = full;
    }
    if (len == 0u) {
        return 0;
    }
    uint8_t *dst   = (uint8_t *)data;
    size_t   r     = b->r;
    size_t   first = b->size - r;
    if (first > len) {
        first = len;
    }
    memcpy(dst, b->buff + r, first);
    if (len > first) {
        memcpy(dst + first, b->buff, len - first);
    }
    b->r = advance(r, len, b->size);
    return len;
}

size_t lwrb_peek(const lwrb_t *b, size_t skip, void *data, size_t len)
{
    if (b == NULL || b->buff == NULL || data == NULL || len == 0u) {
        return 0;
    }
    size_t full = lwrb_get_full(b);
    if (skip >= full) {
        return 0;
    }
    size_t avail = full - skip;
    if (len > avail) {
        len = avail;
    }
    uint8_t *dst   = (uint8_t *)data;
    size_t   r     = advance(b->r, skip, b->size);
    size_t   first = b->size - r;
    if (first > len) {
        first = len;
    }
    memcpy(dst, b->buff + r, first);
    if (len > first) {
        memcpy(dst + first, b->buff, len - first);
    }
    return len;
}

size_t lwrb_skip(lwrb_t *b, size_t len)
{
    if (b == NULL || b->buff == NULL || len == 0u) {
        return 0;
    }
    size_t full = lwrb_get_full(b);
    if (len > full) {
        len = full;
    }
    b->r = advance(b->r, len, b->size);
    return len;
}

size_t lwrb_advance(lwrb_t *b, size_t len)
{
    if (b == NULL || b->buff == NULL || len == 0u) {
        return 0;
    }
    size_t free_bytes = lwrb_get_free(b);
    if (len > free_bytes) {
        len = free_bytes;
    }
    b->w = advance(b->w, len, b->size);
    return len;
}
