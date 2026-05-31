/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stub of nanopb's pb_decode.h.
 */

#ifndef ALP_STUB_PB_DECODE_H_
#define ALP_STUB_PB_DECODE_H_

#include <stdbool.h>
#include <stddef.h>

#include "pb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Streaming input source. */
typedef struct pb_istream_s pb_istream_t;

/** Build an input stream against a flat buffer of @p bufsize bytes. */
pb_istream_t pb_istream_from_buffer(const pb_byte_t *buf, size_t bufsize);

/** Decode the next message into @p dest_struct (described by @p fields). */
bool pb_decode(pb_istream_t *stream, const pb_msgdesc_t *fields, void *dest_struct);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_STUB_PB_DECODE_H_ */
