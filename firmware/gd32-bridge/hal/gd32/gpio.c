/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- GPIO pads.
 * Split move-only from hal/bridge_hw_gd32.c (fw v0.2.8); see
 * hal/gd32/init.c for the backend-wide implementation notes.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_hw.h"
#include "gd32g5x3.h"

#include "gd32_common.h"

/* ----------------------------------------------------------------- */
/* GPIO pad map -- E1M IO logical-index to GD32 (port, pin) lookup.   */
/* Sourced from `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`        */
/* (the "E1M IO*" rows).  Wire-side `mask` bit i selects entry i in   */
/* this table; numbering is compact (0..17) rather than matching the  */
/* physical E1M IO numbering, which has gaps at 15 / 17..23 / 26 / 33 */
/* because those positions are assigned to other peripherals on the   */
/* board.  Host-side translation table lives in                     */
/* `chips/gd32g553/gd32g553.c`.                                       */
/* ----------------------------------------------------------------- */

const gd32_gpio_pad_t gpio_pad_map[] = {
	{ GPIOB, GPIO_PIN_10 }, /* bit  0 = E1M IO8  */
	{ GPIOA, GPIO_PIN_7 },  /* bit  1 = E1M IO9  */
	{ GPIOA, GPIO_PIN_12 }, /* bit  2 = E1M IO10 */
	{ GPIOB, GPIO_PIN_0 },  /* bit  3 = E1M IO11 */
	{ GPIOC, GPIO_PIN_1 },  /* bit  4 = E1M IO12 */
	{ GPIOF, GPIO_PIN_1 },  /* bit  5 = E1M IO13 */
	{ GPIOB, GPIO_PIN_5 },  /* bit  6 = E1M IO14 */
	{ GPIOC, GPIO_PIN_0 },  /* bit  7 = E1M IO16 */
	{ GPIOC, GPIO_PIN_14 }, /* bit  8 = E1M IO24 */
	{ GPIOC, GPIO_PIN_15 }, /* bit  9 = E1M IO25 */
	{ GPIOB, GPIO_PIN_11 }, /* bit 10 = E1M IO27 */
	{ GPIOC, GPIO_PIN_2 },  /* bit 11 = E1M IO28 */
	{ GPIOD, GPIO_PIN_11 }, /* bit 12 = E1M IO29 */
	{ GPIOD, GPIO_PIN_10 }, /* bit 13 = E1M IO30 */
	{ GPIOE, GPIO_PIN_12 }, /* bit 14 = E1M IO31 */
	{ GPIOD, GPIO_PIN_2 },  /* bit 15 = E1M IO32 */
	{ GPIOD, GPIO_PIN_8 },  /* bit 16 = E1M IO34 */
	{ GPIOD, GPIO_PIN_1 },  /* bit 17 = E1M IO35 */
};
_Static_assert(sizeof(gpio_pad_map) / sizeof(gpio_pad_map[0]) == GPIO_PAD_MAP_COUNT,
               "gpio_pad_map size must match GPIO_PAD_MAP_COUNT");

/* Per-pad direction tracking.  Boot configures every pad as INPUT +
 * PULL_UP; bridge_hw_gpio_write() flips an entry to OUTPUT push-pull
 * on first call (sticky until the next chip reset).  Avoids the
 * need for a separate `CMD_GPIO_CONFIGURE` opcode -- read-only
 * callers see the external level until they touch the pad with a
 * write, after which subsequent reads return the driven level. */
bool gpio_is_output[GPIO_PAD_MAP_COUNT];

int  bridge_hw_gpio_read(uint32_t mask, uint32_t *levels)
{
	if (levels == 0) return BRIDGE_HW_ERR_INVAL;
	*levels = 0u;
	/* Bits above `GPIO_PAD_MAP_COUNT` are silently ignored -- the
     * host header documents the mapping as opaque, so out-of-range
     * bits are treated as "no pad selected" rather than an error. */
	for (size_t i = 0; i < GPIO_PAD_MAP_COUNT; ++i) {
		if ((mask & ((uint32_t)1u << i)) == 0u) continue;
		const FlagStatus s = gpio_is_output[i]
		                         ? gpio_output_bit_get(gpio_pad_map[i].periph, gpio_pad_map[i].pin)
		                         : gpio_input_bit_get(gpio_pad_map[i].periph, gpio_pad_map[i].pin);
		if (s == SET) {
			*levels |= ((uint32_t)1u << i);
		}
	}
	return BRIDGE_HW_OK;
}

int bridge_hw_gpio_write(uint32_t mask, uint32_t levels)
{
	/* Out-of-range bits silently ignored, same policy as
     * bridge_hw_gpio_read(). */
	for (size_t i = 0; i < GPIO_PAD_MAP_COUNT; ++i) {
		if ((mask & ((uint32_t)1u << i)) == 0u) continue;
		if (!gpio_is_output[i]) {
			/* First write to this pad since boot: promote
             * INPUT+PULL_UP to OUTPUT push-pull.  12 MHz is the
             * GD32G5's slowest output speed (datasheet §7.4.1);
             * adequate for control lines, low EMI.  The bridge
             * dispatcher is single-threaded so no locking is
             * needed around the mode flip + the flag write. */
			gpio_output_options_set(gpio_pad_map[i].periph, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ,
			                        gpio_pad_map[i].pin);
			gpio_mode_set(gpio_pad_map[i].periph, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
			              gpio_pad_map[i].pin);
			gpio_is_output[i] = true;
		}
		if (levels & ((uint32_t)1u << i)) {
			gpio_bit_set(gpio_pad_map[i].periph, gpio_pad_map[i].pin);
		} else {
			gpio_bit_reset(gpio_pad_map[i].periph, gpio_pad_map[i].pin);
		}
	}
	return BRIDGE_HW_OK;
}
