/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * motor-current-signature
 * =======================
 *
 * DC motor / load current-signature health monitor.  Pipeline:
 *
 *   INA236 current/voltage/power (I2C, ~200 Hz read_all) --256-sample window-->
 *     current_features (mean/ripple/crest/slope/power) -> current_classify
 *       (OFF/NORMAL/INRUSH/OVERLOAD/STALL) + <alp/inference.h> anomaly score
 *       (deterministic fallback) --> one CURR record per window.
 *
 * Honest scope: DC-rail / brushed-DC-motor current-signature analysis (the
 * INA236's domain) -- NOT AC-mains NILM.  Monitor-only.  The model is a stub
 * (see models/README.md); with no model the deterministic classifier + anomaly
 * fallback run.  At 200 Hz the resolvable ripple is < 100 Hz (Nyquist);
 * sensorless RPM from faster commutation ripple is bench-gated.
 */
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/board.h"
#include "alp/inference.h"
#include "alp/peripheral.h"
#include "alp/chips/ina236.h"

#include "current_features.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(curr, LOG_LEVEL_INF);

#define INA236_ADDR       0x40u
#define INA236_SHUNT_OHMS 0.01f /* 10 mOhm sense resistor */
#define INA236_MAX_A      8.0f
#define N_WINDOWS         5

static const struct curr_config CFG = {
	.off_a = 0.05f, .overload_a = 2.5f, .ripple_min_a = 0.05f, .inrush_slope_a = 1.0f
};

/* 1-byte stub so alp_inference_open's non-NULL contract is met; an unusable
 * tensor forces the deterministic anomaly fallback.  See models/README.md. */
static const uint8_t s_model[] = { 0x00 };

/* Synthetic current per window: one window per operating state. */
static struct curr_sample synth_sample(int window, int i)
{
	float              t   = (float)i / CURR_SR_HZ;
	float              rip = 0.1f * sinf(2.0f * (float)M_PI * 40.0f * t);
	struct curr_sample s   = { .current_a = 0.0f, .bus_v = 12.0f, .power_w = 0.0f };
	switch (window) {
	case 0: /* OFF */
		s.current_a = 0.01f;
		break;
	case 1: /* NORMAL: 1 A + ripple */
		s.current_a = 1.0f + rip;
		break;
	case 2: { /* INRUSH: 5 A decaying to 1 A */
		float frac  = (float)i / (float)(CURR_WINDOW_N - 1);
		s.current_a = 5.0f - 4.0f * frac;
		break;
	}
	case 3: /* OVERLOAD: 3 A + ripple */
		s.current_a = 3.0f + rip;
		break;
	default: /* STALL: 3 A, no ripple */
		s.current_a = 3.0f;
		break;
	}
	s.power_w = s.current_a * s.bus_v;
	return s;
}

static float anomaly_score(alp_inference_t *inf, const struct curr_features *f)
{
	if (inf != NULL) {
		float vec[CURR_FEATURE_DIM];
		(void)curr_feat_pack(f, vec, CURR_FEATURE_DIM);
		alp_inference_tensor_t in = { 0 };
		if (alp_inference_get_input(inf, 0, &in) == ALP_OK && in.dtype == ALP_INFERENCE_DTYPE_F32 &&
		    in.data != NULL && in.size_bytes >= sizeof(vec)) {
			memcpy(in.data, vec, sizeof(vec));
			if (alp_inference_invoke(inf) == ALP_OK) {
				alp_inference_tensor_t out = { 0 };
				if (alp_inference_get_output(inf, 0, &out) == ALP_OK &&
				    out.dtype == ALP_INFERENCE_DTYPE_F32 && out.data != NULL &&
				    out.size_bytes >= sizeof(float)) {
					return ((const float *)out.data)[0];
				}
			}
		}
	}
	return curr_anomaly_fallback(f, &CFG);
}

int main(void)
{
	static ina236_t                 mon;
	static struct curr_window_state win;
	bool                            mon_ok = false;

	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = BOARD_I2C_SENSORS, .bitrate_hz = 400000 });
	if (bus != NULL &&
	    ina236_init(
	        &mon, bus, INA236_ADDR, INA236_SHUNT_OHMS, INA236_MAX_A, INA236_ADCRANGE_81MV) ==
	        ALP_OK) {
		mon_ok = true;
	} else {
		LOG_WRN("INA236 unavailable; using synthetic current");
	}

	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	printk("# CURR,t_s,state,mean_a,mean_w,ripple_hz,anomaly_score\n");

	for (int w = 0; w < N_WINDOWS; w++) {
		curr_window_reset(&win);
		for (int i = 0; i < CURR_WINDOW_N; i++) {
			struct curr_sample s;
			if (mon_ok) {
				ina236_sample_t r;
				if (ina236_read_all(&mon, &r) == ALP_OK) {
					s.current_a = (float)r.current_ua / 1e6f;
					s.bus_v     = (float)r.bus_mv / 1e3f;
					s.power_w   = (float)r.power_uw / 1e6f;
				} else {
					s = synth_sample(w, i);
				}
			} else {
				s = synth_sample(w, i);
			}
			curr_window_push(&win, s);
		}

		struct curr_features f;
		curr_feat_extract(&win, CURR_SR_HZ, &f);
		curr_state_t st = current_classify(&f, &CFG);
		float        an = anomaly_score(inf, &f);

		printk("CURR,%.2f,%s,%.2f,%.1f,%.1f,%.2f\n",
		       (double)(w * 1.28f),
		       curr_state_name(st),
		       (double)f.mean_current_a,
		       (double)f.mean_power_w,
		       (double)f.ripple_freq_hz,
		       (double)an);
	}

	if (inf != NULL) {
		alp_inference_close(inf);
	}
	if (mon_ok) {
		(void)ina236_reset(&mon);
		ina236_deinit(&mon);
	}
	if (bus != NULL) {
		alp_i2c_close(bus);
	}
	printk("[curr] done\n");
	return 0;
}
