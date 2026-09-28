/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-module RV-3028-C7 real-time clock + alarm interrupt on the E1M-AEN801
 * (Alif Ensemble E8, Cortex-M55-HE), over UPSTREAM Zephyr's `rtc_*` API.
 *
 * The part is driven by drivers/rtc/rtc_rv3028.c (compatible
 * "microcrystal,rv3028", CONFIG_RTC_RV3028) sitting on the BRD_I2C
 * housekeeping bus = SoC I2C0, and its /INT pin lands on P15_0 = LPGPIO bit 0
 * = IRQ 171.  ADR 0017 Tier-1: no alp-sdk driver, no vendored code, just the
 * devicetree node under boards/ plus this app.
 *
 * What this example demonstrates, in order
 * ---------------------------------------
 *   1. rtc_set_time() a known wall clock, rtc_get_time() it straight back, and
 *      print BOTH so the round trip is visible rather than asserted.
 *   2. Wait a few seconds and read again -- the clock must have ADVANCED.  A
 *      part that merely answers on I2C is not a running clock; only a second
 *      read separated in time proves the 32.768 kHz oscillator is oscillating.
 *   3. Arm an alarm and wait for it via the RTC alarm CALLBACK, with a bounded
 *      timeout so a dead interrupt line ends the run instead of hanging it.
 *   4. Disarm and clear the alarm flag so a re-run behaves identically.
 *   5. Print one machine-greppable RESULT PASS / PARTIAL / FAIL line.
 *
 * ===========================================================================
 * READ THIS BEFORE COPYING -- three module facts that will bite you
 * ===========================================================================
 *
 * (a) NO BACKUP SUPPLY -- THE CLOCK DOES NOT SURVIVE A POWER CYCLE.
 *     VBACKUP (U21 pin 6) has no supply fitted on this batch: its only other
 *     net members are R4 and R68, both 0-ohm and both DNP.  There is no coin
 *     cell, no supercap, and no trickle source, so the devicetree carries
 *     `backup-switch-mode = "disabled"` (the binding REQUIRES the property) and
 *     NO `trickle-resistor-ohms`.
 *
 *     Consequence: neither the time NOR a pending alarm is retained across a
 *     power cycle.  Every cold boot starts from an unset clock -- which is
 *     exactly why step 1 of this example SETS the time rather than assuming it.
 *     Do not design a feature on this RTC that implies persistence; if you need
 *     wall-clock time across a power cycle on this batch, it has to come from
 *     the network or from the host, not from here.
 *
 * (b) RTC_CLKOUT IS CARRIER-FACING AND FIRMWARE CANNOT USE IT.
 *     U21 pin 1 (CLKOUT) goes ONLY to the E1M edge connector, E1 pin AH16.  It
 *     does NOT reach the SoC.  The binding's `clkout-frequency` is therefore a
 *     choice made FOR A CARRIER, and it is deliberately omitted from the
 *     overlay so the pin stays LOW.  No firmware on this module can consume it.
 *
 * (c) MODULE_STBY / EVI IS AN INPUT DRIVEN BY THE CARRIER.
 *     U21 pin 8 (EVI, the external event input) comes FROM the edge connector,
 *     E1 pin O2, with R43 = 100k to +1V8.  It is asserted by carrier hardware.
 *     This module's firmware cannot drive it, so an EVI-triggered timestamp is
 *     a carrier integration feature, not something this app can exercise.
 *
 * ===========================================================================
 * ALARM RESOLUTION -- why the alarm is a MINUTE away, not "a few seconds"
 * ===========================================================================
 * The RV-3028's alarm register file holds MINUTES, HOURS and DATE only -- there
 * is no seconds alarm.  Upstream reflects that exactly:
 * rv3028_alarm_get_supported_fields() returns
 * MINUTE | HOUR | MONTHDAY, and rv3028_alarm_set_time() rejects any other bit
 * with -EINVAL.  So the finest alarm the hardware can express is "at the top of
 * some minute".
 *
 * This example gets a SHORT wait out of a minute-resolution alarm by choosing
 * the start time: it sets the clock to <minute>:SET_SECOND (second 40), then
 * arms the alarm for <minute>+1.  The alarm therefore fires ~20 s later --
 * short enough to watch on a bench, with no hardware capability invented.
 * ALARM_TIMEOUT_S is a full minute past that, so even a worst-case rollover
 * still ends in a bounded, reportable failure.
 *
 * Mask is RTC_ALARM_TIME_MASK_MINUTE alone -- no HOUR, no MONTHDAY.  An alarm
 * matched on minutes only recurs every hour, which is all this demo needs and
 * keeps the arming free of hour/day rollover arithmetic.
 *
 * ===========================================================================
 * CONSOLE
 * ===========================================================================
 * The E1M edge UART0 (Alif UART5, P3_4/P3_5, 115200 8N1).  See prj.conf for
 * the RAM-console alternative if your bench has no serial.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>

/* The RV-3028 node lives in boards/alp_e1m_aen801_m55_he_..._rtss_he.overlay as
 * a child of i2c0 (BRD_I2C).  DEVICE_DT_GET on a missing node is a BUILD error,
 * not a silent runtime NULL, so a dropped overlay cannot slip through. */
#define RTC_NODE DT_NODELABEL(rv3028)

static const struct device *const rtc = DEVICE_DT_GET(RTC_NODE);

/*
 * Wall clock we program in step 1.  The absolute instant is arbitrary -- the
 * example proves the clock ROUND-TRIPS and ADVANCES, not that it is correct.
 *
 * SET_SECOND is NOT arbitrary: it puts the clock 20 s before the top of the
 * next minute, which is what turns a minute-resolution alarm into a ~20 s wait.
 * See the "ALARM RESOLUTION" note in the file header.
 */
#define SET_YEAR   2026 /* struct rtc_time stores tm_year as years since 1900. */
#define SET_MONTH  9    /* 1..12 here; tm_mon is 0-based, converted below.     */
#define SET_DAY    5
#define SET_HOUR   12
#define SET_MINUTE 30
#define SET_SECOND 40

/* Dwell between the two reads that prove the oscillator runs.  3 s comfortably
 * clears the 1 s resolution of the seconds field even if the first read lands
 * just after a tick. */
#define ADVANCE_GAP_MS 3000U

/* Bounded wait for the alarm callback.  The alarm is ~20 s out (see above); 90 s
 * leaves more than a full minute of margin and still GUARANTEES the app reaches
 * its RESULT line instead of blocking forever on dead hardware. */
#define ALARM_TIMEOUT_S 90

/* The alarm callback runs from the system workqueue (rv3028_work_cb submits it
 * from the GPIO ISR), so a semaphore is the right handoff to main. */
static K_SEM_DEFINE(alarm_sem, 0, 1);

/**
 * @brief RTC alarm callback -- runs on the system workqueue, not in the ISR.
 *
 * The upstream driver's GPIO handler only submits work; the I2C register reads
 * that clear the alarm flag happen in that work item, and this callback is
 * invoked afterwards.  Keep it to a handoff: I2C transfers from here would
 * block the shared system workqueue.
 *
 * @param dev       The RTC device that raised the alarm.
 * @param id        Alarm id (the RV-3028 has exactly one, id 0).
 * @param user_data Opaque pointer supplied to rtc_alarm_set_callback().
 */
static void alarm_cb(const struct device *dev, uint16_t id, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	ARG_UNUSED(user_data);

	k_sem_give(&alarm_sem);
}

/**
 * @brief Print one struct rtc_time in ISO-ish form.
 *
 * @param what Short label identifying which read this is.
 * @param t    Time to print. tm_year is years since 1900 and tm_mon is 0-based,
 *             both denormalised here for human reading.
 */
static void print_time(const char *what, const struct rtc_time *t)
{
	printk("%s: %04d-%02d-%02d %02d:%02d:%02d\n",
	       what,
	       t->tm_year + 1900,
	       t->tm_mon + 1,
	       t->tm_mday,
	       t->tm_hour,
	       t->tm_min,
	       t->tm_sec);
}

int main(void)
{
	struct rtc_time set_t      = { 0 };
	struct rtc_time rd1        = { 0 };
	struct rtc_time rd2        = { 0 };
	struct rtc_time alarm_t    = { 0 };
	uint16_t        supported  = 0U;
	bool            time_ok    = false;
	bool            advance_ok = false;
	bool            alarm_ok   = false;
	int             rc;

	printk("\n=== E1M-AEN801 RV-3028-C7 RTC + alarm (upstream rtc_rv3028 on BRD_I2C) ===\n");
	printk("NOTE: VBACKUP is unpopulated on this batch (R4/R68 DNP) -- neither the\n");
	printk("      time nor a pending alarm survives a power cycle.  That is why this\n");
	printk("      example SETS the clock at every boot instead of reading it.\n");

	if (!device_is_ready(rtc)) {
		printk("RESULT FAIL: rv3028 device not ready -- the RTC did not probe on "
		       "BRD_I2C (I2C0). Check the i2c0 pinctrl (input-enable on BOTH "
		       "P7_0 and P7_1, bias-pull-up not pull-down) before suspecting the "
		       "part.\n");
		return 0;
	}

	/* ------------------------------------------------------------------
	 * 1. Set the time, then read it straight back and print BOTH.
	 * ------------------------------------------------------------------ */
	set_t.tm_year = SET_YEAR - 1900; /* struct rtc_time follows struct tm. */
	set_t.tm_mon  = SET_MONTH - 1;   /* 0-based month.                     */
	set_t.tm_mday = SET_DAY;
	set_t.tm_hour = SET_HOUR;
	set_t.tm_min  = SET_MINUTE;
	set_t.tm_sec  = SET_SECOND;

	rc = rtc_set_time(rtc, &set_t);
	if (rc != 0) {
		printk("RESULT FAIL: rtc_set_time() rc=%d\n", rc);
		return 0;
	}
	print_time("set   ", &set_t);

	rc = rtc_get_time(rtc, &rd1);
	if (rc != 0) {
		printk("RESULT FAIL: rtc_get_time() #1 rc=%d\n", rc);
		return 0;
	}
	print_time("read 1", &rd1);
	time_ok = true;

	/* ------------------------------------------------------------------
	 * 2. Prove the OSCILLATOR runs, not merely that the part answers.
	 *
	 * A register read only shows the I2C slave is alive.  Reading the clock
	 * a second time after a real delay is what distinguishes a running
	 * 32.768 kHz crystal from a frozen one -- and a frozen crystal is a
	 * completely different repair (the part, its load caps) than a dead bus.
	 * ------------------------------------------------------------------ */
	k_msleep(ADVANCE_GAP_MS);

	rc = rtc_get_time(rtc, &rd2);
	if (rc != 0) {
		printk("RESULT FAIL: rtc_get_time() #2 rc=%d\n", rc);
		return 0;
	}
	print_time("read 2", &rd2);

	/* Compare the seconds-of-minute, tolerating the 59 -> 00 wrap: a wrap
	 * makes the raw field go DOWN even though time moved forward, so
	 * "changed" is the correct proof here, not "increased". */
	advance_ok = (rd2.tm_sec != rd1.tm_sec) || (rd2.tm_min != rd1.tm_min);
	printk("oscillator: %s over %u ms (%02d:%02d:%02d -> %02d:%02d:%02d)\n",
	       advance_ok ? "RUNNING (clock advanced)" : "DID NOT ADVANCE",
	       ADVANCE_GAP_MS,
	       rd1.tm_hour,
	       rd1.tm_min,
	       rd1.tm_sec,
	       rd2.tm_hour,
	       rd2.tm_min,
	       rd2.tm_sec);

	/* ------------------------------------------------------------------
	 * 3. Arm the alarm and wait for the interrupt, with a BOUNDED timeout.
	 * ------------------------------------------------------------------ */

	/* Report what the hardware can actually match on, rather than assuming.
	 * On the RV-3028 this is MINUTE | HOUR | MONTHDAY -- there is no seconds
	 * alarm, which is why the wait below is a minute boundary and not an
	 * arbitrary "N seconds from now". */
	rc = rtc_alarm_get_supported_fields(rtc, 0U, &supported);
	if (rc != 0) {
		printk("rtc_alarm_get_supported_fields() rc=%d\n", rc);
	} else {
		printk("alarm supported-field mask: 0x%04x (MINUTE=0x%04x HOUR=0x%04x "
		       "MONTHDAY=0x%04x; note: NO seconds alarm on this part)\n",
		       supported,
		       (unsigned int)RTC_ALARM_TIME_MASK_MINUTE,
		       (unsigned int)RTC_ALARM_TIME_MASK_HOUR,
		       (unsigned int)RTC_ALARM_TIME_MASK_MONTHDAY);
	}

	/* Clear any alarm flag left over from a PREVIOUS run BEFORE installing the
	 * callback.  rtc_alarm_is_pending() both reports and clears the RV-3028
	 * STATUS.AF bit, and rtc_alarm_set_callback() submits the driver's work
	 * item immediately -- so a stale AF would otherwise fire alarm_cb() the
	 * instant we arm, and this app would "pass" without the hardware ever
	 * having raised an interrupt. */
	rc = rtc_alarm_is_pending(rtc, 0U);
	if (rc == 1) {
		printk("cleared a STALE alarm flag left set by a previous run\n");
	} else if (rc < 0) {
		printk("rtc_alarm_is_pending() (pre-arm clear) rc=%d\n", rc);
	}

	/* Alarm at the top of the NEXT minute.  We set the clock to second 40
	 * above, so this lands ~20 s out (minus the ADVANCE_GAP_MS already spent). */
	alarm_t.tm_min = (SET_MINUTE + 1) % 60;
	rc             = rtc_alarm_set_time(rtc, 0U, RTC_ALARM_TIME_MASK_MINUTE, &alarm_t);
	if (rc != 0) {
		printk("RESULT FAIL: rtc_alarm_set_time() rc=%d (clock itself is fine -- "
		       "set/get/advance all passed)\n",
		       rc);
		return 0;
	}
	printk("alarm armed for minute %02d (mask=MINUTE only); now %02d:%02d:%02d\n",
	       alarm_t.tm_min,
	       rd2.tm_hour,
	       rd2.tm_min,
	       rd2.tm_sec);

	rc = rtc_alarm_set_callback(rtc, 0U, alarm_cb, NULL);
	if (rc != 0) {
		printk("RESULT FAIL: rtc_alarm_set_callback() rc=%d -- with rc=-ENOTSUP, "
		       "the DT node is missing its int-gpios property\n",
		       rc);
		return 0;
	}

	printk("waiting up to %d s for the alarm interrupt (/INT -> P15_0 -> lpgpio "
	       "bit 0 -> IRQ 171) ...\n",
	       ALARM_TIMEOUT_S);
	alarm_ok = (k_sem_take(&alarm_sem, K_SECONDS(ALARM_TIMEOUT_S)) == 0);

	/* ------------------------------------------------------------------
	 * 4. Disarm and clear, so a RE-RUN behaves identically to this one.
	 *
	 * This block is not housekeeping politeness -- it is load-bearing.  The
	 * RV-3028's /INT is open-drain and stays pulled LOW for as long as
	 * STATUS.AF is set, and the upstream driver configures the GPIO as
	 * GPIO_INT_EDGE_TO_ACTIVE.  Leave AF set and the line is STUCK ASSERTED:
	 * the next run's alarm produces no falling edge, so no interrupt, so no
	 * callback -- and the failure looks like a broken alarm line rather than
	 * an uncleared flag from the run before.
	 *
	 * Order matters:
	 *   set_callback(NULL) first  -- clears CONTROL2.AIE, so nothing new is
	 *                                raised while we tear down.
	 *   set_time(mask 0)          -- writes the alarm-enable bits in all three
	 *                                alarm registers, disabling the match.
	 *   is_pending()              -- reads AND clears STATUS.AF, releasing /INT
	 *                                back high.
	 * ------------------------------------------------------------------ */
	rc = rtc_alarm_set_callback(rtc, 0U, NULL, NULL);
	if (rc != 0) {
		printk("teardown: rtc_alarm_set_callback(NULL) rc=%d\n", rc);
	}
	rc = rtc_alarm_set_time(rtc, 0U, 0U, &alarm_t); /* mask 0 == disable. */
	if (rc != 0) {
		printk("teardown: rtc_alarm_set_time(mask=0) rc=%d\n", rc);
	}
	rc = rtc_alarm_is_pending(rtc, 0U);
	if (rc < 0) {
		printk("teardown: rtc_alarm_is_pending() rc=%d -- alarm flag may still be "
		       "SET; the next run will see /INT stuck asserted\n",
		       rc);
	} else {
		printk("alarm disarmed, flag %s\n", rc == 1 ? "cleared" : "already clear");
	}

	/* ------------------------------------------------------------------
	 * 5. One machine-greppable verdict.
	 *
	 * PARTIAL exists specifically to separate "the RTC is broken" from "the
	 * RTC is fine but the interrupt path is not" -- they are different repairs
	 * on different hardware, and collapsing them into FAIL sends a bench
	 * operator after the wrong part.
	 * ------------------------------------------------------------------ */
	if (time_ok && advance_ok && alarm_ok) {
		printk("RESULT PASS: RV-3028-C7 set/get round-tripped, the oscillator "
		       "advanced the clock over %u ms, and the alarm interrupt arrived on "
		       "P15_0 (lpgpio bit 0, IRQ 171) within %d s\n",
		       ADVANCE_GAP_MS,
		       ALARM_TIMEOUT_S);
	} else if (time_ok && advance_ok && !alarm_ok) {
		printk("RESULT PARTIAL: the RTC ITSELF IS HEALTHY -- set/get round-tripped "
		       "and the clock advanced %02d:%02d:%02d -> %02d:%02d:%02d -- but no "
		       "alarm callback arrived within %d s. This is an INTERRUPT-PATH "
		       "fault, not an RTC fault. Check, in order: (1) the AE822 DFP warns "
		       "\"Note that LPGPIO_CTRL_n register has a different layout!\" for "
		       "port 15, which is the most likely cause -- IRQ 171 may not be "
		       "reaching the core even though the pad is correct; (2) STATUS.AF "
		       "left set by a previous run would hold /INT low with no falling "
		       "edge for GPIO_INT_EDGE_TO_ACTIVE; (3) R98 (100k to +1V8) fitted "
		       "on RTC_ALARM.\n",
		       rd1.tm_hour,
		       rd1.tm_min,
		       rd1.tm_sec,
		       rd2.tm_hour,
		       rd2.tm_min,
		       rd2.tm_sec,
		       ALARM_TIMEOUT_S);
	} else {
		printk("RESULT FAIL: the RTC answered on I2C but the clock did not advance "
		       "over %u ms (both reads %02d:%02d:%02d) -- the 32.768 kHz "
		       "oscillator is not running. Suspect the crystal/part, NOT the bus: "
		       "the bus is proven by the successful set/get above.\n",
		       ADVANCE_GAP_MS,
		       rd2.tm_hour,
		       rd2.tm_min,
		       rd2.tm_sec);
	}

	return 0;
}
