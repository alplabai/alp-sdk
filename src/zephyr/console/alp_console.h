/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal shared helpers for the Alp SoM console command groups.
 */
#ifndef ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_H_
#define ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Parse a decimal or 0x-hex unsigned integer from a shell arg.
 * @return 0 on success, -EINVAL on a malformed / out-of-range token.
 */
int alp_console_parse_ulong(const char *s, unsigned long *out);

/**
 * @brief Parse a contiguous hex string ("0011aabb") into bytes.
 *
 * Two nibbles per byte, no separators.  Rejects an empty string, an odd
 * length, a non-hex character, or more than @p cap bytes.
 *
 * @param s        NUL-terminated hex token from the shell.
 * @param out      Destination buffer.
 * @param cap      Capacity of @p out in bytes.
 * @param out_len  Receives the decoded byte count.
 * @return 0 on success, -EINVAL on a malformed / oversized token.
 */
int alp_console_parse_hex(const char *s, uint8_t *out, size_t cap, size_t *out_len);

#endif /* ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_H_ */
