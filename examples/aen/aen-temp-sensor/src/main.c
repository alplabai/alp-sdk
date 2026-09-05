/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-temp-sensor -- read the E1M-AEN801's on-module TMP112 temperature
 * sensor through the UPSTREAM Zephyr sensor API.
 *
 * Layering (ADR 0017, as amended 2026-08-30 -- alp-sdk rides OVER the vendor
 * SDK and never re-implements a driver upstream already ships):
 *
 *   devicetree  the BOARD layer declares the bus (BRD_I2C == SoC I2C0,
 *               "snps,designware-i2c") and the "ti,tmp112" child node on it.
 *               Nothing in this example's own overlay touches either -- see
 *               boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay.
 *   driver      upstream drivers/sensor/ti/tmp112/ binds that node.
 *               CONFIG_TMP112 self-selects `default y` off the enabled node.
 *   app         this file: sensor_sample_fetch() + sensor_channel_get() on
 *               SENSOR_CHAN_AMBIENT_TEMP.  No I2C register pokes, no chip
 *               driver, no vendor header.
 *
 * The whole app is bus-agnostic on purpose: swap the DT node onto another
 * I2C controller and not a line here changes.
 *
 * What it does, in order
 * ----------------------
 *   1. Resolve the DT-bound TMP112 and check it initialised.  A TMP112 that
 *      does not ACK fails here (upstream tmp112_init() writes CONFIG, T_LOW
 *      and T_HIGH), which is where the address diagnostic below fires.
 *   2. Take TMP112_SAMPLES readings TMP112_PERIOD_MS apart, so a reader sees
 *      the poll shape rather than a single lucky fetch.
 *   3. Sanity-BAND each reading against an indoor range.  This is a
 *      PLAUSIBILITY check, not a correctness claim -- see the band comment.
 *   4. Print one machine-greppable RESULT PASS / PARTIAL / FAIL line.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

/*
 * Bind by COMPATIBLE, not by node label.  The board layer owns the node and
 * may name its label whatever it likes; "ti,tmp112" is the stable contract
 * between the binding, the driver and this app.  When no such node is enabled
 * in the build (e.g. a native_sim compile check), the whole block collapses to
 * a NULL device and main() reports that as a build-configuration fault rather
 * than failing to compile.
 */
#if DT_HAS_COMPAT_STATUS_OKAY(ti_tmp112)
#define TMP112_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(ti_tmp112)
/* The devicetree's OWN address, printed rather than assumed: if a board file
 * ever bakes in something other than the design 0x48, the diagnostic below
 * says so out loud instead of quietly contradicting this file's comments. */
#define TMP112_DT_ADDR ((unsigned int)DT_REG_ADDR(TMP112_NODE))
static const struct device *const tmp112 = DEVICE_DT_GET(TMP112_NODE);
#else
#define TMP112_DT_ADDR 0u
static const struct device *const tmp112 = NULL;
#endif

/* Design address, from the E1M-AEN-2626-R2 netlist: U20 = TMP112DIDPWR with
 * ADD0 tied to 0V.  TI SBOS397 maps ADD0 -> GND = 0x48 (-> V+ = 0x49,
 * -> SDA = 0x4A, -> SCL = 0x4B).  Used for prose only; the real address comes
 * from the devicetree above. */
#define TMP112_DESIGN_ADDR 0x48u

/* The address ONE bench module was observed answering on instead.  Never
 * probed by this example -- see print_address_anomaly_hint(). */
#define TMP112_OBSERVED_ANOMALY_ADDR 0x40u

/* Poll shape: 8 samples 500 ms apart is ~4 s of console output -- long enough
 * to show a customer the loop and to catch an intermittent bus, short enough
 * that a bench operator does not sit waiting.  The TMP112's default
 * conversion-rate is 4 Hz (250 ms per conversion, ti,tmp112.yaml), so every
 * 500 ms fetch returns a genuinely fresh conversion rather than re-reading a
 * stale register. */
#define TMP112_SAMPLES   8
#define TMP112_PERIOD_MS 500

/*
 * PLAUSIBILITY band, in milli-degrees C.  A powered lab bench sits somewhere
 * around 15..35 degC, and the module self-heats a little on top of ambient.
 *
 * READ THIS BEFORE TRUSTING IT: a reading inside the band is NOT proof the
 * sensor is accurate, and a reading outside it is NOT proof the sensor is
 * broken.  The band only separates "this looks like a temperature" from
 * "this looks like a bus fault or a mis-decoded register" -- the failure this
 * check exists to catch is a plausible-looking number coming from the wrong
 * place.  A cold-chamber, outdoor or oven test legitimately lands outside it;
 * widen or delete the band for those, do not treat it as a spec limit.  The
 * TMP112's own operating range is -40..125 degC.
 */
#define TMP112_PLAUSIBLE_LO_MILLI_C 15000
#define TMP112_PLAUSIBLE_HI_MILLI_C 35000

/*
 * The actionable diagnostic for "the sensor is in the devicetree but does not
 * answer".  Printed instead of silently probing somewhere else: a per-unit
 * board defect has to be SEEN and fixed, and firmware that quietly works
 * around it ships the defect to every customer.
 */
static void print_address_anomaly_hint(void)
{
	printk("\n"
	       "  The TMP112 did not respond at its devicetree address 0x%02x.\n"
	       "\n"
	       "  0x%02x is the DESIGN address and is correct: the E1M-AEN-2626-R2\n"
	       "  netlist ties U20 pin 3 (ADD0) to 0V, and the TMP112 strap table\n"
	       "  (TI SBOS397) maps ADD0 -> GND = 0x48, -> V+ = 0x49, -> SDA = 0x4A,\n"
	       "  -> SCL = 0x4B.\n"
	       "\n"
	       "  KNOWN OBSERVED ANOMALY -- one bench module, 2026-09-05: on that\n"
	       "  unit the TMP112 answered at 0x%02x instead of 0x%02x.  The part\n"
	       "  itself was fine: it was confirmed a genuine TMP112 by a\n"
	       "  three-of-three register fingerprint against the datasheet\n"
	       "  power-on defaults -- CONFIG=0x60a0, T_LOW=0x4b00, T_HIGH=0x5000 --\n"
	       "  and it read back 28.062 degC.  Only its address was wrong.\n"
	       "\n"
	       "  0x%02x is NOT a legal TMP112 address, so on that module ADD0 is\n"
	       "  not actually sitting at GND.  Suspected cause: an OPEN JOINT on\n"
	       "  U20 pin 3 (ADD0), leaving the strap floating.  That is a per-unit\n"
	       "  board defect, NOT a design error and NOT a firmware bug.\n"
	       "\n"
	       "  WHAT TO DO\n"
	       "    1. Check continuity from U20 pin 3 (ADD0) to GND on this module.\n"
	       "    2. Confirm what is actually on the bus with\n"
	       "       examples/aen/aen-brd-i2c-scan -- it scans every 7-bit address\n"
	       "       and fingerprints whatever answers.\n"
	       "    3. Only if the joint is genuinely open: rework it.  Do NOT change\n"
	       "       the devicetree to 0x%02x -- that would bake one unit's defect\n"
	       "       into the product and break every correctly-built module.\n",
	       TMP112_DT_ADDR,
	       TMP112_DESIGN_ADDR,
	       TMP112_OBSERVED_ANOMALY_ADDR,
	       TMP112_DESIGN_ADDR,
	       TMP112_OBSERVED_ANOMALY_ADDR,
	       TMP112_OBSERVED_ANOMALY_ADDR);
}

/*
 * Fetch one ambient-temperature reading as integer milli-degrees C.
 *
 * NO FLOAT PRINTF, deliberately -- the same rule the sibling AEN examples
 * follow.  A hard-float printf on the M55 costs code size and, with the
 * minimal libc some bench configs use, silently prints nothing at all for
 * %f, which reads on the console as a dead sensor.  Zephyr's sensor_value is
 * already an exact integer pair (val1 whole units + val2 micro-units), and
 * sensor_value_to_milli() collapses it without ever touching an FPU.
 *
 * Returns 0 and fills *milli_c on success, or the negative errno from the
 * sensor API.
 */
static int read_ambient_milli_c(int32_t *milli_c)
{
	struct sensor_value val;
	int                 rc;

	/* fetch = trigger a bus transaction and latch the result in the driver;
	 * channel_get = convert that latched sample.  They are separate calls so
	 * one fetch can feed several channels -- the TMP112 only has the one. */
	rc = sensor_sample_fetch_chan(tmp112, SENSOR_CHAN_AMBIENT_TEMP);
	if (rc != 0) {
		return rc;
	}

	rc = sensor_channel_get(tmp112, SENSOR_CHAN_AMBIENT_TEMP, &val);
	if (rc != 0) {
		return rc;
	}

	*milli_c = (int32_t)sensor_value_to_milli(&val);
	return 0;
}

int main(void)
{
	unsigned int n_ok      = 0U; /* fetches that returned a value.       */
	unsigned int n_in_band = 0U; /* of those, how many looked plausible. */

	printk("\n=== aen-temp-sensor: on-module TMP112 via the Zephyr sensor API ===\n");

	/* 1a. Is the sensor in this build's devicetree at all?  If not, the fault
	 * is the board/overlay layer, not the wiring -- say exactly that instead
	 * of emitting the address diagnostic, which would send a reader chasing a
	 * soldering iron over a missing DT node. */
	if (tmp112 == NULL) {
		printk("no enabled \"ti,tmp112\" node in this build's devicetree.\n"
		       "  The E1M-AEN801 board files are what declare the on-module\n"
		       "  TMP112 on BRD_I2C; this example's own overlay deliberately\n"
		       "  adds nothing.  If you are building for a board that has no\n"
		       "  such node (native_sim, say), this is expected and the app\n"
		       "  has nothing to measure.\n");
		printk("RESULT FAIL: no ti,tmp112 node bound -- nothing to read\n");
		return 0;
	}

	/* 1b. The node exists, so the driver ran tmp112_init() -- which WRITES the
	 * CONFIG, T_LOW and T_HIGH registers.  A part that does not ACK therefore
	 * fails init and leaves the device un-ready.  This is the branch the
	 * observed-0x40 module lands in. */
	if (!device_is_ready(tmp112)) {
		printk("TMP112 \"%s\" @0x%02x failed to initialise "
		       "(upstream tmp112_init() writes CONFIG/T_LOW/T_HIGH; no ACK).\n",
		       tmp112->name,
		       TMP112_DT_ADDR);
		print_address_anomaly_hint();
		printk("RESULT FAIL: TMP112 @0x%02x not ready -- see the address "
		       "diagnostic above\n",
		       TMP112_DT_ADDR);
		return 0;
	}

	printk("TMP112 \"%s\" ready at devicetree address 0x%02x (design address, "
	       "U20 ADD0 tied to 0V)\n",
	       tmp112->name,
	       TMP112_DT_ADDR);
	printk("taking %d samples %d ms apart ...\n", TMP112_SAMPLES, TMP112_PERIOD_MS);

	/* 2 + 3. Poll, print, band-check.  Every sample is reported individually
	 * so an intermittent bus shows up as a gap in the run rather than being
	 * averaged away into a single summary number. */
	for (int i = 0; i < TMP112_SAMPLES; i++) {
		int32_t milli_c = 0;
		int     rc      = read_ambient_milli_c(&milli_c);

		if (rc != 0) {
			/* rc is the raw errno from the sensor/I2C stack: -5 (-EIO) is a
			 * NACK -- the bus works, the part did not answer; -116
			 * (-ETIMEDOUT) is the line never being released, an electrical
			 * signature.  Printing the value keeps those distinguishable. */
			printk("  sample %d/%d: fetch failed, rc=%d\n", i + 1, TMP112_SAMPLES, rc);
		} else {
			bool plausible =
			    (milli_c >= TMP112_PLAUSIBLE_LO_MILLI_C && milli_c <= TMP112_PLAUSIBLE_HI_MILLI_C);

			n_ok++;
			n_in_band += plausible ? 1U : 0U;

			/* Integer milli-degrees C, printed as-is.  Divide by 1000 for
			 * whole degrees if you want them -- but keep the raw integer in
			 * any log you intend to parse later. */
			printk("  sample %d/%d: %d milli-degC%s\n",
			       i + 1,
			       TMP112_SAMPLES,
			       milli_c,
			       plausible ? ""
			                 : "  <-- outside the plausible indoor band "
			                   "(15000..35000); PLAUSIBILITY only, not a "
			                   "correctness verdict");
		}

		/* Not slept after the last sample: nothing follows it. */
		if (i + 1 < TMP112_SAMPLES) {
			k_msleep(TMP112_PERIOD_MS);
		}
	}

	/* 4. One machine-greppable verdict line.
	 *
	 * PASS requires every sample to have been read AND to have looked like a
	 * temperature.  A run where the reads all worked but the values sit
	 * outside the band is PARTIAL, not FAIL: the firmware and the bus are
	 * demonstrably fine, and the band is a plausibility heuristic that a
	 * legitimate cold/hot environment breaks. */
	if (n_ok == (unsigned int)TMP112_SAMPLES && n_in_band == n_ok) {
		printk("RESULT PASS: %u/%u TMP112 samples read at 0x%02x, all within the "
		       "plausible indoor band (plausibility check, not an accuracy claim)\n",
		       n_ok,
		       (unsigned int)TMP112_SAMPLES,
		       TMP112_DT_ADDR);
	} else if (n_ok > 0U) {
		printk("RESULT PARTIAL: %u/%u TMP112 samples read at 0x%02x, %u of those "
		       "within the plausible indoor band -- the sensor answers, so check "
		       "the per-sample lines above for dropped fetches or an environment "
		       "genuinely outside 15..35 degC\n",
		       n_ok,
		       (unsigned int)TMP112_SAMPLES,
		       TMP112_DT_ADDR,
		       n_in_band);
	} else {
		printk("TMP112 @0x%02x initialised but every fetch failed.\n", TMP112_DT_ADDR);
		print_address_anomaly_hint();
		printk("RESULT FAIL: 0/%u TMP112 samples read at 0x%02x -- see the address "
		       "diagnostic above\n",
		       (unsigned int)TMP112_SAMPLES,
		       TMP112_DT_ADDR);
	}

	return 0;
}
