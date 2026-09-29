/* src/platform/imu.c */
#include <stdint.h>

#include <alp/boards/alp_e1m_evk.h>
#include <alp/chips/bmi323.h>
#include <alp/peripheral.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "imu.h"

/*
 * Tilt is the SECOND control mode, for playing with the board in your hands.
 * The primary mode is body tracking (see vision/), because a player standing
 * in front of the camera cannot reach the board to tilt it.
 *
 * The BMI323 is on the CARRIER (EVK U13), not the SoM.  Bus and address come
 * from <alp/boards/alp_e1m_evk_routes.h> (pulled in via alp_e1m_evk.h):
 * EVK_I2C_BUS_SENSORS (== ALP_E1M_I2C0) and EVK_I2C_ADDR_BMI323 (0x68 -- the
 * 2026W36 respin batch; U13's SDO strap is tied to GND on this carrier rev).
 */

/* Q8 fixed point throughout: this runs every tick and the FPU is not enabled.
 * What a sample MEANS (thresholds, hysteresis, the takeover gesture) is
 * game/tilt.c's job, host-tested -- this file only reads the part. */
#define IMU_FAIL_LIMIT 5

/*
 * Counts-per-g at BMI323_ACCEL_FS_2G is 16384 (16-bit signed range split
 * across +-2 g, the standard Bosch accelerometer scaling for this full-scale
 * setting -- doubles/halves with each step of bmi323_accel_fs_t, per
 * metadata/chips/bmi323.yaml's ranges_g list).  Q8 wants 256 counts per g,
 * so raw / (16384 / 256) == raw / 64 lands a raw sample in Q8 g.
 */
#define BMI323_2G_RAW_PER_Q8 64

/*
 * A completed bus transaction is not proof of a real sample.  chips/bmi323/
 * bmi323.c documents 0x8000 (INT16_MIN) as the part's reset "no valid sample
 * yet" value on every axis (see that file's comment at bmi323_set_gyro()'s
 * tG,SU wait) -- bmi323_read_accel() returns ALP_OK on any transaction that
 * lands, sentinel or not, because the driver has no sentinel special-case.
 * A raw sample pinned at, or right beside, either rail is therefore not
 * tilt data: FS_2G is 16384 counts/g (see BMI323_2G_RAW_PER_Q8's derivation
 * above), so gravity alone on a handheld steering axis tops out around half
 * of full scale -- reaching all the way to +-32768 means a stuck/garbage
 * read, not a violent tilt.  The +-8-count margin catches INT16_MIN's
 * immediate neighbours the same way, in case a marginal transfer lands one
 * count off the documented value.
 */
#define BMI323_RAW_SATURATION_MARGIN 8

static bool bmi323_raw_is_invalid(int16_t v)
{
	return v <= (int16_t)(INT16_MIN + BMI323_RAW_SATURATION_MARGIN) ||
	       v >= (int16_t)(INT16_MAX - BMI323_RAW_SATURATION_MARGIN);
}

static bmi323_t   g_imu;
static alp_i2c_t *g_bus;
static bool       g_ok;
static unsigned   g_fails;

/* Bench-readable: the last sample handed to the game, rest-zeroed Q8 g
 * (256 == 1 g). A board sitting on its mount reads near (0, 0). */
volatile int16_t tr_imu_x_q8;
volatile int16_t tr_imu_y_q8;

/*
 * Rest-attitude zero: captured once, right after the accel config succeeds,
 * and subtracted from every later raw sample. Tilt mode is entered exactly
 * when the camera has already failed, i.e. while the board is sitting in
 * whatever fixture (or hand) put it in whatever attitude it happens to be
 * at -- not necessarily level. Without this, a fixture that merely holds
 * the board past TR_TILT_EDGE_Q8 (game/tilt.h) fires one gesture on the first
 * poll and then wedges: the dead-zone re-arm at raw-zero never lands (see
 * whole-branch review F4).
 *
 * Averaged over IMU_REST_SAMPLES valid reads IMU_REST_GAP_MS apart (32 x
 * 10 ms = ~0.32 s, a fresh sample per read at the 200 Hz ODR), so one
 * noisy or knocked first sample cannot bias the whole session. Invalid raw
 * samples (bmi323_raw_is_invalid()) are skipped; if none is valid the rest
 * stays (0,0) -- the unzeroed behaviour -- rather than being poisoned.
 * No slow re-zero while running: the one-shot average is enough for a
 * board sitting still on its mount; add a drift tracker if the bench ever
 * shows the level reference wandering.
 */
#define IMU_REST_SAMPLES 32
#define IMU_REST_GAP_MS  10

static int16_t g_rest_x, g_rest_y;

int tr_imu_open(void)
{
	alp_i2c_config_t cfg = ALP_I2C_CONFIG_DEFAULT(EVK_I2C_BUS_SENSORS);

	g_bus = alp_i2c_open(&cfg);
	if (g_bus == NULL) {
		printk("imu     : bus unavailable -- tilt mode disabled\n");
		return -1;
	}
	if (bmi323_init(&g_imu, g_bus, EVK_I2C_ADDR_BMI323) != ALP_OK) {
		printk("imu     : BMI323 init failed -- tilt mode disabled\n");
		alp_i2c_close(g_bus);
		return -1;
	}
	/* 200 Hz normal mode, acc_bw = ODR/2 (100 Hz), no averaging (the
	 * driver writes acc_bw 0 / acc_avg_num 0): a sample is <= 5 ms old when
	 * a 40 Hz frame reads it, filter group delay ~1 sample (~5 ms). P3c: was
	 * 100 Hz (<= 10 ms + ~10 ms). */
	if (bmi323_set_accel(&g_imu, BMI323_ODR_200_HZ, BMI323_ACCEL_FS_2G) != ALP_OK) {
		printk("imu     : accel config failed -- tilt mode disabled\n");
		alp_i2c_close(g_bus);
		return -1;
	}

	int32_t sum_x = 0, sum_y = 0, n = 0;

	for (int i = 0; i < IMU_REST_SAMPLES; i++) {
		bmi323_axes_t rest;

		k_msleep(IMU_REST_GAP_MS);
		if (bmi323_read_accel(&g_imu, &rest) == ALP_OK && !bmi323_raw_is_invalid(rest.x) &&
		    !bmi323_raw_is_invalid(rest.y)) {
			sum_x += rest.x;
			sum_y += rest.y;
			n++;
		}
	}
	g_rest_x = (n > 0) ? (int16_t)(sum_x / n) : 0;
	g_rest_y = (n > 0) ? (int16_t)(sum_y / n) : 0;
	printk("imu     : rest from %d/%d samples\n", (int)n, IMU_REST_SAMPLES);

	g_ok = true;
	printk("imu     : READY (tilt mode available)\n");
	return 0;
}

void tr_imu_read_q8(int16_t *x_q8, int16_t *y_q8)
{
	if (!g_ok) {
		/* No IMU (or given up): read as level, so the game never engages
		 * tilt play and an in-progress one walks away to attract. */
		tr_imu_x_q8 = 0;
		tr_imu_y_q8 = 0;
		*x_q8       = 0;
		*y_q8       = 0;
		return;
	}

	bmi323_axes_t a;
	bool          bad_read = (bmi323_read_accel(&g_imu, &a) != ALP_OK);

	/* A sentinel/saturated sample is not a bus error, but it is just as
	 * useless as one -- route it through the same give-up counter. See
	 * BMI323_RAW_SATURATION_MARGIN's comment. */
	if (!bad_read && (bmi323_raw_is_invalid(a.x) || bmi323_raw_is_invalid(a.y))) {
		bad_read = true;
	}

	if (bad_read) {
		/* Coast over a bad read on the last good sample (a fake "level"
		 * would re-arm game/tilt.c's latches and double-fire a gesture);
		 * give up after several so a wedged bus cannot blow the frame
		 * budget every tick for the rest of the run. */
		if (++g_fails >= IMU_FAIL_LIMIT) {
			g_ok = false;
			alp_i2c_close(g_bus); /* Give up for good: release the handle, not just the flag. */
			printk("imu     : %u consecutive read failures -- tilt input off\n", g_fails);
		}
	} else {
		g_fails = 0;
		/* Steer on x, pitch (jump/duck) on y, both relative to the rest
		 * attitude captured at tr_imu_open() -- see g_rest_x's comment and
		 * BMI323_2G_RAW_PER_Q8's derivation. */
		tr_imu_x_q8 = (int16_t)(((int32_t)a.x - g_rest_x) / BMI323_2G_RAW_PER_Q8);
		tr_imu_y_q8 = (int16_t)(((int32_t)a.y - g_rest_y) / BMI323_2G_RAW_PER_Q8);
	}
	*x_q8 = tr_imu_x_q8;
	*y_q8 = tr_imu_y_q8;
}
