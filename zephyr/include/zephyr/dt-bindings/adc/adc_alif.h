/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * DT-binding constants for the Alif Ensemble ADC `interrupt_en` property
 * (compatible "alif,adc"; driver zephyr/drivers/adc/adc_alif.c).
 *
 * PROVENANCE: the Apache-2.0 zephyr_alif fork's e1.dtsi references
 * ADC_DONE0_INTERRUPT / ADC_DONE1_INTERRUPT for `interrupt_en`, but the fork's
 * <zephyr/dt-bindings/adc/adc_alif.h> was NOT present in the fetched source
 * snapshot.  These values are therefore re-derived DIRECTLY from the driver's
 * own interrupt bit layout -- they are NOT invented:
 *
 *   adc_alif.c interrupt-clear macros (the ADC_INTERRUPT register bitfield):
 *     ADC_INTR_DONE0_CLEAR (0x01)  -> DONE0 = bit0
 *     ADC_INTR_DONE1_CLEAR (0x02)  -> DONE1 = bit1
 *     ADC_INTR_COMPA_CLEAR (0x04)  -> COMP_A = bit2  (also ADC_INTR_CMPA_POS=2)
 *     ADC_INTR_COMPB_CLEAR (0x08)  -> COMP_B = bit3  (also ADC_INTR_CMPB_POS=3)
 *
 *   The driver consumes `interrupt_en` as a bitmask: adc_unmask_interrupt()
 *   writes ((~interrupt_en) & 0xF) to ADC_INTERRUPT_MASK, so each set bit here
 *   un-masks the corresponding ADC interrupt.  A single-shot read needs DONE1
 *   (single-shot completion is signalled on DONE1 -- adc_done1_irq_handler);
 *   the fork's reference nodes set DONE0|DONE1, mirrored here.
 *
 * ADR 0017 Tier-2: ships in-tree alongside the vendored fork driver (the fork
 * binding header is missing upstream).  Retires onto the fork header once the
 * fork publishes it and adc nodes are repointed (task #21).
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_ADC_ADC_ALIF_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_ADC_ADC_ALIF_H_

#define ADC_DONE0_INTERRUPT  (1 << 0)
#define ADC_DONE1_INTERRUPT  (1 << 1)
#define ADC_COMPA_INTERRUPT  (1 << 2)
#define ADC_COMPB_INTERRUPT  (1 << 3)

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_ADC_ADC_ALIF_H_ */
