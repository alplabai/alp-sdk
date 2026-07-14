/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include <zephyr/kernel.h>
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

/* Minimal RAM store double: fixed slots of (key,blob). Sized to cover the
 * concurrent-append fault-injection test below (2 threads x CONC_ITERS
 * entries + 1 meta record). */
#define TD_SLOTS 48
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

/* WRITE_ONCE store double for DEFECT 1 (GHSA-r236-29pg-w694): a plain RAM
 * double silently overwrites a re-put key, which would hide the PSA-tier
 * wedge instead of reproducing it. This wraps td_put() to reject an
 * overwrite of an already-created "ulog.<seq>" entry key exactly the way
 * src/update_log/tfm_psa_secure_owner.c's ps_put() creates every entry
 * asset with PSA_STORAGE_FLAG_WRITE_ONCE -- a second psa_ps_set() on that
 * UID returns PSA_ERROR_NOT_PERMITTED, which psa_to_alp() maps to
 * ALP_ERR_IO. "ulog.meta" stays freely rewritable (mutable metadata, same
 * as the real PSA owner). */
static alp_status_t td_put_write_once(void *c, const char *key, const uint8_t *b, size_t n)
{
	struct td_store *t        = td(c);
	bool             is_entry = strncmp(key, "ulog.", 5) == 0 && strcmp(key, "ulog.meta") != 0;
	if (is_entry && td_find(t, key) >= 0) {
		return ALP_ERR_IO; /* mirrors PSA_ERROR_NOT_PERMITTED on a WRITE_ONCE asset */
	}
	return td_put(c, key, b, n);
}

static void
td_ifaces_write_once(struct td_store *t, alp_secure_store_if *s, alp_monotonic_counter_if *ctr)
{
	td_ifaces(t, s, ctr);
	s->put = td_put_write_once;
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

void alp_ulog_sw_tier_test_reset(bool wipe);

static bool                   g_boot_meta_ready;
static alp_update_log_entry_t g_boot_meta;

alp_status_t alp_update_log_boot_metadata_read(alp_update_log_entry_t *entry_out)
{
	if (entry_out == NULL) {
		return ALP_ERR_INVAL;
	}
	if (!g_boot_meta_ready) {
		return ALP_ERR_NOSUPPORT;
	}

	*entry_out = g_boot_meta;
	return ALP_OK;
}

static void set_boot_meta(const char *version, uint8_t hash_byte, alp_update_status_t status)
{
	memset(&g_boot_meta, 0, sizeof(g_boot_meta));
	strncpy(g_boot_meta.fw_version, version, ALP_UPDATE_LOG_FWVER_MAX);
	memset(g_boot_meta.image_hash, hash_byte, sizeof(g_boot_meta.image_hash));
	g_boot_meta.status = status;
	g_boot_meta.seq    = 99; /* provider value must not leak through */
	g_boot_meta_ready  = true;
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

/* --- Crash- and concurrency-safety fault injection (GHSA-r236-29pg-w694)
 *
 * ulog_engine_append() durably mutates the entry blob, then the meta
 * cache, then the counter -- three separate calls a power loss can land
 * between. The tests below write directly against the td_store to land
 * it in exactly the torn state each crash point would leave (bypassing
 * ulog_engine_append entirely, since a real crash mid-call cannot be
 * expressed by calling the function that is supposed to be atomic), then
 * prove the engine recovers deterministically: no orphan wedge, no false
 * rollback, no re-chained duplicate sequence, and real corruption still
 * gets reported rather than silently "fixed".
 */

/* Mirrors engine.c's internal kbuf() key format ("ulog.<decimal>") for
 * small seq values, so this file doesn't need engine.c's static helper
 * (nor libc snprintf) just to poke a key directly into the store double. */
static void test_key(char *out, size_t cap, uint64_t seq)
{
	char tmp[24];
	int  n = 0;
	do {
		tmp[n++] = (char)('0' + (seq % 10u));
		seq /= 10u;
	} while (seq && n < 20);
	size_t      pos = 0;
	const char *pfx = "ulog.";
	while (*pfx && pos < cap - 1)
		out[pos++] = *pfx++;
	while (n > 0 && pos < cap - 1)
		out[pos++] = tmp[--n];
	out[pos] = 0;
}

/* Durably write entry `seq` directly against the store, as if
 * ulog_engine_append() had completed only its entry-write phase (the
 * transaction's sole durable commit point) and then crashed before
 * touching meta or the counter. Returns the entry's wire hash so the
 * caller can drive meta into whichever torn state it wants next. */
static void
raw_write_entry(struct td_store *t, uint64_t seq, const uint8_t prev[32], uint8_t hash_out[32])
{
	alp_update_log_entry_t e = mk_entry((seq + 1) * 10, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED);
	e.seq                    = seq;
	uint8_t wire[ULOG_ENTRY_WIRE_LEN];
	zassert_equal(ulog_entry_encode(&e, prev, wire), ALP_OK);
	char key[24];
	test_key(key, sizeof(key), seq);
	zassert_equal(td_put(t, key, wire, sizeof(wire)), ALP_OK);
	ulog_sha256(wire, sizeof(wire), hash_out);
}

static void raw_write_meta(struct td_store *t, uint64_t count, const uint8_t head_hash[32])
{
	struct ulog_meta m;
	m.count = count;
	memcpy(m.head_hash, head_hash, 32);
	uint8_t metabuf[ULOG_META_WIRE_LEN];
	zassert_equal(ulog_meta_encode(&m, metabuf), ALP_OK);
	zassert_equal(td_put(t, "ulog.meta", metabuf, sizeof(metabuf)), ALP_OK);
}

/* Crash case 1: power loss after the entry write, before meta or the
 * counter. In the PSA secure owner this is the shape that used to
 * permanently wedge the log (the write-once entry could never be
 * rewritten by a later append); here it must self-heal instead. */
ZTEST(alp_update_log, test_recover_orphan_entry_after_entry_write_crash)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 2); /* entries 0,1 committed; counter == meta.count == 2 */

	uint8_t metabuf[ULOG_META_WIRE_LEN];
	size_t  mlen = 0;
	zassert_equal(td_get(&t, "ulog.meta", metabuf, sizeof(metabuf), &mlen), ALP_OK);
	struct ulog_meta m;
	zassert_equal(ulog_meta_decode(metabuf, mlen, &m), ALP_OK);

	uint8_t hash2[32];
	raw_write_entry(&t, 2, m.head_hash, hash2); /* entry 2 landed... */
	zassert_equal(t.counter, 2);                /* ...meta/counter never caught up */

	/* Even before any repair, the orphan is already readable: a
	 * write-once entry that landed on disk is never a wedge by itself,
	 * only the stale bookkeeping around it needs fixing. */
	alp_update_log_entry_t got;
	zassert_equal(ulog_engine_get(&s, 2, &got), ALP_OK);

	/* verify() must self-heal and see one valid 3-entry chain -- not a
	 * truncation, not a chain break, not a rollback. */
	alp_update_log_verdict_t v;
	uint64_t                 bad = 999;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK);
	zassert_equal(t.counter, 3, "recovery must advance the counter past the orphan");

	zassert_equal(td_get(&t, "ulog.meta", metabuf, sizeof(metabuf), &mlen), ALP_OK);
	zassert_equal(ulog_meta_decode(metabuf, mlen, &m), ALP_OK);
	zassert_equal(m.count, 3);
	zassert_mem_equal(m.head_hash, hash2, 32);

	/* The next append must continue at seq 3, never re-chain against 2. */
	alp_update_log_entry_t ent3 = mk_entry(999, "next", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(ulog_engine_append(&s, &c, &ent3), ALP_OK);
	zassert_equal(t.counter, 4);
	alp_update_log_entry_t got3;
	zassert_equal(ulog_engine_get(&s, 3, &got3), ALP_OK);
	zassert_equal(got3.seq, 3);
}

/* Crash case 2: power loss after meta but before the counter increment --
 * "meta ahead of counter". Trusting meta as a second independent
 * authority (the pre-fix design) reports ROLLED_BACK here on an entirely
 * honest log, and a naive retry would re-chain a repeated sequence
 * against the wrong head. */
ZTEST(alp_update_log, test_recover_meta_ahead_of_counter_after_crash)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 2);

	uint8_t metabuf[ULOG_META_WIRE_LEN];
	size_t  mlen = 0;
	zassert_equal(td_get(&t, "ulog.meta", metabuf, sizeof(metabuf), &mlen), ALP_OK);
	struct ulog_meta m;
	zassert_equal(ulog_meta_decode(metabuf, mlen, &m), ALP_OK);

	uint8_t hash2[32];
	raw_write_entry(&t, 2, m.head_hash, hash2);
	raw_write_meta(&t, 3, hash2); /* meta already advanced... */
	zassert_equal(t.counter, 2);  /* ...but the counter increment never landed. */

	alp_update_log_verdict_t v;
	uint64_t                 bad = 999;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(
	    v, ALP_UPDATE_LOG_VERIFY_OK, "meta-ahead-of-counter must not be reported as a rollback");
	zassert_equal(t.counter, 3, "recovery must advance the counter to match the committed meta");

	/* A subsequent append must continue at seq 3, not re-use 2. */
	alp_update_log_entry_t ent3 = mk_entry(999, "next", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(ulog_engine_append(&s, &c, &ent3), ALP_OK);
	alp_update_log_entry_t got3;
	zassert_equal(ulog_engine_get(&s, 3, &got3), ALP_OK);
	zassert_equal(got3.seq, 3);
	uint64_t n = 0;
	zassert_equal(ulog_engine_count(&s, &c, &n), ALP_OK);
	zassert_equal(n, 4);
}

/* Data sitting at "ulog.<hw>" that does NOT chain from the true
 * preceding entry is not the torn-append shape (a real crash can only
 * ever land the exact wire ulog_engine_append() itself encoded, chained
 * from whatever meta.head_hash was at that moment -- see the block
 * comment above ulog_recover()). Recovery must not mistake it for a
 * completed append and must not adopt it into the chain: the counter
 * stays put, and recovery never discards or rewrites the frontier
 * slot's contents (a write-once tier could not anyway) -- it simply
 * declines to count it.
 *
 * DEFECT 1 (GHSA-r236-29pg-w694): ulog_engine_verify()'s chain walk used
 * to cover only [0, hw), never looking at slot hw itself, so this exact
 * state used to report VERIFY_OK -- a silent lie on any tier where
 * append()'s subsequent store->put() to that same occupied key then
 * keeps failing (permanently, on a WRITE_ONCE tier). It must now report
 * CHAIN_BROKEN with bad_seq_out == hw. */
ZTEST(alp_update_log, test_recover_does_not_adopt_non_chaining_frontier_entry)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 2);

	uint8_t bogus_prev[32];
	memset(bogus_prev, 0x77, sizeof(bogus_prev)); /* deliberately wrong */
	uint8_t discard[32];
	raw_write_entry(&t, 2, bogus_prev, discard);
	zassert_equal(t.counter, 2);

	alp_update_log_verdict_t v;
	uint64_t                 bad = 999;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v,
	              ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN,
	              "verify() must not report OK while a non-chaining entry squats the frontier");
	zassert_equal(bad, 2, "bad_seq_out must point at the squatted frontier slot");
	zassert_equal(
	    t.counter, 2, "recovery must not adopt/count non-chaining data at the frontier slot");

	uint64_t n = 0;
	zassert_equal(ulog_engine_count(&s, &c, &n), ALP_OK);
	zassert_equal(n, 2, "the un-adopted frontier entry must not be counted");

	/* This store is mutable (RAM-like, not write-once), so append() can
	 * still make progress: it overwrites the squatted key and the log
	 * self-heals -- the SW-NVS-tier half of DEFECT 1's fix. Contrast with
	 * the write-once double below, where the same shape wedges instead. */
	alp_update_log_entry_t healed = mk_entry(999, "self-heal", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(ulog_engine_append(&s, &c, &healed),
	              ALP_OK,
	              "a mutable tier must be able to overwrite a corrupt frontier slot");
	zassert_equal(t.counter, 3);
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK, "the chain is clean again once the frontier heals");
}

/* DEFECT 1 (GHSA-r236-29pg-w694), write-once tier: the exact same
 * non-chaining-frontier shape as above, but against a store double that
 * mimics PSA Protected Storage's WRITE_ONCE entry assets. Here append()
 * cannot self-heal -- the frontier slot can never be overwritten -- so it
 * must fail loudly and permanently (ALP_ERR_IO) instead of silently
 * wedging behind a verify() that still (incorrectly) said OK. */
ZTEST(alp_update_log, test_recover_frontier_corruption_wedges_write_once_tier)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	td_ifaces_write_once(&t, &s, &c);

	for (int i = 0; i < 2; i++) {
		alp_update_log_entry_t ent =
		    mk_entry((uint64_t)(i + 1) * 10, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED);
		zassert_equal(ulog_engine_append(&s, &c, &ent), ALP_OK);
	}
	zassert_equal(t.counter, 2);

	uint8_t bogus_prev[32];
	memset(bogus_prev, 0x77, sizeof(bogus_prev)); /* deliberately wrong */
	uint8_t discard[32];
	raw_write_entry(&t, 2, bogus_prev, discard); /* direct write: bypasses the write-once gate */

	alp_update_log_verdict_t v;
	uint64_t                 bad = 999;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v,
	              ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN,
	              "verify() must surface the squatted frontier instead of lying with OK");
	zassert_equal(bad, 2);

	/* append() cannot clear a WRITE_ONCE slot: it must return a clear
	 * terminal error, never retry with a forced overwrite, and never
	 * advance the counter over the still-corrupt frontier. */
	alp_update_log_entry_t next = mk_entry(999, "wedge", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(ulog_engine_append(&s, &c, &next),
	              ALP_ERR_IO,
	              "append must return a terminal error against a write-once frontier conflict");
	zassert_equal(t.counter, 2, "a wedged append must not advance the counter");

	/* The wedge is durable: verify() keeps reporting it, not a one-shot
	 * fluke, and the log never lies with OK afterwards either. */
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN);
	zassert_equal(bad, 2);
}

/* --- Issue #759: append() must not trust stale/malformed ulog.meta ----
 *
 * ulog_recover() only ever repairs the single torn-append shape where an
 * entry already sits at the frontier; when nothing is there -- the
 * ordinary steady-state case on every append -- it returns ALP_OK without
 * ever having examined ulog.meta. Before the fix, ulog_engine_append()
 * then read ulog.meta and trusted its cached head_hash unconditionally to
 * chain the NEW entry, without requiring meta.count == hw or verifying
 * the hash against the actual preceding entry. The tests below drive
 * ulog.meta into exactly the states recovery has no reason to touch
 * (directly, since the engine itself never produces a self-inconsistent
 * meta) and prove append() now refuses rather than durably writes against
 * an unverified cache -- damage a write-once tier could never undo.
 */

ZTEST(alp_update_log, test_append_refuses_stale_meta_head_hash)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 3); /* entries 0,1,2 committed; counter == meta.count == 3 */

	uint8_t bogus_head[32];
	memset(bogus_head, 0x42, sizeof(bogus_head)); /* well-formed encoding, wrong content */
	raw_write_meta(&t, 3, bogus_head);            /* count still agrees with hw == 3 */

	alp_update_log_entry_t next = mk_entry(999, "next", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(ulog_engine_append(&s, &c, &next),
	              ALP_ERR_IO,
	              "append must refuse rather than chain from an unverified cached head hash");
	zassert_equal(t.counter, 3, "a refused append must not advance the counter");
	zassert_equal(td_find(&t, "ulog.3"), -1, "a refused append must not write the new entry");

	/* verify() independently flags the same corruption (its own
	 * head-hash-vs-computed check, unrelated to append()'s guard): the
	 * real entries 0..2 chain cleanly among themselves, but the cached
	 * head no longer matches their true hash. */
	alp_update_log_verdict_t v;
	uint64_t                 bad = 999;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN);
	zassert_equal(bad, 2);
}

ZTEST(alp_update_log, test_append_refuses_stale_meta_count)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 3); /* entries 0,1,2 committed; counter == meta.count == 3 */

	uint8_t metabuf[ULOG_META_WIRE_LEN];
	size_t  mlen = 0;
	zassert_equal(td_get(&t, "ulog.meta", metabuf, sizeof(metabuf), &mlen), ALP_OK);
	struct ulog_meta m;
	zassert_equal(ulog_meta_decode(metabuf, mlen, &m), ALP_OK);

	/* Re-write meta with the CORRECT head hash but a stale count: a
	 * hash match alone must not be enough to trust the cache. */
	raw_write_meta(&t, 5, m.head_hash);

	alp_update_log_entry_t next = mk_entry(999, "next", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(ulog_engine_append(&s, &c, &next),
	              ALP_ERR_IO,
	              "append must refuse on a meta.count that disagrees with the counter");
	zassert_equal(t.counter, 3, "a refused append must not advance the counter");
	zassert_equal(td_find(&t, "ulog.3"), -1, "a refused append must not write the new entry");

	/* verify() has its own independent count-vs-counter check and reports
	 * ROLLED_BACK for the same corrupted state. */
	alp_update_log_verdict_t v;
	uint64_t                 bad = 999;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_ROLLED_BACK);
}

ZTEST(alp_update_log, test_append_refuses_missing_meta_with_existing_entries)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 3); /* entries 0,1,2 committed; counter == 3 */

	zassert_equal(td_erase(&t, "ulog.meta"), ALP_OK);

	alp_update_log_entry_t next = mk_entry(999, "next", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(ulog_engine_append(&s, &c, &next),
	              ALP_ERR_IO,
	              "append must refuse rather than fabricate a prev_hash when meta is absent");
	zassert_equal(t.counter, 3, "a refused append must not advance the counter");
	zassert_equal(td_find(&t, "ulog.3"), -1, "a refused append must not write the new entry");
}

/* A partially-written record squatting on the frontier slot: shorter than
 * a valid encoded entry, so ulog_entry_decode() rejects it outright
 * (ALP_ERR_INVAL for a short buffer -- see test_decode_short_buffer).
 * ulog_recover() must leave it untouched exactly like the non-chaining,
 * full-length case above (DEFECT 1): it is not the torn-append shape (a
 * real crash can only ever land the exact wire ulog_engine_append()
 * itself encoded), so it must not be adopted, and on a WRITE_ONCE tier
 * append() can never clear it -- it must fail loudly instead of wedging
 * silently behind a verify() that still said OK. */
ZTEST(alp_update_log, test_recover_leaves_partially_written_frontier_record_write_once)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	td_ifaces_write_once(&t, &s, &c);

	for (int i = 0; i < 2; i++) {
		alp_update_log_entry_t ent =
		    mk_entry((uint64_t)(i + 1) * 10, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED);
		zassert_equal(ulog_engine_append(&s, &c, &ent), ALP_OK);
	}
	zassert_equal(t.counter, 2);

	/* Land a truncated blob directly at "ulog.2" -- a torn write that
	 * stopped short of a complete entry record. Bypasses the write-once
	 * gate deliberately: the point is to reproduce the ON-DISK shape, not
	 * to exercise td_put_write_once() here. */
	uint8_t truncated[ULOG_ENTRY_WIRE_LEN / 2];
	memset(truncated, 0xAB, sizeof(truncated));
	char key[24];
	test_key(key, sizeof(key), 2);
	zassert_equal(td_put(&t, key, truncated, sizeof(truncated)), ALP_OK);

	alp_update_log_verdict_t v;
	uint64_t                 bad = 999;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v,
	              ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN,
	              "verify() must surface a truncated frontier record instead of lying with OK");
	zassert_equal(bad, 2);
	zassert_equal(
	    t.counter, 2, "recovery must not adopt/count a partially-written frontier record");

	/* append() cannot clear a WRITE_ONCE slot: it must return a clear
	 * terminal error, never retry with a forced overwrite, and never
	 * advance the counter over the still-corrupt frontier. */
	alp_update_log_entry_t next = mk_entry(999, "wedge", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(
	    ulog_engine_append(&s, &c, &next),
	    ALP_ERR_IO,
	    "append must return a terminal error against a partially-written write-once frontier");
	zassert_equal(t.counter, 2, "a wedged append must not advance the counter");
}

/* An already-written slot must never be silently rewritten. Here BOTH
 * defenses that guard that invariant are in play at once: the frontier
 * key "ulog.3" is already occupied by data planted out-of-band (as if by
 * a prior, unrelated bug or a bad recovery elsewhere), and ulog.meta is
 * independently stale. append()'s meta re-derivation/consistency check
 * (issue #759) must reject the append BEFORE it ever reaches
 * store->put() -- proving the engine does not even ATTEMPT the rewrite,
 * let alone rely on the store's write-once rejection to catch it after
 * the fact. The pre-existing (garbage) contents of "ulog.3" are left
 * completely undisturbed. */
ZTEST(alp_update_log, test_append_refuses_before_attempting_rewrite_of_occupied_slot)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	td_ifaces_write_once(&t, &s, &c);

	for (int i = 0; i < 3; i++) {
		alp_update_log_entry_t ent =
		    mk_entry((uint64_t)(i + 1) * 10, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED);
		zassert_equal(ulog_engine_append(&s, &c, &ent), ALP_OK);
	}
	zassert_equal(t.counter, 3);

	/* Slot 3 already holds unrelated data (planted directly, bypassing
	 * both the engine and the write-once gate). */
	uint8_t bogus_prev[32];
	memset(bogus_prev, 0x11, sizeof(bogus_prev));
	uint8_t occupant_hash[32];
	raw_write_entry(&t, 3, bogus_prev, occupant_hash);
	uint8_t occupant_snapshot[TD_BLOB];
	int     occupant_idx = td_find(&t, "ulog.3");
	zassert_true(occupant_idx >= 0);
	memcpy(occupant_snapshot, t.s[occupant_idx].buf, TD_BLOB);

	/* meta is independently stale, unrelated to slot 3's occupant. */
	uint8_t bogus_head[32];
	memset(bogus_head, 0x77, sizeof(bogus_head));
	raw_write_meta(&t, 3, bogus_head);

	alp_update_log_entry_t next = mk_entry(999, "rewrite-attempt", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(ulog_engine_append(&s, &c, &next),
	              ALP_ERR_IO,
	              "append must refuse via the meta check before ever touching the occupied slot");
	zassert_equal(t.counter, 3, "a refused append must not advance the counter");

	/* The pre-existing occupant of "ulog.3" is byte-for-byte untouched --
	 * append() never called store->put() against it. */
	occupant_idx = td_find(&t, "ulog.3");
	zassert_true(occupant_idx >= 0);
	zassert_mem_equal(t.s[occupant_idx].buf, occupant_snapshot, TD_BLOB);
}

/* DEFECT 3 (GHSA-r236-29pg-w694): with a functioning monotonic counter, at
 * most ONE validly-chained orphan can ever sit above it (ulog_recover()
 * always runs before append() writes a new entry, so it never lets a
 * second one accumulate). Seeing TWO validly-chained entries above the
 * counter is proof the counter itself regressed -- ulog_recover() must
 * adopt at most one of them (never loop-adopt), and ulog_engine_verify()
 * must report ROLLED_BACK for the one it deliberately leaves behind,
 * not a laundered VERIFY_OK. */
ZTEST(alp_update_log, test_recover_adopts_at_most_one_orphan_reports_rolled_back)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	seed(&t, &s, &c, 2); /* entries 0,1 committed; counter == meta.count == 2 */

	uint8_t metabuf[ULOG_META_WIRE_LEN];
	size_t  mlen = 0;
	zassert_equal(td_get(&t, "ulog.meta", metabuf, sizeof(metabuf), &mlen), ALP_OK);
	struct ulog_meta m;
	zassert_equal(ulog_meta_decode(metabuf, mlen, &m), ALP_OK);

	/* Plant two entries chained back-to-back above the counter -- the
	 * shape an honest crash can never produce by itself. */
	uint8_t hash2[32];
	raw_write_entry(&t, 2, m.head_hash, hash2);
	uint8_t hash3[32];
	raw_write_entry(&t, 3, hash2, hash3);
	zassert_equal(t.counter, 2, "counter untouched until recovery runs");

	alp_update_log_verdict_t v;
	uint64_t                 bad = 999;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(
	    v,
	    ALP_UPDATE_LOG_VERIFY_ROLLED_BACK,
	    "a second orphan above the counter is proof of rollback, not a second torn append");
	zassert_equal(
	    t.counter,
	    3,
	    "recovery adopts AT MOST ONE orphan per call -- it must not loop-adopt the second");
}

/* Counter/sequence overflow guard: refuse rather than risk a truncated
 * "ulog.<seq>" key (kbuf()'s 24-byte buffer) or an eventual counter wrap. */
ZTEST(alp_update_log, test_append_guards_sequence_overflow)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	td_ifaces(&t, &s, &c);
	t.counter = ULOG_SEQ_MAX + 1u;

	alp_update_log_entry_t e = mk_entry(1, "overflow", ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(ulog_engine_append(&s, &c, &e), ALP_ERR_NOMEM);
	zassert_equal(t.counter, ULOG_SEQ_MAX + 1u, "a refused append must not touch the counter");
	for (int i = 0; i < TD_SLOTS; i++) {
		zassert_false(t.s[i].used, "a refused append must not have written anything");
	}
}

/* Concurrency case: two threads call ulog_engine_append() against the
 * SAME store/counter with no external synchronization. Before the fix,
 * both could read the same counter value, each write "ulog.<N>", and the
 * counter would end up claiming N+2 entries while ulog.(N+1) never
 * existed. The engine's internal lock must serialize them into one
 * valid, contiguous chain. */

#define CONC_ITERS      16
#define CONC_STACK_SIZE 2048

K_THREAD_STACK_DEFINE(g_conc_stack_a, CONC_STACK_SIZE);
K_THREAD_STACK_DEFINE(g_conc_stack_b, CONC_STACK_SIZE);

struct conc_worker_ctx {
	alp_secure_store_if      *s;
	alp_monotonic_counter_if *c;
	int                       ok_count;
};

static void conc_worker(void *p1, void *p2, void *p3)
{
	(void)p2;
	(void)p3;
	struct conc_worker_ctx *ctx = (struct conc_worker_ctx *)p1;
	for (int i = 0; i < CONC_ITERS; i++) {
		alp_update_log_entry_t e = mk_entry((uint64_t)i, "race", ALP_UPDATE_STATUS_CONFIRMED);
		if (ulog_engine_append(ctx->s, ctx->c, &e) == ALP_OK) {
			ctx->ok_count++;
		}
		k_yield();
	}
}

ZTEST(alp_update_log, test_concurrent_append_serializes_to_one_valid_chain)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	td_ifaces(&t, &s, &c);

	struct conc_worker_ctx ctx_a = { &s, &c, 0 };
	struct conc_worker_ctx ctx_b = { &s, &c, 0 };

	struct k_thread th_a, th_b;
	k_thread_create(&th_a,
	                g_conc_stack_a,
	                CONC_STACK_SIZE,
	                conc_worker,
	                &ctx_a,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(5),
	                0,
	                K_NO_WAIT);
	k_thread_create(&th_b,
	                g_conc_stack_b,
	                CONC_STACK_SIZE,
	                conc_worker,
	                &ctx_b,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(5),
	                0,
	                K_NO_WAIT);
	k_thread_join(&th_a, K_FOREVER);
	k_thread_join(&th_b, K_FOREVER);

	int total_ok = ctx_a.ok_count + ctx_b.ok_count;
	zassert_equal(total_ok, 2 * CONC_ITERS, "every append must succeed against a RAM store");
	zassert_equal(t.counter,
	              (uint64_t)total_ok,
	              "the counter must equal the successful-append count exactly -- no lost "
	              "and no double-counted sequence number");

	uint64_t n = 0;
	zassert_equal(ulog_engine_count(&s, &c, &n), ALP_OK);
	zassert_equal(n, (uint64_t)total_ok);

	alp_update_log_verdict_t v;
	uint64_t                 bad = 0;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v,
	              ALP_UPDATE_LOG_VERIFY_OK,
	              "concurrent appenders must still produce one valid, unbroken chain");

	/* Every sequence 0..total_ok-1 exists exactly once: td_put() would
	 * silently overwrite a duplicate key, so a successful get() for every
	 * index proves uniqueness+contiguity, not merely that the counter
	 * looks right. */
	for (uint64_t i = 0; i < (uint64_t)total_ok; i++) {
		alp_update_log_entry_t got;
		zassert_equal(ulog_engine_get(&s, i, &got), ALP_OK);
		zassert_equal(got.seq, i);
	}
}

/* Cross-priority concurrency case (GHSA-r236-29pg-w694 follow-up): the
 * equal-priority test above passes even with a spin+k_yield() lock,
 * because k_yield() only cedes the CPU to threads at the SAME OR HIGHER
 * priority as the caller -- a same-priority contender is exactly the one
 * case it handles. It says nothing about a LOWER-priority holder.
 *
 * Here a low-priority thread holds g_engine_lock across a simulated
 * flash-GC stall (td_put_slow_gc()'s k_sleep(), standing in for
 * nvs_write()'s occasional sector erase), while a HIGHER-priority thread
 * contends for the same lock. Under a spin+k_yield() lock this livelocks
 * forever on a single core: the higher-priority spinner is always the
 * highest-priority ready thread, so k_yield() hands the CPU straight back
 * to it instead of the (runnable, not blocked) low-priority holder, which
 * never gets to finish its k_sleep()/put() and release. Under the k_mutex
 * fix, k_mutex_lock()'s priority inheritance boosts the holder above the
 * waiter so it actually runs to completion. Both threads must complete
 * within a bounded k_thread_join() timeout -- a hang here means the
 * livelock is back -- and the resulting chain must still validate. */

K_SEM_DEFINE(g_gc_holder_entered, 0, 1);

/* Wraps td_put() to stand in for an NVS write that triggers garbage
 * collection: signals g_gc_holder_entered (so the contender only starts
 * racing once the holder is provably inside the critical section, not on
 * a lucky scheduling guess) and then sleeps for a stretch, all while
 * g_engine_lock is held by the caller (ulog_engine_append()). */
static alp_status_t td_put_slow_gc(void *c, const char *key, const uint8_t *b, size_t n)
{
	bool is_entry = strncmp(key, "ulog.", 5) == 0 && strcmp(key, "ulog.meta") != 0;
	if (is_entry) {
		k_sem_give(&g_gc_holder_entered);
		k_sleep(K_MSEC(50));
	}
	return td_put(c, key, b, n);
}

struct prio_worker_ctx {
	alp_secure_store_if      *s;
	alp_monotonic_counter_if *c;
	alp_status_t              result;
};

K_THREAD_STACK_DEFINE(g_prio_stack_lo, CONC_STACK_SIZE);
K_THREAD_STACK_DEFINE(g_prio_stack_hi, CONC_STACK_SIZE);

static void low_prio_gc_holder(void *p1, void *p2, void *p3)
{
	(void)p2;
	(void)p3;
	struct prio_worker_ctx *ctx = (struct prio_worker_ctx *)p1;
	alp_update_log_entry_t  e   = mk_entry(1, "gc-holder", ALP_UPDATE_STATUS_CONFIRMED);
	ctx->result                 = ulog_engine_append(ctx->s, ctx->c, &e);
}

static void high_prio_contender(void *p1, void *p2, void *p3)
{
	(void)p2;
	(void)p3;
	struct prio_worker_ctx *ctx = (struct prio_worker_ctx *)p1;
	/* Only contend once the low-priority thread is provably mid-GC inside
	 * ulog_engine_lock() -- makes this a genuine cross-priority
	 * contention, not a race that might get lucky either way. */
	k_sem_take(&g_gc_holder_entered, K_FOREVER);
	alp_update_log_entry_t e = mk_entry(2, "contender", ALP_UPDATE_STATUS_CONFIRMED);
	ctx->result              = ulog_engine_append(ctx->s, ctx->c, &e);
}

ZTEST(alp_update_log, test_concurrent_append_unequal_priority_no_livelock)
{
	struct td_store          t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	td_ifaces(&t, &s, &c);
	s.put = td_put_slow_gc;

	struct prio_worker_ctx ctx_lo = { &s, &c, ALP_ERR_INVAL };
	struct prio_worker_ctx ctx_hi = { &s, &c, ALP_ERR_INVAL };

	struct k_thread th_lo, th_hi;
	/* Numerically higher K_PRIO_PREEMPT() argument == LOWER priority. */
	k_thread_create(&th_lo,
	                g_prio_stack_lo,
	                CONC_STACK_SIZE,
	                low_prio_gc_holder,
	                &ctx_lo,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(7),
	                0,
	                K_NO_WAIT);
	k_thread_create(&th_hi,
	                g_prio_stack_hi,
	                CONC_STACK_SIZE,
	                high_prio_contender,
	                &ctx_hi,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(1),
	                0,
	                K_NO_WAIT);

	/* Bounded, not K_FOREVER: a livelocked cross-priority lock would hang
	 * this join forever instead of failing fast. */
	zassert_equal(k_thread_join(&th_hi, K_MSEC(2000)),
	              0,
	              "higher-priority contender must complete -- a hang here is the "
	              "cross-priority livelock the fix closes");
	zassert_equal(k_thread_join(&th_lo, K_MSEC(2000)),
	              0,
	              "low-priority GC holder must complete and release the lock");

	zassert_equal(ctx_lo.result, ALP_OK);
	zassert_equal(ctx_hi.result, ALP_OK);

	uint64_t n = 0;
	zassert_equal(ulog_engine_count(&s, &c, &n), ALP_OK);
	zassert_equal(n, 2u);

	alp_update_log_verdict_t v;
	uint64_t                 bad = 0;
	zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
	zassert_equal(v,
	              ALP_UPDATE_LOG_VERIFY_OK,
	              "unequal-priority contention must still produce one valid chain");
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

/* --- Trusted boot metadata path (#263) -------------------------------
 *
 * The production provider is weak/default-NOSUPPORT until a board wires
 * MCUboot shared data or Alif SE verification.  This test's strong provider
 * proves the public helper and append path copy identity/status from that
 * provider only; app-filled entries cannot override it.
 */

ZTEST(alp_update_log, test_boot_metadata_helper_reports_nosupport_without_provider)
{
	g_boot_meta_ready        = false;
	alp_update_log_entry_t e = mk_entry(11, "forged", ALP_UPDATE_STATUS_ROLLED_BACK);
	zassert_equal(alp_update_log_entry_from_boot_metadata(&e, 1234), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_update_log, test_boot_metadata_helper_overwrites_app_fields)
{
	set_boot_meta("3.2.1", 0xA5, ALP_UPDATE_STATUS_CONFIRMED);

	alp_update_log_entry_t e = mk_entry(11, "forged", ALP_UPDATE_STATUS_ROLLED_BACK);
	e.seq                    = 7;
	zassert_equal(alp_update_log_entry_from_boot_metadata(&e, 1234), ALP_OK);
	zassert_equal(e.seq, 0);
	zassert_equal(e.timestamp, 1234);
	zassert_equal(e.status, ALP_UPDATE_STATUS_CONFIRMED);
	zassert_equal(strcmp(e.fw_version, "3.2.1"), 0);
	for (size_t i = 0; i < sizeof(e.image_hash); i++) {
		zassert_equal(e.image_hash[i], 0xA5);
	}
}

ZTEST(alp_update_log, test_append_boot_stores_trusted_metadata)
{
	alp_ulog_sw_tier_test_reset(true);
	g_fake_hw_ready = ALP_ERR_NOSUPPORT;
	set_boot_meta("4.5.6", 0x5A, ALP_UPDATE_STATUS_PENDING_CONFIRM);

	alp_update_log_t *log = alp_update_log_open();
	zassert_not_null(log);
	zassert_equal(alp_update_log_append_boot(log, 5678), ALP_OK);

	uint64_t n = 0;
	zassert_equal(alp_update_log_count(log, &n), ALP_OK);
	zassert_equal(n, 1);

	alp_update_log_entry_t got;
	zassert_equal(alp_update_log_get(log, 0, &got), ALP_OK);
	zassert_equal(got.seq, 0);
	zassert_equal(got.timestamp, 5678);
	zassert_equal(got.status, ALP_UPDATE_STATUS_PENDING_CONFIRM);
	zassert_equal(strcmp(got.fw_version, "4.5.6"), 0);
	for (size_t i = 0; i < sizeof(got.image_hash); i++) {
		zassert_equal(got.image_hash[i], 0x5A);
	}
	alp_update_log_close(log);
	alp_ulog_sw_tier_test_reset(true);
	g_boot_meta_ready = false;
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
