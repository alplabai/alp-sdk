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
 *   - count changed, unattended            -> FAIL     (spurious counts; see the
 *                                                      verdict comment below --
 *                                                      PASS needs a human to
 *                                                      attest to the motion)
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
/*
 * 200 * 300 ms = 60 s.  This was 30 samples = 9 s, described in this very
 * comment as "comfortably long enough to grab the knob" -- it is not.  Nine
 * seconds is not enough time to read the prompt, cross to the bench and turn
 * a shaft, and the only measurement this app exists to make is the one a
 * human has to be present for.  Every unattended run reports SKIPPED anyway,
 * so a long window costs nothing where it is not needed and is the difference
 * between usable and useless where it is.
 */
#define SAMPLES   200
#define POLL_MS   300

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

	printf("\n"
	       "[qenc] ============================================================\n"
	       "[qenc]   TURN THE ENCODER SHAFT NOW.\n"
	       "[qenc]\n"
	       "[qenc]   You have %d seconds.  Do this, in order:\n"
	       "[qenc]     1. one detent (one click) -- expect about 15 deg, which\n"
	       "[qenc]        is 4 raw counts at x4 decode\n"
	       "[qenc]     2. one full revolution clockwise\n"
	       "[qenc]     3. one full revolution anticlockwise\n"
	       "[qenc]\n"
	       "[qenc]   If the angle never changes while you are turning it, that\n"
	       "[qenc]   is the open defect reproduced under a hand (#2037) -- and\n"
	       "[qenc]   it is the measurement nobody has taken yet.\n"
	       "[qenc] ============================================================\n\n",
	       (SAMPLES * POLL_MS) / 1000);

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
		/* Print every sample early (so a fast operator sees feedback), then
		 * thin out -- 200 lines of unchanging angle buries the interesting
		 * part, and the RAM console is a fixed-size buffer that wraps. */
		if ((i < 20) || (v.val1 != first) || ((i % 10) == 0)) {
			printf("[qenc] angle[%d] = %d deg (0-359)  [%d s left]\n",
			       i,
			       v.val1,
			       ((SAMPLES - i) * POLL_MS) / 1000);
		}
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
		/*
		 * A count that moves with NOBODY TOUCHING THE SHAFT is not success --
		 * it is the signature of spurious counts, and calling it PASS is how a
		 * noise detector gets mistaken for a working decoder.  Measured on
		 * E1M-AEN803 2026W36-0002 (2026-09-08): with the counter running and
		 * the encoder untouched, the count advanced steadily the whole window.
		 *
		 * The cause is NOT the pads, though it looked like it.  Floating
		 * inputs were the leading theory -- the pad word 0x00210005 gives
		 * AF 5, REN=1, DSC=0 (no bias), SMT=0 (no Schmitt), and the PEC12R's
		 * A/B contacts are open at a detent -- but a bench run walked four pad
		 * configurations and the counter kept advancing through all of them:
		 * no-bias (0x00210005), pull-up (0x00290005), pull-up + Schmitt
		 * (0x002B0005), and AF=0 with the pads deselected from the QEC
		 * altogether (0x00290000).  A floating-input mechanism has to stop
		 * when the pin no longer drives the peripheral.  This does not, so the
		 * count source is internal to the channel.  Still being diagnosed; the
		 * open lead is that UP_1_SRC/DOWN_1_SRC carry the right x4 trigger
		 * masks (0x69/0x96) with PGM_EN (bit 31) CLEAR, where
		 * START_1_SRC/STOP_1_SRC/CLEAR_1_SRC all have it set.
		 *
		 * This app cannot tell spurious counts from real ones -- it has no
		 * operator input to correlate against.  So it reports what it can
		 * defend: motion with no operator is a FAIL, and PASS is reserved for
		 * a run where a human attests to turning the shaft.  Erring the other
		 * way would let floating pins pass as a working encoder.
		 */
		result = "FAIL";
		reason = "the count moved, but this app cannot attest that anyone turned the shaft "
		         "-- on an unattended run a moving count means SPURIOUS COUNTS, not a live "
		         "decode.  Their source is internal to the UTIMER channel, not the pads: "
		         "this app has no operator input to correlate against.  A free-run at the "
		         "peripheral clock caused exactly this and was withdrawn (#2038); if it "
		         "recurs, read CNTR (0x4800D0A0) over SWD with CNTR_PTR widened to "
		         "0xFFFFFFFF -- the shipped reload wraps a revolution every 240 ns and "
		         "hides the rate.  If you DID turn the shaft, this is the expected result "
		         "of a working decoder and only your attestation distinguishes the two";
	} else if (all_clean) {
		result = "SKIPPED";
		reason = "no motion detected -- every read in the window succeeded but the reported "
		         "count never changed.  On an UNATTENDED run this is the expected and "
		         "correct result and proves nothing either way; the decode is only "
		         "settled by turning the shaft.  Do NOT read CNTR_CTRL bit 1 RUNNING as a "
		         "fault: bit 1 clear (CNTR_CTRL 0x00000021) is the CORRECT resting state "
		         "for a trigger-counting channel, and starting the counter to 'fix' it "
		         "makes it free-run on the peripheral clock instead (#2038)";
	} else {
		result = "FAIL";
		reason = "count never moved AND at least one read in the window returned an error -- "
		         "the driver is failing calls it should not";
	}

	printf("[qenc] RESULT %s: %s (%d/%d clean reads)\n", result, reason, ok_reads, SAMPLES);
	printf("[qenc] done\n");
	return 0;
}
