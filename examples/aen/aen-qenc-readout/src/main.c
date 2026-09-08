/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-qenc-readout -- read a quadrature encoder on the Ensemble E8 UTIMER via the
 * vendored alif,utimer-qdec sensor driver, on the E1M-AEN801 (M55-HE).  Drives the
 * standard Zephyr sensor API (sensor_sample_fetch / sensor_channel_get) on
 * DT_ALIAS(alp_qenc0) and reports SENSOR_CHAN_ROTATION.  The driver scales the
 * raw UTIMER counter to DEGREES (val1 = counter * 360 / counts-per-revolution),
 * so the value ranges 0..359, not raw counts -- the sensor API has no separate
 * channel or attribute for the raw counter, so degrees is the only number this
 * app (or any app using this driver) can ever read.
 *
 * The UTIMER runs in quadrature-decoder mode: two phase inputs (X = channel A,
 * Y = channel B) advance/retreat the counter on every edge of both phases (x4
 * decode -- see hal_alif's alif_utimer_config_qdec_triggers(), which arms both
 * rising and falling edges of A and B).  For the carrier's 24-PPR encoder that
 * is 24*4 = 96 counts per mechanical revolution, which is what
 * counts-per-revolution must equal for degrees to mean what they say: the
 * hardware reload wraps the counter at (counts-per-revolution - 1), so a wrong
 * (too large) value doesn't just misreport a bit -- it stretches one real
 * revolution across only a fraction of the 0-359 range, AND makes a single
 * genuine encoder tick integer-truncate to 0 degrees ("moved a little" reads
 * identical to "never moved").  See the board overlay for the corrected value.
 *
 * PASS / SKIPPED / FAIL, not PASS / PARTIAL: a bench run with nobody at the
 * knob produces N clean reads that never change -- that is the CORRECT output
 * of a working decoder sitting idle, and it is byte-for-byte what a decoder
 * that never counts also prints.  The old binary verdict called both of those
 * "PARTIAL", which taught nothing.  This app tells them apart the only way it
 * honestly can without a human: whether every sample_fetch/channel_get in the
 * window returned 0.
 *   - count changed                        -> PASS     (decode is live)
 *   - never changed, every read was clean  -> SKIPPED  (reads work; the count
 *                                              did not move -- turn the shaft
 *                                              and rerun)
 *   - never changed, some read errored     -> FAIL     (the driver itself is
 *                                              failing reads; a real defect)
 *
 * What a SKIPPED verdict does NOT say: that the decoder is armed and running.
 * The sensor API this driver registers is {sample_fetch, channel_get} only --
 * no attr_get, no raw-counter channel -- so there is no way from here to see
 * the counter's run state (UTIMERn_CNTR_CTRL bit 1 RUNNING).  Clean reads of a
 * stopped counter look exactly like clean reads of an idle running one: that
 * is precisely how #2037 (the counter was configured but never started)
 * survived a bench run whose SKIPPED line claimed "decoder armed as
 * configured".  Confirming the run state needs a debugger read of CNTR_CTRL /
 * GLB_CNTR_RUNNING, not another print from this app.
 */

#include <stdio.h>

#include <alp/peripheral.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#define QENC_NODE DT_ALIAS(alp_qenc0)
#define SAMPLES   30
#define POLL_MS   300 /* 30 * 300ms = 9s: comfortably long enough to grab the knob */

int main(void)
{
	const struct device *qenc = DEVICE_DT_GET(QENC_NODE);

	printf("[qenc] open %s (UTIMER quadrature decoder)\n", qenc->name);
	if (!device_is_ready(qenc)) {
		printf("[qenc] RESULT FAIL: device not ready\n[qenc] done\n");
		return 0;
	}

	/* This is the whole state dump available: the sensor_driver_api this
	 * driver registers is {sample_fetch, channel_get} only -- no attr_get, no
	 * second channel for the raw counter -- so there is no live register we
	 * can honestly read back. What we CAN echo, for free and without poking a
	 * single address, is the build-time devicetree config the driver was
	 * handed, straight off the qdec node. If a bench run comes back SKIPPED
	 * or FAIL, this line plus counts-per-revolution's value are the first
	 * thing to check -- no second reservation needed to see them.
	 */
	printf("[qenc] config: counts-per-revolution=%u filter=%s prescaler=%u taps=%u\n",
	       (unsigned)DT_PROP(QENC_NODE, counts_per_revolution),
	       DT_PROP(QENC_NODE, input_filter_enable) ? "on" : "off",
	       (unsigned)DT_PROP(QENC_NODE, filter_prescaler),
	       (unsigned)DT_PROP(QENC_NODE, filter_taps));

	printf("[qenc] >>> turn the encoder shaft now, about one full revolution <<<\n");

	int     ok_reads = 0;
	bool    moved    = false;
	int32_t first    = 0;
	for (int i = 0; i < SAMPLES; i++) {
		int rc = sensor_sample_fetch(qenc);
		if (rc != 0) {
			printf("[qenc] sample_fetch[%d] -> %d\n", i, rc);
			alp_delay_ms(POLL_MS);
			continue;
		}
		struct sensor_value v = { 0 };
		rc                    = sensor_channel_get(qenc, SENSOR_CHAN_ROTATION, &v);
		if (rc != 0) {
			printf("[qenc] channel_get[%d] -> %d\n", i, rc);
			alp_delay_ms(POLL_MS);
			continue;
		}
		ok_reads++;
		if (ok_reads == 1) {
			first = v.val1;
		} else if (v.val1 != first) {
			moved = true;
		}
		printf("[qenc] angle[%d] = %d deg (0-359)\n", i, v.val1);
		alp_delay_ms(POLL_MS);
	}

	/* all_clean: every poll in the window returned a clean 0 from BOTH driver
	 * calls. That is a statement about the READS, and nothing more -- it says
	 * the driver answered every call without error. It is NOT evidence that the
	 * counter is running (see the file header: nothing in this API can see
	 * that), so do not name it "armed" and do not print it as such.
	 */
	bool        all_clean = (ok_reads == SAMPLES);
	const char *result;
	const char *reason;

	if (moved) {
		result = "PASS";
		reason = "angle changed -> live quadrature decode";
	} else if (all_clean) {
		result = "SKIPPED";
		reason = "no motion detected -- every read in the window succeeded but the reported "
		         "count never changed; this app cannot see whether the counter is running, "
		         "so this is neither proof of a working decoder nor of a broken one: turn "
		         "the shaft and rerun, and if it still does not move, check CNTR_CTRL bit 1 "
		         "RUNNING over SWD";
	} else {
		result = "FAIL";
		reason = "count never moved AND at least one read in the window returned an error -- "
		         "the driver is failing calls it should not";
	}

	printf("[qenc] RESULT %s: %s (%d/%d clean reads)\n", result, reason, ok_reads, SAMPLES);
	printf("[qenc] done\n");
	return 0;
}
