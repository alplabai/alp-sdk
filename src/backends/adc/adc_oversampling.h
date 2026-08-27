/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dependency-free guard for a non-power-of-two `alp_adc_config_t.
 * oversampling_ratio` that would otherwise be silently rounded.
 *
 * Originally written for alif_e7.c / alif_e8.c's conversion of the
 * portable RATIO to Zephyr's `adc_sequence.oversampling` log2 EXPONENT
 * (`__builtin_ctz()`, which rounds a non-power-of-two ratio DOWN --
 * 3x and 6x both land on 1x / 2x -- rather than failing).  Those two
 * backends now refuse ANY oversampling_ratio > 1 at open() instead,
 * because their vendored Alif driver rejects every non-zero
 * adc_sequence.oversampling outright, power-of-two or not
 * (zephyr/drivers/adc/adc_alif.c:779) -- so this guard no longer runs
 * there.  It is used by src/backends/adc/gd32_bridge.c instead: the GD32
 * firmware floors a non-power-of-two ratio to the nearest power of two
 * rather than refusing it (docs/gd32-bridge-protocol.md:540), and the
 * portable layer must refuse it here to keep <alp/adc.h>'s documented
 * promise that a non-power-of-two ratio is refused at alp_adc_open.
 *
 * Header-only + no Zephyr includes (mirrors
 * zephyr/drivers/adc/adc_alif_comparator.h's shape) so
 * tests/unit/adc_oversampling can exercise it hermetically on
 * native_sim with no devicetree / MMIO involved.
 */

#ifndef ALP_ADC_OVERSAMPLING_H
#define ALP_ADC_OVERSAMPLING_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief True when @p ratio is a ratio <alp/adc.h> documents as valid.
 *
 * 0 and 1 both mean "no oversampling" and are always accepted; any other
 * value must be an exact power of two no greater than 256 -- the ceiling
 * <alp/adc.h>'s oversampling_ratio field documents (1/2/4/.../256).
 * Without the upper bound, 512/1024/... pass as "powers of two" despite
 * sitting above every ratio the SDK documents as supported.
 */
static inline bool alp_adc_oversampling_ratio_ok(uint16_t ratio)
{
	return (ratio <= 1u) || (ratio <= 256u && (ratio & (ratio - 1u)) == 0u);
}

#endif /* ALP_ADC_OVERSAMPLING_H */
