/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-sensor-int-probe -- exercise the ICM-42670 / BMP581 / TCAL9538
 * interrupt-routing chain end to end, for the E1M-AEN801 (Alif Ensemble
 * E8), bench RAM-run via J-Link.
 *
 * What it proves
 * ----------------
 * Every interrupt API added this session (icm42670_configure_int_pin/
 * route_int, bmp581_configure_int_pin/set_int_sources, and the TCAL9538
 * Agile-IO block: set_input_latch/set_interrupt_mask/get_interrupt_status)
 * is compiled but never called. This app calls them, on the real carrier
 * bus, and follows one physical interrupt from a sensor's INT pin through
 * the expander all the way to a register a host can poll.
 *
 * Routing (E1M-EVK carrier netlist, the same carrier this SoM sits on --
 * see EVK_I2C_ADDR_ICM42670 / _BMP581 / _TCAL9538_MAIN in
 * <alp/boards/alp_e1m_evk_routes.h>, whose evidence comments cite this
 * exact E1M-AEN801/803 hardware):
 *
 *     ICM-42670 INT1   -> TCAL9538 U35 P4  (EVK_IOEXP_ICM42670_INT1)
 *     ICM-42670 INT2   -> TCAL9538 U35 P5  (EVK_IOEXP_ICM42670_INT2)
 *     ICM-42670 FSYNC  -> TCAL9538 U35 P6  (EVK_IOEXP_ICM42670_FSYNC)
 *     BMP581 INT       -> TCAL9538 U35 P7  (EVK_IOEXP_BMP581_INT1)
 *
 * This app drives exactly two of those four: ICM-42670 INT1 (data-ready,
 * routed here) and BMP581 INT (data-ready). FSYNC (P6) is a SENSOR INPUT
 * (frame-sync from an external source), not an interrupt output -- nothing
 * in firmware can assert it, and this app does not try. ICM-42670 INT2
 * (P5) is left unrouted (no source assigned to it) -- expected to stay
 * quiet. Both are still unmasked + polled below so an unexpected assertion
 * on either is visible, not silently dropped.
 *
 * OUT OF SCOPE: BMI323 INT1 goes to a DIRECT SoC pad (E1M IO15,
 * EVK_PIN_BMI323_INT1 in <alp/boards/alp_e1m_evk_routes.h>), not this
 * expander -- it is not exercised here.
 *
 * Sequence
 * --------
 *   1. Bind the expander (TCAL9538 @0x73), set P4..P7 to input, arm the
 *      Agile-IO input latch on P4..P7 (0x42) so a brief pulse survives
 *      until read, and unmask P4..P7 in the interrupt mask (0x45) -- ALL
 *      BITS ARE MASKED AT POWER-UP (SCPS280B p.26 Table 7-12), so a caller
 *      that forgets this step will poll forever and see nothing, even with
 *      both sensors correctly wired and sampling.
 *   2. Bind the ICM-42670, configure INT1's electrical behaviour
 *      (push-pull drive + active-high polarity -- the combination that
 *      actively drives BOTH logic levels into a digital expander input, so
 *      no external pull is needed regardless of the expander's own
 *      internal-pull state; latched signalling so the pin stays asserted
 *      until the sensor's own DATA_RDY_INT is read, belt-and-suspenders
 *      alongside the expander's own input latch), route DATA_RDY to INT1,
 *      and start the accelerometer at 100 Hz (icm42670_init() already
 *      leaves PWR_MGMT0 in Low-Noise mode for both engines, so once an ODR
 *      is set the engine free-runs and DATA_RDY fires every ODR period
 *      with no further action).
 *   3. Bind the BMP581, configure its INT pin the same way (push-pull /
 *      active-high / latched), enable the DRDY source, and start it in
 *      NORMAL (continuous) mode at 50 Hz.
 *   4. Poll tcal9538_get_interrupt_status() (0x46) for a window comfortably
 *      longer than either sensor's period. Register 0x46 is NOT
 *      clear-on-read (SCPS280B p.26 Table 7-13) -- only a read of the
 *      input port register 0x00 clears the latched condition, so every
 *      poll that finds a set bit immediately follows up with
 *      tcal9538_read_all() (which reads 0x00) to acknowledge, then rechecks
 *      0x46 to confirm the bit actually dropped.
 *   5. Also read the driver-level icm42670_data_ready() /
 *      bmp581_data_ready() predicates once at the end, so the run reports
 *      whether the register-level "ready" flag on each sensor and the
 *      physical INT line the expander saw actually agree.
 *
 * Verdict
 * -------
 * PASS = at least one of the two EXPECTED sources (ICM-42670 INT1 -> P4,
 * BMP581 INT -> P7) was seen asserted in 0x46 and then cleared by the
 * 0x00 read during the poll window. A negative result is a real result
 * here: if NEITHER expected source ever asserts, the run is a FAIL, and
 * prints which source(s) never fired plus every source's last-seen
 * register state (0x46, the input port, and each sensor's own data-ready
 * predicate) -- useful for telling "wiring dead" apart from "config
 * wrong" apart from "polled too briefly".
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf's comment) when
 * the bench forces it; Flow C's app UART emits nothing on this bench
 * (e1m-aen-evk-03). BENCH-VALIDATION app -- not a customer teaching
 * example.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h> /* BIT() */

#include "alp/peripheral.h"
#include "alp/boards/alp_e1m_evk.h"
#include "alp/chips/icm42670.h"
#include "alp/chips/bmp581.h"
#include "alp/chips/tcal9538.h"

/* Every expander pin this app touches -- the four sensor-interrupt inputs,
 * P4..P7 (evk_ioexp_pin_t in <alp/boards/alp_e1m_evk_routes.h>). Spelled as
 * an OR of the named pins rather than the bare literal 0xF0 so a reviewer
 * doesn't have to cross-reference the enum to see which four pins these
 * bits are. */
#define IOEXP_SENSOR_INT_MASK \
	(BIT(EVK_IOEXP_ICM42670_INT1) | BIT(EVK_IOEXP_ICM42670_INT2) | BIT(EVK_IOEXP_ICM42670_FSYNC) | \
	 BIT(EVK_IOEXP_BMP581_INT1))

/* Poll window: 15 passes x 25 ms = 375 ms. Comfortably longer than either
 * sensor's period (ICM-42670 accel at 100 Hz -> 10 ms; BMP581 at 50 Hz ->
 * 20 ms), so both get several chances to assert regardless of exact I2C
 * transaction timing. */
#define POLL_ITERATIONS 15u
#define POLL_PERIOD_MS  25u

/** Per-source assert/clear tracking across the poll window. */
typedef struct {
	const char *name;
	uint8_t     pin;              /**< evk_ioexp_pin_t value (0..7). */
	bool        seen;             /**< 0x46 showed this bit set at least once. */
	bool        cleared;          /**< the next 0x46 re-check after the 0x00 ack dropped it. */
	uint8_t     status_at_assert; /**< raw 0x46 the first time this bit appeared. */
} int_source_t;

int main(void)
{
	printk("\n=== AEN801 sensor interrupt chain "
	       "(ICM-42670 + BMP581 -> TCAL9538 -> poll) ===\n");

	(void)alp_init();

	/* Carrier bus (ALP_E1M_I2C0 / EVK_I2C_BUS_SENSORS / alp-i2c0). Board-
	 * layer enabled; see the file header + this app's overlay for why no
	 * bus wiring lives here. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS,
	    .bitrate_hz = 100000u,
	});
	if (bus == NULL) {
		printk("RESULT FAIL: alp_i2c_open(carrier bus) -> NULL, alp_last_error=%d\n",
		       (int)alp_last_error());
		return 0;
	}

	/* --- 1. TCAL9538 expander: bind + arm P4..P7 --------------------- */
	tcal9538_t io;
	if (tcal9538_init(&io, bus, EVK_I2C_ADDR_TCAL9538_MAIN) != ALP_OK) {
		printk("RESULT FAIL: tcal9538_init(@0x%02x) failed, alp_last_error=%d\n",
		       EVK_I2C_ADDR_TCAL9538_MAIN,
		       (int)alp_last_error());
		alp_i2c_close(bus);
		return 0;
	}
	printk("tcal9538_init(@0x%02x) ok\n", EVK_I2C_ADDR_TCAL9538_MAIN);

	/* P4..P7 as inputs (already the POR default -- config resets to 0xFF,
	 * all input -- set explicitly so this app doesn't depend on that
	 * default surviving whatever a prior app on this bench left behind). */
	alp_status_t dir_rc =
	    tcal9538_set_directions(&io, IOEXP_SENSOR_INT_MASK, IOEXP_SENSOR_INT_MASK);

	/* Input latch (0x42): bit=1 latches that pin's transition until the
	 * input port (0x00) is read -- without this, a pulsed (not latched)
	 * sensor INT that de-asserts between our polls would vanish
	 * unobserved (SCPS280B p.25-26, Table 7-9). Both sensors below are ALSO
	 * configured latched at the sensor side; this is the second layer, not
	 * a substitute. */
	alp_status_t latch_rc = tcal9538_set_input_latch(&io, IOEXP_SENSOR_INT_MASK);

	/* Interrupt mask (0x45): bit=1 MASKS (disables) that pin's
	 * contribution to \INT and to the status register -- POWER-UP DEFAULT
	 * IS 0xFF, EVERY PIN MASKED (SCPS280B p.26, Table 7-12). Unmask
	 * (write 0) exactly the four sensor pins; leave P0..P3 (LCD/camera/CTP
	 * control lines, not inputs on this app) masked. */
	alp_status_t mask_rc = tcal9538_set_interrupt_mask(&io, (uint8_t)~IOEXP_SENSOR_INT_MASK);

	printk("tcal9538 arm: set_directions rc=%d set_input_latch(0x42) rc=%d "
	       "set_interrupt_mask(0x45) rc=%d (mask=0x%02x)\n",
	       (int)dir_rc,
	       (int)latch_rc,
	       (int)mask_rc,
	       (uint8_t)~IOEXP_SENSOR_INT_MASK);
	if (dir_rc != ALP_OK || latch_rc != ALP_OK || mask_rc != ALP_OK) {
		printk("RESULT FAIL: expander arm sequence failed "
		       "(dir=%d latch=%d mask=%d)\n",
		       (int)dir_rc,
		       (int)latch_rc,
		       (int)mask_rc);
		alp_i2c_close(bus);
		return 0;
	}

	/* --- 2. ICM-42670: data-ready -> INT1 ---------------------------- */
	icm42670_t imu;
	if (icm42670_init(&imu, bus, EVK_I2C_ADDR_ICM42670) != ALP_OK) {
		printk("RESULT FAIL: icm42670_init(@0x%02x) failed, alp_last_error=%d\n",
		       EVK_I2C_ADDR_ICM42670,
		       (int)alp_last_error());
		alp_i2c_close(bus);
		return 0;
	}
	printk("icm42670_init(@0x%02x) ok\n", EVK_I2C_ADDR_ICM42670);

	/* Push-pull + active-high: the pin actively drives both the asserted
	 * AND the idle level into the expander input, so no external or
	 * expander-internal pull is required either way (TDK DS-000451
	 * Rev 1.0 p.50, INT_CONFIG). Latched so the pin holds its asserted
	 * level until DATA_RDY_INT (INT_STATUS_DRDY) is read on the sensor
	 * side -- the expander's own input latch (armed above) is the second,
	 * independent line of defence against a missed transition. */
	const icm42670_int_pin_config_t icm_int_cfg = {
		.mode     = ICM42670_INT_MODE_LATCHED,
		.drive    = ICM42670_INT_DRIVE_PUSH_PULL,
		.polarity = ICM42670_INT_POLARITY_ACTIVE_HIGH,
	};
	alp_status_t icm_cfg_rc = icm42670_configure_int_pin(&imu, ICM42670_INT_PIN1, &icm_int_cfg);
	alp_status_t icm_rt_rc = icm42670_route_int(&imu, ICM42670_INT_PIN1, ICM42670_INT_SRC_DATA_RDY);
	alp_status_t icm_acc_rc = icm42670_set_accel(&imu, ICM42670_ODR_100_HZ, ICM42670_ACCEL_FS_2G);
	printk("icm42670 int setup: configure_int_pin rc=%d route_int(DATA_RDY->INT1) rc=%d "
	       "set_accel(100Hz) rc=%d\n",
	       (int)icm_cfg_rc,
	       (int)icm_rt_rc,
	       (int)icm_acc_rc);

	/* --- 3. BMP581: data-ready -> INT ---------------------------------- */
	bmp581_t baro;
	if (bmp581_init(&baro, bus, EVK_I2C_ADDR_BMP581) != ALP_OK) {
		printk("RESULT FAIL: bmp581_init(@0x%02x) failed, alp_last_error=%d\n",
		       EVK_I2C_ADDR_BMP581,
		       (int)alp_last_error());
		alp_i2c_close(bus);
		return 0;
	}
	printk("bmp581_init(@0x%02x) ok\n", EVK_I2C_ADDR_BMP581);

	/* Same push-pull / active-high / latched reasoning as the ICM-42670
	 * above (BST-BMP581-DS004-13 Sec.7.5, pp.52-53). */
	alp_status_t bmp_cfg_rc = bmp581_configure_int_pin(
	    &baro, true, BMP581_INT_PUSH_PULL, BMP581_INT_ACTIVE_HIGH, BMP581_INT_LATCHED);
	alp_status_t bmp_src_rc = bmp581_set_int_sources(&baro, BMP581_INT_SRC_DRDY);
	/* NORMAL: continuous periodic conversion, so DRDY -- and therefore the
	 * expander bit -- reasserts every ODR period for the whole poll
	 * window, not just once. */
	alp_status_t bmp_smp_rc = bmp581_set_sampling(
	    &baro, BMP581_OSR_X1, BMP581_OSR_X1, BMP581_ODR_50_HZ, BMP581_MODE_NORMAL);
	printk("bmp581 int setup: configure_int_pin rc=%d set_int_sources(DRDY) rc=%d "
	       "set_sampling(NORMAL,50Hz) rc=%d\n",
	       (int)bmp_cfg_rc,
	       (int)bmp_src_rc,
	       (int)bmp_smp_rc);

	/* Startup margin before the first real sample is guaranteed in either
	 * sensor's pipeline (10 ms ICM-42670 accel startup + one 10 ms ODR
	 * period; BMP581's first ~2 ms conversion easily lands inside the
	 * same window) -- avoids catching either engine mid-startup on the
	 * very first poll pass. */
	k_msleep(20);

	/* --- 4. Poll 0x46, ack via 0x00, track assert+clear per source --- */
	int_source_t sources[] = {
		{ "ICM42670 INT1 (P4, DATA_RDY)", EVK_IOEXP_ICM42670_INT1, false, false, 0 },
		{ "ICM42670 INT2 (P5, unrouted)", EVK_IOEXP_ICM42670_INT2, false, false, 0 },
		{ "ICM42670 FSYNC (P6, sensor input, not driven here)",
		  EVK_IOEXP_ICM42670_FSYNC,
		  false,
		  false,
		  0 },
		{ "BMP581 INT (P7, DRDY)", EVK_IOEXP_BMP581_INT1, false, false, 0 },
	};
	const size_t n_sources = ARRAY_SIZE(sources);

	for (unsigned pass = 0; pass < POLL_ITERATIONS; pass++) {
		uint8_t      status = 0;
		alp_status_t st_rc  = tcal9538_get_interrupt_status(&io, &status);
		if (st_rc != ALP_OK) {
			printk("pass %2u: tcal9538_get_interrupt_status rc=%d\n", pass, (int)st_rc);
			k_msleep(POLL_PERIOD_MS);
			continue;
		}
		if (status == 0u) {
			k_msleep(POLL_PERIOD_MS);
			continue;
		}

		printk("pass %2u: 0x46=0x%02x\n", pass, status);
		for (size_t i = 0; i < n_sources; i++) {
			if (!sources[i].seen && (status & BIT(sources[i].pin))) {
				sources[i].seen             = true;
				sources[i].status_at_assert = status;
			}
		}

		/* Acknowledge: 0x46 is NOT clear-on-read (SCPS280B p.26, Table
		 * 7-13) -- only a read of the input port register (0x00) clears
		 * the latched condition. tcal9538_read_all() is that read. */
		uint8_t      in0   = 0;
		alp_status_t in_rc = tcal9538_read_all(&io, &in0);

		uint8_t      status_after_ack = 0;
		alp_status_t st2_rc           = tcal9538_get_interrupt_status(&io, &status_after_ack);
		printk("         ack: read_all(0x00) rc=%d in0=0x%02x  re-check 0x46=0x%02x rc=%d\n",
		       (int)in_rc,
		       in0,
		       status_after_ack,
		       (int)st2_rc);
		if (st2_rc == ALP_OK) {
			for (size_t i = 0; i < n_sources; i++) {
				if (sources[i].seen && !sources[i].cleared &&
				    (status_after_ack & BIT(sources[i].pin)) == 0) {
					sources[i].cleared = true;
				}
			}
		}

		k_msleep(POLL_PERIOD_MS);
	}

	/* --- 5. Driver-level ready predicates, for agreement with the wire - */
	bool         icm_ready = false, bmp_ready = false;
	alp_status_t icm_rdy_rc = icm42670_data_ready(&imu, &icm_ready);
	alp_status_t bmp_rdy_rc = bmp581_data_ready(&baro, &bmp_ready);
	printk("driver-level: icm42670_data_ready() rc=%d ready=%s | "
	       "bmp581_data_ready() rc=%d ready=%s\n",
	       (int)icm_rdy_rc,
	       icm_ready ? "true" : "false",
	       (int)bmp_rdy_rc,
	       bmp_ready ? "true" : "false");

	alp_i2c_close(bus);

	/* Full per-source dump -- printed regardless of verdict, so a FAIL
	 * tells you which register state to go read on the bench. */
	for (size_t i = 0; i < n_sources; i++) {
		printk("  %-52s seen=%s cleared=%s last_0x46=0x%02x\n",
		       sources[i].name,
		       sources[i].seen ? "yes" : "no",
		       sources[i].cleared ? "yes" : "no",
		       sources[i].status_at_assert);
	}

	/*
	 * Verdict: the two EXPECTED sources are ICM-42670 INT1 (P4) and
	 * BMP581 INT (P7) -- the only two pins this app actually routes a
	 * data-ready source onto. PASS = at least one of them was seen
	 * asserted in 0x46 and then cleared by the 0x00 ack during the poll
	 * window. INT2 (P5) and FSYNC (P6) are diagnostic-only: neither is
	 * expected to fire, and an unexpected assertion on either is reported
	 * above but does not affect the verdict.
	 */
	const int_source_t *icm_int1 = &sources[0];
	const int_source_t *bmp_int  = &sources[3];
	bool                icm_ok   = icm_int1->seen && icm_int1->cleared;
	bool                bmp_ok   = bmp_int->seen && bmp_int->cleared;

	if (icm_ok || bmp_ok) {
		printk("RESULT PASS: %s%s%s asserted and cleared via the expander input-port read "
		       "during the %u ms poll window\n",
		       icm_ok ? "ICM42670 INT1 (P4)" : "",
		       (icm_ok && bmp_ok) ? " and " : "",
		       bmp_ok ? "BMP581 INT (P7)" : "",
		       POLL_ITERATIONS * POLL_PERIOD_MS);
	} else {
		printk("RESULT FAIL: neither ICM42670 INT1 (P4) nor BMP581 INT (P7) asserted+cleared "
		       "during the %u ms poll window (icm seen=%s cleared=%s last_0x46=0x%02x; "
		       "bmp seen=%s cleared=%s last_0x46=0x%02x) -- check the expander mask (0x45) "
		       "unmasked P4/P7, the input latch (0x42) armed them, and each sensor's own INT "
		       "routing + power mode\n",
		       POLL_ITERATIONS * POLL_PERIOD_MS,
		       icm_int1->seen ? "yes" : "no",
		       icm_int1->cleared ? "yes" : "no",
		       icm_int1->status_at_assert,
		       bmp_int->seen ? "yes" : "no",
		       bmp_int->cleared ? "yes" : "no",
		       bmp_int->status_at_assert);
	}

	return 0;
}
