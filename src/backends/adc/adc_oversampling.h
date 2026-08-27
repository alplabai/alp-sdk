/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dependency-free guard shared by the Alif E7 / E8 ADC backends
 * (src/backends/adc/alif_e7.c, alif_e8.c).  Zephyr's `adc_sequence.
 * oversampling` field is a log2 EXPONENT; alp-sdk's portable
 * `alp_adc_config_t.oversampling_ratio` is a RATIO.  A ratio that
 * is not itself a power of two has no exact exponent -- converting
 * it with `__builtin_ctz()` silently rounds DOWN (3x and 6x both
 * land on ctz() == 0 / 1, i.e. 1x / 2x) rather than failing, so
 * every open() caller must reject a non-power-of-two ratio up front
 * with this check before it ever reaches __builtin_ctz().
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
 * @brief True when @p ratio has an exact log2 exponent Zephyr's
 *        adc_sequence.oversampling can represent.
 *
 * 0 and 1 both mean "no oversampling" (exponent 0) and are always
 * accepted; any other value must be an exact power of two.
 */
static inline bool alp_adc_oversampling_ratio_ok(uint16_t ratio)
{
	return (ratio <= 1u) || ((ratio & (ratio - 1u)) == 0u);
}

#endif /* ALP_ADC_OVERSAMPLING_H */
