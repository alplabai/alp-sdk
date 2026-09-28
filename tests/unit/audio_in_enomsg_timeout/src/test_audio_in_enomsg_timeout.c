/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for issue #2133 round 4d: src/backends/audio/
 * zephyr_drv.c's errno_to_alp() overrides -ENOMSG to ALP_ERR_TIMEOUT
 * (alp_status_from_zephyr_errno() has no arm for -ENOMSG at all, so it
 * would otherwise fall through to ALP_ERR_IO -- "dropped data, sticky",
 * wrong for a non-blocking read that simply found nothing queued yet).
 * Nothing exercised this override end to end: tests/unit/errno_mapping
 * only pins the override MECHANISM (alp_status_from_zephyr_errno_ex()),
 * not that z_in_read() actually reaches it -- deleting the override
 * arm in errno_to_alp() left every existing test green (issue #2133
 * round 4e finding 4).
 *
 * This test supplies its own fake DMIC controller (src/fake_dmic.c,
 * bound via ../dts/bindings/alp,test-dmic.yaml and the boards
 * overlays) so alp_audio_in_open() resolves a real device and
 * alp_audio_in_read() genuinely runs through zephyr_drv.c's
 * z_in_read() -- not src/backends/audio/sw_fallback.c, which never
 * calls dmic_read() at all and would prove nothing about the mapping.
 */

#include <zephyr/ztest.h>

#include <alp/audio.h>

ZTEST_SUITE(alp_audio_in_enomsg_timeout, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_audio_in_enomsg_timeout, test_nonblocking_empty_read_is_timeout_not_io)
{
	alp_audio_config_t cfg = ALP_AUDIO_CONFIG_DEFAULT(0);
	alp_audio_in_t    *in  = alp_audio_in_open(&cfg);

	/* A NULL handle here means this regressed back to resolving a NULL
	 * device (sw_fallback territory) -- fail loudly instead of the
	 * fake DMIC's -ENOMSG path silently never being reached. */
	zassert_not_null(in, "alp_audio_in_open() must succeed against the fake alp-pdm0 device");

	uint8_t buf[64];
	size_t  out_frames = 0;

	/* timeout_ms=0 -> non-blocking -> the fake DMIC's read() returns
	 * -ENOMSG (the same errno k_msgq_get(K_NO_WAIT) returns on a real
	 * driver for "nothing queued yet"). Must map to ALP_ERR_TIMEOUT,
	 * not the sticky-drop ALP_ERR_IO. */
	alp_status_t rc = alp_audio_in_read(in, buf, sizeof(buf) / 4u, &out_frames, 0u);

	zassert_equal(rc,
	              ALP_ERR_TIMEOUT,
	              "non-blocking empty read (-ENOMSG) must map to ALP_ERR_TIMEOUT, not "
	              "ALP_ERR_IO -- see errno_to_alp() in src/backends/audio/zephyr_drv.c");

	alp_audio_in_close(in);
}
