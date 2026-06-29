/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cold-chain-monitor
 * ==================
 *
 * Pharma/food cold-chain integrity monitor.  Pipeline:
 *
 *   BME280 (I2C) --read_raw + compensate--> T(C) / RH(%) / P(Pa)
 *     --sliding window--> cold_chain (mean/min/max, MKT, dewpoint, excursion)
 *       -> cc_classify (OK / TEMP_EXCURSION / MKT_EXCEEDED / CONDENSATION_RISK)
 *       + <alp/inference.h> anomaly score (deterministic fallback)
 *     --> one CC record per report window.
 *
 * Honest scope: a reference cold-chain logger.  MKT, dewpoint, and excursion
 * time are the real metrics; thresholds are configurable per product.  This is
 * NOT a certified GxP / 21-CFR-Part-11 data logger (no validated audit trail,
 * no calibration traceability, no tamper-proof storage).  The model is a stub
 * (see models/README.md); with no model the deterministic classifier + anomaly
 * fallback run.
 *
 *
 * ── Why the BME280 ────────────────────────────────────────────────────────
 *
 * The Bosch BME280 is a combined temperature / humidity / barometric pressure
 * sensor well suited to cold-chain loggers:
 *   - Temperature range -40..+85 C, accuracy ±1.0 C RMS, sufficient for
 *     the ±0.5 C inter-unit spread allowed by GxP-adjacent monitoring.
 *   - Humidity ±3% RH at 25 C enables dewpoint estimation (condensation /
 *     mould risk on packaging and metal surfaces).
 *   - Pressure is logged but not classified by this demo; it's useful for
 *     altitude compensation and for detecting seal-break events in insulated
 *     shippers where a damaged seal opens to ambient.
 *   - I2C single-chip integration keeps the BOM to a minimum; SPI arrives
 *     in the v0.3 driver.
 *   - Normal-mode continuous sampling at a user-selected standby interval
 *     eliminates the MCU-driven trigger polling needed in Forced mode.
 *
 *
 * ── Measurement cadence and window sizing ─────────────────────────────────
 *
 * CC_WINDOW_N (256 samples) * CC_SAMPLE_MIN (1.0 min/sample) = 256 minutes
 * = ~4.3 hours per report window.  That matches a common GDP-compliant
 * logging interval (log every 5..15 min over a 24 h transit) while keeping
 * the window SRAM footprint under 4 kB (256 × 12 B = 3072 B).
 *
 * In this demo the "1 minute" cadence is simulated: all 256 synthetic samples
 * are pushed immediately so the pipeline exercises the framing path without
 * sleeping.  On real hardware a k_timer drives one bme280_read_raw() call per
 * real minute (or whatever the product's logging period is).
 *
 *
 * ── Cold-chain standards ──────────────────────────────────────────────────
 *
 * MKT (Mean Kinetic Temperature, ICH Q1A / USP <1079>):
 *   The single equivalent temperature that delivers the same cumulative
 *   Arrhenius thermal stress as the fluctuating temperature profile.
 *   A brief hot excursion contributes more than an equal-duration cool dip,
 *   so MKT >= arithmetic mean.  The regulatory limit for most vaccines is
 *   MKT <= 8 C (the same as the storage upper bound), so a distribution that
 *   averaged 6 C but had a 12 C spike for 30 min would still breach.
 *   ΔH/R = 10 000 K is the ICH default (ΔH = 83.144 kJ/mol for pharma).
 *
 * Dewpoint (Magnus formula):
 *   When ambient temperature approaches the dewpoint, water condenses on
 *   cold surfaces -- packaging labels detach, metal contacts corrode,
 *   biological samples degrade.  The dewpoint_margin_c config knob controls
 *   how close the reading must be to the dewpoint before CONDENSATION_RISK
 *   is raised.
 *
 *
 * ── Classifier priority ordering ──────────────────────────────────────────
 *
 * The 4-state classifier (implemented in cold_chain.c) reports the HIGHEST
 * severity condition present:
 *   CC_TEMP_EXCURSION    -- product is currently out of the safe band, or
 *                           the cumulative out-of-band time exceeds the limit.
 *                           ACUTE: operator must act immediately.
 *   CC_MKT_EXCEEDED      -- the equivalent thermal stress has crossed the
 *                           regulatory limit even if the product is currently
 *                           in-band.  CUMULATIVE: product may be degraded.
 *   CC_CONDENSATION_RISK -- high humidity or temperature near the dewpoint;
 *                           packaging and surface integrity may be compromised.
 *   CC_OK                -- all metrics within limits.
 *
 *
 * ── AI anomaly score ──────────────────────────────────────────────────────
 *
 * The threshold-based classifier catches known bad states.  The AI score
 * catches SUBTLE DRIFT that no single threshold catches: a slowly degrading
 * compressor that nudges the mean temperature up by 0.3 C per week, or a
 * door-seal micro-leak that correlates the humidity rise with the warming
 * slope.  The fallback (cc_anomaly_fallback) implements the same idea
 * deterministically: it returns max(excursion depth, MKT overshoot),
 * saturating at 1.0.  A trained autoencoder running via <alp/inference.h>
 * replaces this with a learned reconstruction error.
 *
 *
 * ── native_sim / no-sensor fallback ──────────────────────────────────────
 *
 * On native_sim the I2C emul controller answers open() / ioctl() but has no
 * BME280 target attached, so bme280_init() fails the CHIP_ID check.  main()
 * detects this and fills every window with synthetic environment data via
 * synth_sample(), covering the three interesting states:
 *   window 0: stable in-band cold  -> CC_OK
 *   window 1: warming ramp         -> CC_TEMP_EXCURSION
 *   window 2: near-saturated air   -> CC_CONDENSATION_RISK
 * On real silicon fill the window from bme280_read_raw() instead.
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Board-portable alias for the sensor I2C bus: BOARD_I2C_SENSORS resolves to
 * the correct on-board I2C controller for whatever EVK is targeted via the
 * CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK" build flag. */
#include "alp/board.h"
/* AI inference dispatch: AUTO routes to Ethos-U55 on AEN, DEEPX DX-M1 on V2N,
 * and the reference TFLM CPU kernels on native_sim. */
#include "alp/inference.h"
/* Portable I2C open/close/read/write primitives (no vendor names). */
#include "alp/peripheral.h"
/* BME280 combined T/RH/P chip driver (I2C v0.2, paper-correct). */
#include "alp/chips/bme280.h"

/* Pure-C cold-chain metrics, classifier, and anomaly fallback (arch-neutral,
 * no Zephyr deps -- identical source compiles for native_sim and the M55). */
#include "cold_chain.h"

LOG_MODULE_REGISTER(cc, LOG_LEVEL_INF);

/* BME280 I2C address: SDO-tied-low = 0x76 on the EVK (0x77 if SDO is high).
 * Override at compile time if your carrier boards SDO differently. */
#define BME280_ADDR 0x76u
/* Number of report windows in the bounded demo run (3 covers OK / EXCURSION /
 * CONDENSATION in the synthetic scenario; a real logger runs indefinitely). */
#define N_REPORTS 3

/* Vaccine-fridge thresholds (2..8 C, USP <1079> Annex 1 cold chain).
 * A real deployment loads the limits for the specific product (frozen at
 * -20 C, ambient-stable at +25 C, etc.) from NVS or a compile-time overlay. */
static const struct cc_config CFG = {
	.t_lo                = 2.0f,  /* safe-band lower edge, Celsius */
	.t_hi                = 8.0f,  /* safe-band upper edge, Celsius */
	.mkt_limit_c         = 8.0f,  /* MKT above this -> cumulative damage */
	.excursion_min_limit = 30.0f, /* out-of-band minutes before EXCURSION */
	.dewpoint_margin_c   = 2.0f,  /* T within this of dewpoint -> CONDENSATION */
};

/* 1-byte stub so alp_inference_open's non-NULL model-data contract is met.
 * An unusable tensor (wrong shape from the stub backend) causes every
 * alp_inference_invoke() to return no usable output, which anomaly_score()
 * detects and routes to cc_anomaly_fallback().  See models/README.md for
 * the autoencoder training recipe to replace this stub. */
static const uint8_t s_model[] = { 0x00 };

/*
 * synth_sample() -- synthetic environment data for native_sim / no sensor.
 *
 * Generates a deterministic (T, RH, P) triple for sample @p i of report
 * window @p report.  Three scenarios are exercised across the N_REPORTS
 * windows so the classifier produces a meaningful state spread:
 *
 *   report 0: steady in-band cold storage (5 C, 50 % RH)        -> CC_OK
 *   report 1: warming ramp (5..12 C linear)                      -> CC_TEMP_EXCURSION
 *   report 2: cold but near-saturated air (5 C, 95 % RH)        -> CC_CONDENSATION_RISK
 *
 * Pressure is fixed at 101325 Pa (sea-level standard) for all windows;
 * it does not affect classification in this demo.
 */
static struct cc_sample synth_sample(int report, int i)
{
	/* Baseline: stable cold storage in the 2..8 C vaccine fridge band. */
	struct cc_sample s = { .temp_c = 5.0f, .rh_pct = 50.0f, .pressure_pa = 101325.0f };
	switch (report) {
	case 0:
		break; /* stable cold -- all defaults are fine */
	case 1:
		/* Linear warming ramp from 5 C to 12 C across the window.
		 * This drives the mean above t_hi and the excursion counter
		 * past the limit, triggering CC_TEMP_EXCURSION. */
		s.temp_c = 5.0f + 7.0f * (float)i / (float)(CC_WINDOW_N - 1);
		s.rh_pct = 55.0f;
		break;
	default:
		/* Near-saturated cold air: temperature stays in-band but RH
		 * is high enough that the Magnus dewpoint is within
		 * dewpoint_margin_c of the ambient temperature, raising
		 * CC_CONDENSATION_RISK. */
		s.rh_pct = 95.0f;
		break;
	}
	return s;
}

/*
 * anomaly_score() -- AI model if available, else the deterministic fallback.
 *
 * Packs the cc_features vector, invokes the loaded model, and reads the
 * first float output as a 0..1 anomaly score.  Falls back to
 * cc_anomaly_fallback() when:
 *   - @p inf is NULL (inference handle failed to open),
 *   - the stub model returns an empty / wrongly-typed output tensor, or
 *   - alp_inference_invoke() returns a non-OK status.
 * This "try model, fall back cleanly" pattern matches the sibling examples
 * and means the demo always produces a meaningful score offline.
 */
static float anomaly_score(alp_inference_t *inf, const struct cc_features *f)
{
	if (inf != NULL) {
		/* Pack the 8 metrics into the flat float vector the model expects. */
		float vec[CC_FEATURE_DIM];
		(void)cc_feat_pack(f, vec, CC_FEATURE_DIM);
		alp_inference_tensor_t in = { 0 };
		/* Copy into the model's input tensor only if the shape matches. */
		if (alp_inference_get_input(inf, 0, &in) == ALP_OK && in.dtype == ALP_INFERENCE_DTYPE_F32 &&
		    in.data != NULL && in.size_bytes >= sizeof(vec)) {
			memcpy(in.data, vec, sizeof(vec));
			if (alp_inference_invoke(inf) == ALP_OK) {
				alp_inference_tensor_t out = { 0 };
				/* Read the scalar anomaly score from the output tensor. */
				if (alp_inference_get_output(inf, 0, &out) == ALP_OK &&
				    out.dtype == ALP_INFERENCE_DTYPE_F32 && out.data != NULL &&
				    out.size_bytes >= sizeof(float)) {
					return ((const float *)out.data)[0];
				}
			}
		}
	}
	/* Deterministic fallback: max(excursion depth, MKT overshoot), clamped
	 * to 1.0.  Transparent, always correct, gives the demo a meaningful
	 * score without any trained model. */
	return cc_anomaly_fallback(f, &CFG);
}

int main(void)
{
	/* Driver context and window state are static so they do not consume
	 * stack on the Zephyr M55 thread (the BME280 calib struct is 26 bytes;
	 * the window is 256 × 12 B = 3072 B). */
	static bme280_t               dev;
	static struct cc_window_state win;
	bool                          sensor_ok = false;

	/* ── Sensor bring-up ─────────────────────────────────────────────────
	 * Open the shared sensor I2C bus (BOARD_I2C_SENSORS resolves to the
	 * correct on-board controller for the target EVK) and initialise the
	 * BME280.  bme280_init() reads CHIP_ID and loads the per-die calibration
	 * coefficients; it returns ALP_ERR_IO if no BME280 responds.
	 * On native_sim the emul controller has no target attached, so init
	 * fails cleanly and we fall back to synth_sample(). */
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = BOARD_I2C_SENSORS, .bitrate_hz = 400000 });
	if (bus != NULL && bme280_init(&dev, bus, BME280_ADDR) == ALP_OK) {
		/* bme280_init() leaves the oversampling / mode registers untouched.
		 * Configure continuous-conversion (NORMAL) mode with X1 oversampling
		 * on all three channels (T, P, H), a 1 s standby period between
		 * measurements, and no IIR filter.  bme280_read_raw() will then
		 * return a fresh reading without needing to poll the status register.
		 * For a cold-chain logger the 1 s cadence is already faster than the
		 * CC_SAMPLE_MIN (1 min) window rate, so X1/1 s is well matched. */
		bme280_set_sampling(&dev,
		                    BME280_OVERSAMPLING_X1, /* temperature channel */
		                    BME280_OVERSAMPLING_X1, /* pressure channel */
		                    BME280_OVERSAMPLING_X1, /* humidity channel */
		                    BME280_MODE_NORMAL,     /* continuous conversion */
		                    BME280_STANDBY_1000_MS, /* 1 s between measurements */
		                    BME280_FILTER_OFF);     /* no IIR smoothing */
		sensor_ok = true;
	} else {
		LOG_WRN("BME280 unavailable; using synthetic environment data");
	}

	/* ── Inference bring-up ──────────────────────────────────────────────
	 * Open the inference handle.  BACKEND_AUTO selects Ethos-U on AEN,
	 * DEEPX DX-M1 on V2N, or the TFLM CPU kernels on native_sim.
	 * The 1-byte stub model causes every invoke() to return an unusable
	 * output, so anomaly_score() falls back to the deterministic path. */
	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	/* CSV header: one line per report window.  Field order matches the
	 * documented output schema and the README example. */
	printk("# CC,t_s,state,temp_c,rh_pct,dewpoint_c,mkt_c,excursion_min\n");

	/* ── Report loop ─────────────────────────────────────────────────────
	 * Each iteration resets the window, fills it (real or synthetic), then
	 * reduces it to metrics and emits one CC record.  N_REPORTS = 3 covers
	 * the OK / EXCURSION / CONDENSATION states in the synthetic scenario. */
	for (int r = 0; r < N_REPORTS; r++) {
		/* Reset the window for this report period. */
		cc_window_reset(&win);

		/* Fill one window of readings (real sensor or synthetic). */
		for (int i = 0; i < CC_WINDOW_N; i++) {
			struct cc_sample s;
			if (sensor_ok) {
				/* Real sensor path: burst-read the raw 20/20/16-bit
				 * conversion result, then compensate with the per-die
				 * calibration loaded at init.  The BME280 is running
				 * continuously (NORMAL mode), so the result is fresh
				 * within the 1 s standby period. */
				bme280_raw_t         raw;
				bme280_compensated_t c;
				if (bme280_read_raw(&dev, &raw) == ALP_OK &&
				    bme280_compensate(&dev, &raw, &c) == ALP_OK) {
					/* Convert from fixed-point to float physical units:
					 *   temperature_c100 is degrees × 100 -> C
					 *   humidity_milli_pct is %RH × 1024 -> %
					 *   pressure_pa is already pascals. */
					s.temp_c      = (float)c.temperature_c100 / 100.0f;
					s.rh_pct      = (float)c.humidity_milli_pct / 1024.0f;
					s.pressure_pa = (float)c.pressure_pa;
				} else {
					/* I/O error mid-window: fall back to synth for
					 * this sample so the window stays complete. */
					s = synth_sample(r, i);
				}
			} else {
				/* No sensor: generate deterministic synthetic data. */
				s = synth_sample(r, i);
			}
			cc_window_push(&win, s);
		}

		/* Reduce the window to metrics, classify, score the anomaly, emit. */
		struct cc_features f;
		cc_feat_extract(&win, &CFG, &f);
		cc_state_t st = cc_classify(&f, &CFG);
		/* Compute the anomaly score (model or deterministic fallback).
		 * In a real gateway deployment `an` is forwarded via MQTT / OPC-UA
		 * alongside the classifier state; here we suppress the unused-var
		 * warning with (void) since this demo only emits the state string. */
		float an = anomaly_score(inf, &f);
		(void)an;

		/* Emit one CSV record.  t_s = elapsed monitoring time in minutes
		 * = samples-completed × CC_SAMPLE_MIN (1.0 min/sample). */
		printk("CC,%.1f,%s,%.1f,%.1f,%.1f,%.1f,%.1f\n",
		       (double)((r + 1) * CC_WINDOW_N * CC_SAMPLE_MIN),
		       cc_state_name(st),
		       (double)f.mean_temp_c,
		       (double)f.mean_rh_pct,
		       (double)f.dewpoint_c,
		       (double)f.mkt_c,
		       (double)f.excursion_min);
	}

	/* ── Lifecycle teardown ──────────────────────────────────────────────
	 * Release in reverse acquisition order: model, sensor, bus.
	 * bme280_soft_reset() writes 0xB6 to the RESET register so the chip
	 * returns to its power-on state; the bus is then safe to release. */
	if (inf != NULL) {
		alp_inference_close(inf);
	}
	if (sensor_ok) {
		/* Soft-reset the chip before releasing the bus so it stops
		 * converting and returns to sleep (lowest power state). */
		bme280_soft_reset(&dev);
	}
	if (bus != NULL) {
		alp_i2c_close(bus);
	}
	printk("[cc] done\n");
	return 0;
}
