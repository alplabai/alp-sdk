/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generic public alp header surface smokes -- not tied to any single chip
 * driver.  Covers the "all public headers compile together" contract,
 * the <alp/gui.h> LVGL guard-clause, and the v0.2/v0.3 stubbed
 * audio/ble/security/mproc surfaces.
 */

#include <zephyr/ztest.h>

#include "alp/audio.h"
#include "alp/ble.h"
#include "alp/boards/alp_e1m_evk.h"
#include "alp/camera.h"
#include "alp/chips/bme280.h"
#include "alp/chips/lis2dw12.h"
#include "alp/chips/lsm6dso.h"
#include "alp/chips/ov5640.h"
#include "alp/chips/ssd1306.h"
#include "alp/chips/ssd1331.h"
#include "alp/display.h"
#include "alp/e1m_pinout.h"
#include "alp/gui.h"
#include "alp/iot.h"
#include "alp/blocks/button_led.h"
#include "alp/blocks/pdm_mic.h"
#include "alp/mproc.h"
#include "alp/peripheral.h"
#include "alp/security.h"

/* ------------------------------------------------------------------ */
/* All public headers compile cleanly when included together           */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_public_headers_co_compile)
{
	/* If any of the headers above introduce a typedef/macro
     * collision the translation unit fails to build — getting
     * here at runtime is the success signal. */
	zassert_equal((int)ALP_OK, 0, "ALP_OK must remain 0 across header-set evolution");
	zassert_equal((unsigned)ALP_E1M_GPIO_IO0, 0u);
	zassert_equal((unsigned)EVK_PWM_LED_RED,
	              ALP_E1M_PWM3,
	              "EVK feature names must layer atop the global e1m_pinout map");
}

/* ------------------------------------------------------------------ */
/* <alp/gui.h> -- alp_gui_lvgl_attach() guard-clause contract           */
/*                                                                      */
/* The real LVGL v9 hand-off (issue #23) lives in src/gui_lvgl.c under  */
/* #ifdef ALP_HAS_LVGL -- see tests/zephyr/gui_lvgl/ for that path.     */
/* This native_sim test build never sets CONFIG_LVGL / ALP_HAS_LVGL, so */
/* it exercises the #else guard clause: NULL display -> INVAL, every    */
/* other build without the LVGL bridge wired in -> NOSUPPORT.           */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_gui_lvgl_attach_null_display_is_inval)
{
	zassert_equal(alp_gui_lvgl_attach(NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_gui_lvgl_attach_valid_display_is_nosupport)
{
	/* No backend wires the real bridge yet, so a non-NULL handle
     * (even a bogus one -- the guard clause never dereferences it)
     * degrades to NOSUPPORT rather than a fabricated success. */
	alp_display_t *fake_handle = (alp_display_t *)0x1;
	zassert_equal(alp_gui_lvgl_attach(fake_handle), ALP_ERR_NOSUPPORT);
}

/* ------------------------------------------------------------------ */
/* v0.2 / v0.3 stubbed surfaces — link-cleanliness + NOSUPPORT contract */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_audio_surface_v01_nosupport)
{
	/* audio.h went stub -> real on AEN-Zephyr in v0.2.  The contract
     * shifted: open(NULL cfg) still returns NULL (with last_error =
     * INVAL); start/stop/etc. on a NULL handle now report NOT_READY
     * (the standard wrapper convention) rather than NOSUPPORT.
     * The "v0.1 stubbed" naming is kept for the suite history; the
     * assertions match the v0.2 reality.  */
	zassert_is_null(alp_audio_in_open(NULL), "open(NULL cfg) -> NULL");
	zassert_is_null(alp_audio_out_open(NULL), "open(NULL cfg) -> NULL");
	zassert_equal(alp_audio_in_start(NULL), ALP_ERR_NOT_READY);
	zassert_equal(alp_audio_out_start(NULL), ALP_ERR_NOT_READY);
	alp_audio_in_close(NULL);
	alp_audio_out_close(NULL);
}

ZTEST(alp_chips, test_ble_surface_v01_nosupport)
{
	/* ble.h went stub -> real on AEN-Zephyr in v0.3.  Same contract
     * shift as audio: open() with no controller still returns NULL
     * (NOSUPPORT), but operations on NULL handles now report
     * NOT_READY (the standard wrapper convention). */
	zassert_is_null(alp_ble_open(), "no BT controller -> NULL");
	zassert_equal(alp_ble_advertise_start(NULL, NULL), ALP_ERR_NOT_READY);
	zassert_equal(alp_ble_scan_stop(NULL), ALP_ERR_NOT_READY);
	alp_ble_close(NULL);
}

ZTEST(alp_chips, test_security_surface_v01_nosupport)
{
	zassert_is_null(alp_hash_open(ALP_HASH_SHA256));
	zassert_is_null(alp_aead_open(ALP_AEAD_AES_128_GCM, NULL, 0));
	uint8_t buf[16];
	zassert_equal(alp_random_bytes(buf, sizeof buf), ALP_ERR_NOSUPPORT);
	alp_hash_close(NULL);
	alp_aead_close(NULL);
}

ZTEST(alp_chips, test_mproc_surface_v01_nosupport)
{
	/* mproc.h went stub -> real on AEN-Zephyr in v0.3.  NULL-handle
     * operations move from NOSUPPORT to NOT_READY (the standard
     * wrapper convention); open() still falls through to NULL when
     * the underlying mbox/hwsem device isn't present. */
	zassert_is_null(alp_shmem_open(NULL));
	zassert_is_null(alp_mbox_open(NULL));
	zassert_is_null(alp_hwsem_open(0));
	zassert_equal(alp_hwsem_try_lock(NULL), ALP_ERR_NOT_READY);
	zassert_equal(alp_mbox_send(NULL, NULL, 0, 0), ALP_ERR_NOT_READY);
	alp_shmem_close(NULL);
	alp_mbox_close(NULL);
	alp_hwsem_close(NULL);
}
