/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 bridge HAL backend -- single-shot ADC.
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
/* ADC channels.                                                      */
/* ----------------------------------------------------------------- */

/* E1M ADC0..7 -> (ADC peripheral, channel index, pad).  Sourced from
 * maintainer-confirmed `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`
 * with channel + peripheral assignments cross-checked against the
 * GD32G553xx datasheet pin alt-function summary:
 *
 *   E1M ADC0  PD9   ADC3  CH12
 *   E1M ADC1  PB12  ADC3  CH2
 *   E1M ADC2  PE13  ADC2  CH2
 *   E1M ADC3  PE11  ADC2  CH13
 *   E1M ADC4  PC4   ADC1  CH4
 *   E1M ADC5  PA5   ADC1  CH12
 *   E1M ADC6  PA2   ADC0  CH2
 *   E1M ADC7  PA3   ADC0  CH3
 *
 * All four ADC peripherals carry two of the eight channels; the
 * init loop brings up each ADC once and the read body reconfigures
 * the routine channel + triggers a single conversion on demand. */

const gd32_adc_ch_t adc_channels_map[] = {
	[0] = { ADC3, ADC_CHANNEL_12, GPIOD, GPIO_PIN_9 },
	[1] = { ADC3, ADC_CHANNEL_2, GPIOB, GPIO_PIN_12 },
	[2] = { ADC2, ADC_CHANNEL_2, GPIOE, GPIO_PIN_13 },
	[3] = { ADC2, ADC_CHANNEL_13, GPIOE, GPIO_PIN_11 },
	[4] = { ADC1, ADC_CHANNEL_4, GPIOC, GPIO_PIN_4 },
	[5] = { ADC1, ADC_CHANNEL_12, GPIOA, GPIO_PIN_5 },
	[6] = { ADC0, ADC_CHANNEL_2, GPIOA, GPIO_PIN_2 },
	[7] = { ADC0, ADC_CHANNEL_3, GPIOA, GPIO_PIN_3 },
};
_Static_assert(sizeof(adc_channels_map) / sizeof(adc_channels_map[0]) == ADC_CHANNEL_MAP_COUNT,
               "adc_channels_map size must match ADC_CHANNEL_MAP_COUNT");

/* Per-channel sticky sample-cycle override applied by
 * bridge_hw_adc_configure.  Defaults to ADC_DEFAULT_SAMPLE_CYCLES;
 * caller's tighter values (e.g. 24 cycles for low-impedance sources)
 * override on the next bridge_hw_adc_read.  Resolution + oversample
 * are still gated to defaults at v0.3 and live in a follow-up. */
uint16_t adc_sample_cycles_cache[8];

/* Bounded reimplementation of the vendor's adc_calibration_enable().
 * The SPL body spins `while (RSTCLB)` then `while (CLB)` with NO
 * timeout -- and the calibration FSM only advances on a healthy,
 * clocked converter.  adc_periph_init() is reachable from the CS-EXTI
 * request handler (bridge_hw_adc_read's timeout self-heal and
 * bridge_hw_adc_stream_end's restore), where an unbounded spin on a
 * wedged ADC takes the WHOLE LINK down -- the exact failure class the
 * read path's own EOC bound was added to stop (silicon 2026-06-04).
 * Without the irony: the self-heal for a wedged converter must not
 * itself trust that converter to terminate a loop.  Same register
 * sequence as the vendor, same bound family as the other handler-safe
 * waits in this file; returns false if either phase never completes. */
static bool adc_calibrate_bounded(uint32_t periph)
{
	uint32_t to;

	ADC_CTL1(periph) |= (uint32_t)ADC_CTL1_RSTCLB;
	for (to = 100000u; to != 0u; --to) {
		if (RESET == (ADC_CTL1(periph) & ADC_CTL1_RSTCLB)) break;
	}
	if (to == 0u) return false;

	ADC_CTL1(periph) |= (uint32_t)ADC_CTL1_CLB;
	for (to = 100000u; to != 0u; --to) {
		if (RESET == (ADC_CTL1(periph) & ADC_CTL1_CLB)) break;
	}
	return to != 0u;
}

bool adc_periph_init(uint32_t periph)
{
	adc_deinit(periph);
	adc_clock_config(periph, ADC_CLK_SYNC_HCLK_DIV6);
	adc_data_alignment_config(periph, ADC_DATAALIGN_RIGHT);
	adc_channel_length_config(periph, ADC_ROUTINE_CHANNEL, 1u);
	adc_external_trigger_config(periph, ADC_ROUTINE_CHANNEL, EXTERNAL_TRIGGER_DISABLE);
	adc_enable(periph);

	/* Stabilisation + calibration.  The datasheet wants tSTAB after
     * ADCON before calibrating; a generous spin costs microseconds.
     * The bounded calibration then waits on RSTCLB/CLB -- without it
     * the converter runs uncalibrated for the whole session (linearity
     * offsets land directly in every reported millivolt).  A false
     * return means the calibration FSM never finished: the converter
     * is configured but its accuracy is unknown -- callers on the
     * request path surface that as an IO error rather than serving
     * readings from a converter in an unproven state. */
	for (volatile uint32_t stab = 0u; stab < 4096u; ++stab) {
	}
	return adc_calibrate_bounded(periph);
}

int bridge_hw_adc_read(uint8_t channel, uint8_t samples, uint16_t *mv)
{
	if (mv == 0) return BRIDGE_HW_ERR_INVAL;
	if (samples == 0u) return BRIDGE_HW_ERR_INVAL;
	if (channel >= ADC_CHANNEL_MAP_COUNT) return BRIDGE_HW_ERR_RANGE;
	if (!vref_ready_check()) return BRIDGE_HW_ERR_IO; /* dead reference -- fail loud */

	const gd32_adc_ch_t *ch = &adc_channels_map[channel];

	/* Converter-sharing guard, the read-side mirror of stream_begin's
     * stream-vs-stream check: two bridge channels ride each ADC
     * peripheral (ch0/1 -> ADC3, 2/3 -> ADC2, 4/5 -> ADC1, 6/7 ->
     * ADC0), and a stream owns its converter outright between BEGIN
     * and END (external-trigger + circular DMA + DDM).  A single-shot
     * read on the sibling channel would re-point routine rank 0 out
     * from under the stream's DMA, software-trigger an unpaced
     * conversion next to the pacing timer's, and consume the EOC the
     * DDM path depends on -- corrupting the live ring AND returning
     * bogus data.  Refuse honestly; the host retries after STREAM_END. */
	for (uint8_t si = 0u; si < BRIDGE_ADC_STREAM_COUNT; ++si) {
		if (adc_streams[si].in_use &&
		    adc_channels_map[adc_streams[si].channel].periph == ch->periph) {
			return BRIDGE_HW_ERR_BUSY;
		}
	}

	/* Configure the routine channel for this op (each call re-applies
     * because multiple bridge channels can share an ADC peripheral -- a
     * prior bridge_hw_adc_read on a different bridge channel may have
     * pointed the routine slot elsewhere).  Sample-cycle count comes
     * from the per-channel cache so bridge_hw_adc_configure's choice
     * survives across reads. */
	adc_routine_channel_config(ch->periph, 0u, ch->channel, adc_sample_cycles_cache[channel]);

	/* A stale EOC (e.g. the in-flight conversion that completes after
     * a stream END drops continuous mode) would satisfy the first poll
     * instantly and serve OLD data as sample 1 -- clear it so every
     * EOC we consume belongs to a conversion WE triggered. */
	adc_flag_clear(ch->periph, ADC_FLAG_EOC);

	/* Take `samples` consecutive conversions, software-triggered per
     * sample.  Polled EOC with a HARD BOUND: this body runs inside the
     * CS-EXTI handler, and an unbounded spin on a wedged ADC took the
     * WHOLE LINK down with it (silicon 2026-06-04: after an adc_stream
     * cycle the next read's EOC never came; the handler never returned,
     * the SPI RX DMA was never re-armed -- captured live with CH3
     * frozen disabled at CNT=66 -- and every subsequent command on
     * every surface failed).  A healthy conversion is ~6.3 us; the
     * ~100k-iteration bound is the abort latch, and on timeout the
     * peripheral is re-initialised (deinit + reconfig + recalibrate)
     * so the NEXT read starts from a clean converter -- same
     * self-healing shape as the TRNG fault path. */
	for (uint8_t i = 0; i < samples; ++i) {
		adc_software_trigger_enable(ch->periph, ADC_ROUTINE_CHANNEL);
		uint32_t to = 100000u;
		while (!adc_flag_get(ch->periph, ADC_FLAG_EOC) && --to) {
			/* spin, bounded */
		}
		if (to == 0u) {
			/* Self-heal is best-effort: the re-init's calibration is
             * itself bounded (a wedged converter must not convert a
             * read timeout into a link wedge), and this path already
             * reports IO either way. */
			(void)adc_periph_init(ch->periph);
			return BRIDGE_HW_ERR_IO;
		}
		adc_flag_clear(ch->periph, ADC_FLAG_EOC);
		uint32_t code = adc_routine_data_read(ch->periph);
		if (code > ADC_FULL_SCALE) code = ADC_FULL_SCALE;
		mv[i] = (uint16_t)((code * ADC_VREF_MV) / ADC_FULL_SCALE);
	}
	return BRIDGE_HW_OK;
}

int bridge_hw_adc_configure(uint8_t  channel,
                            uint16_t oversample_ratio,
                            uint16_t sample_cycles,
                            uint8_t  resolution_bits)
{
	if (channel >= ADC_CHANNEL_MAP_COUNT) return BRIDGE_HW_ERR_RANGE;

	/* v0.3 partial: only sample_cycles is sticky.  The oversample +
     * resolution paths are still gated to their defaults (1, 12).
     * Non-default values return NOSUPPORT so the host gets a clear
     * "this commit doesn't support that yet" rather than silently
     * accepting and ignoring.  A follow-up commit will land the
     * resolution + oversample apply path. */
	if (oversample_ratio != 1u) return BRIDGE_HW_ERR_NOTIMPL;
	if (resolution_bits != 12u) return BRIDGE_HW_ERR_NOTIMPL;

	/* Sample cycles: clamp to the vendor's accepted 2..638 cycle
     * range (per `adc_routine_channel_config`'s sample_time
     * parameter).  Round caller-supplied values into this range
     * rather than rejecting -- matches the contract's "firmware
     * rounds down" wording. */
	uint16_t sc = sample_cycles;
	if (sc < 2u) sc = 2u;
	if (sc > 638u) sc = 638u;

	adc_sample_cycles_cache[channel] = sc;
	return BRIDGE_HW_OK;
}
