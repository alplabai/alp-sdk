/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-qenc-readout -- measure a quadrature encoder on the E1M-AEN801 (M55-HE)
 * through TWO independent paths, neither of which is bench-proven yet:
 *
 *  1. HARDWARE: the Ensemble E8 UTIMER QEC0 channel, via the vendored
 *     alif,utimer-qdec sensor driver (sensor_sample_fetch/channel_get on
 *     DT_ALIAS(alp_qenc0), SENSOR_CHAN_ROTATION).  MEASURED (#2037,
 *     2026-09-13, 120000 unaliased CNTR reads over 25 s of continuous hand
 *     motion): this channel counts QEC_TRIGGER0 rising edges up and
 *     QEC_TRIGGER1 rising edges down with NO qualification of the other
 *     input -- an edge counter, not a quadrature decoder.  A genuine
 *     quadrature pair nets 0 per revolution at any speed under this mapping;
 *     the bench instead measured net +654 (up +2385, down -1731) with 293 of
 *     734 non-zero steps |step| >= 2 inside 0.21 ms -- contact bounce, not
 *     motion.  See zephyr/drivers/sensor/qdec_alif/qdec_alif_utimer.c and
 *     the board overlay for the full register/vendor-source evidence. The
 *     driver still scales its raw counter to a 0..359 "degrees" value
 *     (counter * 360 / counts-per-revolution) -- this app prints that value
 *     labelled as what it now is, an unqualified edge count, NOT a position.
 *
 *  2. SOFTWARE: Zephyr's gpio-qdec input driver (zephyr/drivers/input/
 *     input_gpio_qdec.c upstream), a debounced Gray-code state machine over
 *     the SAME P3_0/P3_1 pads, reached through &gpio3 instead of the UTIMER
 *     -- see the board overlay's SOFTWARE DECODE paragraph for exactly how
 *     it is wired. Configured to POLL (idle-poll-time-us set), not to use
 *     GPIO interrupts: snps,designware-gpio (gpio3's compatible) cannot
 *     deliver the GPIO_INT_EDGE_BOTH this driver's interrupt mode requires
 *     (gpio_dw.c returns -ENOTSUP for that combination) -- a driver-
 *     capability mismatch, not a question only a bench can answer. Polling
 *     reads through gpio_pin_get_dt() -> gpio_dw_port_get_raw() ->
 *     EXT_PORTA (0x49003050), the SAME register this file's own raw-pad
 *     read below already measures live under the QEC0 mux, so THAT part is
 *     bench-proven already. This DOES decode direction, unlike path 1 -- it
 *     only posts an event every steps-per-period=4 real phase transitions.
 *     What is NOT yet bench-run: the decode itself, under a hand on the
 *     shaft -- the operator left the bench before this path existed.
 *
 * A portable surface DOES exist -- <alp/counter.h>'s alp_qenc_open() /
 * alp_qenc_get_position(), resolving the SAME alp-qenc0 alias this file's
 * board overlay declares, backed by src/backends/qenc/zephyr_drv.c -- but
 * this app does not call it, for two different reasons per path. Path 1:
 * that backend IS the sensor_sample_fetch/SENSOR_CHAN_ROTATION pair path 1
 * already binds directly, so going through the wrapper would only hide the
 * raw register access this bench tool exists to show (pad levels, the
 * hw/sw split, register dumps) -- exactly why examples/peripheral-io/
 * qenc-readout, the portable-API demo, is a SEPARATE example and has no
 * AEN801 overlay of its own. Path 2: the wrapper's only backend speaks
 * SENSOR_CHAN_ROTATION, not INPUT_REL_* events, so gpio-qdec does not fit
 * it without new backend code -- code the backend's own comment already
 * anticipates ("Real pulse counts come via the v0.3 input-subsystem
 * fast-path") but that does not exist yet. Whether to build that backend,
 * and have this app (or its successor) migrate onto alp_qenc_open() once it
 * does, is a design decision this branch does NOT make -- see #2095.
 *
 * PASS / SKIPPED / FAIL, not PASS / PARTIAL: a bench run with nobody at the
 * knob produces N clean reads that never change -- that is the CORRECT output
 * of a working decoder sitting idle, and it is byte-for-byte what a decoder
 * that never counts also prints.  The old binary verdict called both of those
 * "PARTIAL", which taught nothing.
 *
 * The verdict is now keyed ONLY on the software decoder (path 2), the one
 * capable of establishing a real position -- path 1's "moved" is printed but
 * can NEVER produce PASS by itself, because it is an edge count, not a
 * decode (see #2037 above; an earlier version of this app treated "pads
 * moved and the hardware angle moved" as PASS, which the bounce measurement
 * refutes).  This version still reads the RAW pad levels of P3_0/P3_1 off
 * the GPIO3 controller (GPIO_EXT_PORTA, bits 0/1), independent of both
 * decoders, and tracks whether EITHER the pads OR the software decoder's
 * tick count ever CHANGED across the window -- "changed", not "net
 * nonzero": the bench prompt below asks for a detent, a full CW revolution
 * and a full CCW revolution, which nets back toward the start, so gating on
 * a nonzero FINAL delta would fail a working decoder that returned to where
 * it began. `sw_moved` folds in one read taken right after the sample loop
 * ends, closing a race where the last ~POLL_MS of motion would otherwise
 * never be sampled by the loop itself:
 *   - pad levels changed AND the software decoder's tick count changed ->
 *     PASS: a debounced quadrature decode observed real motion
 *   - pad levels changed BUT the software decoder's tick count never did ->
 *     FAIL: either the polling path never sampled a transition (unexpected
 *     -- see the board overlay, this read path is bench-proven under this
 *     exact pad mux) or the motion never cleared one x4 step; the hardware
 *     edge count (path 1) is printed for reference but is explicitly NOT
 *     evidence either way -- it counts bounce as readily as motion (#2037)
 *   - pad levels never changed at all -> SKIPPED: nothing reached the pads.
 *     This does NOT distinguish an absent/unfitted encoder from a broken or
 *     disconnected one -- it only says no edges arrived at P3_0/P3_1.
 *   - any sample_fetch/channel_get in the window returned an error -> FAIL
 *     regardless of the above (the driver itself is failing reads)
 *   - the software decoder ticked without the pads ever registering a
 *     change -> FAIL, but most likely benign sample-rate aliasing, not an
 *     anomaly: this app polls the raw pads once per POLL_MS (300 ms) while
 *     gpio-qdec samples every 500-2000 us, so a knob turned through one or
 *     more full quadrature cycles between two 300 ms pad samples can tick
 *     the decoder without the slower raw-pad read ever catching a
 *     mid-transition level -- the FAIL still reports it (SW ticked but the
 *     app's own pad evidence did not corroborate it within the window),
 *     but the reason names the likely cause instead of sending a bench
 *     operator chasing a phantom (#2038's own history is the warning)
 *
 * What none of this says: that either decoder is armed and running when
 * idle.  The hardware sensor API this driver registers is {sample_fetch,
 * channel_get} only -- no attr_get, no raw-counter channel -- so there is no
 * way from here to see the counter's run state (UTIMERn_CNTR_CTRL bit 1
 * RUNNING).  Clean reads of a stopped counter look exactly like clean reads
 * of an idle running one.  That is precisely how #2037 (the counter was
 * configured but never started) survived a bench run whose SKIPPED line
 * claimed "decoder armed as configured".  Confirming the hardware run state
 * needs a debugger read of CNTR_CTRL / GLB_CNTR_RUNNING, not a print from
 * this app.
 */

#include <stdio.h>

#include <alp/peripheral.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/sys_io.h>

#define QENC_NODE DT_ALIAS(alp_qenc0)

/*
 * Raw pad sampling for #2037 -- see the file header.  These read P3_0/P3_1
 * (QEC0 X/Y) directly, bypassing the qdec driver and the sensor API, so a
 * dead/unwired/miswired pad and a wired-but-not-counted pad print
 * differently instead of both showing a stuck angle.
 *
 * GPIO3 GPIO_EXT_PORTA: a `snps,designware-gpio` node for GPIO3 DOES exist
 * (zephyr/dts/alif/ensemble_e8_peripherals.dtsi, label `gpio3`, `status =
 * "disabled"` there, enabled by this example's board overlay for the
 * software decoder below) -- an earlier version of this comment claimed no
 * GPIO node existed anywhere in this tree, which was simply not checked
 * thoroughly enough. This raw MMIO read predates that overlay change and is
 * kept as a hardcoded constant rather than rebased onto the DT node: it
 * needs no `gpio_dw` device bind (works before device_is_ready() on gpio3
 * would even pass) and this driver's own read/write path is not the thing
 * under test -- adding a dependency on it here would make a raw-pad check
 * depend on the very peripheral driver it exists to double-check.  Defined
 * here from the Alif DFP AE822FA0E5597 SoC header, Device/soc/AE822FA0E5597/
 * include/rtss_he/soc.h: `GPIO3_BASE` 0x49003000UL, and `GPIO_EXT_PORTA` (an
 * __IM "GPIO External Port Read Register") sits at offset 0x50 within
 * `GPIO_Type` -> 0x49003050, bit N = pin N within the port, so bit 0 = P3_0
 * and bit 1 = P3_1.
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
 * Software quadrature decoder (#2037) -- see the file header path 2 and the
 * board overlay's SOFTWARE DECODE paragraph for how `qdec_sw` is wired
 * (same P3_0/P3_1 pads, reached through &gpio3, no pinctrl change). The
 * portable <alp/counter.h> alp_qenc_* surface exists but its only backend
 * speaks SENSOR_CHAN_ROTATION, not INPUT_REL_* events -- see the file
 * header -- so it is bound here as a raw Zephyr
 * device + input callback, the same way path 1 (the sensor API) already is.
 *
 * INPUT_CALLBACK_DEFINE registers a callback invoked on the input
 * subsystem's own thread (CONFIG_INPUT_MODE_THREAD, the default) whenever
 * qdec_sw posts an event -- asynchronously with respect to main()'s polling
 * loop below, hence the atomic accumulator rather than a plain int32_t.
 * gpio-qdec posts one INPUT_EV_REL / INPUT_REL_WHEEL event per
 * steps-per-period (4, one mechanical detent on this 24-PPR part) real
 * phase transitions, signed by direction -- unlike the hardware UTIMER
 * channel, this driver's Gray-code state machine (QDEC_LL_LH / QDEC_HH_HL /
 * ... in zephyr/drivers/input/input_gpio_qdec.c) genuinely decodes
 * direction, it does not just count edges.
 */
static atomic_t qdec_sw_ticks = ATOMIC_INIT(0);

static void qdec_sw_on_event(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	if ((evt->type == INPUT_EV_REL) && (evt->code == INPUT_REL_WHEEL)) {
		atomic_add(&qdec_sw_ticks, evt->value);
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(qdec_sw)), qdec_sw_on_event, NULL);

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

	const struct device *qdec_sw = DEVICE_DT_GET(DT_NODELABEL(qdec_sw));

	printf("[qenc] open %s (UTIMER hardware edge counter, NOT a decoder -- see #2037)\n",
	       qenc->name);
	if (!device_is_ready(qenc)) {
		printf("[qenc] RESULT FAIL: device not ready\n[qenc] done\n");
		return 0;
	}

	/* qdec_sw not being ready is NOT fatal to the run: the hardware path
	 * above still produces its (now honestly-labelled) diagnostic, and a
	 * missing/failed software decoder is itself useful bench information.
	 * A dead qdec_sw makes sw_moved permanently false below, which the
	 * verdict already treats as "FAIL, edges reached the pads but nothing
	 * decoded them" -- the correct outcome, not a special case.
	 */
	bool qdec_sw_ready = device_is_ready(qdec_sw);
	printf("[qenc] open %s (software gpio-qdec decoder) ready=%d\n", qdec_sw->name, qdec_sw_ready);

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
	       "[qenc]     1. one detent (one click) -- expect 1 software tick\n"
	       "[qenc]        (steps-per-period=4); the hardware edge count is\n"
	       "[qenc]        NOT a position, ignore its magnitude (#2037)\n"
	       "[qenc]     2. one full revolution clockwise\n"
	       "[qenc]     3. one full revolution anticlockwise\n"
	       "[qenc]\n"
	       "[qenc]   The software decoder ('sw ticks' below) is what actually\n"
	       "[qenc]   decodes direction -- if it never moves while you turn the\n"
	       "[qenc]   shaft, that is a live #2037 finding on its own path, not\n"
	       "[qenc]   the already-confirmed hardware defect.\n"
	       "[qenc] ============================================================\n\n",
	       (SAMPLES * POLL_MS) / 1000);

	int      ok_reads    = 0;
	bool     hw_moved    = false;
	bool     sw_moved    = false;
	int32_t  first       = 0;
	int32_t  first_sw    = (int32_t)atomic_get(&qdec_sw_ticks);
	bool     pad_moved   = false;
	uint32_t first_porta = sys_read32(AEN_GPIO3_EXT_PORTA) & AEN_GPIO3_P3_MASK;
	for (int i = 0; i < SAMPLES; i++) {
		/* Raw pad read: independent of both decoders, never errors, so it
		 * is taken every sample regardless of the sensor calls below. */
		uint32_t porta    = sys_read32(AEN_GPIO3_EXT_PORTA) & AEN_GPIO3_P3_MASK;
		bool     pad_x    = (porta & AEN_GPIO3_P3_0_BIT) != 0;
		bool     pad_y    = (porta & AEN_GPIO3_P3_1_BIT) != 0;
		bool     pad_diff = (porta != first_porta);
		if (pad_diff) {
			pad_moved = true;
		}

		/* Software decoder tick count: updated asynchronously by
		 * qdec_sw_on_event() on the input subsystem's own thread, read here
		 * every sample regardless of the hardware sensor calls below. */
		int32_t sw_ticks = (int32_t)atomic_get(&qdec_sw_ticks);
		bool    sw_diff  = (sw_ticks != first_sw);
		if (sw_diff) {
			sw_moved = true;
		}

		int rc = sensor_sample_fetch(qenc);
		if (rc != 0) {
			printf("[qenc] sample_fetch[%d] -> %d  P3_0=%d P3_1=%d sw_ticks=%d\n",
			       i,
			       rc,
			       pad_x,
			       pad_y,
			       sw_ticks);
			alp_delay_ms(POLL_MS);
			continue;
		}
		struct sensor_value v = { 0 };
		rc                    = sensor_channel_get(qenc, SENSOR_CHAN_ROTATION, &v);
		if (rc != 0) {
			printf("[qenc] channel_get[%d] -> %d  P3_0=%d P3_1=%d sw_ticks=%d\n",
			       i,
			       rc,
			       pad_x,
			       pad_y,
			       sw_ticks);
			alp_delay_ms(POLL_MS);
			continue;
		}
		ok_reads++;
		if (ok_reads == 1) {
			first = v.val1;
		} else if (v.val1 != first) {
			hw_moved = true;
		}
		/* Print every sample early (so a fast operator sees feedback), then
		 * thin out -- 200 lines of an unchanging read buries the interesting
		 * part, and the RAM console is a fixed-size buffer that wraps.  A pad
		 * or software-tick transition always prints, even outside that
		 * thinning window: those are the events that discriminate a real
		 * decode from a dead or bounce-only path, and thinning them out
		 * would hide it.
		 */
		if ((i < 20) || (v.val1 != first) || pad_diff || sw_diff || ((i % 10) == 0)) {
			printf("[qenc] hw_edges[%d]=%d (mod 96, NOT degrees/position -- #2037)  "
			       "P3_0=%d P3_1=%d  sw_ticks=%d  [%d s left]\n",
			       i,
			       v.val1,
			       pad_x,
			       pad_y,
			       sw_ticks,
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
	bool    all_clean = (ok_reads == SAMPLES);
	int32_t sw_net    = (int32_t)atomic_get(&qdec_sw_ticks) - first_sw;

	/* Close the race the in-loop sw_diff tracking above cannot: the last
	 * sample point is up to POLL_MS before this line runs (the final
	 * alp_delay_ms(POLL_MS) happens AFTER the last in-loop read), so a
	 * tick that lands in that last gap sets sw_net without ever being
	 * seen by sw_diff. OR the two together rather than replacing one with
	 * the other: sw_diff still catches ticks that occurred mid-window and
	 * later cancelled back to sw_net == 0 (see the file header -- gating
	 * on sw_net != 0 alone would fail a run that did a detent, a full CW
	 * revolution and a full CCW revolution, exactly what the prompt below
	 * asks for, and which nets back toward zero by design). */
	sw_moved = sw_moved || (sw_net != 0);

	const char *result;
	const char *reason;

	if (!all_clean) {
		result = "FAIL";
		reason = "at least one read in the window returned an error -- the driver is failing "
		         "calls it should not, independent of whether the pads or either decoder moved";
	} else if (pad_moved && sw_moved) {
		/*
		 * The raw pad read is what makes this branch trustworthy: P3_0/P3_1
		 * actually transitioned, AND the SOFTWARE decoder's tick count
		 * changed. This is the only branch that can claim PASS: gpio-qdec's
		 * Gray-code state machine genuinely decodes direction (unlike the
		 * hardware UTIMER channel, which is a measured edge counter -- see
		 * the file header and #2037). Spurious internal counts on the
		 * hardware path cannot make BOTH the GPIO3 pad bits AND an
		 * independent software state machine move together the same way a
		 * single free-running register could fool one channel alone.
		 *
		 * Says "tick count CHANGED", not "net ticks": sw_moved is true on
		 * any observed change, not a nonzero FINAL sw_net -- see the
		 * comment above sw_moved's computation for why (the bench prompt's
		 * own CW-then-CCW sequence nets back toward zero by design; sw_net
		 * printed below is informational, not the gate).
		 */
		result = "PASS";
		reason = "the raw P3_0/P3_1 pad levels changed AND the software gpio-qdec decoder's "
		         "tick count changed with them -- a debounced quadrature decode observed real "
		         "motion. The hardware UTIMER channel is diagnostic only (see hw_edges above) "
		         "and is NOT part of this verdict -- it is a measured edge counter, not a "
		         "decoder (#2037)";
	} else if (pad_moved) {
		/*
		 * This is the live #2037 question on the SOFTWARE path: the signal
		 * is proven to reach the SoC pins (the pad bits transitioned), but
		 * qdec_sw's tick count never changed. The board overlay's
		 * idle-poll-time-us makes this driver poll gpio_pin_get_dt() on a
		 * timer rather than rely on a GPIO interrupt snps,designware-gpio
		 * cannot deliver in this driver's edge-triggered mode -- that read
		 * path is bench-proven live under this exact pad mux (same
		 * EXT_PORTA register this app's own raw-pad read uses), so an
		 * unreachable read is not the expected explanation here. More
		 * likely: the motion never cleared one steps-per-period=4 threshold
		 * (less than one detent), or sample-time-us (500 us) still missed a
		 * transition at an unusually fast flick. The hardware edge count is
		 * explicitly NOT corroborating evidence either way -- it counts
		 * contact bounce as readily as real motion, which is the whole
		 * reason this verdict no longer keys on it.
		 */
		result = "FAIL";
		reason = "the raw P3_0/P3_1 pad levels changed but the software gpio-qdec decoder's "
		         "tick count never did -- its read path is bench-proven live under this pad "
		         "mux (see the board overlay), so the likelier explanation is the motion never "
		         "cleared one x4 step. The hardware UTIMER edge count is not evidence here "
		         "either way -- see #2037";
	} else if (!sw_moved) {
		result = "SKIPPED";
		reason = "neither the raw P3_0/P3_1 pad levels nor the software decoder's tick count "
		         "ever changed -- nothing reached the pads. This does NOT distinguish an "
		         "absent/unfitted encoder from a broken or disconnected one; it only says no "
		         "edges arrived at P3_0/P3_1. Do NOT read CNTR_CTRL bit 1 RUNNING as a fault: "
		         "bit 1 clear (CNTR_CTRL 0x00000021) is the CORRECT resting state for a "
		         "trigger-counting channel, and starting the counter to 'fix' it makes it "
		         "free-run on the peripheral clock instead (#2038)";
	} else {
		/* pad_moved == false but sw_moved == true: IS reachable, by plain
		 * sample-rate aliasing, not an anomaly -- this app's raw-pad read
		 * runs once per POLL_MS (300 ms), while gpio-qdec samples every
		 * 500-2000 us (sample-time-us / idle-poll-time-us). A knob turned
		 * through one or more whole quadrature cycles between two of this
		 * app's 300 ms samples can leave `porta` reading the SAME level at
		 * both samples (pad_diff false) while gpio-qdec, sampling ~150-600x
		 * faster, caught the intervening edges and ticked. The same gap
		 * that motivated folding the post-loop sw_net read into sw_moved
		 * above (real ticks landing after the last in-loop pad sample) is
		 * a second, narrower instance of this same asymmetry. Report the
		 * likely-benign explanation, not "worth a bench look" -- a bench
		 * operator chasing this as a fault would be chasing exactly the
		 * kind of phantom this issue's own history (#2038) already warns
		 * about. */
		result = "FAIL";
		reason = "the software decoder's tick count changed without the raw P3_0/P3_1 pad "
		         "levels ever differing between two samples -- most likely sample-rate "
		         "aliasing, not a fault: this app polls the raw pads once per 300 ms while "
		         "gpio-qdec samples every 500-2000 us, so a knob turned through one or more "
		         "full quadrature cycles between two 300 ms pad samples can tick the software "
		         "decoder without ever being caught mid-transition by the slower raw-pad read";
	}

	printf("[qenc] RESULT %s: %s (%d/%d clean reads, pad moved=%d, hw edges moved=%d "
	       "[diagnostic only, NOT part of this verdict], sw ticks moved=%d, sw net=%d)\n",
	       result,
	       reason,
	       ok_reads,
	       SAMPLES,
	       pad_moved,
	       hw_moved,
	       sw_moved,
	       sw_net);
	printf("[qenc] done\n");
	return 0;
}
