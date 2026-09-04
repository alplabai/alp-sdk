/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TEST-ONLY STUB. native_sim has no SoC-provided <pinctrl_soc.h> (pinctrl
 * is meaningless for the host build), but <zephyr/drivers/pinctrl.h>
 * unconditionally #includes it. display_cdc200.c only stores a
 * `struct pinctrl_dev_config *pcfg` (an opaque pointer) in its config
 * struct -- it never calls a pinctrl function that would need this type's
 * real Alif definition -- so a minimal typedef is enough to let the real
 * driver source compile under native_sim. Not used by, and must never be
 * added to, any non-test build.
 */
#ifndef ALP_TEST_STUB_PINCTRL_SOC_H_
#define ALP_TEST_STUB_PINCTRL_SOC_H_

#include <stdint.h>

typedef uint32_t pinctrl_soc_pin_t;

#endif /* ALP_TEST_STUB_PINCTRL_SOC_H_ */
