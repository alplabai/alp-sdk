/* SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for issue #1619: src/backends/i2s/zephyr_drv.c's
 * z_write() must refuse a caller-supplied length larger than the slab
 * block negotiated at open(), and it must do so BEFORE the memcpy that
 * used to corrupt the neighbouring slab block and the k_malloc heap
 * behind it.
 *
 * The original regression test (tests/zephyr/peripheral/src/i2s.c,
 * removed alongside this file) always reported `skipped` on
 * native_sim: alp_i2s_open() resolves its device via
 * DT_ALIAS(alp_i2s0), native_sim ships no I2S device, so the Zephyr
 * backend bailed with ALP_ERR_NOT_READY on a NULL device before ever
 * reaching z_write() -- a skip proves nothing about the fix.
 *
 * This test supplies its own fake I2S controller (src/fake_i2s.c,
 * bound via ../dts/bindings/alp,test-i2s.yaml and the boards
 * overlays) so
 * alp_i2s_open() resolves a real device, i2s_configure() succeeds, and
 * alp_i2s_write() genuinely runs through zephyr_drv.c's z_write() --
 * not src/backends/i2s/sw_fallback.c, which would reject every write
 * with ALP_ERR_NOSUPPORT regardless of length and prove nothing about
 * the bound either.  zephyr_drv (priority 100) always wins backend
 * selection over sw_fallback (priority 0) once a real device is
 * present, so an exact-size write reaching ALP_OK (not NOSUPPORT)
 * confirms this test exercised the fixed code path.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/i2s.h>

ZTEST_SUITE(alp_i2s_write_bounds, NULL, NULL, NULL, NULL, NULL);

/* block_frames=256 * channels=2 * (word_bits=16 / 8) = 1024 bytes --
 * matches the config open()s below and the arithmetic in
 * src/backends/i2s/zephyr_drv.c:161. */
#define BLOCK_BYTES 1024u

static alp_i2s_t *open_i2s(void)
{
	alp_i2s_config_t cfg = {
		.bus_id         = 0u,
		.sample_rate_hz = 48000u,
		.word_bits      = 16u,
		.channels       = 2u,
		.format         = ALP_I2S_FMT_I2S,
		.direction      = ALP_I2S_DIR_TX,
		.block_frames   = 256u,
	};
	alp_i2s_t *h = alp_i2s_open(&cfg);
	/* A NULL handle here means the test regressed back to opening
	 * against a NULL device (the original skip) -- fail loudly
	 * instead of silently skipping. */
	zassert_not_null(h, "alp_i2s_open() must succeed against the fake alp-i2s0 device");
	return h;
}

ZTEST(alp_i2s_write_bounds, test_write_rejects_oversize_block)
{
	alp_i2s_t *h = open_i2s();

	static uint8_t oversize[2u * BLOCK_BYTES];
	memset(oversize, 0xAA, sizeof(oversize));

	/* Larger than the negotiated 1024-byte block: must be refused
	 * before the memcpy that used to corrupt the neighbouring slab
	 * block (#1619). */
	zassert_equal(alp_i2s_write(h, oversize, sizeof(oversize), 100u), ALP_ERR_OUT_OF_RANGE);

	alp_i2s_close(h);
}

ZTEST(alp_i2s_write_bounds, test_write_exact_block_reaches_zephyr_backend)
{
	alp_i2s_t *h = open_i2s();

	static uint8_t exact[BLOCK_BYTES];
	memset(exact, 0x55, sizeof(exact));

	alp_status_t rc = alp_i2s_write(h, exact, sizeof(exact), 100u);

	/* Must not be rejected as oversize ... */
	zassert_not_equal(rc, ALP_ERR_OUT_OF_RANGE);
	/* ... and must not be sw_fallback's blanket NOSUPPORT, which would
	 * mean this test never reached zephyr_drv's z_write() at all. */
	zassert_not_equal(rc, ALP_ERR_NOSUPPORT);
	zassert_equal(rc, ALP_OK);

	alp_i2s_close(h);
}
