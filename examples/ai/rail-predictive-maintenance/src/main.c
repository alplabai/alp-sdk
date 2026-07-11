/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail-predictive-maintenance
 * ===========================
 *
 * Train-mounted rail-condition survey.  Pipeline:
 *
 *   ICM-42670 accel (I2C) --256-sample window @ 800 Hz-->
 *     rail_features (DSP: RMS/crest/kurtosis/FFT bands/dom-freq/wavelength)
 *       --feature vector--> <alp/inference.h> AI classifier
 *                           (deterministic fallback when no model)
 *   NEO-M9N GNSS (UART NMEA) --> rail_position (haversine chainage + 25 m
 *                                segment binning)
 *     --> one CSV record per segment (worst class in the segment wins).
 *
 * The model is a stub: drop a Vela-compiled (AEN / Ethos-U) or DX-M1
 * (V2N) .tflite into models/ and point alp_inference_open at it.  With no
 * model (native_sim), the deterministic fallback classifier runs so the
 * survey still produces sensible geotagged output.
 *
 * Asset boundary: this surveys the RAIL (track), not the wheel.  Wheel
 * flats (periodic at wheel-rotation frequency) are out of scope.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "alp/board.h"
#include "alp/inference.h"
#include "alp/peripheral.h"
#include "alp/chips/icm42670.h"
#include "alp/chips/ublox_neo_m9n.h"

#include "rail_features.h"
#include "rail_position.h"

LOG_MODULE_REGISTER(rail_pdm, LOG_LEVEL_INF);

/* ICM-42670 ±2 g sensitivity: 16384 LSB/g (datasheet DS-000451). */
#define ICM_LSB_PER_G 16384.0f
#define IMU_I2C_ADDR  ICM42670_I2C_ADDR_HIGH /* E1M EVK straps AP_AD0 high. */

/* Number of windows to process in the bounded demo run. */
#define RAIL_DEMO_WINDOWS 64

/* 1-byte stub model -- real deployment drops a Vela / DX-M1 .tflite here.
 * The stub causes alp_inference_open to return a handle; the invoke /
 * get_output path produces an unusable tensor, so classify() falls back to
 * the deterministic rail_classify_fallback().  native_sim proves the full
 * pipeline end-to-end without a trained model. */
static const uint8_t s_model[] = { 0x00 };

/* Canned native_sim track. Checksums are placeholders (*00) -- rail_pos_parse_rmc
 * is checksum-agnostic by design; real NMEA from the GNSS carries valid checksums. */
static const char *const s_canned_track[] = {
	"$GNRMC,083559.00,A,5919.99990,N,01803.74400,E,12.0,0.0,250626,,,A*00",
	"$GNRMC,083600.00,A,5920.01000,N,01803.74400,E,12.0,0.0,250626,,,A*00",
	"$GNRMC,083601.00,A,5920.02000,N,01803.74400,E,12.0,0.0,250626,,,A*00",
	"$GNRMC,083602.00,A,5920.03000,N,01803.74400,E,12.0,0.0,250626,,,A*00",
};

/* Synthetic vibration when no real IMU answers: alternate clean / tone /
 * impulse so the demo emits a mix of classes.
 * Period-64 joint impulses give crest~7.94 and high kurtosis, which
 * exceeds the fallback classifier's crest>6 threshold for JOINT_WELD. */
static float synth_sample(int window, int i)
{
	switch (window % 3) {
	case 0: /* clean */
		return 0.001f;
	case 1: /* corrugation tone ~120 Hz */
		return sinf(2.0f * (float)M_PI * 120.0f * (float)i / RAIL_ODR_HZ);
	default: /* joint impulses -- period 64 -> crest~7.94, kurtosis high */
		return (i % 64 == 0) ? 1.0f : 0.0f;
	}
}

/*
 * All cross-window state for one survey run lives here so main() can pass a
 * single pointer around instead of threading five separate locals through
 * classify()/emit_record().  imu_ok/gps_uart track whether the real sensors
 * answered at boot -- once false, the run stays on synthetic data / the
 * canned NMEA track for its whole lifetime (no per-window re-probe).
 */
struct rail_ctx {
	icm42670_t       imu;
	bool             imu_ok;
	ublox_neo_m9n_t  gps;
	alp_uart_t      *gps_uart;
	alp_inference_t *inf;

	struct rail_pos_state pos;
	/* worst verdict seen in the current segment. */
	rail_class_t         worst_cls;
	float                worst_sev;
	struct rail_features worst_feat;
};

/* Leading '#' marks this as a comment line to CSV consumers (most CSV
 * readers and MQTT/GIS ingest scripts skip '#'-prefixed lines by
 * convention), so the field-name header can live in the same stream as
 * the data records below without a separate schema file. */
static void emit_header(void)
{
	printk("# RAIL,chainage_m,lat,lon,speed_mps,class,severity,dom_freq_hz,"
	       "rail_wavelength_m,fix\n");
}

/* Field order here must track emit_header() above -- there is no named-field
 * encoding, so a reordered printk here silently desyncs the CSV schema. */
static void emit_record(const struct rail_ctx *c, double lat, double lon, float speed, bool fix)
{
	printk("RAIL,%.1f,%.6f,%.6f,%.1f,%s,%.2f,%.1f,%.4f,%d\n",
	       c->pos.chainage_m,
	       lat,
	       lon,
	       (double)speed,
	       rail_class_name(c->worst_cls),
	       (double)c->worst_sev,
	       (double)c->worst_feat.dom_freq_hz,
	       (double)c->worst_feat.rail_wavelength_m,
	       fix ? 1 : 0);
}

/*
 * Classify a feature vector via the AI model, else the deterministic fallback.
 *
 * c->inf is non-NULL whenever alp_inference_open() accepted s_model, which
 * happens even for the 1-byte stub (open only checks the container, not that
 * the payload is a real graph).  So the dtype/size_bytes checks below are the
 * real gate: they catch a stub or mismatched model at RUN time and drop to
 * rail_classify_fallback() instead of reading/writing past a tensor that
 * does not exist.  On native_sim with the stub model every one of these
 * checks fails and every window takes the fallback path -- that is the
 * expected, exercised behaviour, not an error.
 */
static struct rail_verdict classify(struct rail_ctx *c, const struct rail_features *f)
{
	if (c->inf != NULL) {
		float vec[RAIL_FEATURE_DIM];
		(void)rail_feat_pack(f, vec, RAIL_FEATURE_DIM);

		alp_inference_tensor_t in = { 0 };
		if (alp_inference_get_input(c->inf, 0, &in) == ALP_OK && in.data != NULL &&
		    in.dtype == ALP_INFERENCE_DTYPE_F32 && in.size_bytes >= sizeof(vec)) {
			memcpy(in.data, vec, sizeof(vec));
			if (alp_inference_invoke(c->inf) == ALP_OK) {
				alp_inference_tensor_t out = { 0 };
				if (alp_inference_get_output(c->inf, 0, &out) == ALP_OK &&
				    out.dtype == ALP_INFERENCE_DTYPE_F32 && out.data != NULL &&
				    out.size_bytes >= RAIL_CLASS_COUNT * sizeof(float)) {
					/* argmax over the per-class scores selects the predicted
					 * class; severity is derived from 1 - P(HEALTHY) rather
					 * than the winning score, so a confident non-healthy
					 * call and a low-margin one still rank by how far the
					 * model is from calling it healthy. */
					const float *scores = (const float *)out.data;
					int          best   = 0;
					float        bestv  = scores[0];
					for (int k = 1; k < RAIL_CLASS_COUNT; k++) {
						if (scores[k] > bestv) {
							bestv = scores[k];
							best  = k;
						}
					}
					struct rail_verdict v = { (rail_class_t)best, 1.0f - scores[RAIL_HEALTHY] };
					/* Clamp: scores are not guaranteed to be a normalised
					 * softmax (a raw-logit model would violate [0,1]), and
					 * downstream severity display/thresholds assume [0,1]. */
					if (v.severity < 0.0f) {
						v.severity = 0.0f;
					}
					if (v.severity > 1.0f) {
						v.severity = 1.0f;
					}
					return v;
				}
			}
		}
	}
	return rail_classify_fallback(f);
}

static void seg_reset(struct rail_ctx *c)
{
	c->worst_cls = RAIL_HEALTHY;
	c->worst_sev = -1.0f;
}

int main(void)
{
	static struct rail_ctx c;
	struct rail_feat_state st;

	memset(&c, 0, sizeof(c));
	rail_feat_state_reset(&st);
	/* 25 m along-track segment length: matches the README's contract of one
	 * CSV record per 25 m of track, independent of survey speed. */
	rail_pos_init(&c.pos, 25.0f);
	seg_reset(&c);

	/* --- I2C accelerometer (tolerate a missing chip on native_sim). --- */
	/* BOARD_I2C_SENSORS is the board-level alias for the sensor I2C bus
	 * (see board.yaml pins:) -- using it instead of a raw bus index keeps
	 * this source portable across the E1M-EVK and E1M-X-EVK carriers, which
	 * wire the ICM-42670 to different underlying I2C controllers. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = BOARD_I2C_SENSORS,
	    .bitrate_hz = 400000,
	});
	if (bus != NULL && icm42670_init(&c.imu, bus, IMU_I2C_ADDR) == ALP_OK &&
	    icm42670_set_accel(&c.imu, ICM42670_ODR_800_HZ, ICM42670_ACCEL_FS_2G) == ALP_OK) {
		c.imu_ok = true;
	} else {
		/* Open or config failure is expected on native_sim (no real I2C
		 * device answers) -- the demo keeps running on synth_sample()
		 * rather than treating this as fatal. */
		LOG_WRN("ICM-42670 unavailable; using synthetic vibration");
	}

	/* --- GNSS UART (tolerate absence on native_sim -> canned track). --- */
	c.gps_uart = alp_uart_open(&(alp_uart_config_t){
	    .port_id  = ALP_E1M_UART1,
	    .baudrate = 9600,
	});
	if (c.gps_uart != NULL) {
		(void)ublox_neo_m9n_init(&c.gps, c.gps_uart);
	} else {
		/* No UART backend -> every window below falls through to
		 * s_canned_track so the pipeline still exercises chainage/segment
		 * binning end-to-end without live GNSS. */
		LOG_WRN("GNSS UART unavailable; replaying canned NMEA track");
	}

	/* --- AI model: stub backend; fallback classifier runs on native_sim. --- */
	/* s_model is a 1-byte placeholder, not a trained graph: it exists only
	 * so alp_inference_open's non-NULL model_data contract is satisfied.
	 * classify() detects the resulting unusable tensor and calls
	 * rail_classify_fallback() every time -- see classify() above. */
	c.inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	emit_header();

	double  lat = 0.0, lon = 0.0;
	float   speed = 0.0f;
	bool    fix   = false;
	uint8_t nmea[128];
	size_t  track_i = 0;

	/* Each outer iteration is one 256-sample IMU window (320 ms of vibration
	 * at 800 Hz), paired with one GNSS sentence -- the demo treats "one
	 * window" and "one GNSS read" as roughly synchronous, which is close
	 * enough at survey speed since GNSS updates far slower than the IMU. */
	for (int w = 0; w < RAIL_DEMO_WINDOWS; w++) {
		/* Fill one window. */
		rail_feat_state_reset(&st);
		for (int i = 0; i < RAIL_WINDOW_N; i++) {
			float sample;
			if (c.imu_ok) {
				icm42670_axes_t ax;
				if (icm42670_read_accel(&c.imu, &ax) == ALP_OK) {
					/* Vector magnitude of all three axes, not a single axis:
					 * the ICM-42670's mount orientation on the bogie is not
					 * fixed by this example, so |a| stays valid regardless
					 * of which axis actually faces the rail. */
					float g = sqrtf((float)ax.x * ax.x + (float)ax.y * ax.y + (float)ax.z * ax.z) /
					          ICM_LSB_PER_G;
					sample  = g;
				} else {
					sample = synth_sample(w, i);
				}
			} else {
				sample = synth_sample(w, i);
			}
			rail_feat_window_push(&st, sample);
		}

		/* Position: one GNSS sentence per window. */
		size_t      len  = 0;
		const char *line = NULL;
		if (c.gps_uart != NULL &&
		    ublox_neo_m9n_read_nmea_line(&c.gps, nmea, sizeof(nmea), &len, 5) == ALP_OK) {
			line = (const char *)nmea;
		} else {
			/* No live sentence this window (uart absent, or the read timed
			 * out/returned nothing) -- replay the canned track so the demo
			 * still advances chainage deterministically on native_sim. */
			line = s_canned_track[track_i % ARRAY_SIZE(s_canned_track)];
			track_i++;
		}
		double plat = lat, plon = lon;
		float  pspd = speed;
		bool   pfix = false;
		if (rail_pos_parse_rmc(line, &plat, &plon, &pspd, &pfix) && pfix) {
			/* Only commit a fresh fix: an unfixed sentence (pfix == false)
			 * leaves lat/lon/speed at their last known values rather than
			 * resetting to 0, since this demo does not dead-reckon between
			 * fixes. */
			lat   = plat;
			lon   = plon;
			speed = pspd;
			fix   = true;
		}

		/* Features + classification. */
		struct rail_features f;
		rail_feat_extract(&st, RAIL_ODR_HZ, speed, &f);
		struct rail_verdict v = classify(&c, &f);

		/* Track the worst verdict in the current segment: a segment is
		 * reported once (on segment change, below), so multiple windows
		 * within the same 25 m stretch must be reduced to the single worst
		 * finding rather than emitting one record per window. */
		if (v.severity > c.worst_sev) {
			c.worst_sev  = v.severity;
			c.worst_cls  = v.cls;
			c.worst_feat = f;
		}

		/* Advance position; emit a record on segment change. rail_pos_update
		 * returns true exactly when the chainage crosses into a new 25 m
		 * segment, which is the trigger to flush the worst verdict
		 * accumulated for the segment just completed. */
		if (rail_pos_update(&c.pos, lat, lon, fix)) {
			emit_record(&c, lat, lon, speed, fix);
			seg_reset(&c);
		}
	}

	/* Teardown mirrors the open order above; guard each close on the
	 * corresponding _ok/handle so a partially-initialised run (e.g. IMU
	 * absent) does not deinit a chip that was never inited. */
	alp_inference_close(c.inf);
	if (c.imu_ok) {
		icm42670_deinit(&c.imu);
	}
	alp_i2c_close(bus);
	printk("[rail] done\n");
	return 0;
}
