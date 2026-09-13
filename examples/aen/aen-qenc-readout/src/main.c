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
 * The UTIMER runs in quadrature-decoder mode on P3_0/P3_1 (QEC0_X_A/QEC0_Y_A).
 * These are QEC_TRIGGER0/1 inputs on the SRC_0 trigger-source registers, NOT
 * "channel input A/B" on SRC_1 -- that SRC_1 naming belongs to the
 * lputimer0/1/2 instances Alif's own tree binds this same driver to, and its
 * x4-decode arming (hal_alif's alif_utimer_config_qdec_triggers(), both
 * rising and falling edges of A and B) never counted a single QEC0 edge on
 * this bench (#2037: pads toggled, UTIMER_CNTR stayed 0x00000000).  See
 * zephyr/drivers/sensor/qdec_alif/qdec_alif_utimer.c for the SRC_0 fix.  The
 * decode ratio under SRC_0 is unproven -- see the board overlay's comment and
 * this example's README.md for the predicted per-build raw-count deltas.
 * counts-per-revolution stays 96 (24-PPR encoder * the old x4 SRC_1 decode)
 * pending that bench confirmation: the hardware reload wraps the counter at
 * (counts-per-revolution - 1), so a wrong (too large) value doesn't just
 * misreport a bit -- it stretches one real revolution across only a fraction
 * of the 0-359 range, AND makes a single genuine encoder tick integer-
 * truncate to 0 degrees ("moved a little" reads identical to "never moved").
 *
 * PASS / SKIPPED / FAIL, not PASS / PARTIAL: a bench run with nobody at the
 * knob produces N clean reads that never change -- that is the CORRECT output
 * of a working decoder sitting idle, and it is byte-for-byte what a decoder
 * that never counts also prints.  The old binary verdict called both of those
 * "PARTIAL", which taught nothing.
 *
 * A prior version of this app could not tell a live decode from spurious
 * internal counts on an unattended run -- it had no operator input to
 * correlate the decoded angle against.  This version reads the RAW pad
 * levels of P3_0/P3_1 (QEC0 X/Y) straight off the GPIO3 controller
 * (GPIO_EXT_PORTA, bits 0/1), independent of the qdec driver and the sensor
 * API entirely, and tracks whether either one ever CHANGED across the
 * window.  That gives three outcomes instead of two:
 *   - pad levels changed AND the decoded angle changed -> PASS (the decoder
 *     works: edges reach the pad and the channel counts them)
 *   - pad levels changed BUT the decoded angle never changed -> FAIL, and
 *     specifically the #2037 defect: signal reaches the SoC pins and the
 *     channel still does not count it
 *   - pad levels never changed at all -> SKIPPED: nothing reached the pads.
 *     This does NOT distinguish an absent/unfitted encoder from a broken or
 *     disconnected one -- it only says no edges arrived at P3_0/P3_1.
 *   - any sample_fetch/channel_get in the window returned an error -> FAIL
 *     regardless of the above (the driver itself is failing reads)
 *
 * What none of this says: that the decoder is armed and running when idle.
 * The sensor API this driver registers is {sample_fetch, channel_get} only --
 * no attr_get, no raw-counter channel -- so there is no way from here to see
 * the counter's run state (UTIMERn_CNTR_CTRL bit 1 RUNNING).  Clean reads of a
 * stopped counter look exactly like clean reads of an idle running one, and
 * the raw pad read added here does not change that -- it tells you whether
 * edges arrived, not whether the channel would have counted them if the
 * decoder were otherwise dead.  That is precisely how #2037 (the counter was
 * configured but never started) survived a bench run whose SKIPPED line
 * claimed "decoder armed as configured".  Confirming the run state needs a
 * debugger read of CNTR_CTRL / GLB_CNTR_RUNNING, not a print from this app.
 */

#include <stdio.h>

#include <alp/peripheral.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/sys_io.h>

#define QENC_NODE DT_ALIAS(alp_qenc0)

/*
 * Raw pad sampling for #2037 -- see the file header.  These read P3_0/P3_1
 * (QEC0 X/Y) directly, bypassing the qdec driver and the sensor API, so a
 * dead/unwired/miswired pad and a wired-but-not-counted pad print
 * differently instead of both showing a stuck angle.
 *
 * GPIO3 GPIO_EXT_PORTA: there is no `alif,gpio` (or any GPIO) devicetree
 * node anywhere in this Zephyr tree for the Alif Ensemble family (checked:
 * no gpio0..17 node under dts/arm/alif), so there is nothing DT-derived to
 * point at -- this is defined here from the Alif DFP AE822FA0E5597 SoC
 * header, Device/soc/AE822FA0E5597/include/rtss_he/soc.h: `GPIO3_BASE`
 * 0x49003000UL, and `GPIO_EXT_PORTA` (an __IM "GPIO External Port Read
 * Register") sits at offset 0x50 within `GPIO_Type` -> 0x49003050, bit N =
 * pin N within the port, so bit 0 = P3_0 and bit 1 = P3_1.
 *
 * This register reads the pad's physical level continuously, independent of
 * which peripheral function (if any) is muxed onto the pin, gated only by
 * the pad's own input-enable (REN, pinctrl's `input-enable`) -- which
 * `pinctrl_qec0` in the board overlay already sets for P3_0/P3_1, so no
 * additional mux or pad change is needed here.  Verified against
 * zephyr/soc/alif/ensemble/pinctrl_soc.h (REN is bit 16 of the pad config
 * word, independent of the AF field in bits 0:2) and against a prior bench
 * session that read P3_4 over SWD while it was muxed to UART (not GPIO) and
 * saw live values tracking UART traffic rather than a fixed idle read -- the
 * GPIO controller's port-input sampler is wired in parallel with the muxed
 * peripheral's input, both fed from the same pad, both gated by REN alone.
 */
#define AEN_GPIO3_EXT_PORTA 0x49003050u
#define AEN_GPIO3_P3_0_BIT  BIT(0)
#define AEN_GPIO3_P3_1_BIT  BIT(1)
#define AEN_GPIO3_P3_MASK   (AEN_GPIO3_P3_0_BIT | AEN_GPIO3_P3_1_BIT)

/*
 * Pad-control (mux + electrical) register for a given port/pin, printed
 * once at start-up so a missing input-enable is visible without a debugger.
 * zephyr/drivers/pinctrl/pinctrl_alif.c (alif_pinctrl_get_reg_addr())
 * computes exactly this: pinctrl-base + port*32 + pin*4, one 32-bit word per
 * pin.  Unlike GPIO3 above, the pinctrl block DOES have a devicetree node
 * (dts/arm/alif/ensemble/common/ensemble_common.dtsi, label `pinctrl`,
 * reg-name "pinctrl" -> 0x1A603000), so the base is pulled from DT instead
 * of being re-hardcoded; only the per-pin stride (driver-internal, not
 * DT-exposed) is a local constant, matching the driver's own math.  For
 * P3_0/P3_1 (port 3, pins 0/1) that resolves to 0x1A603060 / 0x1A603064.
 */
#define AEN_PINCTRL_BASE       DT_REG_ADDR_BY_NAME(DT_NODELABEL(pinctrl), pinctrl)
#define AEN_PAD_REG(port, pin) (AEN_PINCTRL_BASE + (uint32_t)(port) * 32u + (uint32_t)(pin) * 4u)
/*
 * 200 * 300 ms = 60 s.  This was 30 samples = 9 s, described in this very
 * comment as "comfortably long enough to grab the knob" -- it is not.  Nine
 * seconds is not enough time to read the prompt, cross to the bench and turn
 * a shaft, and the only measurement this app exists to make is the one a
 * human has to be present for.  Every unattended run reports SKIPPED anyway,
 * so a long window costs nothing where it is not needed and is the difference
 * between usable and useless where it is.
 */
#define SAMPLES 200
#define POLL_MS 300

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

	/* Pad-control register dump for #2037 -- see the AEN_PAD_REG comment
	 * above.  A missing input-enable (bit 16, REN) here means P3_0/P3_1's
	 * physical level can never reach the pad, independent of anything the
	 * qdec driver or the raw GPIO3 read below can show.
	 */
	uint32_t pad_p3_0 = sys_read32(AEN_PAD_REG(3, 0));
	uint32_t pad_p3_1 = sys_read32(AEN_PAD_REG(3, 1));
	printf("[qenc] pad-config P3_0@0x%08x=0x%08x P3_1@0x%08x=0x%08x (REN=bit16)\n",
	       (unsigned)AEN_PAD_REG(3, 0),
	       (unsigned)pad_p3_0,
	       (unsigned)AEN_PAD_REG(3, 1),
	       (unsigned)pad_p3_1);

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

	int      ok_reads    = 0;
	bool     moved       = false;
	int32_t  first       = 0;
	bool     pad_moved   = false;
	uint32_t first_porta = sys_read32(AEN_GPIO3_EXT_PORTA) & AEN_GPIO3_P3_MASK;
	for (int i = 0; i < SAMPLES; i++) {
		/* Raw pad read: independent of the qdec driver, never errors, so it
		 * is taken every sample regardless of the sensor calls below. */
		uint32_t porta    = sys_read32(AEN_GPIO3_EXT_PORTA) & AEN_GPIO3_P3_MASK;
		bool     pad_x    = (porta & AEN_GPIO3_P3_0_BIT) != 0;
		bool     pad_y    = (porta & AEN_GPIO3_P3_1_BIT) != 0;
		bool     pad_diff = (porta != first_porta);
		if (pad_diff) {
			pad_moved = true;
		}

		int rc = sensor_sample_fetch(qenc);
		if (rc != 0) {
			printf("[qenc] sample_fetch[%d] -> %d  P3_0=%d P3_1=%d\n", i, rc, pad_x, pad_y);
			alp_delay_ms(POLL_MS);
			continue;
		}
		struct sensor_value v = { 0 };
		rc                    = sensor_channel_get(qenc, SENSOR_CHAN_ROTATION, &v);
		if (rc != 0) {
			printf("[qenc] channel_get[%d] -> %d  P3_0=%d P3_1=%d\n", i, rc, pad_x, pad_y);
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
		 * part, and the RAM console is a fixed-size buffer that wraps.  A pad
		 * transition always prints, even outside that thinning window: it is
		 * the one event that discriminates the #2037 defect (pad moves,
		 * angle doesn't) from a dead pad, and thinning it out would hide it.
		 */
		if ((i < 20) || (v.val1 != first) || pad_diff || ((i % 10) == 0)) {
			printf("[qenc] angle[%d] = %d deg (0-359)  P3_0=%d P3_1=%d  [%d s left]\n",
			       i,
			       v.val1,
			       pad_x,
			       pad_y,
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

	if (!all_clean) {
		result = "FAIL";
		reason = "at least one read in the window returned an error -- the driver is failing "
		         "calls it should not, independent of whether the pads or the count moved";
	} else if (moved && pad_moved) {
		/*
		 * The raw pad read is what makes this branch trustworthy: P3_0/P3_1
		 * actually transitioned, AND the decoded angle tracked it.  A count
		 * that moves with nobody touching the shaft used to be
		 * indistinguishable from a live decode from inside this app alone --
		 * see the #2038 history below -- but spurious internal counts cannot
		 * make the GPIO3 EXT_PORTA pad bits move too, since those come
		 * straight off the physical pin, upstream of the UTIMER channel
		 * entirely.  Correlated motion on both sides is the decoder working.
		 */
		result = "PASS";
		reason = "the raw P3_0/P3_1 pad levels changed AND the decoded angle changed with "
		         "them -- edges reach the pad and the channel counts them: the decoder works";
	} else if (moved) {
		/*
		 * The angle moved but the raw pads did not -- exactly the spurious-
		 * count signature this app could not previously rule out.  Measured
		 * on E1M-AEN803 2026W36-0002 (2026-09-08): with the counter running
		 * and the encoder untouched, the count advanced steadily the whole
		 * window while UP_1_SRC and DOWN_1_SRC were both zeroed, so no
		 * quadrature edge of any polarity could have contributed -- the count
		 * source was internal to the channel, not the pads.
		 *
		 * RESOLVED: that free-run was caused by a GLB_CNTR_START write added
		 * under #2037 and since WITHDRAWN.  Starting a trigger-counting
		 * channel puts it into free-running clocked mode -- it advanced at
		 * 400,010,738 counts/s -- and GLB_CNTR_STOP froze it instantly.  With
		 * the start call gone, CNTR reads a stable 0x00000000, so this branch
		 * is now near-unreachable; if it reproduces, the free-run is back.
		 */
		result = "FAIL";
		reason = "the decoded angle moved but the raw P3_0/P3_1 pad levels never did -- "
		         "spurious counts internal to the UTIMER channel, not real quadrature edges.  "
		         "A free-run at the peripheral clock caused exactly this and was withdrawn "
		         "(#2038); if it recurs, read CNTR (0x4800D0A0) over SWD with CNTR_PTR "
		         "widened to 0xFFFFFFFF -- the shipped reload wraps a revolution every 240 ns "
		         "and hides the rate";
	} else if (pad_moved) {
		/* This is the #2037 defect, reproduced under a hand on the shaft: the
		 * signal is proven to reach the SoC pins (the pad bits transitioned),
		 * and the channel still reports zero motion.  Not the pads, not an
		 * absent encoder -- the decode path itself.
		 */
		result = "FAIL";
		reason = "the raw P3_0/P3_1 pad levels changed but the decoded angle never did -- "
		         "signal reaches the SoC pins and the UTIMER QEC0 channel does not count it.  "
		         "This is the #2037 defect, reproduced under a hand on the shaft";
	} else {
		result = "SKIPPED";
		reason = "neither the decoded angle nor the raw P3_0/P3_1 pad levels ever changed -- "
		         "nothing reached the pads.  This does NOT distinguish an absent/unfitted "
		         "encoder from a broken or disconnected one; it only says no edges arrived at "
		         "P3_0/P3_1.  Do NOT read CNTR_CTRL bit 1 RUNNING as a fault: bit 1 clear "
		         "(CNTR_CTRL 0x00000021) is the CORRECT resting state for a trigger-counting "
		         "channel, and starting the counter to 'fix' it makes it free-run on the "
		         "peripheral clock instead (#2038)";
	}

	printf("[qenc] RESULT %s: %s (%d/%d clean reads, pad moved=%d)\n",
	       result,
	       reason,
	       ok_reads,
	       SAMPLES,
	       pad_moved);
	printf("[qenc] done\n");
	return 0;
}
