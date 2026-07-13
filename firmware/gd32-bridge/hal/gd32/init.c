/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GD32G5x3 backend for the bridge HAL.  Selected by setting
 * BRIDGE_HAL_BACKEND=gd32 in firmware/gd32-bridge/CMakeLists.txt.  Links
 * against the GigaDevice firmware-library wrapper at
 * vendors/gd32_firmware_library/ (git submodule pointing at
 * https://github.com/alplabai/gd32g5x3-firmware-library, a verbatim
 * mirror of GD's v1.5.0 release).
 *
 * Status (this commit):
 *   The file exists and compiles against the GigaDevice library so
 *   the `BRIDGE_HAL_BACKEND=gd32` build path is exercisable end-to-
 *   end without any peripheral I/O.  Every hook below is a STUB
 *   returning BRIDGE_HW_ERR_NOTIMPL -- identical wire behaviour to
 *   the bridge_hw_stub.c default backend, but routed through this
 *   file when `gd32` is selected so subsequent commits can replace
 *   stubs with real bodies one peripheral at a time.
 *
 * Implementation order (planned, in increasing risk):
 *
 *   1. RESET_REASON          -- DONE: RCU_RSTSCK decode + RSTFC clear.
 *   2. GPIO_READ / WRITE     -- DONE: 18-pad map (E1M IO8..IO35),
 *                               boot configures all as INPUT + PULL_UP,
 *                               write auto-promotes to OUTPUT push-pull.
 *   3. TRNG_READ             -- DONE: NIST SP800-90B mode init in
 *                               bridge_hw_init, DRDY-polled byte read
 *                               with bounded timeout.
 *   4. TMU_COMPUTE           -- DONE: 9 of 12 host functions via TMU
 *                               (tan / exp / tanh NOSUPPORT -- host
 *                               wrapper can libm-fallback in a future
 *                               commit).  F32 + Q31 paths.
 *   5. DAC_SET / GET         -- DONE: channel 0 -> DAC0_OUT0 / PA4,
 *                               channel 1 -> DAC1_OUT0 / PA6, mV<->12-bit
 *                               code at VREF=1800mV (V2N analog supply).
 *   6. PWM_SET / GET         -- DONE: 8 channels across TIMER0 + TIMER7
 *                               at 1 us LSB; pin AFs per datasheet Rev2.0.
 *   7. PWM_CONFIGURE         -- PARTIAL: defaults (edge-aligned, no
 *                               dead-time, no break input) accepted;
 *                               non-defaults NOSUPPORT pending follow-up.
 *   8. ADC_READ              -- DONE: 8-pad map across ADC0..3, single-
 *                               shot polling, mV<->code at VREF=1800mV.
 *   9. ADC_CONFIGURE         -- PARTIAL: per-channel sample_cycles
 *                               cached + applied; oversample_ratio +
 *                               resolution_bits gated to defaults (1,
 *                               12) until a follow-up commit.
 *   10. ADC_STREAM_*         -- DONE (§C.23): DMA0/1-backed
 *                               continuous acquisition.  Two parallel
 *                               streams; each owns a 1024-sample
 *                               circular ring buffer + a DMA channel
 *                               driving it peripheral-to-memory.
 *                               Host drains via polled stream_read;
 *                               write cursor recovered from the DMA
 *                               counter.
 *   11. QENC_READ / RESET    -- DONE: 4 encoders across TIMER1/2/3/4
 *                               in quadrature decoder mode 2 (X4).
 *   12. COUNTER_READ         -- DONE: Cortex-M33 DWT cycle counter
 *                               (32-bit free-running at core clock).
 *   13. PWM_CAPTURE_*        -- DONE (§C.15a): TIMERx input-capture
 *                               with polled drain.  V2N pad map binds
 *                               the COMPLEMENTARY (CHxN) outputs --
 *                               BEGIN reconfigures the channel as
 *                               input-capture but the captured edges
 *                               only land once a future hardware-
 *                               bring-up commit reworks the pad
 *                               routing onto the timer's main CHx
 *                               input mux.  Structural code path
 *                               (config + drain + ns conversion) is
 *                               complete and exercised end-to-end.
 *   14. PWM_SINGLE_PULSE     -- DONE: TIMERx OPM (one-pulse mode).
 *                               Switches the timer's whole SP-bit so
 *                               other channels on the same timer also
 *                               run as single-pulse until a PWM_SET
 *                               flips back to repetitive.
 *   15. TIMER_SYNC           -- DONE (§C.15b): master-slave SMC
 *                               config via timer_slave_mode_select
 *                               + timer_master_output0_trigger_source_select
 *                               + timer_master_slave_mode_config.
 *                               Wire `mode` byte translated to the
 *                               vendor's TIMER_SLAVE_MODE_* /
 *                               TIMER_QUAD_DECODER_MODE* encoding.
 *                               Slave listens to ITI0; SYSCFG router
 *                               left at chip default.
 *   16. POWER_MODE_SET       -- DONE (§C.15c + §C.25): mode 0/1
 *                               (run/sleep) accepted no-ops, mode 2
 *                               (deep-sleep) calls
 *                               pmu_to_deepsleepmode(LDO_LOWPOWER, WFI),
 *                               mode 3 (standby) calls
 *                               pmu_to_standbymode().  Wake-source
 *                               bitmap: GPIO enables PMU_WAKEUP_PIN0..4,
 *                               RTC + TIMER arm the RTC wakeup timer
 *                               at 0.5 ms LSB (IRC32K / DIV16); same
 *                               timer also honours wake_after_ms.
 *                               UART_RX / USB / ETH_LINK reject as
 *                               NOSUPPORT (no HW path on GD32G5).
 *   17. DA9292 fault forward -- 0xFF sentinel UNCONDITIONALLY: this
 *                               SoM rev wires the DA9292 fault nets
 *                               only to the Renesas (P37/P36); the
 *                               GD32 has no connection to them and no
 *                               I2C path to the PMIC.  Pin sampling
 *                               awaits a HW rev that mirrors the nets.
 *   18. ADC_DSP_*            -- DONE (§C.15d + §C.24): chain_open +
 *                               stage_push implemented as a 4-chain
 *                               pool with per-stage chunk reassembly
 *                               into a 260-byte buffer.  chain_bind
 *                               validates completeness + ordering
 *                               rules (FFT terminal, WINDOW preceding
 *                               FFT) and stores the binding on both
 *                               sides; runtime FFT/FAC dispatch inside
 *                               stream_read follows in a later commit.
 *
 * Each follow-up commit replaces ONE hook's stub body with a real
 * implementation and updates this header comment + the CHANGELOG.
 * The HIL turn-on cadence is determined by maintainer access to the
 * V2N EVK; the structural skeleton landing today lets the rest of the
 * tree (host-side ZTESTs, the alp_*_* portable surfaces, the
 * docs/test-plan rows) gate against the real backend as soon as the
 * first hook flips from stub to real.
 *
 * Build assumptions:
 *   - arm-none-eabi-gcc on PATH (toolchain file
 *     firmware/gd32-bridge/toolchain/arm-none-eabi.cmake handles the rest).
 *   - vendors/gd32_firmware_library/upstream/ submodule initialised
 *     (`git submodule update --init --recursive` from the repo root).
 *   - Cortex-M33 + Thumb + soft-float ABI (matches the GigaDevice
 *     library's compile flags).
 *
 * Layout note (2026-06-05): this backend was split move-only into per-peripheral TUs under hal/gd32/ (fw v0.2.8 refactor); this file owns boot bring-up + board-level reads, shared tables/helpers are declared in gd32_common.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bridge_hw.h"

/* The GigaDevice library headers are available via the wrapper's
 * PUBLIC include directories.  Including the device header here --
 * even when nothing below references its symbols yet -- gives us a
 * compile-time check that the submodule is in place and the include
 * path resolves.  Subsequent commits adding real hook bodies will
 * additionally include the matching peripheral header (e.g.
 * gd32g5x3_trng.h, gd32g5x3_tmu.h, gd32g5x3_gpio.h, ...) and
 * gd32-bridge will gain its own per-project libopt.h to pin which
 * standard-peripheral driver units actually link. */
#include "gd32g5x3.h"
#include "gd32_common.h"

/* ----------------------------------------------------------------- */
/* Boot hooks (overrides of the weak defaults in src/main.c)         */
/* ----------------------------------------------------------------- */

/* Called once on entry to main() before the transport ISRs come
 * online.  Future commits also wire up the remaining peripherals
 * the bridge uses (TMU, ADC0..ADC3, DAC, TIMER0/7/19, SysTick, etc.).
 * NOTE: no DA9292 wiring exists on this SoM rev -- the fault nets
 * reach only the Renesas (P37/P36); see bridge_hw_da9292_status_cached. */
void bridge_hw_init(void)
{
#if defined(BRIDGE_OTA_PARTITIONED) && defined(BRIDGE_APP_SLOT_BASE)
	/* OTA Path-A: the app runs from a flash slot, not 0x08000000, so move
     * the vector table off the vendor SystemInit default before any NVIC
     * use.  Runs first (main calls bridge_hw_init before the transports
     * enable interrupts). */
	SCB->VTOR = (uint32_t)(BRIDGE_APP_SLOT_BASE);
	__DSB();
	__ISB();
	/* The Path-A bootloader hands off with PRIMASK SET (it runs
     * __disable_irq() before swapping MSP/VTOR for the jump) -- without
     * clearing it here no interrupt ever fires in the slot app and the
     * transports go silent (silicon-caught 2026-06-04: CM33 retried init
     * 1600+ times against a slot app whose CS-EXTI could never run).
     * Re-enable now that OUR vector table is live; on a plain power-on
     * boot PRIMASK is already clear and this is a no-op. */
	__enable_irq();
#endif

	/* Enable AHB2 clocks for every GPIO port the pad map references.
     * The chip's RCU keeps unused GPIO ports clock-gated to save
     * power; we enable A..F unconditionally because the E1M IO map
     * spans those ports.  Port G isn't referenced by any pad. */
	rcu_periph_clock_enable(RCU_GPIOA);
	rcu_periph_clock_enable(RCU_GPIOB);
	rcu_periph_clock_enable(RCU_GPIOC);
	rcu_periph_clock_enable(RCU_GPIOD);
	rcu_periph_clock_enable(RCU_GPIOE);
	rcu_periph_clock_enable(RCU_GPIOF);

	/* Configure every entry in `gpio_pad_map` as INPUT + PULL_UP.
     * Safe default per the GPIO direction policy: no driven
     * contention with whatever the board might pull / drive on
     * those pads.  bridge_hw_gpio_write() promotes individual
     * pads to OUTPUT on demand. */
	for (size_t i = 0; i < GPIO_PAD_MAP_COUNT; ++i) {
		gpio_mode_set(
		    gpio_pad_map[i].periph, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, gpio_pad_map[i].pin);
		gpio_is_output[i] = false;
	}

	/* TRNG bring-up: configure + enable only.  The NIST pipeline's
     * first conditioned word can lag past any boot-time wait we are
     * willing to spin; readiness is promoted lazily by the first
     * TRNG_READ's short DRDY poll (by which point the host's boot
     * settle has given the analog source seconds, not milliseconds). */
	trng_started = trng_start();
	if (trng_started) {
		/* Boot-time readiness grace: the transports aren't up yet, so
         * spending a few milliseconds here is free and spares the
         * FIRST host TRNG_READ the one-time conditioning latency
         * (observed on the functional tier: the inaugural read paid
         * the promotion and answered STATUS_IO once).  Bounded; the
         * lazy per-read path remains the safety net. */
		for (unsigned round = 0u; round < 8u && trng_started && !trng_ready; ++round) {
			(void)trng_poll_ready();
		}
	}

	/* TMU clock enable.  Per-op configuration happens in
     * bridge_hw_tmu_compute because the mode + I/O width vary per
     * call. */
	rcu_periph_clock_enable(RCU_TMU);

	/* DAC bring-up.  Two channels per `dac_channels[]`:
     *   ch0 -> DAC0_OUT0 -> PA4
     *   ch1 -> DAC1_OUT0 -> PA6
     * Configure both pads as analog (GPIOA clock was enabled
     * above for the IO pad map), enable each DAC peripheral in
     * NORMAL_PIN_BUFFON mode so the output drives the pad
     * through the chip's built-in buffer. */
	gpio_mode_set(GPIOA, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, GPIO_PIN_4 | GPIO_PIN_6);
	rcu_periph_clock_enable(RCU_DAC0);
	rcu_periph_clock_enable(RCU_DAC1);
	for (size_t i = 0; i < DAC_CHANNEL_COUNT; ++i) {
		dac_deinit(dac_channels[i].periph);
		dac_trigger_disable(dac_channels[i].periph, dac_channels[i].out);
		dac_wave_mode_config(dac_channels[i].periph, dac_channels[i].out, DAC_WAVE_DISABLE);
		dac_mode_config(dac_channels[i].periph, dac_channels[i].out, NORMAL_PIN_BUFFON);
		dac_enable(dac_channels[i].periph, dac_channels[i].out);
	}

	/* Secure-element reset (SE_RST = PC13): park it DEASSERTED so the
     * OPTIGA Trust M runs and the line stops floating at the GPIO POR
     * default.  The host pulses it via CMD_SE_RESET to recover a
     * BRD_I2C bus the SE has clock-stretched low.  (GPIOC clock was
     * enabled above.) */
	se_reset_init();

	/* Free-running counter: enable the Cortex-M33 DWT cycle counter.
     * TRCENA in CoreDebug->DEMCR gates the entire DWT/ITM trace block;
     * setting CYCCNTENA in DWT->CTRL starts the 32-bit free-running
     * counter at the core clock rate (216 MHz on the GD32G553 in the
     * stock clock config -> ~19.9 s wrap, ~4.63 ns LSB).  The counter
     * is the source for bridge_hw_counter_read(). */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0u;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	/* PWM bring-up: configure the 8 PWM pads as alt-function outputs,
     * enable TIMER0 + TIMER7 clocks, run the per-timer + per-channel
     * init.  After this the timers are running at 1 us tick with 0%
     * duty on every channel; bridge_hw_pwm_set programs both the
     * channel compare register and the timer ARR per call. */
	for (size_t i = 0; i < PWM_CHANNEL_COUNT; ++i) {
		const gd32_pwm_ch_t *ch = &pwm_channels[i];
		gpio_mode_set(ch->gpio_port, GPIO_MODE_AF, GPIO_PUPD_NONE, ch->gpio_pin);
		gpio_output_options_set(ch->gpio_port, GPIO_OTYPE_PP, GPIO_OSPEED_12MHZ, ch->gpio_pin);
		gpio_af_set(ch->gpio_port, ch->gpio_af, ch->gpio_pin);
	}
	rcu_periph_clock_enable(RCU_TIMER0);
	rcu_periph_clock_enable(RCU_TIMER7);
	pwm_timer_init(TIMER0);
	pwm_timer_init(TIMER7);
	for (size_t i = 0; i < PWM_CHANNEL_COUNT; ++i) {
		pwm_channel_init(&pwm_channels[i]);
	}

	/* Analog REFERENCE bring-up -- MUST precede any ADC/DAC use.
     *
     * On this module revision the converters' reference node is served
     * by the GD32's ON-CHIP reference buffer -- there is no external
     * reference source (hardware rationale in the internal bench
     * notes).  At reset VREF_CS defaults to 0x02 (HIPM high-impedance):
     * the buffer is parked, the reference node is undriven, and EVERY
     * ADC channel + both DACs reference a dead node -- the entire
     * analog subsystem read garbage/zero (silently, since the old ADC
     * assertions were ceiling-only and DAC_GET echoes the digital hold
     * register, not the pad).
     *
     * Fix: enable the buffer.  Its three targets (2.048 / 2.5 /
     * 2.9 V) all exceed the module's 1.8 V VDDA, so the buffer
     * regulates as high as the rail allows (~VDDA); the lowest
     * target (2.048 V) is the closest fit and least headroom stress.
     * Bench-proven: VREFEN -> VREFRDY sets, and a DAC->ADC copper
     * loopback then tracks 1:1 (DAC 2730 -> ADC 2730).  The reference
     * cancels ratiometrically in that loop, so correctness is
     * independent of the exact railed reference value; the absolute mV
     * scale (ADC_VREF_MV / DAC_VREF_MV) tracks the railed VDDA.
     *
     * VREFRDY wait is BOUNDED (boot-time, no SysTick yet): a spin
     * cap, not an unbounded poll -- a never-ready buffer must not hang
     * the bridge before the transports come online. */
	rcu_periph_clock_enable(RCU_VREF);
	vref_voltage_select(VREF_VOLTAGE_SEL_2_048V); /* lowest target = closest under VDDA */
	/* CLEAR HIPM first.  VREF_CS resets to 0x02 (HIPM high-impedance),
     * and the SPL's vref_enable() is a read-modify-write that only
     * sets VREFEN -- it PRESERVES the reset HIPM bit, leaving the
     * buffer output high-Z so VREFRDY never sets and the reference
     * node stays dead (silicon-caught 2026-06-05: VREF_CS read 0x03 =
     * VREFEN|HIPM, ADC still zero).  HIPM must be cleared for the
     * buffer to drive the node. */
	vref_high_impedance_mode_disable();
	vref_enable();
	for (uint32_t vr = 0u; vr < 100000u; ++vr) {
		if (vref_status_get() == SET) break; /* VREFRDY -- buffer locked */
	}
	/* Latch the verdict.  A buffer that never locked leaves vref_ok
     * false and every ADC/DAC op answers IO instead of serving
     * garbage referenced to a dead node (the exact silent failure the
     * VREF bring-up exists to cure).  vref_ready_check() re-probes on
     * each analog op, so a late lock self-promotes. */
	vref_ok = (vref_status_get() == SET);

	/* ADC bring-up: configure 8 pads as analog, enable all four ADC
     * peripheral clocks, run the per-peripheral init.  Calibration
     * inside adc_periph_init now runs against a LIVE reference (it
     * previously self-calibrated against the undriven reference node,
     * baking in a bogus offset); the VREF bring-up above is the
     * prerequisite that makes that calibration meaningful. */
	for (size_t i = 0; i < ADC_CHANNEL_MAP_COUNT; ++i) {
		gpio_mode_set(adc_channels_map[i].gpio_port,
		              GPIO_MODE_ANALOG,
		              GPIO_PUPD_NONE,
		              adc_channels_map[i].gpio_pin);
	}
	rcu_periph_clock_enable(RCU_ADC0);
	rcu_periph_clock_enable(RCU_ADC1);
	rcu_periph_clock_enable(RCU_ADC2);
	rcu_periph_clock_enable(RCU_ADC3);
	/* Boot-time calibration result intentionally not latched: a
     * converter that fails here is also the converter every request-
     * path op re-times against (the read path's bounded EOC wait +
     * self-heal), so the failure surfaces loudly on first use instead
     * of wedging boot. */
	(void)adc_periph_init(ADC0);
	(void)adc_periph_init(ADC1);
	(void)adc_periph_init(ADC2);
	(void)adc_periph_init(ADC3);
	for (size_t i = 0; i < ADC_CHANNEL_MAP_COUNT; ++i) {
		adc_sample_cycles_cache[i]    = ADC_DEFAULT_SAMPLE_CYCLES;
		adc_resolution_bits_cache[i]  = ADC_RES_BITS_DEFAULT;
		adc_oversample_ratio_cache[i] = 1u; /* no oversampling */
	}

	/* Quadrature encoder bring-up: TIMER1..4 host the four E1M
     * encoder pairs.  Each timer uses its CH0 + CH1 input-capture
     * units as the X / Y quadrature inputs (decoder mode 2 -> X4). */
	rcu_periph_clock_enable(RCU_TIMER1);
	rcu_periph_clock_enable(RCU_TIMER2);
	rcu_periph_clock_enable(RCU_TIMER3);
	rcu_periph_clock_enable(RCU_TIMER4);
	for (size_t i = 0; i < QENC_CHANNEL_COUNT; ++i) {
		qenc_channel_init(&qenc_map[i]);
	}
}

/* Called from the SysTick handler (or the main loop's idle path) on a
 * fixed cadence.  Intentionally a no-op on this SoM revision: the
 * DA9292 fault nets (DA9292_INT/DA9292_TW) reach only the Renesas
 * (P37/P36) -- the GD32 has no pin to sample and no I2C path to the
 * PMIC.  When a future HW rev mirrors the fault nets onto GD32
 * inputs, this hook samples them and updates the byte returned by
 * bridge_hw_da9292_status_cached(). */
/* Base-level DSP pump (#496): drains each bound FIR/IIR stream's raw
 * samples through the FAC into its processed ring.  Runs here, off the
 * main WFI loop -- never in the CS-EXTI stream_read path. */
extern void bridge_hw_dsp_pump(void);

void bridge_hw_tick(void)
{
	bridge_hw_dsp_pump();
}

/* ----------------------------------------------------------------- */
/* Stub bodies -- shape mirrors bridge_hw_stub.c so the gd32 backend  */
/* is binary-compatible with the stub backend pending real impls.    */
/* ----------------------------------------------------------------- */

uint8_t bridge_hw_reset_reason(void)
{
	/* Read RCU_RSTSCK (reset/clock control status register, GD32G5xx
     * Reference Manual §6.6.13) and decode the sticky reset-cause
     * flags in the high byte: PORRSTF (bit 27), BORRSTF (25),
     * EPRSTF (26, NRST pin), SWRSTF (28), FWDGTRSTF (29),
     * WWDGTRSTF (30), LPRSTF (31).
     *
     * The hardware can latch multiple flags across nested resets, so
     * we decode in coldest-first priority order: a power-on event
     * dominates a brownout, which dominates an external-pin reset,
     * which dominates a watchdog or software trigger.  Encoded byte
     * matches the host's `gd32g553_reset_cause_t` in
     * <alp/chips/gd32g553.h>:
     *
     *   0 = UNKNOWN, 1 = POWER_ON, 2 = NRST_PIN, 3 = SOFT,
     *   4 = WDT, 5 = BROWNOUT, 6 = LOWPOWER.
     *
     * RSTFC (bit 24) clears every cause flag in one write; the vendor
     * helper `rcu_all_reset_flag_clear()` is functionally identical
     * but we keep the access inline to avoid pulling rcu.c stages we
     * don't otherwise need.  After the write the next reader sees
     * UNKNOWN unless something resets the chip again. */
	const uint32_t rstsck = RCU_RSTSCK;
	uint8_t        cause  = 0u; /* UNKNOWN */

	if (rstsck & RCU_RSTSCK_PORRSTF) {
		cause = 1u; /* POWER_ON */
	} else if (rstsck & RCU_RSTSCK_BORRSTF) {
		cause = 5u; /* BROWNOUT */
	} else if (rstsck & RCU_RSTSCK_EPRSTF) {
		cause = 2u; /* NRST_PIN */
	} else if (rstsck & RCU_RSTSCK_LPRSTF) {
		cause = 6u; /* LOWPOWER */
	} else if (rstsck & (RCU_RSTSCK_FWDGTRSTF | RCU_RSTSCK_WWDGTRSTF)) {
		cause = 4u; /* WDT */
	} else if (rstsck & RCU_RSTSCK_SWRSTF) {
		cause = 3u; /* SOFT */
	}

	RCU_RSTSCK |= RCU_RSTSCK_RSTFC;
	return cause;
}

uint8_t bridge_hw_da9292_status_cached(void)
{
	/* 0xFF sentinel unconditionally: no DA9292 net reaches the GD32 on
     * this SoM rev (fault pins are Renesas P37/P36 inputs). */
	return 0xFFu;
}
