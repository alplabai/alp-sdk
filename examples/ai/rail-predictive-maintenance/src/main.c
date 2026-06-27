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

static void emit_header(void)
{
	printk("# RAIL,chainage_m,lat,lon,speed_mps,class,severity,dom_freq_hz,"
	       "rail_wavelength_m,fix\n");
}

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

/* Classify a feature vector via the AI model, else the deterministic fallback. */
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
	rail_pos_init(&c.pos, 25.0f);
	seg_reset(&c);

	/* --- I2C accelerometer (tolerate a missing chip on native_sim). --- */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = BOARD_I2C_SENSORS,
	    .bitrate_hz = 400000,
	});
	if (bus != NULL && icm42670_init(&c.imu, bus, IMU_I2C_ADDR) == ALP_OK &&
	    icm42670_set_accel(&c.imu, ICM42670_ODR_800_HZ, ICM42670_ACCEL_FS_2G) == ALP_OK) {
		c.imu_ok = true;
	} else {
		LOG_WRN("ICM-42670 unavailable; using synthetic vibration");
	}

	/* --- GNSS UART (tolerate absence on native_sim -> canned track). --- */
	c.gps_uart = alp_uart_open(&(alp_uart_config_t){
	    .port_id  = E1M_UART1,
	    .baudrate = 9600,
	});
	if (c.gps_uart != NULL) {
		(void)ublox_neo_m9n_init(&c.gps, c.gps_uart);
	} else {
		LOG_WRN("GNSS UART unavailable; replaying canned NMEA track");
	}

	/* --- AI model: stub backend; fallback classifier runs on native_sim. --- */
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

	for (int w = 0; w < RAIL_DEMO_WINDOWS; w++) {
		/* Fill one window. */
		rail_feat_state_reset(&st);
		for (int i = 0; i < RAIL_WINDOW_N; i++) {
			float sample;
			if (c.imu_ok) {
				icm42670_axes_t ax;
				if (icm42670_read_accel(&c.imu, &ax) == ALP_OK) {
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
			line = s_canned_track[track_i % ARRAY_SIZE(s_canned_track)];
			track_i++;
		}
		double plat = lat, plon = lon;
		float  pspd = speed;
		bool   pfix = false;
		if (rail_pos_parse_rmc(line, &plat, &plon, &pspd, &pfix) && pfix) {
			lat   = plat;
			lon   = plon;
			speed = pspd;
			fix   = true;
		}

		/* Features + classification. */
		struct rail_features f;
		rail_feat_extract(&st, RAIL_ODR_HZ, speed, &f);
		struct rail_verdict v = classify(&c, &f);

		/* Track the worst verdict in the current segment. */
		if (v.severity > c.worst_sev) {
			c.worst_sev  = v.severity;
			c.worst_cls  = v.cls;
			c.worst_feat = f;
		}

		/* Advance position; emit a record on segment change. */
		if (rail_pos_update(&c.pos, lat, lon, fix)) {
			emit_record(&c, lat, lon, speed, fix);
			seg_reset(&c);
		}
	}

	alp_inference_close(c.inf);
	if (c.imu_ok) {
		icm42670_deinit(&c.imu);
	}
	alp_i2c_close(bus);
	printk("[rail] done\n");
	return 0;
}
