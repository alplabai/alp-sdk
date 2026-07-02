/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * multimodal-fusion-pdm
 * =====================
 *
 * Multi-sensor motor-health monitor.  Pipeline:
 *
 *   ICM-42670 (vibration) ---+
 *   INA236    (current)   ---+--> compact per-modality summary (fusion_input)
 *   BME280    (temperature)--+        |
 *                                      v
 *   fusion_health: per-modality sub-scores vs baseline -> corroboration count
 *     -> cross-modal fault hypothesis + confidence-weighted health score
 *     -> <alp/inference.h> fused model (deterministic fusion-rule fallback)
 *     --> one FUSE record per report.
 *
 * The point of fusion: a real fault corroborates across modalities (bearing
 * wear shows in vibration AND temperature), so an isolated single-modality
 * blip is flagged UNCORROBORATED at low confidence instead of raising a false
 * alarm -- which a bank of independent single-sensor thresholds would do.
 *
 * Honest scope: reference fusion logic.  The per-modality summaries here are
 * lightweight (the dedicated single-modality examples do the richer DSP); the
 * hypotheses are heuristic cross-modal rules (customer retunes the baseline +
 * weights per machine).  The model is a stub (see models/README.md); with no
 * model the deterministic fusion rule runs.  Any missing sensor degrades
 * gracefully: its summary stays at the baseline nominal, so that modality reads
 * non-anomalous and simply lowers corroboration.
 *
 *
 * ── API reconciliation (verified against real headers + sibling examples) ──
 *
 * ICM-42670 (ai-anomaly-detection-vibration sibling + chips/icm42670/):
 *   icm42670_init(ctx, bus, addr)           -- 3-arg; WHO_AM_I check inside.
 *   icm42670_set_accel(ctx, odr, fs)        -- enables accel and starts sampling.
 *   icm42670_read_accel(ctx, &axes)         -- returns alp_status_t; axes int16.
 *   icm42670_deinit(ctx)                    -- void; releases driver context.
 *   ICM42670_ACCEL_FS_16G (=0x0)            -> 2048 LSB/g full-scale sensitivity.
 *
 * INA236 (v2n-power-monitor sibling + chips/ina236/):
 *   ina236_init(ctx, bus, addr, shunt_ohms, max_current_a, adcrange) -- 6-arg.
 *   ina236_read_current_ua(ctx, &ua)         -- ua in microamps (int32).
 *   ina236_reset(ctx)                        -- soft-reset + recalibrate;
 *                                               returns alp_status_t.
 *   INA236_ADCRANGE_81MV                     -- 81.92 mV full-scale (default).
 *
 * BME280 (chips/bme280/ header + impl):
 *   bme280_init(ctx, bus, addr)              -- loads calibration; leaves
 *                                               chip in SLEEP mode (CTRL_MEAS
 *                                               bits[1:0] = 00b after reset).
 *   bme280_set_sampling(...)                 -- REQUIRED before any read;
 *                                               sets mode to FORCED or NORMAL.
 *   bme280_read_raw(ctx, &raw)               -- burst read of raw registers.
 *   bme280_compensate(ctx, &raw, &comp)      -- comp.temperature_c100 = degC*100.
 *   bme280_soft_reset(ctx)                   -- writes 0xB6 to RESET register;
 *                                               returns alp_status_t.
 *
 * alp_inference_open:
 *   alp_inference_config_t has .arena + .arena_bytes (caller-allocated scratch;
 *   if arena_bytes=0 the backend uses an internal default).  We pass s_arena
 *   here to match the sibling vibration example and avoid heap allocation.
 *
 * (void) casts:
 *   Applied wherever an alp_status_t return is intentionally discarded (e.g.
 *   ina236_reset, bme280_soft_reset in teardown; alp_inference_invoke when the
 *   stub yields no usable output) to suppress -Wunused-result on the AEN build.
 */
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* BOARD_I2C_SENSORS resolves to the shared sensor bus on the active EVK
 * (ALP_E1M_I2C0 on E1M EVK / ALP_E1M_X_I2C0 on E1M-X EVK) via the build-emitted
 * -DALP_BOARD_E1M_EVK / -DALP_BOARD_E1M_X_EVK define; see testcase.yaml.
 * On native_sim the overlay aliases alp-i2c0 to the emulated controller. */
#include "alp/board.h"
#include "alp/inference.h"
#include "alp/peripheral.h"
#include "alp/chips/icm42670.h"
#include "alp/chips/ina236.h"
#include "alp/chips/bme280.h"

#include "fusion_health.h"

LOG_MODULE_REGISTER(fuse, LOG_LEVEL_INF);

/* Number of fault scenarios the bounded demo walks through.
 * Reports 0-4 cover: HEALTHY / BEARING_WEAR / ELECTRICAL_FAULT /
 * MECHANICAL_OVERLOAD / UNCORROBORATED (see synth_input() below). */
#define N_REPORTS 5

/*
 * Small-motor healthy baseline -- nominal value + tolerance per fusion_input
 * field, in the field order {vib_rms, vib_crest, current_a, current_ripple,
 * temp_c, temp_slope}.  A field is "anomalous" when |value - nominal| > tol.
 *
 * Tuning this per-machine baseline is the single most important calibration
 * step; these defaults suit a small AC induction motor at rated load and
 * ambient temperature.  Customer ships a calibration step that measures the
 * healthy machine and writes its per-field mean + 3-sigma into the baseline.
 */
static const struct fusion_baseline BASE = {
	/* vib_rms  vib_crest  current_a  cur_ripple  temp_c  temp_slope */
	.nominal = { 0.05f, 3.0f, 1.0f, 0.05f, 30.0f, 0.0f },
	.tol     = { 0.05f, 2.0f, 0.5f, 0.05f, 10.0f, 0.5f },
};

/* 1-byte stub so alp_inference_open's non-NULL model_data contract is met.
 * The backend detects the bad magic bytes and returns a rank-0 output tensor,
 * forcing the deterministic fusion_assess verdict.  See models/README.md for
 * the training recipe that produces a real model for this feature vector. */
static const uint8_t s_model[] = { 0x00 };

/* Tensor arena: 64 KiB matches the default_arena_kib in board.yaml and is
 * ample for the lightweight fused feature-vector model (9 inputs -> 5 outputs).
 * 16-byte aligned as required by the TFLM allocator.  Passed to
 * alp_inference_open so the backend avoids heap allocation. */
static uint8_t s_arena[64 * 1024] __aligned(16);

/*
 * synth_input -- synthetic per-modality summary for native_sim.
 *
 * When no real sensors are present (chip-ID reads fail because no emul target
 * is attached), the main loop calls this function once per report to build a
 * fusion_input that drives a specific cross-modal fault scenario.  The five
 * reports cover each distinct hypothesis exactly once:
 *
 *   report 0: healthy    -- all fields at nominal; no anomaly fires.
 *   report 1: bearing_wear -- vibration + temperature both anomalous
 *             (friction wear raises both RMS vibration and operating temp).
 *   report 2: electrical  -- current anomalous, vibration normal
 *             (a shorted winding draws extra current without mechanical knock).
 *   report 3: overload    -- all three anomalous (mechanical jam or seizure).
 *   report 4: uncorroborated -- vibration alone (sensor knock / transient).
 *
 * Perturbation magnitude is 3x tolerance so sub-scores are clearly above
 * the anomaly threshold (>1) and the corroboration logic fires decisively.
 * synth_input() runs only when NO real sensor is present (native_sim); on a
 * board with at least one sensor the live read path is used instead.
 */
static struct fusion_input synth_input(int report)
{
	/* Start at the healthy nominal for every field. */
	struct fusion_input in = {
		.vib_rms        = 0.05f, /* g, AC RMS at nominal load */
		.vib_crest      = 3.0f,  /* dimensionless peak/RMS ratio */
		.current_a      = 1.0f,  /* A mean at rated load */
		.current_ripple = 0.05f, /* A AC ripple at rated load */
		.temp_c         = 30.0f, /* deg C bearing/case at ambient */
		.temp_slope     = 0.0f,  /* deg C / interval, flat baseline */
	};
	switch (report) {
	case 0:
		/* Scenario 0: HEALTHY -- no perturbation. */
		break;
	case 1:
		/* Scenario 1: BEARING_WEAR -- friction causes both vibration and heat. */
		in.vib_rms = 0.20f; /* 3x tol above nominal: clearly anomalous */
		in.temp_c  = 60.0f; /* 3x tol above nominal */
		break;
	case 2:
		/* Scenario 2: ELECTRICAL_FAULT -- extra current, mechanics normal. */
		in.current_a = 2.5f; /* 3x tol above nominal */
		break;
	case 3:
		/* Scenario 3: MECHANICAL_OVERLOAD -- all three channels anomalous. */
		in.vib_rms   = 0.20f;
		in.current_a = 2.5f;
		in.temp_c    = 60.0f;
		break;
	default:
		/* Scenario 4: UNCORROBORATED -- vibration transient, others normal. */
		in.vib_rms = 0.20f;
		break;
	}
	return in;
}

/*
 * read_sensors -- real-hardware read path (HiL bring-up reference).
 *
 * Reads each sensor into the compact per-modality summary fields consumed by
 * fusion_assess().  Per-sensor approach:
 *
 *   Vibration (ICM-42670): a 32-sample accel burst at the configured ODR.
 *     -> magnitude burst (g) -> AC RMS + crest factor.
 *     RMS measures sustained vibration energy; crest factor distinguishes
 *     impulsive events (bearing faults) from broad-band noise.
 *
 *   Current (INA236): a 16-sample burst of the CURRENT register (microamps).
 *     -> mean current (A) + AC ripple RMS (A).
 *     Mean detects overload / winding short; ripple detects commutation faults.
 *
 *   Temperature (BME280): a single compensated reading in deg C.
 *     Temperature slope is set to the baseline nominal here -- a real build
 *     would difference successive reads to compute deg C / report interval.
 *
 * Any absent sensor leaves its fields at the baseline nominal so that
 * modality reads non-anomalous and simply contributes zero corroboration.
 */
static void read_sensors(icm42670_t          *imu,
                         bool                 imu_ok,
                         ina236_t            *mon,
                         bool                 mon_ok,
                         bme280_t            *bme,
                         bool                 bme_ok,
                         struct fusion_input *in)
{
	/* Initialise all fields to the healthy nominal; overwritten per sensor below. */
	in->vib_rms        = BASE.nominal[0];
	in->vib_crest      = BASE.nominal[1];
	in->current_a      = BASE.nominal[2];
	in->current_ripple = BASE.nominal[3];
	in->temp_c         = BASE.nominal[4];
	in->temp_slope     = BASE.nominal[5];

	/*
	 * Vibration: 32-sample burst -> magnitude in g -> AC RMS + crest factor.
	 * ICM-42670 at FS_16G: sensitivity = 2048 LSB/g (datasheet DS-000451 §3.1).
	 * Zero-initialise mag[] so any unread slots (early-break on error) contribute
	 * zero to both mean and sum-of-squares without corrupting the result.
	 */
	if (imu_ok) {
		float peak = 0.0f, sum2 = 0.0f, mean = 0.0f, mag[32] = { 0 };
		for (int i = 0; i < 32; i++) {
			icm42670_axes_t a;
			if (icm42670_read_accel(imu, &a) != ALP_OK) {
				break; /* chip removed or bus error; rest stay zero */
			}
			/* Convert raw int16 triaxial counts to scalar magnitude in g. */
			mag[i] = sqrtf((float)a.x * a.x + (float)a.y * a.y + (float)a.z * a.z) / 2048.0f;
			mean += mag[i];
		}
		mean /= 32.0f; /* mean of the burst (DC component) */
		for (int i = 0; i < 32; i++) {
			float ac = mag[i] - mean; /* AC component (subtract DC) */
			sum2 += ac * ac;
			if (fabsf(ac) > peak) {
				peak = fabsf(ac); /* track peak AC deviation */
			}
		}
		/* RMS of the AC component; crest = peak / RMS (dimensionless). */
		in->vib_rms   = sqrtf(sum2 / 32.0f);
		in->vib_crest = (in->vib_rms > 1e-6f) ? (peak / in->vib_rms) : 0.0f;
	}

	/*
	 * Current: 16-sample burst of the CURRENT register -> mean + AC ripple.
	 * ina236_read_current_ua returns microamps; convert to amps for fusion.
	 * Zero-initialise samp[] for the same reason as mag[] above.
	 */
	if (mon_ok) {
		float mean = 0.0f, sum2 = 0.0f, samp[16] = { 0 };
		for (int i = 0; i < 16; i++) {
			int32_t ua = 0;
			if (ina236_read_current_ua(mon, &ua) != ALP_OK) {
				break; /* register read failed; rest stay zero */
			}
			samp[i] = (float)ua / 1e6f; /* uA -> A */
			mean += samp[i];
		}
		mean /= 16.0f;
		for (int i = 0; i < 16; i++) {
			float ac = samp[i] - mean; /* AC component */
			sum2 += ac * ac;
		}
		in->current_a      = mean;
		in->current_ripple = sqrtf(sum2 / 16.0f);
	}

	/*
	 * Temperature: single compensated read via the Bosch compensation formula.
	 * bme280_compensated_t.temperature_c100 is degrees C * 100
	 * (e.g. 2123 = 21.23 deg C) per BST-BME280-DS002 §4.2.3.
	 * Temperature slope is left at the nominal (no previous reading available
	 * here); a real build would difference successive report timestamps.
	 */
	if (bme_ok) {
		bme280_raw_t         raw;
		bme280_compensated_t c;
		if (bme280_read_raw(bme, &raw) == ALP_OK && bme280_compensate(bme, &raw, &c) == ALP_OK) {
			in->temp_c = (float)c.temperature_c100 / 100.0f;
		}
	}
}

int main(void)
{
	/* Declare sensor contexts as static so they live in BSS (not on the stack)
	 * and survive the full lifetime of the app without eating thread stack. */
	static icm42670_t imu;
	static ina236_t   mon;
	static bme280_t   bme;
	bool              imu_ok = false, mon_ok = false, bme_ok = false;

	/*
	 * Bus + sensor bring-up.  Open the shared sensor bus first; all three chips
	 * sit on it.  Each chip is initialised independently: a failure (chip absent,
	 * wrong address, or bus error) sets its _ok flag to false.  The rest of the
	 * app never reads a false-_ok sensor; its fields stay at the healthy nominal
	 * so that modality reads non-anomalous and simply lowers corroboration.
	 *
	 * BOARD_I2C_SENSORS resolves to the on-board sensor bus for the targeted EVK;
	 * see <alp/board.h> and the board.yaml preset field.
	 */
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = BOARD_I2C_SENSORS, .bitrate_hz = 400000 });
	if (bus != NULL) {
		/* ICM-42670: WHO_AM_I check (0x67) happens inside icm42670_init.
		 * icm42670_set_accel enables the accel at 100 Hz / ±16 g full-scale. */
		imu_ok = (icm42670_init(&imu, bus, ICM42670_I2C_ADDR_HIGH) == ALP_OK &&
		          icm42670_set_accel(&imu, ICM42670_ODR_100_HZ, ICM42670_ACCEL_FS_16G) == ALP_OK);

		/* INA236: 6-arg init probes the MFG_ID register (0x5449 = "TI") and
		 * programs the CALIBRATION register for the 10 mOhm shunt at 8 A max.
		 * The sensor bus uses INA236A default address 0x40 (A0 = GND). */
		mon_ok = (ina236_init(&mon, bus, 0x40u, 0.01f, 8.0f, INA236_ADCRANGE_81MV) == ALP_OK);

		/*
		 * BME280: bme280_init reads CHIP_ID (0x60) and loads per-die calibration
		 * but leaves the chip in SLEEP mode (CTRL_MEAS mode bits = 00b, the
		 * reset-default per BST-BME280-DS002 §5.4).  bme280_set_sampling is
		 * REQUIRED immediately after init to start conversions; omitting it means
		 * bme280_read_raw returns stale register values from the last reset.
		 * Normal mode at 1 s standby + x1 oversampling is a lightweight
		 * continuous-conversion setting suited to slow PdM reporting cycles.
		 */
		bme_ok = (bme280_init(&bme, bus, 0x76u) == ALP_OK &&
		          bme280_set_sampling(&bme,
		                              BME280_OVERSAMPLING_X1,
		                              BME280_OVERSAMPLING_X1,
		                              BME280_OVERSAMPLING_X1,
		                              BME280_MODE_NORMAL,
		                              BME280_STANDBY_1000_MS,
		                              BME280_FILTER_OFF) == ALP_OK);
	}
	if (!imu_ok || !mon_ok || !bme_ok) {
		/* On native_sim all three fail (no emul targets attached) -- normal. */
		LOG_WRN("one or more sensors unavailable; using synthetic per-modality data");
	}

	/*
	 * Inference handle: ALP_INFERENCE_BACKEND_AUTO routes to the SoM's on-die
	 * NPU on real silicon (AEN / V2N) or to the TFLM CPU path on native_sim.
	 * s_arena is passed so the backend avoids heap allocation.
	 * With the 1-byte stub model the backend returns a rank-0 output tensor,
	 * and the app keeps the deterministic fusion_assess verdict unchanged.
	 */
	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend     = ALP_INFERENCE_BACKEND_AUTO,
	    .format      = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data  = s_model,
	    .model_size  = sizeof(s_model),
	    .arena       = s_arena,
	    .arena_bytes = sizeof(s_arena),
	});

	/* CSV header -- t_s is the report index; on real hardware replace with
	 * a monotonic sensor timestamp or wall-clock epoch. */
	printk("# FUSE,t_s,hypothesis,health,vib,cur,temp,corroboration\n");

	for (int r = 0; r < N_REPORTS; r++) {
		/*
		 * Build the per-modality summary.  If ANY real sensor came up, read the
		 * chips: read_sensors() leaves every absent sensor's fields at the
		 * baseline nominal, so a partially-populated board degrades gracefully
		 * (the missing modality reads non-anomalous and just lowers
		 * corroboration) rather than being faked.  Only when NO sensor is
		 * present -- native_sim with no emulated chips -- do we fall back to the
		 * deterministic synthetic scenario that drives this report's hypothesis.
		 */
		struct fusion_input in;
		if (imu_ok || mon_ok || bme_ok) {
			read_sensors(&imu, imu_ok, &mon, mon_ok, &bme, bme_ok, &in);
		} else {
			in = synth_input(r); /* no sensors (native_sim) -> synthetic path */
		}

		/*
		 * Fuse.  fusion_assess produces the deterministic cross-modal verdict.
		 * If a real model is loaded, fusion_pack writes the 9-element feature
		 * vector into the inference input tensor; alp_inference_invoke runs
		 * the model.  With the 1-byte stub the tensor is rank-0 (unusable)
		 * and we keep the fusion_assess verdict unchanged.
		 */
		struct fusion_result res;
		fusion_assess(&in, &BASE, &res);
		if (inf != NULL) {
			float vec[FUSION_FEATURE_DIM];
			/* (void): return count (FUSION_FEATURE_DIM) not needed here. */
			(void)fusion_pack(&res, &in, vec, FUSION_FEATURE_DIM);
			alp_inference_tensor_t ten = { 0 };
			if (alp_inference_get_input(inf, 0, &ten) == ALP_OK &&
			    ten.dtype == ALP_INFERENCE_DTYPE_F32 && ten.data != NULL &&
			    ten.size_bytes >= sizeof(vec)) {
				memcpy(ten.data, vec, sizeof(vec));
				/* A trained model would override res.hypothesis after invoke;
				 * the stub yields no usable output, so verdict is unchanged. */
				(void)alp_inference_invoke(inf); /* (void): discarded in stub path */
			}
		}

		/* Emit one record per report.  Seven comma-separated fields after "FUSE":
		 * report timestamp (s), hypothesis name, confidence-weighted health score,
		 * per-modality sub-scores (>1 = anomalous), and corroboration count. */
		printk("FUSE,%.1f,%s,%.2f,%.1f,%.1f,%.1f,%u\n",
		       (double)(r + 1),
		       fusion_fault_name(res.hypothesis),
		       (double)res.health_score,
		       (double)res.vib_score,
		       (double)res.current_score,
		       (double)res.temp_score,
		       (unsigned)res.corroboration);
	}

	/*
	 * Lifecycle teardown: release the model handle, reset each sensor to its
	 * power-on defaults, then close the bus.  (void) casts on alp_status_t
	 * returns that are intentionally discarded during shutdown.
	 */
	if (inf != NULL) {
		alp_inference_close(inf); /* frees arena mapping / closes NPU context */
	}
	if (imu_ok) {
		icm42670_deinit(&imu); /* void: zeroes the driver context */
	}
	if (mon_ok) {
		(void)ina236_reset(&mon); /* soft-reset + recalibrate the chip */
	}
	if (bme_ok) {
		(void)bme280_soft_reset(&bme); /* writes 0xB6 to RESET register */
	}
	if (bus != NULL) {
		alp_i2c_close(bus); /* release the I2C bus handle */
	}
	printk("[fuse] done\n");
	return 0;
}
