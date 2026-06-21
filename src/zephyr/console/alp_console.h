/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal shared helpers for the Alp SoM console command groups.
 */
#ifndef ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_H_
#define ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_H_

/**
 * @brief Parse a decimal or 0x-hex unsigned integer from a shell arg.
 * @return 0 on success, -EINVAL on a malformed / out-of-range token.
 */
int alp_console_parse_ulong(const char *s, unsigned long *out);

#endif /* ALP_INTERNAL_ZEPHYR_CONSOLE_ALP_CONSOLE_H_ */
