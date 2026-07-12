/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G553 supervisor MCU bridge smokes: the host driver's NULL-arg /
 * NOT_READY contract (v0.2 + v0.5 helpers), and gd32_swd (the host-side
 * bit-bang SWD controller used to flash it).
 */

#include <zephyr/ztest.h>

#include "alp/chips/gd32_swd.h"
#include "alp/chips/gd32g553.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

/* ------------------------------------------------------------------ */
/* gd32g553 -- V2N supervisor MCU host driver, NULL-arg validation     */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_gd32g553_init_null_ctx)
{
	zassert_equal(gd32g553_init(NULL, NULL, NULL, 0u), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gd32g553_init_no_bus_handles)
{
	gd32g553_t ctx;
	zassert_equal(gd32g553_init(&ctx, NULL, NULL, 0u), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gd32g553_init_invalid_i2c_addr)
{
	gd32g553_t ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 100000,
	});
	zassert_not_null(bus);
	zassert_equal(gd32g553_init(&ctx, NULL, bus, 0x80u),
	              ALP_ERR_INVAL,
	              "8-bit address through the 7-bit API must be rejected");
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_gd32g553_post_init_calls_reject_uninitialised)
{
	gd32g553_t ctx = { 0 };
	zassert_equal(gd32g553_set_default_transport(&ctx, GD32G553_TRANSPORT_SPI), ALP_ERR_NOT_READY);

	uint32_t levels;
	zassert_equal(gd32g553_gpio_read(&ctx, 0u, &levels), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_gpio_write(&ctx, 0u, 0u), ALP_ERR_NOT_READY);

	uint8_t pmic = 0u;
	zassert_equal(gd32g553_da9292_status_forward(&ctx, &pmic), ALP_ERR_NOT_READY);

	/* v0.2 wrappers must obey the same NOT_READY contract. */
	uint16_t mv    = 0u;
	int32_t  pos   = 0;
	uint32_t ticks = 0u;
	zassert_equal(gd32g553_dac_set(&ctx, 0u, 0u), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_dac_get(&ctx, 0u, &mv), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_qenc_read(&ctx, 0u, &pos), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_qenc_reset(&ctx, 0u), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_counter_read(&ctx, 0u, &ticks), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_gd32g553_pwm_set_invalid_duty)
{
	gd32g553_t ctx = { .initialised = true };
	zassert_equal(gd32g553_pwm_set(&ctx, 0u, 100000u, 200000u), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gd32g553_adc_read_invalid_samples)
{
	gd32g553_t ctx = { .initialised = true };
	uint16_t   mv[16];

	zassert_equal(gd32g553_adc_read(&ctx, 0u, 0u, mv), ALP_ERR_INVAL);
	zassert_equal(gd32g553_adc_read(&ctx, 0u, GD32G553_BRIDGE_ADC_MAX_SAMPLES + 1u, mv),
	              ALP_ERR_INVAL);
	zassert_equal(gd32g553_adc_read(&ctx, 0u, 1u, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gd32g553_v02_invalid_args)
{
	/* Range + NULL-arg validation on the v0.2 wrappers happens before
     * any transport call so it's safe to assert against an
     * `initialised=true` stub context with no real bus handles. */
	gd32g553_t ctx   = { .initialised = true };
	uint16_t   mv    = 0u;
	int32_t    pos   = 0;
	uint32_t   ticks = 0u;

	/* DAC: channel out of range rejects, NULL out-pointer on DAC_GET rejects. */
	zassert_equal(gd32g553_dac_set(&ctx, GD32G553_BRIDGE_DAC_CHANNELS, 1650u), ALP_ERR_INVAL);
	zassert_equal(gd32g553_dac_get(&ctx, 0u, NULL), ALP_ERR_INVAL);
	zassert_equal(gd32g553_dac_get(&ctx, GD32G553_BRIDGE_DAC_CHANNELS, &mv), ALP_ERR_INVAL);

	/* QENC: encoder out of range rejects, NULL out-pointer on QENC_READ rejects. */
	zassert_equal(gd32g553_qenc_read(&ctx, 0u, NULL), ALP_ERR_INVAL);
	zassert_equal(gd32g553_qenc_read(&ctx, GD32G553_BRIDGE_QENC_CHANNELS, &pos), ALP_ERR_INVAL);
	zassert_equal(gd32g553_qenc_reset(&ctx, GD32G553_BRIDGE_QENC_CHANNELS), ALP_ERR_INVAL);

	/* COUNTER: counter out of range rejects, NULL out-pointer rejects. */
	zassert_equal(gd32g553_counter_read(&ctx, 0u, NULL), ALP_ERR_INVAL);
	zassert_equal(gd32g553_counter_read(&ctx, GD32G553_BRIDGE_COUNTER_CHANNELS, &ticks),
	              ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* gd32_swd -- bit-bang SWD controller (host-side)                    */
/*                                                                    */
/* No real GPIO emul can drive the SWD protocol -- the bits hit the   */
/* wire faster than gpio_emul can latch state.  These tests cover the */
/* argument-validation surface + the uninitialised post-init paths.   */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_gd32_swd_init_null_args)
{
	gd32_swd_t  ctx;
	alp_gpio_t *bogus = (alp_gpio_t *)0xDEADBEEFu;

	/* NULL ctx -> INVAL.  Even with non-NULL pin handles. */
	zassert_equal(gd32_swd_init(NULL, bogus, bogus, NULL), ALP_ERR_INVAL);
	/* NULL swdio -> INVAL. */
	zassert_equal(gd32_swd_init(&ctx, NULL, bogus, NULL), ALP_ERR_INVAL);
	/* NULL swclk -> INVAL. */
	zassert_equal(gd32_swd_init(&ctx, bogus, NULL, NULL), ALP_ERR_INVAL);
	/* NULL nrst is allowed (boards that don't route it work via
     * AIRCR.SYSRESETREQ).  Not asserted here -- the gpio_emul-backed
     * init would still try alp_gpio_configure on the two bogus
     * pointers, which is not a contract this layer tests. */
}

ZTEST(alp_chips, test_gd32_swd_calls_reject_uninitialised)
{
	gd32_swd_t ctx = { 0 };

	/* Every post-init helper must report NOT_READY rather than
     * dereferencing NULL swdio / swclk on a zeroed context. */
	zassert_equal(gd32_swd_set_clock_delay(&ctx, 4u), ALP_ERR_NOT_READY);
	zassert_equal(gd32_swd_connect(&ctx), ALP_ERR_NOT_READY);
	zassert_equal(gd32_swd_halt(&ctx), ALP_ERR_NOT_READY);
	zassert_equal(gd32_swd_flash_erase(&ctx, GD32_SWD_FMC_FLASH_BASE, 4096u), ALP_ERR_NOT_READY);
	uint8_t buf[8] = { 0 };
	zassert_equal(gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, sizeof buf),
	              ALP_ERR_NOT_READY);
	zassert_equal(gd32_swd_flash_verify(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, sizeof buf),
	              ALP_ERR_NOT_READY);
	zassert_equal(gd32_swd_reset_and_run(&ctx), ALP_ERR_NOT_READY);

	/* deinit must be safe on a zero context + on NULL. */
	gd32_swd_deinit(&ctx);
	gd32_swd_deinit(NULL);
}

ZTEST(alp_chips, test_gd32_swd_flash_helpers_reject_unconnected)
{
	/* .initialised but not .connected -- erase / write / verify
     * call gd32_swd_connect()'s outcome.  These should still report
     * NOT_READY because the SW-DP hasn't been brought up. */
	gd32_swd_t ctx    = { .initialised = true };
	uint8_t    buf[8] = { 0 };

	zassert_equal(gd32_swd_halt(&ctx), ALP_ERR_NOT_READY);
	zassert_equal(gd32_swd_flash_erase(&ctx, GD32_SWD_FMC_FLASH_BASE, 4096u), ALP_ERR_NOT_READY);
	zassert_equal(gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, sizeof buf),
	              ALP_ERR_NOT_READY);
	zassert_equal(gd32_swd_flash_verify(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, sizeof buf),
	              ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_gd32_swd_flash_arg_validation)
{
	/* .connected = true so the function reaches the argument-check
     * branch before bus access. */
	gd32_swd_t     ctx       = { .initialised = true, .connected = true };
	uint8_t        buf[8]    = { 0 };
	const uint32_t flash_end = GD32_SWD_FMC_FLASH_BASE + 512u * 1024u;

	/* size == 0 -> INVAL. */
	zassert_equal(gd32_swd_flash_erase(&ctx, GD32_SWD_FMC_FLASH_BASE, 0u), ALP_ERR_INVAL);
	/* addr below flash base -> INVAL. */
	zassert_equal(gd32_swd_flash_erase(&ctx, 0x00000000u, 4096u), ALP_ERR_INVAL);
	/* Flash range checks reject exact-end, past-end, and wrapped
     * ranges before any FMC unlock/program/read operation. */
	zassert_equal(gd32_swd_flash_erase(&ctx, flash_end, 1u), ALP_ERR_INVAL);
	zassert_equal(gd32_swd_flash_erase(&ctx, flash_end - 1u, 2u), ALP_ERR_INVAL);
	zassert_equal(gd32_swd_flash_erase(&ctx, UINT32_MAX - 1u, 4u), ALP_ERR_INVAL);

	/* NULL data / zero len -> INVAL on write + verify. */
	zassert_equal(gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE, NULL, 8u), ALP_ERR_INVAL);
	zassert_equal(gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, 0u), ALP_ERR_INVAL);
	zassert_equal(gd32_swd_flash_verify(&ctx, GD32_SWD_FMC_FLASH_BASE, NULL, 8u), ALP_ERR_INVAL);

	/* Misaligned addr -> INVAL.  Write requires doubleword (8-byte)
     * alignment; verify requires word (4-byte) alignment. */
	zassert_equal(gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE + 1u, buf, sizeof buf),
	              ALP_ERR_INVAL);
	zassert_equal(gd32_swd_flash_verify(&ctx, GD32_SWD_FMC_FLASH_BASE + 1u, buf, sizeof buf),
	              ALP_ERR_INVAL);

	/* Aligned addresses that fall outside the 512 KiB flash window
     * must be rejected as ranges, not passed through to the SWD bus. */
	zassert_equal(gd32_swd_flash_write(&ctx, flash_end, buf, sizeof buf), ALP_ERR_INVAL);
	zassert_equal(gd32_swd_flash_write(&ctx, flash_end - 8u, buf, sizeof buf + 1u), ALP_ERR_INVAL);
	zassert_equal(gd32_swd_flash_write(&ctx, UINT32_MAX - 7u, buf, sizeof buf), ALP_ERR_INVAL);
	zassert_equal(gd32_swd_flash_write(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, SIZE_MAX),
	              ALP_ERR_INVAL);

	zassert_equal(gd32_swd_flash_verify(&ctx, flash_end, buf, sizeof buf), ALP_ERR_INVAL);
	zassert_equal(gd32_swd_flash_verify(&ctx, flash_end - 4u, buf, sizeof buf), ALP_ERR_INVAL);
	zassert_equal(gd32_swd_flash_verify(&ctx, UINT32_MAX - 3u, buf, sizeof buf), ALP_ERR_INVAL);
	zassert_equal(gd32_swd_flash_verify(&ctx, GD32_SWD_FMC_FLASH_BASE, buf, SIZE_MAX),
	              ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gd32_swd_clock_delay_clamp)
{
	/* set_clock_delay clamps to [0, 2048].  We can't read the
     * private field, but the contract is that the call accepts any
     * value (no INVAL) and only NOT_READY when uninit -- exercise
     * the latter only here. */
	gd32_swd_t ctx = { 0 };
	zassert_equal(gd32_swd_set_clock_delay(&ctx, 0u), ALP_ERR_NOT_READY);
	zassert_equal(gd32_swd_set_clock_delay(&ctx, 100000u), ALP_ERR_NOT_READY);

	/* Once initialised the call returns OK even for an out-of-range
     * value (internally clamped). */
	ctx.initialised = true;
	zassert_equal(gd32_swd_set_clock_delay(&ctx, 100000u),
	              ALP_OK,
	              "delay 100000 must clamp + return OK, not INVAL");
}

/* ------------------------------------------------------------------ */
/* GD32G553 v0.5 host helpers (§2B.2 + §2B.3 + Task #10) -- NOT_READY  */
/* + INVAL contracts against an uninitialised ctx.  Wire-side          */
/* behaviour validates against the firmware HAL bodies in HW-in-loop. */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_gd32g553_v05_calls_reject_uninitialised)
{
	gd32g553_t ctx       = { 0 };
	uint32_t   period_ns = 0u;
	uint32_t   pulse_ns  = 0u;
	uint8_t    chain_id  = 0u;

	/* Every v0.5 host helper must reject an uninitialised ctx with
     * NOT_READY -- the standard chip-driver lifecycle contract.
     * Real wire dispatch only happens after gd32g553_init() flips
     * ctx.initialised; until then the helpers short-circuit. */
	zassert_equal(gd32g553_pwm_capture_begin(&ctx, 0u, 0u), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_pwm_capture_read(&ctx, 0u, &period_ns, &pulse_ns), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_pwm_capture_end(&ctx, 0u), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_pwm_single_pulse(&ctx, 0u, 1000u), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_timer_sync(&ctx, 0u, 1u, 0u), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_power_mode_set(&ctx, 1u, 0u, 0u), ALP_ERR_NOT_READY);
	/* §2B wave-2 chunked DSP-chain upload helpers honour the same
     * NOT_READY contract -- chain_open, stage_push, chain_bind all
     * short-circuit before serialising the wire envelope. */
	zassert_equal(gd32g553_adc_dsp_chain_open(&ctx, &chain_id), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_adc_dsp_stage_push(&ctx, 0u, 0u, 0u, NULL, 0u), ALP_ERR_NOT_READY);
	zassert_equal(gd32g553_adc_dsp_chain_bind(&ctx, 0u, 0u), ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_gd32g553_v05_invalid_args)
{
	gd32g553_t ctx = { .initialised = true };

	/* pwm_capture_begin rejects edge > 2 (i.e. outside RISING /
     * FALLING / BOTH). */
	zassert_equal(gd32g553_pwm_capture_begin(&ctx, 0u, 3u), ALP_ERR_INVAL);
	zassert_equal(gd32g553_pwm_capture_begin(&ctx, 0u, 99u), ALP_ERR_INVAL);

	/* pwm_capture_read rejects both NULL out-params (caller wants
     * something out of the call). */
	zassert_equal(gd32g553_pwm_capture_read(&ctx, 0u, NULL, NULL), ALP_ERR_INVAL);

	/* pwm_single_pulse rejects pulse_ns == 0 (zero-width pulse
     * is meaningless and likely caller error). */
	zassert_equal(gd32g553_pwm_single_pulse(&ctx, 0u, 0u), ALP_ERR_INVAL);

	/* power_mode_set rejects mode > 3 (outside RUN / SLEEP /
     * DEEP_SLEEP / STANDBY). */
	zassert_equal(gd32g553_power_mode_set(&ctx, 4u, 0u, 0u), ALP_ERR_INVAL);
	zassert_equal(gd32g553_power_mode_set(&ctx, 99u, 0u, 0u), ALP_ERR_INVAL);

	/* §2B wave-2 DSP-chain helpers reject malformed args before they
     * hit cmd_send.  Each constraint mirrors the firmware-side
     * decoder's expectations so callers don't waste a wire trip to
     * surface obvious typos. */
	/* chain_open rejects NULL out-param (caller can't observe the
     * assigned chain_id without it). */
	zassert_equal(gd32g553_adc_dsp_chain_open(&ctx, NULL), ALP_ERR_INVAL);

	/* stage_push rejects stage_index outside [0, MAX_STAGES). */
	zassert_equal(
	    gd32g553_adc_dsp_stage_push(&ctx, 0u, GD32G553_BRIDGE_ADC_DSP_MAX_STAGES, 0u, NULL, 0u),
	    ALP_ERR_INVAL);
	zassert_equal(gd32g553_adc_dsp_stage_push(&ctx, 0u, 99u, 0u, NULL, 0u), ALP_ERR_INVAL);
	/* stage_push rejects kind > 3 (outside FIR/IIR/WINDOW/FFT). */
	zassert_equal(gd32g553_adc_dsp_stage_push(&ctx, 0u, 0u, 4u, NULL, 0u), ALP_ERR_INVAL);
	zassert_equal(gd32g553_adc_dsp_stage_push(&ctx, 0u, 0u, 99u, NULL, 0u), ALP_ERR_INVAL);
	/* stage_push rejects NULL params when len > 0 (caller asked to
     * upload bytes from a NULL pointer). */
	zassert_equal(gd32g553_adc_dsp_stage_push(&ctx, 0u, 0u, 0u, NULL, 4u), ALP_ERR_INVAL);
	/* stage_push rejects oversized payload (would overrun the
     * firmware's per-stage buffer). */
	{
		static const uint8_t oversized[GD32G553_BRIDGE_ADC_DSP_MAX_STAGE_BYTES + 4u] = { 0 };
		zassert_equal(
		    gd32g553_adc_dsp_stage_push(&ctx, 0u, 0u, 0u, oversized, (uint16_t)sizeof(oversized)),
		    ALP_ERR_OUT_OF_RANGE);
	}

	/* chain_bind rejects stream_id outside [0, ADC_STREAM_COUNT). */
	zassert_equal(gd32g553_adc_dsp_chain_bind(&ctx, 0u, GD32G553_BRIDGE_ADC_STREAM_COUNT),
	              ALP_ERR_INVAL);
	zassert_equal(gd32g553_adc_dsp_chain_bind(&ctx, 0u, 99u), ALP_ERR_INVAL);
}
