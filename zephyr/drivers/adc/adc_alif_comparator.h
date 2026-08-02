/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NULL-safe comparator user_data extraction for the vendored Alif ADC driver
 * (see adc_alif.c for the full ADR 0017 provenance banner and the #1120
 * divergence note). Split into its own tiny, dependency-free header
 * (<zephyr/drivers/adc.h> only) so tests/unit/adc_alif_comparator can
 * exercise the exact guard adc_start_read() runs, without pulling in
 * DEVICE_MMIO or the analog_ctrl HAL helpers.
 */
#ifndef ZEPHYR_DRIVERS_ADC_ADC_ALIF_COMPARATOR_H_
#define ZEPHYR_DRIVERS_ADC_ADC_ALIF_COMPARATOR_H_

#include <zephyr/drivers/adc.h>

/**
 * @brief Extract the caller's comparator status pointer from an adc_sequence.
 *
 * `sequence->options` is optional per the Zephyr adc_sequence contract -- a
 * NULL options is the normal one-shot case, not a caller error. Only
 * dereference it when the caller actually supplied one (#1120).
 *
 * @param sequence ADC sequence describing the requested read. MUST NOT be
 *                  NULL.
 *
 * @return sequence->options->user_data if options were supplied, NULL
 *         otherwise.
 */
static inline void *adc_alif_sequence_comparator(const struct adc_sequence *sequence)
{
	return sequence->options ? sequence->options->user_data : NULL;
}

#endif /* ZEPHYR_DRIVERS_ADC_ADC_ALIF_COMPARATOR_H_ */
