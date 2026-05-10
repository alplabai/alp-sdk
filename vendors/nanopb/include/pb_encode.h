/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stub of nanopb's pb_encode.h.  See vendors/nanopb/pb.h for the
 * rationale -- this is a minimum-surface compile-anchor.
 */

#ifndef ALP_STUB_PB_ENCODE_H_
#define ALP_STUB_PB_ENCODE_H_

#include <stdbool.h>
#include <stddef.h>

#include "pb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Streaming output sink.  Real impl has function pointers + state. */
typedef struct pb_ostream_s pb_ostream_t;

/** Build an output stream against a flat buffer of @p bufsize bytes. */
pb_ostream_t pb_ostream_from_buffer(pb_byte_t *buf, size_t bufsize);

/** Encode @p src_struct (described by @p fields) into @p stream. */
bool pb_encode(pb_ostream_t *stream, const pb_msgdesc_t *fields, const void *src_struct);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_STUB_PB_ENCODE_H_ */
