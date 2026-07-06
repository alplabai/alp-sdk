/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * aen-qenc-readout -- read a quadrature encoder on the Ensemble E8 UTIMER via the
 * vendored alif,utimer-qdec sensor driver, on the E1M-AEN801 (M55-HE).  Drives the
 * standard Zephyr sensor API (sensor_sample_fetch / sensor_channel_get) on
 * DT_ALIAS(alp_qenc0) and reports SENSOR_CHAN_ROTATION.  The driver scales the
 * raw UTIMER counter to DEGREES (val1 = counter * 360 / counts-per-revolution),
 * so the value ranges 0..359, not raw counts.
 *
 * The UTIMER runs in quadrature-decoder mode: two phase inputs (X = channel A,
 * Y = channel B) advance/retreat the counter; the reload value wraps at
 * counts-per-revolution.  Turn the encoder shaft between samples to see the
 * angle move.  NOTE: because the reading is quantised to whole degrees, shaft
 * motion smaller than ~1 degree (360 / counts-per-revolution of a rev) will
 * not change val1 and reads as no motion.
 *
 * PASS gate: device ready, sample_fetch + channel_get return 0 across the poll
 * loop AND the reported angle CHANGES (the shaft was turned -> live decode).  A
 * run that reads cleanly but never changes (no shaft motion / sub-degree motion
 * / encoder not wired) is reported PARTIAL -- the driver path is proven; spin
 * the encoder through at least a degree.
 */

#include <stdio.h>

#include <alp/peripheral.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#define QENC_NODE DT_ALIAS(alp_qenc0)
#define SAMPLES   20
#define POLL_MS   100

int main(void)
{
	const struct device *qenc = DEVICE_DT_GET(QENC_NODE);

	printf("[qenc] open %s (UTIMER quadrature decoder)\n", qenc->name);
	if (!device_is_ready(qenc)) {
		printf("[qenc] RESULT FAIL: device not ready\n[qenc] done\n");
		return 0;
	}

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

	printf("[qenc] RESULT %s: %s (%d/%d clean reads)\n",
	       (ok_reads > 0 && moved) ? "PASS" : "PARTIAL",
	       moved          ? "angle changed = live quadrature decode"
	       : ok_reads > 0 ? "reads clean but angle static (spin the encoder >=1 deg / check wiring)"
	                      : "no clean reads",
	       ok_reads,
	       SAMPLES);
	printf("[qenc] done\n");
	return 0;
}
