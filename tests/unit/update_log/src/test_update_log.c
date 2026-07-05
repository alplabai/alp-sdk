/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include <zephyr/ztest.h>

#include <alp/peripheral.h>
#include <alp/update_log.h>

#include "../../../../src/backends/update_log/update_log_ops.h"
#include "../../../../src/update_log/engine.h"
#include "../../../../src/update_log/store.h"
#include "../../../../src/update_log/sha256.h"

ZTEST_SUITE(alp_update_log, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_update_log, test_entry_roundtrip)
{
	alp_update_log_entry_t e = { 0 };
	e.seq                    = 7;
	e.status                 = ALP_UPDATE_STATUS_CONFIRMED;
	e.timestamp              = 1234;
	strcpy(e.fw_version, "1.2.3");
	uint8_t prev[32];
	memset(prev, 0xAB, sizeof(prev));

	uint8_t wire[ULOG_ENTRY_WIRE_LEN];
	zassert_equal(ulog_entry_encode(&e, prev, wire), ALP_OK);

	alp_update_log_entry_t got = { 0 };
	uint8_t                got_prev[32];
	zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, got_prev), ALP_OK);
	zassert_equal(got.seq, 7);
	zassert_equal(got.status, ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(got.timestamp, 1234);
	zassert_equal(strcmp(got.fw_version, "1.2.3"), 0);
	zassert_mem_equal(got_prev, prev, 32);
}

ZTEST(alp_update_log, test_decode_short_buffer)
{
	uint8_t                wire[ULOG_ENTRY_WIRE_LEN - 1] = { 0 };
	alp_update_log_entry_t got;
	uint8_t                p[32];
	zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, p), ALP_ERR_INVAL);
}

ZTEST(alp_update_log, test_decode_bad_version)
{
	alp_update_log_entry_t e = { 0 };
	e.seq                    = 1;
	strcpy(e.fw_version, "x");
	uint8_t prev[32] = { 0 };
	uint8_t wire[ULOG_ENTRY_WIRE_LEN];
	(void)ulog_entry_encode(&e, prev, wire);
	wire[0] = 0xFF;
	wire[1] = 0xFF; /* corrupt version */
	alp_update_log_entry_t got;
	uint8_t                p[32];
	zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, p), ALP_ERR_VERSION);
}

/* Minimal RAM store double: fixed slots of (key,blob). */
#define TD_SLOTS 16
#define TD_BLOB  128
struct td_store {
	struct {
		char    key[24];
		uint8_t buf[TD_BLOB];
		size_t  len;
		bool    used;
	} s[TD_SLOTS];
	uint64_t counter;
};
static struct td_store *td(void *ctx)
{
	return (struct td_store *)ctx;
}

static int td_find(struct td_store *t, const char *key)
{
	for (int i = 0; i < TD_SLOTS; i++) {
		if (t->s[i].used && strcmp(t->s[i].key, key) == 0) return i;
	}
	return -1;
}
static alp_status_t td_put(void *c, const char *key, const uint8_t *b, size_t n)
{
	struct td_store *t = td(c);
	if (n > TD_BLOB) return ALP_ERR_NOMEM;
	int i = td_find(t, key);
	if (i < 0) {
		for (i = 0; i < TD_SLOTS; i++) {
			if (!t->s[i].used) break;
		}
		if (i == TD_SLOTS) return ALP_ERR_NOMEM;
	}
	t->s[i].used = true;
	strncpy(t->s[i].key, key, sizeof(t->s[i].key) - 1);
	t->s[i].key[sizeof(t->s[i].key) - 1] = 0;
	memcpy(t->s[i].buf, b, n);
	t->s[i].len = n;
	return ALP_OK;
}
static alp_status_t td_get(void *c, const char *key, uint8_t *b, size_t cap, size_t *out)
{
	struct td_store *t = td(c);
	int              i = td_find(t, key);
	if (i < 0) return ALP_ERR_NOT_FOUND;
	if (t->s[i].len > cap) return ALP_ERR_NOMEM;
	memcpy(b, t->s[i].buf, t->s[i].len);
	if (out) *out = t->s[i].len;
	return ALP_OK;
}
static alp_status_t td_erase(void *c, const char *key)
{
	struct td_store *t = td(c);
	int              i = td_find(t, key);
	if (i < 0) return ALP_ERR_NOT_FOUND;
	t->s[i].used = false;
	return ALP_OK;
}
static alp_status_t td_cread(void *c, uint32_t id, uint64_t *v)
{
	(void)id;
	*v = td(c)->counter;
	return ALP_OK;
}
static alp_status_t td_cinc(void *c, uint32_t id, uint64_t *v)
{
	(void)id;
	td(c)->counter++;
	*v = td(c)->counter;
	return ALP_OK;
}

static void td_ifaces(struct td_store *t, alp_secure_store_if *s, alp_monotonic_counter_if *ctr)
{
	memset(t, 0, sizeof(*t));
	s->put         = td_put;
	s->get         = td_get;
	s->erase       = td_erase;
	s->ctx         = t;
	ctr->read      = td_cread;
	ctr->increment = td_cinc;
	ctr->ctx       = t;
}

static alp_update_log_entry_t mk_entry(uint64_t ts, const char *ver, alp_update_status_t st)
{
	alp_update_log_entry_t e = { 0 };
	e.timestamp              = ts;
	e.status                 = st;
	strncpy(e.fw_version, ver, ALP_UPDATE_LOG_FWVER_MAX);
	memset(e.image_hash, (int)ts, sizeof(e.image_hash));
	return e;
}

ZTEST(alp_update_log, test_append_assigns_seq_and_chains)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	td_ifaces(&t, &s, &c);

	alp_update_log_entry_t ent0 = mk_entry(100, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED);
	alp_update_log_entry_t ent1 = mk_entry(200, "1.1.0", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(ulog_engine_append(&s, &c, &ent0), ALP_OK);
	zassert_equal(ulog_engine_append(&s, &c, &ent1), ALP_OK);

	/* counter advanced to 2; meta.count == 2 */
	zassert_equal(t.counter, 2);
	uint8_t metabuf[ULOG_META_WIRE_LEN];
	size_t  mlen = 0;
	zassert_equal(td_get(&t, "ulog.meta", metabuf, sizeof(metabuf), &mlen), ALP_OK);
	struct ulog_meta m;
	zassert_equal(ulog_meta_decode(metabuf, mlen, &m), ALP_OK);
	zassert_equal(m.count, 2);

	/* entry 0 has zero prev_hash; entry 1's prev_hash == sha256(entry0 wire) */
	uint8_t e0[ULOG_ENTRY_WIRE_LEN];
	size_t  n0 = 0;
	zassert_equal(td_get(&t, "ulog.0", e0, sizeof(e0), &n0), ALP_OK);
	alp_update_log_entry_t d1;
	uint8_t                prev1[32];
	uint8_t                e1[ULOG_ENTRY_WIRE_LEN];
	size_t                 n1 = 0;
	zassert_equal(td_get(&t, "ulog.1", e1, sizeof(e1), &n1), ALP_OK);
	zassert_equal(ulog_entry_decode(e1, n1, &d1, prev1), ALP_OK);
	uint8_t expect[32];
	ulog_sha256(e0, n0, expect);
	zassert_mem_equal(prev1, expect, 32);
	zassert_equal(d1.seq, 1);
}

/* --- Task 3: verify() helpers and tests --- */

static void seed(struct td_store *t, alp_secure_store_if *s, alp_monotonic_counter_if *c, int n)
{
	td_ifaces(t, s, c);
	for (int i = 0; i < n; i++) {
		alp_update_log_entry_t ent =
		    mk_entry((uint64_t)(i + 1) * 10, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED);
		(void)ulog_engine_append(s, c, &ent);
	}
}

ZTEST(alp_update_log, test_verify_ok)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 3);
	alp_update_log_verdict_t v;
	uint64_t                 bad = 999;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK);
}

ZTEST(alp_update_log, test_verify_detects_mutation)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 3);
	int i = td_find(&t, "ulog.1");
	t.s[i].buf[12] ^= 0xFF; /* flip a timestamp byte in entry 1 */
	alp_update_log_verdict_t v;
	uint64_t                 bad = 0;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN);
	zassert_equal(bad, 2); /* entry 2's prev_hash no longer matches mutated entry 1 */
}

ZTEST(alp_update_log, test_verify_detects_truncation)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 3);
	(void)td_erase(&t, "ulog.2"); /* drop the tail; counter still says 3 */
	alp_update_log_verdict_t v;
	uint64_t                 bad = 0;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_TRUNCATED);
}

ZTEST(alp_update_log, test_verify_detects_rollback)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 3);
	t.counter = 5; /* counter advanced past the stored meta.count (=3): store regressed */
	alp_update_log_verdict_t v;
	uint64_t                 bad = 0;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_ROLLED_BACK);
}

ZTEST(alp_update_log, test_verify_detects_reorder)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 3);
	int     a = td_find(&t, "ulog.0"), b = td_find(&t, "ulog.1");
	uint8_t tmp[TD_BLOB];
	memcpy(tmp, t.s[a].buf, TD_BLOB);
	memcpy(t.s[a].buf, t.s[b].buf, TD_BLOB);
	memcpy(t.s[b].buf, tmp, TD_BLOB); /* swap contents */
	alp_update_log_verdict_t v;
	uint64_t                 bad = 0;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN);
}

ZTEST(alp_update_log, test_count_and_get)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	td_ifaces(&t, &s, &c);
	alp_update_log_entry_t ent0 = mk_entry(10, "a", ALP_UPDATE_STATUS_CONFIRMED);
	alp_update_log_entry_t ent1 = mk_entry(20, "b", ALP_UPDATE_STATUS_ROLLED_BACK);
	(void)ulog_engine_append(&s, &c, &ent0);
	(void)ulog_engine_append(&s, &c, &ent1);

	uint64_t n = 0;
	zassert_equal(ulog_engine_count(&s, &c, &n), ALP_OK);
	zassert_equal(n, 2);

	alp_update_log_entry_t e;
	zassert_equal(ulog_engine_get(&s, 1, &e), ALP_OK);
	zassert_equal(e.seq, 1);
	zassert_equal(e.status, ALP_UPDATE_STATUS_ROLLED_BACK);
	zassert_equal(strcmp(e.fw_version, "b"), 0);
	uint8_t want[32];
	memset(want, 20, 32);
	zassert_mem_equal(e.image_hash, want, 32);

	zassert_equal(ulog_engine_get(&s, 9, &e), ALP_ERR_NOT_FOUND);
}

/* --- Tier selection + degrade (issues #111, #239) ---------------------
 *
 * The real HW_ENFORCED TF-M tier only registers under BUILD_WITH_TFM, which
 * native_sim cannot build, so we exercise the dispatcher's ranked-walk +
 * open-time fall-through with a stand-in HW_ENFORCED backend registered
 * straight into the update_log class section. Its ready() verdict is driven
 * by g_fake_hw_ready so a single backend covers all three dispatcher
 * branches: bind (ALP_OK), degrade to the next tier (ALP_ERR_NOSUPPORT),
 * and surface a hard error (anything else). It is priority 20 (> the SW
 * tier's 10) so alp_update_log_open() always offers it first; its default
 * verdict is ALP_ERR_NOSUPPORT so every OTHER test in this suite still
 * degrades cleanly to the SW tier, exactly as on a real board whose HW tier
 * is not yet provisioned. */

static alp_status_t g_fake_hw_ready = ALP_ERR_NOSUPPORT;

static alp_status_t fake_hw_ready(void)
{
	return g_fake_hw_ready;
}
static alp_status_t fake_hw_append(const alp_update_log_entry_t *e)
{
	(void)e;
	return ALP_OK;
}
static alp_status_t fake_hw_verify(alp_update_log_verdict_t *v, uint64_t *bad)
{
	(void)bad;
	if (v) *v = ALP_UPDATE_LOG_VERIFY_OK;
	return ALP_OK;
}
static alp_status_t fake_hw_count(uint64_t *out)
{
	if (out) *out = 0;
	return ALP_OK;
}
static alp_status_t fake_hw_get(uint64_t seq, alp_update_log_entry_t *out)
{
	(void)seq;
	(void)out;
	return ALP_ERR_NOT_FOUND;
}
static const alp_update_log_ops_t _fake_hw_ops = {
	.assurance = ALP_UPDATE_LOG_HW_ENFORCED,
	.ready     = fake_hw_ready,
	.append    = fake_hw_append,
	.verify    = fake_hw_verify,
	.count     = fake_hw_count,
	.get       = fake_hw_get,
};
ALP_BACKEND_REGISTER(update_log,
                     fake_hw,
                     {
                         .silicon_ref = "*",
                         .vendor      = "fake_hw",
                         .base_caps   = 0u,
                         .priority    = 20,
                         .ops         = &_fake_hw_ops,
                         .probe       = NULL,
                     });

/* HW tier ready -> the dispatcher binds it and reports HW_ENFORCED. */
ZTEST(alp_update_log, test_selection_prefers_hw_when_ready)
{
	g_fake_hw_ready       = ALP_OK;
	alp_update_log_t *log = alp_update_log_open();
	zassert_not_null(log);
	zassert_equal(alp_update_log_assurance(log), ALP_UPDATE_LOG_HW_ENFORCED);
	alp_update_log_close(log);
	g_fake_hw_ready = ALP_ERR_NOSUPPORT; /* restore for the rest of the suite */
}

/* HW tier declines with NOSUPPORT -> the dispatcher falls through to the SW
 * tamper-evident tier, which serves a fully working log. */
ZTEST(alp_update_log, test_selection_degrades_to_sw_when_hw_not_ready)
{
	g_fake_hw_ready       = ALP_ERR_NOSUPPORT;
	alp_update_log_t *log = alp_update_log_open();
	zassert_not_null(log);
	zassert_equal(alp_update_log_assurance(log), ALP_UPDATE_LOG_SW_TAMPER_EVIDENT);
	alp_update_log_entry_t e = mk_entry(1, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(alp_update_log_append(log, &e), ALP_OK);
	alp_update_log_close(log);
}

/* A non-NOSUPPORT ready() verdict is a hard error: it is surfaced, NOT
 * masked by silently dropping to a lower tier. */
ZTEST(alp_update_log, test_selection_hard_error_surfaces)
{
	g_fake_hw_ready       = ALP_ERR_IO;
	alp_update_log_t *log = alp_update_log_open();
	zassert_is_null(log);
	zassert_equal(alp_last_error(), ALP_ERR_IO);
	g_fake_hw_ready = ALP_ERR_NOSUPPORT; /* restore for the rest of the suite */
}

/* --- Task 5: public-surface smoke (dispatch + sw_tier backend). Keep LAST. --- */

ZTEST(alp_update_log, test_public_surface_sw_tier)
{
	alp_update_log_t *log = alp_update_log_open();
	zassert_not_null(log);
	zassert_equal(alp_update_log_assurance(log), ALP_UPDATE_LOG_SW_TAMPER_EVIDENT);

	alp_update_log_entry_t e = mk_entry(42, "2.0.0", ALP_UPDATE_STATUS_PENDING_CONFIRM);
	zassert_equal(alp_update_log_append(log, &e), ALP_OK);

	uint64_t n = 0;
	zassert_equal(alp_update_log_count(log, &n), ALP_OK);
	zassert_true(n >= 1);
	alp_update_log_verdict_t v;
	zassert_equal(alp_update_log_verify(log, &v, NULL), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK);
	alp_update_log_close(log);
}

/* --- Persistence (#262): sw-tier store modes ------------------------
 *
 * alp_ulog_sw_tier_test_reset(false) emulates a reboot: it drops every
 * piece of RAM state the backend holds (RAM store, RAM counter, cached
 * NVS mount), so anything still readable afterwards came from the NVS
 * partition.  wipe=true additionally clears the partition (pristine
 * store) -- each test starts and ends with a wipe so the suite is
 * order-independent.
 */

#ifdef CONFIG_ALP_SDK_UPDATE_LOG_PERSIST
alp_status_t
alp_ulog_sw_tier_test_corrupt_persisted_entry(uint64_t seq, size_t offset, uint8_t xor_mask);
alp_status_t alp_ulog_sw_tier_test_delete_persisted_entry(uint64_t seq);

static void append_three_persisted_entries(void)
{
	alp_update_log_t *log = alp_update_log_open();
	zassert_not_null(log);
	const char *vers[3] = { "1.0.0", "1.1.0", "2.0.0" };
	for (int i = 0; i < 3; i++) {
		alp_update_log_entry_t e =
		    mk_entry((uint64_t)(i + 1) * 100, vers[i], ALP_UPDATE_STATUS_CONFIRMED);
		zassert_equal(alp_update_log_append(log, &e), ALP_OK);
	}
	alp_update_log_close(log);
}

ZTEST(alp_update_log, test_persist_entries_survive_reinit)
{
	alp_ulog_sw_tier_test_reset(true); /* pristine store */

	append_three_persisted_entries();

	alp_ulog_sw_tier_test_reset(false); /* "reboot": keep flash only */

	alp_update_log_t *log = alp_update_log_open();
	zassert_not_null(log);
	const char *vers[3] = { "1.0.0", "1.1.0", "2.0.0" };
	uint64_t    n       = 0;
	zassert_equal(alp_update_log_count(log, &n), ALP_OK);
	zassert_equal(n, 3, "entries lost across re-init (got %llu)", (unsigned long long)n);

	alp_update_log_verdict_t v;
	zassert_equal(alp_update_log_verify(log, &v, NULL), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK);

	for (int i = 0; i < 3; i++) {
		alp_update_log_entry_t got;
		zassert_equal(alp_update_log_get(log, (uint64_t)i, &got), ALP_OK);
		zassert_equal(got.seq, (uint64_t)i);
		zassert_equal(got.timestamp, (uint64_t)(i + 1) * 100);
		zassert_equal(got.status, ALP_UPDATE_STATUS_CONFIRMED);
		zassert_equal(strcmp(got.fw_version, vers[i]), 0);
		uint8_t want[32];
		memset(want, (int)((uint64_t)(i + 1) * 100), 32);
		zassert_mem_equal(got.image_hash, want, 32);
	}
	alp_update_log_close(log);
	alp_ulog_sw_tier_test_reset(true); /* leave a clean store behind */
}

ZTEST(alp_update_log, test_persist_verify_detects_mutated_entry)
{
	alp_ulog_sw_tier_test_reset(true);
	append_three_persisted_entries();

	alp_ulog_sw_tier_test_reset(false); /* "reboot": keep flash only */
	zassert_equal(alp_ulog_sw_tier_test_corrupt_persisted_entry(1, 12, 0xFF), ALP_OK);
	alp_ulog_sw_tier_test_reset(false);

	alp_update_log_t *log = alp_update_log_open();
	zassert_not_null(log);
	alp_update_log_verdict_t v;
	uint64_t                 bad = 0;
	zassert_equal(alp_update_log_verify(log, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN);
	zassert_equal(bad, 2);
	alp_update_log_close(log);
	alp_ulog_sw_tier_test_reset(true);
}

ZTEST(alp_update_log, test_persist_verify_detects_deleted_entry)
{
	alp_ulog_sw_tier_test_reset(true);
	append_three_persisted_entries();

	alp_ulog_sw_tier_test_reset(false); /* "reboot": keep flash only */
	zassert_equal(alp_ulog_sw_tier_test_delete_persisted_entry(2), ALP_OK);
	alp_ulog_sw_tier_test_reset(false);

	alp_update_log_t *log = alp_update_log_open();
	zassert_not_null(log);
	alp_update_log_verdict_t v;
	uint64_t                 bad = 0;
	zassert_equal(alp_update_log_verify(log, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_TRUNCATED);
	alp_update_log_close(log);
	alp_ulog_sw_tier_test_reset(true);
}

ZTEST(alp_update_log, test_persist_full_log_nomem_no_wrap)
{
	alp_ulog_sw_tier_test_reset(true);

	alp_update_log_t *log = alp_update_log_open();
	zassert_not_null(log);

	/* Fill the 16 KiB partition. Each append persists entry + meta +
	 * counter; the store must eventually report NOMEM -- never wrap. */
	uint64_t     appended = 0;
	alp_status_t rc       = ALP_OK;
	for (int i = 0; i < 2000; i++) {
		alp_update_log_entry_t e = mk_entry((uint64_t)i + 1, "9.9.9", ALP_UPDATE_STATUS_CONFIRMED);
		rc                       = alp_update_log_append(log, &e);
		if (rc != ALP_OK) {
			break;
		}
		appended++;
	}
	zassert_equal(rc, ALP_ERR_NOMEM, "full log must report NOMEM (got %d)", (int)rc);
	zassert_true(appended > 0);

	/* The failed append must not have damaged the chain. */
	uint64_t n = 0;
	zassert_equal(alp_update_log_count(log, &n), ALP_OK);
	zassert_equal(n, appended);
	alp_update_log_verdict_t v;
	zassert_equal(alp_update_log_verify(log, &v, NULL), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK);

	/* Still full (and still verifiable) after a "reboot". */
	alp_update_log_close(log);
	alp_ulog_sw_tier_test_reset(false);
	log = alp_update_log_open();
	zassert_not_null(log);
	alp_update_log_entry_t e = mk_entry(7, "9.9.9", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(alp_update_log_append(log, &e), ALP_ERR_NOMEM);
	zassert_equal(alp_update_log_verify(log, &v, NULL), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK);

	alp_update_log_close(log);
	alp_ulog_sw_tier_test_reset(true);
}

#else /* !CONFIG_ALP_SDK_UPDATE_LOG_PERSIST */

ZTEST(alp_update_log, test_ram_fallback_entries_do_not_survive_reinit)
{
	alp_ulog_sw_tier_test_reset(true);

	alp_update_log_t *log = alp_update_log_open();
	zassert_not_null(log);
	zassert_equal(alp_update_log_assurance(log), ALP_UPDATE_LOG_SW_TAMPER_EVIDENT);
	alp_update_log_entry_t e = mk_entry(1, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(alp_update_log_append(log, &e), ALP_OK);
	uint64_t n = 0;
	zassert_equal(alp_update_log_count(log, &n), ALP_OK);
	zassert_equal(n, 1);
	alp_update_log_close(log);

	/* RAM store: a re-init (reboot) loses everything -- the documented
	 * fallback behaviour when no alp_ulog_partition exists. */
	alp_ulog_sw_tier_test_reset(false);
	log = alp_update_log_open();
	zassert_not_null(log);
	zassert_equal(alp_update_log_count(log, &n), ALP_OK);
	zassert_equal(n, 0, "RAM fallback must not persist (got %llu)", (unsigned long long)n);
	alp_update_log_verdict_t v;
	zassert_equal(alp_update_log_verify(log, &v, NULL), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK); /* empty log verifies clean */
	alp_update_log_close(log);
	alp_ulog_sw_tier_test_reset(true);
}

#endif /* CONFIG_ALP_SDK_UPDATE_LOG_PERSIST */
