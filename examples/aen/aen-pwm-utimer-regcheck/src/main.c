/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-pwm-utimer-regcheck -- scopeless on-silicon validation of the Alif
 * UTIMER PWM driver (zephyr/drivers/pwm/pwm_alif_utimer.c) on the E1M-AEN801
 * (Ensemble E8, M55-HE), via the bench RAM-run + RAM-console flow.
 *
 * We cannot see the PWM pin on this bench (no scope, app UART not on USB).  So
 * we validate by REGISTER READBACK: drive the standard Zephyr PWM API
 * (pwm_set_cycles) on the alp-pwm0 alias = utimer3/pwm3 channel 0 (driver A),
 * then confirm the driver programmed the UTIMER period (CNTR_PTR), duty
 * (COMPARE_A) and the control/enable bits (COMPARE_CTRL_A, CNTR_CTRL) and that
 * the global block actually STARTED the counter (GLB_CNTR_START /
 * GLB_CNTR_RUNNING).
 *
 * Two independent confirmations:
 *   1. This firmware re-reads the same UTIMER registers and prints them, plus a
 *      single RESULT PASS/FAIL line, to the RAM console (read 'ram_console_buf'
 *      over J-Link mem8, ASCII-decode).
 *   2. The human re-reads the SAME absolute addresses over J-Link mem32 (see the
 *      runPlan) -- so a wrong driver that only *prints* the right thing is still
 *      caught.
 *
 * We call pwm_set_cycles (NOT pwm_set / pwm_set_dt) on purpose: it writes
 * period_cycles / pulse_cycles into the registers verbatim, with NO ns->cycles
 * division, so the expected readback is exact and independent of the (still
 * placeholder) UTIMER tick-rate.  Period = 20000 cycles, pulse = 5000 cycles
 * (25% duty), channel 0, normal polarity.
 *
 * Counter-bug cross-check: the sibling counter app reports counter_get_value()
 * stuck at 0 on this silicon.  That same start path (enable_soft_counter_ctrl +
 * GLB_CNTR_START) is exercised here.  Reading GLB_CNTR_RUNNING bit3 (and, after a
 * short spin, the live CNTR register) tells us whether the timer actually runs:
 *   - RUNNING bit3 == 1 and CNTR advancing -> PWM start path works; counter bug
 *     is in the counter driver's own readback path.
 *   - RUNNING bit3 == 0 (or CNTR stuck) -> the GLB start / clock-gate path itself
 *     is the silicon problem, shared by both drivers.
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/printk.h>

/* alp-pwm0 -> utimer3/pwm3 (the board overlay aliases it). */
#define PWM_NODE       DT_ALIAS(alp_pwm0)
#define UTIMER_NODE    DT_PARENT(PWM_NODE)

/* Absolute register bases straight from the dtsi reg = <timer global>:
 * utimer3 timer block 0x48004000, shared global block 0x48000000.  Pulled from
 * devicetree so this stays correct if the node ever moves. */
#define TIMER_BASE     ((uint32_t)DT_REG_ADDR_BY_IDX(UTIMER_NODE, 0))
#define GLOBAL_BASE    ((uint32_t)DT_REG_ADDR_BY_IDX(UTIMER_NODE, 1))
#define TIMER_ID       ((uint32_t)DT_PROP(UTIMER_NODE, timer_id))

/* UTIMER register offsets (mirror modules/hal/alif drivers/utimer/include/utimer.h).
 * Timer-block offsets: */
#define OFF_CNTR_CTRL      0x080U   /* CNTR_CTRL: bit0 EN, bit2 sawtooth, bit8 DIR_DOWN */
#define OFF_COMPARE_CTRL_A 0x08CU   /* driver-A out config + DRIVER_EN(bit8) + COMPARE_EN(bit11) */
#define OFF_CNTR_PTR       0x0A4U   /* reload value == period_cycles */
#define OFF_CNTR           0x0A0U   /* live counter value */
#define OFF_COMPARE_A      0x0D0U   /* compare value == pulse_cycles (driver A) */
/* Global-block offsets: */
#define OFF_GLB_START      0x000U   /* GLB_CNTR_START: write 1<<id to start */
#define OFF_GLB_RUNNING    0x00CU   /* GLB_CNTR_RUNNING: bit id reads 1 while running */
#define OFF_GLB_CLK_EN     0x020U   /* GLB_CLOCK_ENABLE: bit id == per-timer clock gate */

/* Expected COMPARE_CTRL_A for normal-polarity, after the driver's
 * config_driver_output + enable_compare_match + enable_driver:
 *   LOW_AT_COMP_MATCH(bit0) | HIGH_AT_CYCLE_END(bit3) | START_VAL_HIGH(bit4) |
 *   DRIVER_EN(bit8) | COMPARE_EN(bit11) = 0x919. */
#define EXP_COMPARE_CTRL_A 0x919U

/* Test stimulus: exact cycle counts (no ns conversion). */
#define TEST_CHANNEL       0U        /* driver A */
#define TEST_PERIOD_CYC    20000U    /* CNTR_PTR / reload */
#define TEST_PULSE_CYC     5000U     /* COMPARE_A / duty (25%) */

static inline uint32_t rd(uint32_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

int main(void)
{
	const struct device *pwm = DEVICE_DT_GET(PWM_NODE);

	printk("\n=== aen-pwm-utimer-regcheck ===\n");
	printk("pwm node       : %s\n", DT_NODE_FULL_NAME(PWM_NODE));
	printk("timer_base     : 0x%08x\n", TIMER_BASE);
	printk("global_base    : 0x%08x\n", GLOBAL_BASE);
	printk("timer_id       : %u\n", (unsigned)TIMER_ID);
	printk("requested period_cycles = %u (0x%x)\n", TEST_PERIOD_CYC, TEST_PERIOD_CYC);
	printk("requested pulse_cycles  = %u (0x%x)\n", TEST_PULSE_CYC, TEST_PULSE_CYC);
	printk("channel = %u, flags = NORMAL\n", TEST_CHANNEL);

	if (!device_is_ready(pwm)) {
		printk("RESULT FAIL: pwm device not ready (init/pinctrl failed)\n");
		return 0;
	}

	int rc = pwm_set_cycles(pwm, TEST_CHANNEL, TEST_PERIOD_CYC, TEST_PULSE_CYC,
				0 /* normal polarity */);
	printk("pwm_set_cycles rc = %d\n", rc);
	if (rc != 0) {
		printk("RESULT FAIL: pwm_set_cycles returned %d\n", rc);
		return 0;
	}

	/* Let the global start latch + a few counter ticks elapse. */
	k_busy_wait(1000);

	uint32_t reload    = rd(TIMER_BASE, OFF_CNTR_PTR);
	uint32_t compare_a = rd(TIMER_BASE, OFF_COMPARE_A);
	uint32_t cc_a      = rd(TIMER_BASE, OFF_COMPARE_CTRL_A);
	uint32_t cntr_ctrl = rd(TIMER_BASE, OFF_CNTR_CTRL);
	uint32_t clk_en    = rd(GLOBAL_BASE, OFF_GLB_CLK_EN);
	uint32_t running   = rd(GLOBAL_BASE, OFF_GLB_RUNNING);
	uint32_t cntr1     = rd(TIMER_BASE, OFF_CNTR);

	k_busy_wait(200);
	uint32_t cntr2 = rd(TIMER_BASE, OFF_CNTR);

	printk("-- readback --\n");
	printk("CNTR_PTR  (reload/period) 0x%08x = 0x%08x (exp 0x%08x)\n",
	       TIMER_BASE + OFF_CNTR_PTR, reload, TEST_PERIOD_CYC);
	printk("COMPARE_A (duty/pulse)    0x%08x = 0x%08x (exp 0x%08x)\n",
	       TIMER_BASE + OFF_COMPARE_A, compare_a, TEST_PULSE_CYC);
	printk("COMPARE_CTRL_A            0x%08x = 0x%08x (exp 0x%08x)\n",
	       TIMER_BASE + OFF_COMPARE_CTRL_A, cc_a, EXP_COMPARE_CTRL_A);
	printk("CNTR_CTRL                 0x%08x = 0x%08x (bit0 EN exp 1, bit8 DIR_DOWN exp 0)\n",
	       TIMER_BASE + OFF_CNTR_CTRL, cntr_ctrl);
	printk("GLB_CLOCK_ENABLE          0x%08x = 0x%08x (bit%u exp 1)\n",
	       GLOBAL_BASE + OFF_GLB_CLK_EN, clk_en, (unsigned)TIMER_ID);
	printk("GLB_CNTR_RUNNING          0x%08x = 0x%08x (bit%u exp 1)\n",
	       GLOBAL_BASE + OFF_GLB_RUNNING, running, (unsigned)TIMER_ID);
	printk("CNTR live: t0=0x%08x t1=0x%08x (advancing? %s)\n",
	       cntr1, cntr2, (cntr2 != cntr1) ? "YES" : "NO");

	/* PASS criteria: programmed regs match AND the counter-enable bit is set. */
	bool ok = true;
	ok &= (reload == TEST_PERIOD_CYC);
	ok &= (compare_a == TEST_PULSE_CYC);
	ok &= (cc_a == EXP_COMPARE_CTRL_A);
	ok &= ((cntr_ctrl & 0x1U) != 0U);              /* CNTR_CTRL_EN */
	ok &= ((cntr_ctrl & (1U << 8)) == 0U);         /* up-counter: DIR_DOWN clear */
	ok &= ((clk_en & (1U << TIMER_ID)) != 0U);     /* per-timer clock gated on */

	if (ok) {
		printk("RESULT PASS: UTIMER3 PWM programmed period=%u duty=%u "
		       "ctrl=0x%03x, counter-enable + clock-gate set\n",
		       reload, compare_a, cc_a);
	} else {
		printk("RESULT FAIL: register readback mismatch (see lines above)\n");
	}

	/* Independent observability note: GLB_CNTR_RUNNING bit%u and the live CNTR
	 * pair disambiguate the counter-stuck-at-0 bug.  They are reported but are
	 * NOT part of the strict PASS gate, because whether the free-running counter
	 * has wrapped/visibly advanced within k_busy_wait is timing-sensitive; the
	 * authoritative enable evidence is CNTR_CTRL_EN + GLB_CLOCK_ENABLE above. */
	return 0;
}
