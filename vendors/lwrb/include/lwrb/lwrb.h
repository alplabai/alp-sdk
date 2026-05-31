/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stub of the LwRB (Lightweight Ring Buffer) header.  Mirrors the
 * upstream public ABI as of v3.2.0 (<https://github.com/MaJerle/lwrb>)
 * so SDK source compiles against CONFIG_ALP_SDK_USE_LWRB=y on hosts
 * that haven't fetched the upstream module via west.
 *
 * When the real LwRB is on the include path (typically pulled in via
 * west.yml under modules/lib/lwrb), that header is picked up instead.
 *
 * The stub functions return error / zero so accidental linkage
 * against the stub instead of the real library surfaces quickly --
 * the SDK's audio + UART backends only call lwrb_* through their
 * own internal helpers, never expose the type via <alp/...> headers.
 */

#ifndef ALP_STUB_LWRB_LWRB_H_
#define ALP_STUB_LWRB_LWRB_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public ring-buffer struct.  Real implementation has volatile
 * indices + a couple of bookkeeping fields; for stub purposes only
 * the size + base pointer matter. */
typedef struct {
    uint8_t        *buff;
    size_t          size;
    volatile size_t r;
    volatile size_t w;
} lwrb_t;

/** Return 1 if @p b is initialised and ready to use, 0 otherwise. */
static inline uint8_t lwrb_is_ready(const lwrb_t *b)
{
    return (b != NULL && b->buff != NULL && b->size > 0) ? 1u : 0u;
}

/** Bind a backing store + capacity to @p b.  Returns 1 on success,
 *  0 on argument error.  Capacity is the *backing-store* size; the
 *  usable capacity is one byte less (the canonical empty/full
 *  disambiguation trick). */
static inline uint8_t lwrb_init(lwrb_t *b, void *buf, size_t size)
{
    if (b == NULL || buf == NULL || size < 2u) return 0u;
    b->buff = (uint8_t *)buf;
    b->size = size;
    b->r    = 0;
    b->w    = 0;
    return 1u;
}

/** Drop the binding; subsequent calls return 0. */
static inline void lwrb_free(lwrb_t *b)
{
    if (b == NULL) return;
    b->buff = NULL;
    b->size = 0;
}

/** Write up to @p len bytes; returns the number actually written. */
size_t lwrb_write(lwrb_t *b, const void *data, size_t len);

/** Read up to @p len bytes; returns the number actually read. */
size_t lwrb_read(lwrb_t *b, void *data, size_t len);

/** Inspect bytes without removing them. */
size_t lwrb_peek(const lwrb_t *b, size_t skip, void *data, size_t len);

/** Number of bytes currently stored. */
size_t lwrb_get_full(const lwrb_t *b);

/** Free bytes available for write. */
size_t lwrb_get_free(const lwrb_t *b);

/** Advance the read cursor by @p len bytes; returns bytes skipped. */
size_t lwrb_skip(lwrb_t *b, size_t len);

/** Advance the write cursor by @p len bytes (after a direct-write to
 *  the linear region returned by lwrb_get_linear_block_write_*).
 *  Returns bytes advanced. */
size_t lwrb_advance(lwrb_t *b, size_t len);

/** Drop all buffered data; preserves the binding. */
static inline void lwrb_reset(lwrb_t *b)
{
    if (b == NULL) return;
    b->r = 0;
    b->w = 0;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_STUB_LWRB_LWRB_H_ */
