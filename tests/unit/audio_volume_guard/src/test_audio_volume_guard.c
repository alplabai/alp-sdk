/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1648 tier 1: `alp_audio_out_set_volume()` recorded a non-unity
 * volume for S24_LE / S32_LE handles and returned ALP_OK, but neither
 * the Yocto (ALSA) nor the Zephyr (I2S) audio-out backend can
 * actually apply a software scale to those formats (both only scale
 * S16_LE) -- so a handle opened S24/S32 played at full scale
 * regardless of what the caller asked for, with no error anywhere.
 *
 * #2002: the sw_fallback null-sink backend had the SAME accept-and-
 * discard shape for EVERY format (it can scale nothing at all), and
 * this file's only coverage was the extracted guard predicate,
 * alp_audio_volume_settable() -- never a real backend's out_set_volume.
 * That circularity means the test stayed green through any of:
 *   - a backend forgetting to call the guard,
 *   - a backend passing the wrong format into the guard,
 *   - the dispatcher discarding/rewriting the backend's return code,
 *   - (post-#2002) sw_out_set_volume regressing back to unconditional
 *     ALP_OK.
 * None of those are exercised by asserting the guard function alone.
 *
 * Fix: below the guard-predicate tests (still valid coverage of that
 * shared helper), a second group calls the REAL registered
 * out_set_volume function pointer for the sw_fallback backend --
 * obtained the same way tests/unit/audio_registry does, via
 * alp_backend_select() + alp_backend_select_next() and the internal
 * audio_ops.h vtable -- and asserts on ITS return code and on
 * state.volume, not on the guard.
 *
 * This build cannot reach the Yocto/Zephyr backends' real
 * out_set_volume the same way: audio_dispatch.c's alp_audio_out_open()
 * calls alp_backend_select() exactly once with no fallback (unlike
 * e.g. storage/security), and zephyr_drv is an unconditional
 * priority-100 "*" wildcard registration in every Zephyr build of this
 * module (zephyr/CMakeLists.txt), so it always wins arbitration ahead
 * of sw_fallback (priority 0) -- and its own out_open() either
 * requires a real I2S device (CONFIG_ALP_SDK_AUDIO_OUT=y) or refuses
 * unconditionally (CONFIG_ALP_SDK_AUDIO_OUT=n, the native_sim
 * default). No live alp_audio_out_t handle backed by sw_fallback, or
 * backed by zephyr_drv without real hardware, is reachable from this
 * test binary. Calling the backend's registered ops entry point
 * directly is the closest hermetic equivalent available on native_sim
 * -- it is the exact function pointer alp_audio_out_set_volume()
 * dispatches to, not the guard it (thinly) wraps.
 */
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/audio.h>
#include <alp/backend.h>
#include <alp/peripheral.h>

#include "audio_ops.h"
#include "audio_volume_guard.h"

ZTEST_SUITE(audio_volume_guard, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* Guard predicate -- alp_audio_volume_settable() itself.               */
/* Still real production code (consumed by yocto_drv.c/zephyr_drv.c), */
/* just not the whole story -- see the real-entry-point group below.  */
/* ------------------------------------------------------------------ */

/* Full-scale (255) is a scale-identity no-op on every format, so it
 * must always be accepted -- refusing it would break callers who
 * harmlessly set 255 on a format this SDK can't yet scale.
 */
ZTEST(audio_volume_guard, test_unity_accepted_on_every_format)
{
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S16_LE, 255u));
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S24_LE, 255u));
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S32_LE, 255u));
}

/* S16_LE is the one format both backends can actually scale -- any
 * volume must be accepted.
 */
ZTEST(audio_volume_guard, test_any_volume_accepted_on_s16le)
{
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S16_LE, 0u));
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S16_LE, 1u));
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S16_LE, 128u));
	zassert_true(alp_audio_volume_settable(ALP_AUDIO_FMT_S16_LE, 254u));
}

/* A non-unity volume on S24_LE / S32_LE is exactly issue #1648's
 * silent-drop case -- must be refused.
 */
ZTEST(audio_volume_guard, test_non_unity_refused_on_s24_and_s32)
{
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S24_LE, 0u));
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S24_LE, 128u));
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S24_LE, 254u));
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S32_LE, 0u));
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S32_LE, 128u));
	zassert_false(alp_audio_volume_settable(ALP_AUDIO_FMT_S32_LE, 254u));
}

/* ------------------------------------------------------------------ */
/* Real entry point -- the sw_fallback backend's registered            */
/* out_set_volume, fetched from the same backend registry the         */
/* dispatcher itself queries.  Not the guard: sw_out_set_volume does   */
/* not call alp_audio_volume_settable() at all (it can scale no        */
/* format), so these tests are the ONLY coverage of #2002's fix.       */
/* ------------------------------------------------------------------ */

/* Both backends registered for "audio" on this test build are "*"
 * wildcards (zephyr_drv priority 100, sw_fallback priority 0), so
 * alp_backend_select() always returns zephyr_drv first and
 * alp_backend_select_next() walks to the next-ranked entry,
 * sw_fallback -- see tests/unit/audio_registry's identical pattern.
 */
static const alp_audio_ops_t *sw_fallback_out_ops(void)
{
	const alp_backend_t *first = alp_backend_select("audio", "alif:ensemble:e7");
	zassert_not_null(first, "expected at least one audio backend registered");
	zassert_equal(
	    strcmp(first->vendor, "zephyr"), 0, "test build assumption: zephyr_drv is prio 100");

	const alp_backend_t *sw = alp_backend_select_next("audio", "alif:ensemble:e7", first);
	zassert_not_null(sw, "expected a second (sw_fallback) audio backend registered");
	zassert_equal(strcmp(sw->vendor, "sw_fallback"), 0);

	return (const alp_audio_ops_t *)sw->ops;
}

ZTEST(audio_volume_guard, test_sw_fallback_real_out_set_volume_refuses_non_unity_every_format)
{
	const alp_audio_ops_t *ops = sw_fallback_out_ops();
	zassert_not_null(ops);
	zassert_not_null(ops->out_set_volume);

	const alp_audio_format_t formats[] = {
		ALP_AUDIO_FMT_S16_LE,
		ALP_AUDIO_FMT_S24_LE,
		ALP_AUDIO_FMT_S32_LE,
	};
	const uint8_t vols[] = { 0u, 1u, 128u, 254u };

	for (size_t f = 0; f < ARRAY_SIZE(formats); f++) {
		for (size_t v = 0; v < ARRAY_SIZE(vols); v++) {
			alp_audio_out_backend_state_t state = { 0 };
			state.cfg.format                    = formats[f];
			state.volume                        = 255u; /* sentinel: unity */

			alp_status_t rc = ops->out_set_volume(&state, vols[v]);

			zassert_equal(rc,
			              ALP_ERR_NOSUPPORT,
			              "format %d vol %u: sw_fallback can scale nothing, "
			              "expected ALP_ERR_NOSUPPORT, got %d",
			              (int)formats[f],
			              (unsigned)vols[v],
			              (int)rc);
			/* sw_out_set_volume must not stash the refused value --
			 * it is the dispatcher's job to record volume, and only
			 * on ALP_OK (src/audio_dispatch.c). A backend that
			 * writes state->volume on refusal is the #1648 shape one
			 * layer down. */
			zassert_equal(state.volume, 255u, "refused call must not record the requested volume");
		}
	}
}

ZTEST(audio_volume_guard, test_sw_fallback_real_out_set_volume_accepts_full_scale_every_format)
{
	const alp_audio_ops_t *ops = sw_fallback_out_ops();
	zassert_not_null(ops);
	zassert_not_null(ops->out_set_volume);

	const alp_audio_format_t formats[] = {
		ALP_AUDIO_FMT_S16_LE,
		ALP_AUDIO_FMT_S24_LE,
		ALP_AUDIO_FMT_S32_LE,
	};

	for (size_t f = 0; f < ARRAY_SIZE(formats); f++) {
		alp_audio_out_backend_state_t state = { 0 };
		state.cfg.format                    = formats[f];
		state.volume                        = 0u; /* sentinel != 255 */

		alp_status_t rc = ops->out_set_volume(&state, 255u);

		zassert_equal(rc,
		              ALP_OK,
		              "format %d: full-scale must always succeed, got %d",
		              (int)formats[f],
		              (int)rc);
	}
}
