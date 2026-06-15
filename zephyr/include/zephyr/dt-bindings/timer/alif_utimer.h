/*
 * Copyright (c) 2024 Alif Semiconductor
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Devicetree binding constants for the Alif Ensemble UTIMER `counter-direction`
 * cell (compatible "alif,utimer").  Referenced by the fork dts
 * (dts/arm/alif/ensemble/common/e1.dtsi `#include
 * <zephyr/dt-bindings/timer/alif_utimer.h>`) and the alif,utimer.yaml binding's
 * `counter-direction` description, both of which name these three symbols.
 *
 * The header is NOT shipped in the local zephyr_alif reference tree (which
 * carries only a subset of driver .c files), so it is re-authored here from the
 * binding's own documented enumeration (UP / DOWN / TRIANGLE) and the UTIMER
 * CNTR_CTRL semantics in hal_alif drivers/utimer (CNTR_CTRL_DIR_DOWN is bit 8;
 * sawtooth/up is the reset default, triangle is the buffered up/down mode).
 * These are devicetree selector ordinals consumed by the alp-sdk PWM driver
 * (pwm_alif_utimer.c), not raw register values.  vendor-ext, BENCH-UNVERIFIED.
 */
#ifndef ALP_DT_BINDINGS_TIMER_ALIF_UTIMER_H_
#define ALP_DT_BINDINGS_TIMER_ALIF_UTIMER_H_

/* Up counter (edge-aligned / sawtooth PWM).  Counter increments from 0 to the
 * reload value, then wraps -- the canonical single-ramp PWM carrier. */
#define ALIF_UTIMER_COUNTER_DIRECTION_UP       0

/* Down counter.  Counter decrements from the reload value to 0. */
#define ALIF_UTIMER_COUNTER_DIRECTION_DOWN     1

/* Triangle counter (center-aligned PWM).  Counter ramps up then down. */
#define ALIF_UTIMER_COUNTER_DIRECTION_TRIANGLE 2

#endif /* ALP_DT_BINDINGS_TIMER_ALIF_UTIMER_H_ */
