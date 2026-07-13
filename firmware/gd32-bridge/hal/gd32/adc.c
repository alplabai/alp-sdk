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

/* Per-channel sticky conversion-format overrides applied by
 * bridge_hw_adc_configure.  All three are pure caches: configure
 * validates + stores, and the read / stream-begin paths program them
 * into the converter (inside the ADCON==0 window DRES/OVSAMPCTL
 * require).  Defaults: 240 sample cycles, 12-bit resolution, no
 * oversampling.  Zero-initialised at boot, so bridge_hw_init's ADC
 * bring-up loop (init.c) seeds the non-zero defaults. */
uint16_t adc_sample_cycles_cache[8];
uint8_t  adc_resolution_bits_cache[8];
uint16_t adc_oversample_ratio_cache[8];

/* Resolution bits (12/10/8/6) -> ADC_RESOLUTION_* register value.
 * Returns false for any other width so bridge_hw_adc_configure can
 * reject it as NOSUPPORT rather than silently clamping. */
static bool adc_resolution_reg(uint8_t bits, uint32_t *reg_out)
{
	switch (bits) {
	case 12u:
		*reg_out = ADC_RESOLUTION_12B;
		return true;
	case 10u:
		*reg_out = ADC_RESOLUTION_10B;
		return true;
	case 8u:
		*reg_out = ADC_RESOLUTION_8B;
		return true;
	case 6u:
		*reg_out = ADC_RESOLUTION_6B;
		return true;
	default:
		return false;
	}
}

/* Right-aligned code range for a resolution: (1<<bits)-1.  Used by
 * both read paths to scale a code to millivolts.  Unknown widths fall
 * back to the 12-bit range (they never reach here -- configure gates
 * them -- but the mv math must not divide by a bogus value). */
uint16_t adc_full_scale_for_bits(uint8_t bits)
{
	switch (bits) {
	case 10u:
		return 1023u;
	case 8u:
		return 255u;
	case 6u:
		return 63u;
	default:
		return ADC_FULL_SCALE; /* 12-bit */
	}
}

/* Oversample ratio -> (OVSR register value, OVSS shift enum).  The
 * wire contract (docs/gd32-bridge-protocol.md §3.9) rounds a caller's
 * ratio DOWN to the nearest power of two in 1..256: 0 or 1 means "no
 * oversampling", and any value >1 is floored to 2^n.  The GD32 OVSR
 * field is the conversion count minus one, and a matching right-shift
 * of log2(ratio) normalises the accumulator back to the selected
 * resolution's full-scale (sum of `ratio` codes each <= full_scale,
 * shifted right by log2(ratio), is again <= full_scale) -- so the mv
 * math divides by the resolution's full-scale regardless of ratio.
 * enable_out == false tells the caller to disable the mode rather than
 * program a 1x accumulator. */
static void
adc_oversample_params(uint16_t ratio, bool *enable_out, uint16_t *ovsr_out, uint32_t *shift_out)
{
	if (ratio <= 1u) {
		*enable_out = false;
		return;
	}
	if (ratio > ADC_OVERSAMPLE_RATIO_MAX) ratio = ADC_OVERSAMPLE_RATIO_MAX;
	/* Largest power of two <= ratio, with its exponent. */
	uint32_t log2 = 0u;
	uint16_t pow2 = 1u;
	while ((uint16_t)(pow2 << 1) <= ratio) {
		pow2 <<= 1;
		++log2;
	}
	*enable_out = true;
	*ovsr_out   = (uint16_t)(pow2 - 1u);
	*shift_out  = OVSCR_OVSS(log2); /* ADC_OVERSAMPLING_SHIFT_<log2>B */
}

/* Program a channel's cached resolution + oversample into its ADC.
 * The caller MUST have the converter disabled (DRES lives in CTL0 and
 * OVSAMPCTL only latches with ADCON==0 -- the vendor's own
 * ADC3_resolution_oversample example brackets every change with
 * adc_disable/adc_enable).  Shared by the single-shot read (which
 * wraps it in a disable/enable/tSTAB) and stream_begin (already inside
 * its own disable..enable window). */
void adc_apply_conv_format(uint32_t periph, uint8_t channel)
{
	uint32_t res_reg;
	if (adc_resolution_reg(adc_resolution_bits_cache[channel], &res_reg)) {
		adc_resolution_config(periph, res_reg);
	}

	bool     ovs_en;
	uint16_t ovsr;
	uint32_t shift;
	adc_oversample_params(adc_oversample_ratio_cache[channel], &ovs_en, &ovsr, &shift);
	if (ovs_en) {
		/* ALL_CONVERT: one trigger runs all `ratio` conversions and
		 * raises a single EOC with the shifted average -- the read
		 * loop's per-sample software trigger is unchanged. */
		adc_oversample_mode_config(periph, ADC_OVERSAMPLING_ALL_CONVERT, shift, ovsr);
		adc_oversample_mode_enable(periph);
	} else {
		adc_oversample_mode_disable(periph);
	}
}

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
     * survives across reads.
     *
     * Resolution + oversample (also cached) must be programmed with
     * the converter DISABLED -- DRES/OVSAMPCTL only latch while
     * ADCON==0 -- so bracket the format apply in a disable/enable with
     * the same tSTAB dwell adc_periph_init uses.  Two bridge channels
     * share each converter and may hold different formats, so this
     * re-applies every read; the sibling-stream case already returned
     * BUSY above, so no live stream owns the converter here.  The
     * ADCON toggle preserves the boot calibration. */
	adc_disable(ch->periph);
	adc_apply_conv_format(ch->periph, channel);
	adc_routine_channel_config(ch->periph, 0u, ch->channel, adc_sample_cycles_cache[channel]);
	adc_enable(ch->periph);
	for (volatile uint32_t stab = 0u; stab < 4096u; ++stab) {
		/* tSTAB dwell after ADCON */
	}

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
     * every surface failed).  The ~100k-iteration bound is the abort
     * latch; on timeout the peripheral is re-initialised (deinit +
     * reconfig + recalibrate) so the NEXT read starts from a clean
     * converter -- same self-healing shape as the TRNG fault path.
     *
     * SCALE the bound by the oversample ratio: with oversampling ON,
     * ONE triggered "conversion" is `ratio` back-to-back samples (up to
     * 256), so a healthy oversampled conversion legitimately takes up to
     * ~ratio x longer than the ~6.3 us un-oversampled case.  A fixed
     * 100k bound would false-timeout every legal high-ratio read and
     * needlessly recalibrate.  The floored-to-pow2 ratio is what the
     * hardware actually runs; clamp the raw cache to [1,256] to match. */
	uint32_t ovs_ratio = adc_oversample_ratio_cache[channel];
	if (ovs_ratio < 1u) ovs_ratio = 1u;
	if (ovs_ratio > ADC_OVERSAMPLE_RATIO_MAX) ovs_ratio = ADC_OVERSAMPLE_RATIO_MAX;
	const uint32_t eoc_bound = 100000u * ovs_ratio;
	for (uint8_t i = 0; i < samples; ++i) {
		adc_software_trigger_enable(ch->periph, ADC_ROUTINE_CHANNEL);
		uint32_t to = eoc_bound;
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
		/* Scale by the channel's cached resolution, not a fixed 4095:
		 * a 10/8/6-bit conversion (or an oversampled result, which the
		 * shift normalises back to the same range) tops out below 4095. */
		const uint16_t fs = adc_full_scale_for_bits(adc_resolution_bits_cache[channel]);
		if (code > fs) code = fs;
		mv[i] = (uint16_t)((code * ADC_VREF_MV) / fs);
	}
	return BRIDGE_HW_OK;
}

int bridge_hw_adc_configure(uint8_t  channel,
                            uint16_t oversample_ratio,
                            uint16_t sample_cycles,
                            uint8_t  resolution_bits)
{
	if (channel >= ADC_CHANNEL_MAP_COUNT) return BRIDGE_HW_ERR_RANGE;

	/* Validate resolution BEFORE mutating any cache so a rejected field
	 * leaves the channel's format untouched.  All three fields are
	 * pure-cache here (like sample_cycles): the read / stream_begin
	 * paths program them into the converter inside their ADCON==0
	 * windows.  Deferring the register write also keeps configure from
	 * disturbing a converter that a sibling channel may be streaming
	 * on -- the new format takes effect on this channel's next read or
	 * stream_begin.
	 *
	 * Field semantics follow the wire contract (docs/gd32-bridge-
	 * protocol.md §3.9):
	 *   resolution_bits: 0 -> default (12).  6/8/10/12 map to the
	 *     hardware DRES field.  14/16 are effective-resolution modes
	 *     that the GD32 can only reach by under-shifting an oversampled
	 *     accumulator (DRES tops out at 12 bit); that extension is not
	 *     implemented yet -> NOSUPPORT.  Any other width is invalid ->
	 *     INVAL.
	 *   oversample_ratio: 0/1 -> off; anything larger is floored to the
	 *     nearest power of two in 2..256 (adc_oversample_params, never
	 *     rejects -- matches the doc's "rounds down" wording).
	 * The stored resolution is normalised to 12 when 0 so the read
	 * path's full-scale lookup and register apply see a concrete
	 * width. */
	uint8_t  res_bits = (resolution_bits == 0u) ? ADC_RES_BITS_DEFAULT : resolution_bits;
	uint32_t res_reg;
	if (!adc_resolution_reg(res_bits, &res_reg)) {
		if (res_bits == 14u || res_bits == 16u) return BRIDGE_HW_ERR_NOTIMPL;
		return BRIDGE_HW_ERR_INVAL;
	}

	/* Sample cycles: 0 means "firmware default" (per the wire contract),
     * NOT the fastest window -- collapsing 240 -> 2 cycles on the
     * high-impedance divider inputs the default exists to serve would
     * leave the S/H cap unsettled and read systematically low (worse
     * still under oversampling).  Any non-zero value is a direct cycle
     * count clamped into the vendor's accepted 2..638 range (per
     * `adc_routine_channel_config`'s sample_time parameter). */
	uint16_t sc = (sample_cycles == 0u) ? ADC_DEFAULT_SAMPLE_CYCLES : sample_cycles;
	if (sc < 2u) sc = 2u;
	if (sc > 638u) sc = 638u;

	adc_sample_cycles_cache[channel]    = sc;
	adc_resolution_bits_cache[channel]  = res_bits;         /* 0 normalised to 12 above */
	adc_oversample_ratio_cache[channel] = oversample_ratio; /* apply-time floors to pow2 */
	return BRIDGE_HW_OK;
}
