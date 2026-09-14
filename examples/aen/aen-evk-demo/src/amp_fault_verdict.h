/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2097: examples/aen/aen-evk-demo's Phase 11 (TAS2563 amps) raw
 * AMP_FAULT (IRQ_N, P5_0) pin mapping, pulled out of main.c for the same
 * reason as ioexp_verdict.h -- one ternary, but it was inverted for a
 * whole release and a test is cheaper than a second silent flip.
 *
 * `raw` is exactly what gpio_pin_get() returns on AMP_FAULT_PIN: that
 * pin is configured plain GPIO_INPUT, no GPIO_ACTIVE_LOW, so the return
 * value is the raw electrical level, not IRQ_N's logical sense. IRQ_N
 * is open-drain, active-low (SLASET3D Sec7.3.12), and R124 (10k to
 * +VIO, fitted) pulls the net high when neither amp is pulling it down
 * -- so raw 1 is idle and raw 0 is the fault. gpio_pin_get() itself can
 * also return a negative errno on a real read failure; that is neither
 * 0 nor 1 and must not be read as either verdict.
 */
#ifndef ALP_EVK_DEMO_AMP_FAULT_VERDICT_H
#define ALP_EVK_DEMO_AMP_FAULT_VERDICT_H

#include <stdbool.h>

/* Returns true iff `raw` is a valid gpio_pin_get() level (0 or 1);
 * `*asserted_out` is only meaningful when this returns true. Mirrors
 * chips/tas2563/tas2563.c's tas2563_fault_asserted() `!level` sense --
 * this pin just is not reachable through that call on this board (see
 * changelog.d/2097-evk-demo-fault-pin-polarity.md for why). */
static inline bool amp_fault_pin_verdict(int raw, bool *asserted_out)
{
	if (raw != 0 && raw != 1) return false;
	*asserted_out = (raw == 0);
	return true;
}

#endif /* ALP_EVK_DEMO_AMP_FAULT_VERDICT_H */
