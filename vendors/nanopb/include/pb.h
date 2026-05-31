/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stub of the nanopb common header.  Mirrors the upstream public
 * ABI as of v0.4.9 (<https://github.com/nanopb/nanopb>) so SDK
 * source compiles against CONFIG_ALP_SDK_USE_NANOPB=y on hosts
 * that haven't fetched the upstream module via west.
 *
 * When the real nanopb is on the include path (typically pulled in
 * via west.yml under modules/lib/nanopb), that header wins.
 */

#ifndef ALP_STUB_PB_H_
#define ALP_STUB_PB_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol version the stub claims to implement -- matches the
 * upstream 0.4.x header so encoder/decoder symbol-versioning lines
 * up. */
#define PB_PROTO_HEADER_VERSION 40

typedef uint8_t  pb_byte_t;
typedef uint32_t pb_size_t;
typedef int32_t  pb_ssize_t;
typedef uint8_t  pb_type_t;

/* Forward decls of opaque types the encode / decode entries reference. */
typedef struct pb_field_iter_s pb_field_iter_t;
typedef struct pb_msgdesc_s    pb_msgdesc_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_STUB_PB_H_ */
