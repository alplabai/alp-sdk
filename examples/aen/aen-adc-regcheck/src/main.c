/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-adc-regcheck -- scopeless on-silicon validation of the Alif ADC driver
 * (zephyr/drivers/adc/adc_alif.c, compatible "alif,adc", adc12_0 @ 0x49020000)
 * on the E1M-AEN801 (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console
 * flow.
 *
 * We cannot see the analog pin on this bench (no scope, app UART not on USB).
 * So we validate by driving the standard Zephyr ADC API (adc_channel_setup +
 * adc_read, a single-shot conversion) and then doing a REGISTER READBACK of the
 * controller's key registers.
 *
 * Two independent confirmations, exactly like aen-spi-regcheck:
 *   1. This firmware drives the API, prints the raw sample + the registers it
 *      reads back, plus a single RESULT PASS/FAIL line, to the RAM console (read
 *      'ram_console_buf' over J-Link mem8, ASCII-decode).
 *   2. The human re-reads the SAME absolute addresses over J-Link mem32 (see the
 *      readback plan) -- so a driver that only PRINTS the right thing is caught.
 *
 * SINGLE-SHOT path (the bit the strict PASS gate cares about), from
 * adc_alif.c::adc_enable_single_shot_conv():
 *   - ADC_START_SRC  (0x00) bit7 ADC_START_ENABLE        = 0x80
 *   - ADC_CONTROL    (0x30) bit0 ADC_START_SINGLE_SHOT   = 0x01
 *   - result lands in ADC_SAMPLE_REG_0 (0x50) + 4*channel
 * The driver disables those again on completion (adc_context_on_complete ->
 * disable_adc -> adc_disable_single_shot_conv clears both bits), so post-read the
 * START_SRC ENABLE bit and the CONTROL SINGLE_SHOT bit read back 0 (idle).  That
 * idle-after-completion state is the register evidence the conversion ran and
 * tore down cleanly -- analogous to SSIENR=0 after the SPI transfer.
 *
 * The analog pad routed to channel 0 is a BENCH UNKNOWN on this batch, so the raw
 * sample value is REPORTED, not part of the strict PASS gate (an open pad reads a
 * floating / near-VREF value).  The PASS gate is: adc_channel_setup() and
 * adc_read() both returned 0 (the driver programmed the IP + ran a single-shot
 * conversion through the analog block + the DONE1 IRQ delivered a sample), and
 * the controller is idle afterwards.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/printk.h>

#define ADC_NODE DT_NODELABEL(adc12_0)

/* Absolute reg base straight from the dtsi reg[0] = "adc_reg" (0x49020000),
 * pulled from devicetree so this stays correct if the node ever moves. */
#define ADC_BASE ((uint32_t)DT_REG_ADDR_BY_NAME(ADC_NODE, adc_reg))

/* ADC register offsets -- VERBATIM from adc_alif.c. */
#define OFF_START_SRC 0x00U
#define OFF_CONTROL   0x30U
#define OFF_SEL       0x3CU
#define OFF_SAMPLE_0  0x50U

/* ADC_START_SRC bit7 = ADC_START_ENABLE; ADC_CONTROL bit0 = SINGLE_SHOT.
 * Both are cleared by the driver's completion teardown, so post-read = 0. */
#define START_ENABLE_BIT (1U << 7)
#define CTRL_SINGLE_SHOT (1U << 0)

/* Channel under test (single-channel scan, single-shot). */
#define TEST_CHANNEL 0U

/* 12-bit SAR: the resolution the driver advertises for ADC12 instances.  The
 * Alif driver ignores adc_sequence.resolution (it returns -ENOTSUP if non-zero),
 * so we pass 0 in the sequence; this is for buffer sizing / reporting only. */
#define ADC_RESOLUTION 12U

static inline uint32_t rd(uint32_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

int main(void)
{
	const struct device *adc = DEVICE_DT_GET(ADC_NODE);

	printk("\n=== aen-adc-regcheck ===\n");
	printk("adc node   : %s\n", DT_NODE_FULL_NAME(ADC_NODE));
	printk("adc_base   : 0x%08x\n", ADC_BASE);

	if (!device_is_ready(adc)) {
		printk("RESULT FAIL: adc device not ready (init/clock/VREF failed)\n");
		return 0;
	}

	/* Channel 0, single-ended, default gain / reference (the per-instance VREF
	 * + clock are programmed by adc_init() from the DT node properties). */
	struct adc_channel_cfg ch_cfg = {
		.gain             = ADC_GAIN_1,
		.reference        = ADC_REF_INTERNAL,
		.acquisition_time = ADC_ACQ_TIME_DEFAULT,
		.channel_id       = TEST_CHANNEL,
		.differential     = 0,
	};

	int rc_setup = adc_channel_setup(adc, &ch_cfg);
	printk("adc_channel_setup(ch=%u) rc = %d\n", TEST_CHANNEL, rc_setup);

	/*
	 * Single-shot read of channel 0.  NOTE: adc_alif.c::adc_start_read()
	 * dereferences sequence->options->user_data (it stashes a comparator-status
	 * pointer there), so options MUST be non-NULL with a valid user_data -- pass
	 * a stack byte to satisfy that.  (A bare adc_read with options==NULL would
	 * fault inside the driver; this is a driver quirk we honour, not invent.)
	 */
	/* The driver's check_buffer_size() needs active_channels * sizeof(uint32_t)
	 * (it stores a 32-bit result per channel) -- a uint16_t buffer is too small
	 * and adc_read() returns -ENOMEM.  Use uint32_t. */
	uint32_t sample     = 0;
	uint8_t  cmp_status = 0;

	const struct adc_sequence_options opts = {
		.interval_us     = 0,
		.callback        = NULL,
		.user_data       = &cmp_status,
		.extra_samplings = 0,
	};

	struct adc_sequence seq = {
		.options      = &opts,
		.channels     = BIT(TEST_CHANNEL),
		.buffer       = &sample,
		.buffer_size  = sizeof(sample),
		.resolution   = 0, /* driver rejects non-zero; programmed via DT */
		.oversampling = 0,
		.calibrate    = false,
	};

	int rc_read = adc_read(adc, &seq);
	printk("adc_read rc = %d\n", rc_read);
	printk("raw sample : %d (0x%04x)  [analog pad on ch0 is a bench unknown]\n",
	       (int)sample,
	       (uint16_t)sample);

	/*
	 * Register readback.  After a completed single-shot the driver has cleared
	 * the START_SRC ENABLE bit + the CONTROL SINGLE_SHOT bit (disable_adc), so
	 * both read 0 (idle).  ADC_SEL holds the last-converted channel.  The
	 * per-channel sample register retains the last conversion result.
	 */
	uint32_t start_src = rd(ADC_BASE, OFF_START_SRC);
	uint32_t control   = rd(ADC_BASE, OFF_CONTROL);
	uint32_t sel       = rd(ADC_BASE, OFF_SEL);
	uint32_t samp_reg  = rd(ADC_BASE, OFF_SAMPLE_0 + 4U * TEST_CHANNEL);

	printk("-- readback --\n");
	printk("START_SRC 0x%08x = 0x%08x (bit7 ENABLE exp 0 after completion)\n",
	       ADC_BASE + OFF_START_SRC,
	       start_src);
	printk("CONTROL   0x%08x = 0x%08x (bit0 SINGLE_SHOT exp 0 after completion)\n",
	       ADC_BASE + OFF_CONTROL,
	       control);
	printk("ADC_SEL   0x%08x = 0x%08x (last converted channel)\n", ADC_BASE + OFF_SEL, sel);
	printk("SAMPLE[%u] 0x%08x = 0x%08x (raw conversion result register)\n",
	       TEST_CHANNEL,
	       ADC_BASE + OFF_SAMPLE_0 + 4U * TEST_CHANNEL,
	       samp_reg);

	/*
	 * PASS gate:
	 *   - adc_channel_setup returned 0 (channel programmed, VREF/clock ok),
	 *   - adc_read returned 0 (single-shot conversion ran + DONE1 delivered),
	 *   - the controller is idle afterwards (START_SRC ENABLE + CONTROL
	 *     SINGLE_SHOT both cleared by the driver's completion teardown).
	 * The raw sample value is REPORTED only (the channel-0 analog pad is a bench
	 * unknown -- an open pad floats), so it is NOT part of the gate.
	 */
	bool idle_ok = ((start_src & START_ENABLE_BIT) == 0U) && ((control & CTRL_SINGLE_SHOT) == 0U);

	bool ok = true;

	ok &= (rc_setup == 0);
	ok &= (rc_read == 0);
	ok &= idle_ok;

	if (ok) {
		printk("RESULT PASS: adc12_0 setup+read rc=0, single-shot ran, "
		       "controller idle, raw=%d\n",
		       (int)sample);
	} else {
		printk("RESULT FAIL: setup_rc=%d read_rc=%d idle=%s "
		       "(start_src=0x%08x control=0x%08x)\n",
		       rc_setup,
		       rc_read,
		       idle_ok ? "OK" : "NOT-IDLE",
		       start_src,
		       control);
	}

	return 0;
}
