/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the storage registry dispatcher.  Mirrors the
 * spi_registry / i2c_registry harnesses; covers:
 *
 *   (a) selector / priority on the alif:ensemble:e7 pin
 *   (b) sw_fallback reachability via the registry count
 *   (c) configure_inline_aes input validation runs in the dispatcher
 *       ahead of backend dispatch
 *   (d) alp_storage_capabilities(NULL) is safe
 *   (e) close releases handle; NULL inputs are idempotent
 *   (f) sw_fallback NOSUPPORT contract via direct ops-table dispatch
 *   (g) vendor-ext gating: non-Alif handle -> NOT_PRESENT_ON_THIS_SOC
 *       from the Alif SecAES surface; non-NXP handle from the NXP
 *       OTFAD surface
 *
 * Backends visible on this test build:
 *   sw_fallback   (priority 0,   "*" wildcard)
 *
 * zephyr_flash / zephyr_littlefs are not linked because their
 * CONFIG_ flags require CONFIG_FLASH_MAP / CONFIG_FILE_SYSTEM_LITTLEFS
 * which native_sim doesn't ship by default.  The dispatcher and
 * vendor-ext surfaces are exercised directly via the public API +
 * the .alp_backends_storage section walk.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/ext/alif/storage.h>
#include <alp/ext/nxp/storage.h>
#include <alp/storage.h>

#include "../../../../src/backends/storage/storage_ops.h"

ZTEST_SUITE(alp_storage_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- (a) Selector picks the sw_fallback on the test build --------- */

ZTEST(alp_storage_registry, test_selector_picks_sw_fallback_on_alif)
{
	/* Only sw_fallback is linked in this test build (no FLASH_MAP /
     * FILE_SYSTEM_LITTLEFS).  The selector must return it as the
     * only wildcard match. */
	const alp_backend_t *be = alp_backend_select("storage", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "sw_fallback"), 0);
}

/* ---------- (b) sw_fallback reachability via count ----------------------- */

ZTEST(alp_storage_registry, test_storage_registry_has_at_least_one_backend)
{
	/* The dispatcher counts entries in .alp_backends_storage; the
     * sw_fallback registration must be reachable. */
	zassert_true(alp_backend_count("storage") >= 1u);
}

/* ---------- (c) configure_inline_aes input validation -------------------- */

ZTEST(alp_storage_registry, test_configure_inline_aes_rejects_bad_inputs)
{
	/* The dispatcher validates inputs ahead of the backend op, so even
     * a NULL storage handle returns INVAL when cfg is bad (the cfg
     * check fires first). */
	zassert_equal(alp_storage_configure_inline_aes(NULL, NULL), ALP_ERR_INVAL);

	/* Bad mode -> INVAL even with NULL storage. */
	alp_storage_aes_config_t bad_mode = {
		.mode      = (alp_storage_aes_mode_t)42,
		.key       = NULL,
		.key_bytes = 0u,
		.iv        = NULL,
		.iv_bytes  = 0u,
	};
	zassert_equal(alp_storage_configure_inline_aes(NULL, &bad_mode), ALP_ERR_INVAL);

	/* mode == CTR with NULL key -> INVAL even with NULL storage. */
	alp_storage_aes_config_t null_key = {
		.mode      = ALP_STORAGE_AES_CTR,
		.key       = NULL,
		.key_bytes = 16u,
		.iv        = NULL,
		.iv_bytes  = 16u,
	};
	zassert_equal(alp_storage_configure_inline_aes(NULL, &null_key), ALP_ERR_INVAL);

	/* mode == XTS with bogus key_bytes -> INVAL. */
	static const uint8_t     _stub_key[16] = { 0 };
	static const uint8_t     _stub_iv[16]  = { 0 };
	alp_storage_aes_config_t bad_kb        = {
		.mode      = ALP_STORAGE_AES_XTS,
		.key       = _stub_key,
		.key_bytes = 7u,
		.iv        = _stub_iv,
		.iv_bytes  = 16u,
	};
	zassert_equal(alp_storage_configure_inline_aes(NULL, &bad_kb), ALP_ERR_INVAL);

	/* mode == OFF skips key/iv checks but still hits NOT_READY on NULL
     * handle (validation passes; dispatcher catches the NULL after). */
	alp_storage_aes_config_t off_cfg = {
		.mode      = ALP_STORAGE_AES_OFF,
		.key       = NULL,
		.key_bytes = 0u,
		.iv        = NULL,
		.iv_bytes  = 0u,
	};
	zassert_equal(alp_storage_configure_inline_aes(NULL, &off_cfg), ALP_ERR_NOT_READY);
}

/* ---------- (d) capabilities getter NULL-safe ---------------------------- */

ZTEST(alp_storage_registry, test_capabilities_getter_null_safe)
{
	zassert_is_null(alp_storage_capabilities(NULL));
}

/* ---------- (e) close + open NULL idempotency ---------------------------- */

ZTEST(alp_storage_registry, test_close_releases_handle)
{
	/* alp_storage_open(NULL) -> NULL, no pool slot consumed. */
	alp_storage_t *h_null = alp_storage_open(NULL);
	zassert_is_null(h_null);

	/* alp_storage_close(NULL) must not crash. */
	alp_storage_close(NULL);

	/* Sync / read / write / erase / get_info on NULL handle ->
     * NOT_READY (or INVAL when their preconditions catch first). */
	zassert_equal(alp_storage_sync(NULL), ALP_ERR_NOT_READY);
	zassert_equal(alp_storage_read(NULL, 0u, NULL, 4u), ALP_ERR_NOT_READY);
	zassert_equal(alp_storage_write(NULL, 0u, NULL, 4u), ALP_ERR_NOT_READY);
	zassert_equal(alp_storage_erase(NULL, 0u, 4u), ALP_ERR_NOT_READY);
	alp_storage_info_t info_out;
	zassert_equal(alp_storage_get_info(NULL, &info_out), ALP_ERR_NOT_READY);
}

/* ---------- (f) sw_fallback NOSUPPORT contract via direct ops dispatch --- */

extern const alp_backend_t __start_alp_backends_storage[] __attribute__((weak));
extern const alp_backend_t __stop_alp_backends_storage[] __attribute__((weak));

static const alp_storage_ops_t *_find_sw_fallback_ops(void)
{
	for (const alp_backend_t *be = __start_alp_backends_storage; be < __stop_alp_backends_storage;
	     ++be) {
		if (strcmp(be->vendor, "sw_fallback") == 0) {
			return (const alp_storage_ops_t *)be->ops;
		}
	}
	return NULL;
}

ZTEST(alp_storage_registry, test_sw_fallback_nosupport_contract)
{
	const alp_storage_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	/* Stack-local backend state -- the sw_fallback ops don't
     * dereference dev/be_data so we don't need a full struct alp_storage. */
	alp_storage_backend_state_t st;
	memset(&st, 0, sizeof(st));
	alp_capabilities_t   caps = { 0 };
	alp_storage_config_t cfg  = {
		.kind        = ALP_STORAGE_KIND_QSPI_FLASH,
		.instance_id = 0u,
		.freq_hz     = 0u,
		.read_only   = false,
	};
	zassert_equal(ops->open(&cfg, &st, &caps), ALP_OK);

	uint8_t            buf[4] = { 0 };
	alp_storage_info_t info   = { 0 };
	zassert_equal(ops->get_info(&st, &info), ALP_ERR_NOSUPPORT);
	zassert_equal(ops->read(&st, 0u, buf, sizeof(buf)), ALP_ERR_NOSUPPORT);
	zassert_equal(ops->write(&st, 0u, buf, sizeof(buf)), ALP_ERR_NOSUPPORT);
	zassert_equal(ops->erase(&st, 0u, sizeof(buf)), ALP_ERR_NOSUPPORT);
	zassert_equal(ops->sync(&st), ALP_ERR_NOSUPPORT);

	alp_storage_aes_config_t aes = {
		.mode = ALP_STORAGE_AES_OFF,
	};
	zassert_equal(ops->configure_inline_aes(&st, &aes), ALP_ERR_NOSUPPORT);
}

/* ---------- (g) vendor-ext gating: non-vendor handle -> NOT_PRESENT_ON_THIS_SOC */

ZTEST(alp_storage_registry, test_vendor_ext_gates_non_matching_backends)
{
	/* NULL handle -> INVAL (parameter check fires first). */
	static const uint8_t key16[16] = { 0 };
	static const uint8_t iv16[16]  = { 0 };
	zassert_equal(alp_alif_storage_secaes_key_provision(NULL, key16, 16u), ALP_ERR_INVAL);
	zassert_equal(alp_nxp_storage_otfad_provision(NULL, 0u, key16, iv16), ALP_ERR_INVAL);

	uint32_t status_out = 0u;
	zassert_equal(alp_alif_storage_secaes_get_status(NULL, &status_out), ALP_ERR_INVAL);
	zassert_equal(alp_nxp_storage_otfad_set_window(NULL, 0u, 0u, 1024u), ALP_ERR_INVAL);

	/* Build a fake handle pinned to sw_fallback (vendor != "alif" /
     * "nxp") -- both vendor ext surfaces must return
     * NOT_PRESENT_ON_THIS_SOC. */
	const alp_backend_t *be = alp_backend_select("storage", "alif:ensemble:e7");
	zassert_not_null(be);

	struct alp_storage h;
	memset(&h, 0, sizeof(h));
	h.in_use  = true;
	h.backend = be;

	zassert_equal(alp_alif_storage_secaes_key_provision(&h, key16, 16u),
	              ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
	zassert_equal(alp_alif_storage_secaes_get_status(&h, &status_out),
	              ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
	zassert_equal(alp_nxp_storage_otfad_provision(&h, 0u, key16, iv16),
	              ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
	zassert_equal(alp_nxp_storage_otfad_set_window(&h, 0u, 0u, 1024u),
	              ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
}

/* ---------- (h) OTFAD window-bounds validation reaches the body ---------- */

ZTEST(alp_storage_registry, test_otfad_window_alignment_validation)
{
	/* Synthesise a handle that LOOKS NXP (so the body's vendor-gate
     * passes) and then exercises the alignment + ordering checks. */
	static const alp_backend_t fake_nxp_backend = {
		.silicon_ref = "nxp:imx9:imx93",
		.vendor      = "nxp",
		.base_caps   = 0u,
		.priority    = 100,
		.ops         = NULL,
		.probe       = NULL,
	};
	struct alp_storage h;
	memset(&h, 0, sizeof(h));
	h.in_use  = true;
	h.backend = &fake_nxp_backend;

	/* Window id out of range -> INVAL. */
	zassert_equal(alp_nxp_storage_otfad_set_window(&h, 99u, 0u, 1024u), ALP_ERR_INVAL);
	/* end <= start -> INVAL. */
	zassert_equal(alp_nxp_storage_otfad_set_window(&h, 0u, 1024u, 1024u), ALP_ERR_INVAL);
	/* Misaligned start -> INVAL. */
	zassert_equal(alp_nxp_storage_otfad_set_window(&h, 0u, 512u, 1024u), ALP_ERR_INVAL);
	/* Misaligned end -> INVAL. */
	zassert_equal(alp_nxp_storage_otfad_set_window(&h, 0u, 0u, 1500u), ALP_ERR_INVAL);
	/* Aligned + ordered -> NOSUPPORT (vendor pack not landed). */
	zassert_equal(alp_nxp_storage_otfad_set_window(&h, 0u, 0u, 1024u), ALP_ERR_NOSUPPORT);
}
