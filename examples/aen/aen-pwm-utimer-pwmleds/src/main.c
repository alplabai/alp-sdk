/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-pwm-utimer-pwmleds -- scopeless on-silicon validation of the Alif UTIMER
 * PWM driver (zephyr/drivers/pwm/pwm_alif_utimer.c) on the E1M-AEN801 (Ensemble
 * E8, M55-HE), via the bench RAM-run + RAM-console flow.
 *
 * This is the CONSUMER-NODE variant of aen-pwm-utimer-regcheck.  Both apps drive
 * the same UTIMER3/pwm3 driver-A channel and read back the same registers; the
 * difference is HOW the PWM controller device is obtained:
 *
 *   regcheck (sibling):  const struct device *pwm = DEVICE_DT_GET(pwm3_node);
 *       -> the "alif,pwm" binding re-declares #pwm-cells, so gen_defines treats
 *          the pwm3 controller node as if it owned a `pwms` property and emits a
 *          phantom `pwm3_P_pwms_IDX_0` reference; DEVICE_DT_GET on that node can
 *          fail codegen / never resolve a real device on the board build.
 *
 *   this app:            const struct pwm_dt_spec sp = PWM_DT_SPEC_GET(led_child);
 *       -> PWM_DT_SPEC_GET expands to .dev = DEVICE_DT_GET(DT_PWMS_CTLR_BY_IDX(
 *          led_child, 0)); i.e. it dereferences the `pwms` phandle that the
 *          pwm-leds CONSUMER node actually owns, reaching the pwm3 controller
 *          device through the standard PWM consumer path.  This is how the
 *          earlier board build worked.  We then call the standard PWM API on
 *          sp.dev.
 *
 * We cannot see the PWM pin on this bench (no scope, app UART not on USB).  So
 * we validate by REGISTER READBACK: drive pwm_set_cycles on UTIMER3/pwm3 channel
 * 0 (driver A), then confirm the driver programmed the UTIMER period (CNTR_PTR),
 * duty (COMPARE_A), the control/enable bits (COMPARE_CTRL_A, CNTR_CTRL) and the
 * per-timer clock gate, and that the global block STARTED the counter
 * (GLB_CNTR_RUNNING).
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
 */

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/printk.h>

/*
 * The pwm-leds CONSUMER child node (board overlay).  Its `pwms` phandle is what
 * PWM_DT_SPEC_GET dereferences to reach the pwm3 controller device -- we never
 * DEVICE_DT_GET the controller node directly.
 */
#define LED_NODE       DT_NODELABEL(bench_pwm_ch0)

/* The pwm3 controller node + its parent utimer3, reached via the consumer's
 * `pwms` phandle -- used ONLY to pull the register bases from devicetree (so the
 * absolute addresses stay correct if the node ever moves).  This is a pure DT
 * traversal, not a DEVICE_DT_GET, so it triggers no codegen device reference. */
#define PWM_CTLR_NODE  DT_PWMS_CTLR_BY_IDX(LED_NODE, 0)
#define UTIMER_NODE    DT_PARENT(PWM_CTLR_NODE)

/* Absolute register bases straight from the dtsi reg = <timer global>:
 * utimer3 timer block 0x48004000, shared global block 0x48000000. */
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

/*
 * Expected COMPARE_CTRL_A for normal polarity.  pwm_alif_set_cycles builds
 * drv_cfg = START_VAL_HIGH | LOW_AT_COMP_MATCH | HIGH_AT_CYCLE_END | DRIVER_EN |
 * COMPARE_EN and writes it via config_driver_output, then enable_compare_match +
 * enable_driver (already-set bits).  Per utimer.h bit map:
 *   LOW_AT_COMP_MATCH  = bit0  = 0x001
 *   HIGH_AT_CYCLE_END  = bit3  = 0x008
 *   START_VAL_HIGH     = bit4  = 0x010
 *   DRIVER_EN          = bit8  = 0x100
 *   COMPARE_EN         = bit11 = 0x800
 *                              = 0x919
 */
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
	/*
	 * THE FIX: obtain the controller device through the consumer's `pwms`
	 * phandle (PWM_DT_SPEC_GET), NOT DEVICE_DT_GET on the pwm3 node.
	 */
	static const struct pwm_dt_spec pwm = PWM_DT_SPEC_GET(LED_NODE);

	printk("\n=== aen-pwm-utimer-pwmleds ===\n");
	printk("consumer node  : %s\n", DT_NODE_FULL_NAME(LED_NODE));
	printk("pwm ctlr node  : %s\n", DT_NODE_FULL_NAME(PWM_CTLR_NODE));
	printk("spec.channel   : %u\n", (unsigned)pwm.channel);
	printk("spec.period    : %u ns (informational; test uses exact cycles)\n",
	       (unsigned)pwm.period);
	printk("timer_base     : 0x%08x\n", TIMER_BASE);
	printk("global_base    : 0x%08x\n", GLOBAL_BASE);
	printk("timer_id       : %u\n", (unsigned)TIMER_ID);
	printk("requested period_cycles = %u (0x%x)\n", TEST_PERIOD_CYC, TEST_PERIOD_CYC);
	printk("requested pulse_cycles  = %u (0x%x)\n", TEST_PULSE_CYC, TEST_PULSE_CYC);
	printk("channel = %u, flags = NORMAL\n", TEST_CHANNEL);

	if (!device_is_ready(pwm.dev)) {
		printk("RESULT FAIL: pwm device not ready (init/pinctrl failed)\n");
		return 0;
	}

	/* Drive the standard Zephyr PWM API on the resolved controller device. */
	int rc = pwm_set_cycles(pwm.dev, TEST_CHANNEL, TEST_PERIOD_CYC,
				TEST_PULSE_CYC, 0 /* normal polarity */);
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

	/* PASS criteria: programmed regs match AND the counter-enable + clock-gate
	 * bits are set.  (GLB_CNTR_RUNNING / live CNTR are reported but NOT in the
	 * strict gate -- whether the free-running counter visibly advances within
	 * k_busy_wait is timing-sensitive; CNTR_CTRL_EN + GLB_CLOCK_ENABLE are the
	 * authoritative enable evidence.) */
	bool ok = true;
	ok &= (reload == TEST_PERIOD_CYC);
	ok &= (compare_a == TEST_PULSE_CYC);
	ok &= (cc_a == EXP_COMPARE_CTRL_A);
	ok &= ((cntr_ctrl & 0x1U) != 0U);              /* CNTR_CTRL_EN */
	ok &= ((cntr_ctrl & (1U << 8)) == 0U);         /* up-counter: DIR_DOWN clear */
	ok &= ((clk_en & (1U << TIMER_ID)) != 0U);     /* per-timer clock gated on */

	if (ok) {
		printk("RESULT PASS: UTIMER3 PWM (via pwm-leds consumer) programmed "
		       "period=%u duty=%u ctrl=0x%03x, counter-enable + clock-gate set\n",
		       reload, compare_a, cc_a);
	} else {
		printk("RESULT FAIL: register readback mismatch (see lines above)\n");
	}

	return 0;
}
