/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * A concurrent close() must wake a call blocked in the backend (issue
 * #2977 item 3): camera capture, hwsem lock, audio read/write and BLE GATT
 * read each park forever here, and the matching close()/disconnect() has
 * to wake them with ALP_ERR_NOT_READY and finish.
 *
 * Each class registers a priority-255 test double (the reserved
 * test-double priority documented on ALP_BACKEND_REGISTER) whose blocking
 * op waits on a k_sem with no timeout.  The ONLY thing that can wake it
 * is the backend's cancel() hook, which the dispatcher must call after
 * marking the handle closing and before draining.  Without that hook
 * close() waits for the op to leave -- forever -- so every close here
 * runs in its own thread and the test joins it with a bound.
 */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <alp/audio.h>
#include <alp/backend.h>
#include <alp/ble.h>
#include <alp/camera.h>
#include <alp/mproc.h>

#include "backends/audio/audio_ops.h"
#include "backends/ble/ble_ops.h"
#include "backends/camera/camera_ops.h"
#include "backends/mproc/mproc_ops.h"

/* ------------------------------------------------------------------ */
/* Fake blocking                                                       */
/* ------------------------------------------------------------------ */

enum { SEM_CAM, SEM_HWSEM, SEM_AIN, SEM_AOUT, SEM_BLE, SEM_COUNT };
static struct k_sem g_sem[SEM_COUNT];

/* Block until cancel() gives the sem; report what a real backend must. */
static alp_status_t fake_block(int which, const bool *closing)
{
	if (__atomic_load_n(closing, __ATOMIC_ACQUIRE)) {
		return ALP_ERR_NOT_READY;
	}
	(void)k_sem_take(&g_sem[which], K_FOREVER);
	return __atomic_load_n(closing, __ATOMIC_ACQUIRE) ? ALP_ERR_NOT_READY : ALP_OK;
}

/* ---- camera ---- */
static alp_status_t
cam_open(const alp_camera_config_t *cfg, alp_camera_backend_state_t *st, alp_capabilities_t *caps)
{
	ARG_UNUSED(cfg);
	ARG_UNUSED(st);
	ARG_UNUSED(caps);
	return ALP_OK;
}
static alp_status_t
cam_capture(alp_camera_backend_state_t *st, alp_camera_frame_t *out, uint32_t timeout_ms)
{
	ARG_UNUSED(out);
	ARG_UNUSED(timeout_ms);
	return fake_block(SEM_CAM, &st->closing);
}
static void cam_cancel(alp_camera_backend_state_t *st)
{
	ARG_UNUSED(st);
	k_sem_give(&g_sem[SEM_CAM]);
}
static const alp_camera_ops_t cam_ops = { .open    = cam_open,
	                                      .capture = cam_capture,
	                                      .cancel  = cam_cancel };
ALP_BACKEND_REGISTER(
    camera,
    unit_close_cancel,
    { .silicon_ref = "*", .vendor = "alp/testing", .priority = 255u, .ops = &cam_ops });

/* ---- mproc (hwsem) ---- */
static alp_status_t hw_open(uint32_t id, alp_hwsem_backend_state_t *st, alp_capabilities_t *caps)
{
	ARG_UNUSED(id);
	ARG_UNUSED(st);
	ARG_UNUSED(caps);
	return ALP_OK;
}
static alp_status_t hw_lock(alp_hwsem_backend_state_t *st, uint32_t timeout_ms)
{
	ARG_UNUSED(timeout_ms);
	return fake_block(SEM_HWSEM, &st->closing);
}
static void hw_cancel(alp_hwsem_backend_state_t *st)
{
	ARG_UNUSED(st);
	k_sem_give(&g_sem[SEM_HWSEM]);
}
static const alp_mproc_ops_t mproc_ops = { .hwsem_open   = hw_open,
	                                       .hwsem_lock   = hw_lock,
	                                       .hwsem_cancel = hw_cancel };
ALP_BACKEND_REGISTER(
    mproc,
    unit_close_cancel,
    { .silicon_ref = "*", .vendor = "alp/testing", .priority = 255u, .ops = &mproc_ops });

/* ---- audio ---- */
static alp_status_t
ain_open(const alp_audio_config_t *cfg, alp_audio_in_backend_state_t *st, alp_capabilities_t *caps)
{
	ARG_UNUSED(cfg);
	ARG_UNUSED(st);
	ARG_UNUSED(caps);
	return ALP_OK;
}
static alp_status_t ain_read(alp_audio_in_backend_state_t *st,
                             void                         *buf,
                             size_t                        frames,
                             size_t                       *out_frames,
                             uint32_t                      timeout_ms)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(frames);
	ARG_UNUSED(out_frames);
	ARG_UNUSED(timeout_ms);
	return fake_block(SEM_AIN, &st->closing);
}
static void ain_cancel(alp_audio_in_backend_state_t *st)
{
	ARG_UNUSED(st);
	k_sem_give(&g_sem[SEM_AIN]);
}
static alp_status_t aout_open(const alp_audio_config_t      *cfg,
                              alp_audio_out_backend_state_t *st,
                              alp_capabilities_t            *caps)
{
	ARG_UNUSED(cfg);
	ARG_UNUSED(st);
	ARG_UNUSED(caps);
	return ALP_OK;
}
static alp_status_t aout_write(alp_audio_out_backend_state_t *st,
                               const void                    *buf,
                               size_t                         frames,
                               size_t                        *out_frames,
                               uint32_t                       timeout_ms)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(frames);
	ARG_UNUSED(out_frames);
	ARG_UNUSED(timeout_ms);
	return fake_block(SEM_AOUT, &st->closing);
}
static void aout_cancel(alp_audio_out_backend_state_t *st)
{
	ARG_UNUSED(st);
	k_sem_give(&g_sem[SEM_AOUT]);
}
static const alp_audio_ops_t audio_ops = { .in_open    = ain_open,
	                                       .in_read    = ain_read,
	                                       .in_cancel  = ain_cancel,
	                                       .out_open   = aout_open,
	                                       .out_write  = aout_write,
	                                       .out_cancel = aout_cancel };
ALP_BACKEND_REGISTER(
    audio,
    unit_close_cancel,
    { .silicon_ref = "*", .vendor = "alp/testing", .priority = 255u, .ops = &audio_ops });

/* ---- ble ---- */
static alp_status_t ble_open(alp_ble_radio_state_t *st, alp_capabilities_t *caps)
{
	ARG_UNUSED(st);
	ARG_UNUSED(caps);
	return ALP_OK;
}
static alp_status_t ble_connect(alp_ble_radio_state_t *st,
                                const alp_ble_addr_t  *peer,
                                uint32_t               timeout_ms,
                                alp_ble_conn_state_t  *conn)
{
	ARG_UNUSED(st);
	ARG_UNUSED(peer);
	ARG_UNUSED(timeout_ms);
	ARG_UNUSED(conn);
	return ALP_OK;
}
static alp_status_t ble_read(alp_ble_conn_state_t *st,
                             alp_ble_attr_handle_t handle,
                             uint8_t              *out,
                             size_t                out_cap,
                             size_t               *out_len,
                             uint32_t              timeout_ms)
{
	ARG_UNUSED(handle);
	ARG_UNUSED(out);
	ARG_UNUSED(out_cap);
	ARG_UNUSED(out_len);
	ARG_UNUSED(timeout_ms);
	return fake_block(SEM_BLE, &st->closing);
}
static void ble_cancel(alp_ble_conn_state_t *st)
{
	ARG_UNUSED(st);
	k_sem_give(&g_sem[SEM_BLE]);
}
static alp_status_t ble_disconnect(alp_ble_conn_state_t *st)
{
	ARG_UNUSED(st);
	return ALP_OK;
}
static const alp_ble_ops_t ble_ops = { .open        = ble_open,
	                                   .connect     = ble_connect,
	                                   .conn_cancel = ble_cancel,
	                                   .gatt_read   = ble_read,
	                                   .disconnect  = ble_disconnect };
ALP_BACKEND_REGISTER(
    ble,
    unit_close_cancel,
    { .silicon_ref = "*", .vendor = "alp/testing", .priority = 255u, .ops = &ble_ops });

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

struct job {
	alp_status_t (*fn)(void *arg);
	void        *arg;
	alp_status_t rc;
};

#define STACK_SZ 2048
static K_THREAD_STACK_DEFINE(waiter_stack, STACK_SZ);
static K_THREAD_STACK_DEFINE(closer_stack, STACK_SZ);
static struct k_thread waiter_thr, closer_thr;

static void job_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	struct job *j = p1;
	j->rc         = j->fn(j->arg);
}

/* Block in @p blocked, close from another thread, and require both to
 * finish within a bound.  The bound is what makes a hang (close waiting
 * for an op nothing wakes) a test failure instead of a stuck suite. */
static void run_close_wakes(struct job *blocked, struct job *closer)
{
	blocked->rc = ALP_ERR_BUSY; /* sentinel: never written if it hangs */
	k_thread_create(&waiter_thr,
	                waiter_stack,
	                STACK_SZ,
	                job_entry,
	                blocked,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(1),
	                0,
	                K_NO_WAIT);
	k_msleep(100); /* let it reach the wait */
	k_thread_create(&closer_thr,
	                closer_stack,
	                STACK_SZ,
	                job_entry,
	                closer,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(1),
	                0,
	                K_NO_WAIT);

	int64_t t0 = k_uptime_get();
	zassert_equal(k_thread_join(&closer_thr, K_SECONDS(2)), 0, "close() hung on a blocked call");
	zassert_equal(k_thread_join(&waiter_thr, K_SECONDS(2)), 0, "blocked call was not woken");
	zassert_true(k_uptime_get() - t0 < 1500, "close/wake took too long");
	zassert_equal(blocked->rc, ALP_ERR_NOT_READY, "woken call must return NOT_READY");
}

static void *suite_setup(void)
{
	for (int i = 0; i < SEM_COUNT; ++i) {
		k_sem_init(&g_sem[i], 0, K_SEM_MAX_LIMIT);
	}
	return NULL;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	suite_setup();
}

/* ---- jobs ---- */
static alp_status_t j_capture(void *h)
{
	alp_camera_frame_t fr;
	return alp_camera_capture(h, &fr, UINT32_MAX);
}
static alp_status_t j_camera_close(void *h)
{
	alp_camera_close(h);
	return ALP_OK;
}
static alp_status_t j_hwsem_lock(void *h)
{
	return alp_hwsem_lock(h, UINT32_MAX);
}
static alp_status_t j_hwsem_close(void *h)
{
	alp_hwsem_close(h);
	return ALP_OK;
}
static alp_status_t j_ain_read(void *h)
{
	int16_t b[4];
	size_t  got;
	return alp_audio_in_read(h, b, 4, &got, UINT32_MAX);
}
static alp_status_t j_ain_close(void *h)
{
	alp_audio_in_close(h);
	return ALP_OK;
}
static alp_status_t j_aout_write(void *h)
{
	int16_t b[4] = { 0 };
	size_t  got;
	return alp_audio_out_write(h, b, 4, &got, UINT32_MAX);
}
static alp_status_t j_aout_close(void *h)
{
	alp_audio_out_close(h);
	return ALP_OK;
}
static alp_status_t j_gatt_read(void *h)
{
	uint8_t b[4];
	size_t  n;
	return alp_ble_gatt_read(h, 1, b, sizeof(b), &n, UINT32_MAX);
}
static alp_status_t j_disconnect(void *h)
{
	return alp_ble_disconnect(h);
}

ZTEST(dispatch_close_cancels_waits, test_camera_close_wakes_capture)
{
	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(0);
	alp_camera_t       *h   = alp_camera_open(&cfg);
	zassert_not_null(h);
	struct job blocked = { j_capture, h, 0 };
	struct job closer  = { j_camera_close, h, 0 };
	run_close_wakes(&blocked, &closer);
}

ZTEST(dispatch_close_cancels_waits, test_hwsem_close_wakes_lock)
{
	alp_hwsem_t *h = alp_hwsem_open(0);
	zassert_not_null(h);
	struct job blocked = { j_hwsem_lock, h, 0 };
	struct job closer  = { j_hwsem_close, h, 0 };
	run_close_wakes(&blocked, &closer);
}

static const alp_audio_config_t audio_cfg = {
	.peripheral_id    = 0,
	.sample_rate_hz   = 16000,
	.channels         = 1,
	.format           = ALP_AUDIO_FMT_S16_LE,
	.frames_per_block = 4,
};

ZTEST(dispatch_close_cancels_waits, test_audio_in_close_wakes_read)
{
	alp_audio_in_t *h = alp_audio_in_open(&audio_cfg);
	zassert_not_null(h);
	struct job blocked = { j_ain_read, h, 0 };
	struct job closer  = { j_ain_close, h, 0 };
	run_close_wakes(&blocked, &closer);
}

ZTEST(dispatch_close_cancels_waits, test_audio_out_close_wakes_write)
{
	alp_audio_out_t *h = alp_audio_out_open(&audio_cfg);
	zassert_not_null(h);
	struct job blocked = { j_aout_write, h, 0 };
	struct job closer  = { j_aout_close, h, 0 };
	run_close_wakes(&blocked, &closer);
}

ZTEST(dispatch_close_cancels_waits, test_ble_disconnect_wakes_gatt_read)
{
	alp_ble_t *radio = alp_ble_open();
	zassert_not_null(radio);
	alp_ble_addr_t  peer = { 0 };
	alp_ble_conn_t *conn = NULL;
	zassert_equal(alp_ble_connect(radio, &peer, 0, &conn), ALP_OK);
	struct job blocked = { j_gatt_read, conn, 0 };
	struct job closer  = { j_disconnect, conn, 0 };
	run_close_wakes(&blocked, &closer);
	alp_ble_close(radio);
}

ZTEST_SUITE(dispatch_close_cancels_waits, NULL, suite_setup, before, NULL, NULL);
