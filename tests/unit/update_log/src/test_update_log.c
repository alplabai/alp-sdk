/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include <zephyr/ztest.h>

#include <alp/peripheral.h>
#include <alp/update_log.h>

#include "../../../../src/update_log/engine.h"
#include "../../../../src/update_log/store.h"
#include "../../../../src/update_log/sha256.h"

ZTEST_SUITE(alp_update_log, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_update_log, test_entry_roundtrip)
{
    alp_update_log_entry_t e = {0};
    e.seq = 7; e.status = ALP_UPDATE_STATUS_CONFIRMED; e.timestamp = 1234;
    strcpy(e.fw_version, "1.2.3");
    uint8_t prev[32]; memset(prev, 0xAB, sizeof(prev));

    uint8_t wire[ULOG_ENTRY_WIRE_LEN];
    zassert_equal(ulog_entry_encode(&e, prev, wire), ALP_OK);

    alp_update_log_entry_t got = {0};
    uint8_t got_prev[32];
    zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, got_prev), ALP_OK);
    zassert_equal(got.seq, 7);
    zassert_equal(got.status, ALP_UPDATE_STATUS_CONFIRMED);
    zassert_equal(got.timestamp, 1234);
    zassert_equal(strcmp(got.fw_version, "1.2.3"), 0);
    zassert_mem_equal(got_prev, prev, 32);
}

ZTEST(alp_update_log, test_decode_short_buffer)
{
    uint8_t wire[ULOG_ENTRY_WIRE_LEN - 1] = {0};
    alp_update_log_entry_t got; uint8_t p[32];
    zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, p), ALP_ERR_INVAL);
}

ZTEST(alp_update_log, test_decode_bad_version)
{
    alp_update_log_entry_t e = {0}; e.seq = 1; strcpy(e.fw_version, "x");
    uint8_t prev[32] = {0}; uint8_t wire[ULOG_ENTRY_WIRE_LEN];
    (void)ulog_entry_encode(&e, prev, wire);
    wire[0] = 0xFF; wire[1] = 0xFF;  /* corrupt version */
    alp_update_log_entry_t got; uint8_t p[32];
    zassert_equal(ulog_entry_decode(wire, sizeof(wire), &got, p), ALP_ERR_VERSION);
}

/* Minimal RAM store double: fixed slots of (key,blob). */
#define TD_SLOTS 16
#define TD_BLOB  128
struct td_store {
    struct { char key[24]; uint8_t buf[TD_BLOB]; size_t len; bool used; } s[TD_SLOTS];
    uint64_t counter;
};
static struct td_store *td(void *ctx) { return (struct td_store *)ctx; }

static int td_find(struct td_store *t, const char *key) {
    for (int i = 0; i < TD_SLOTS; i++) {
        if (t->s[i].used && strcmp(t->s[i].key, key)==0) return i;
    }
    return -1;
}
static alp_status_t td_put(void *c, const char *key, const uint8_t *b, size_t n) {
    struct td_store *t = td(c);
    if (n > TD_BLOB) return ALP_ERR_NOMEM;
    int i = td_find(t, key);
    if (i < 0) {
        for (i = 0; i < TD_SLOTS; i++) {
            if (!t->s[i].used) break;
        }
        if (i == TD_SLOTS) return ALP_ERR_NOMEM;
    }
    t->s[i].used = true; strncpy(t->s[i].key, key, sizeof(t->s[i].key)-1);
    t->s[i].key[sizeof(t->s[i].key)-1]=0; memcpy(t->s[i].buf, b, n); t->s[i].len = n; return ALP_OK;
}
static alp_status_t td_get(void *c, const char *key, uint8_t *b, size_t cap, size_t *out) {
    struct td_store *t = td(c);
    int i = td_find(t, key);
    if (i < 0) return ALP_ERR_NOT_FOUND;
    if (t->s[i].len > cap) return ALP_ERR_NOMEM;
    memcpy(b, t->s[i].buf, t->s[i].len);
    if (out) *out = t->s[i].len;
    return ALP_OK;
}
static alp_status_t td_erase(void *c, const char *key) {
    struct td_store *t = td(c); int i = td_find(t, key); if (i<0) return ALP_ERR_NOT_FOUND;
    t->s[i].used = false; return ALP_OK;
}
static alp_status_t td_cread(void *c, uint32_t id, uint64_t *v) { (void)id; *v = td(c)->counter; return ALP_OK; }
static alp_status_t td_cinc(void *c, uint32_t id, uint64_t *v) { (void)id; td(c)->counter++; *v = td(c)->counter; return ALP_OK; }

static void td_ifaces(struct td_store *t, alp_secure_store_if *s, alp_monotonic_counter_if *ctr) {
    memset(t, 0, sizeof(*t));
    s->put = td_put; s->get = td_get; s->erase = td_erase; s->ctx = t;
    ctr->read = td_cread; ctr->increment = td_cinc; ctr->ctx = t;
}

static alp_update_log_entry_t mk_entry(uint64_t ts, const char *ver, alp_update_status_t st) {
    alp_update_log_entry_t e = {0}; e.timestamp = ts; e.status = st;
    strncpy(e.fw_version, ver, ALP_UPDATE_LOG_FWVER_MAX);
    memset(e.image_hash, (int)ts, sizeof(e.image_hash)); return e;
}

ZTEST(alp_update_log, test_append_assigns_seq_and_chains)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c;
    td_ifaces(&t, &s, &c);

    alp_update_log_entry_t ent0 = mk_entry(100, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED);
    alp_update_log_entry_t ent1 = mk_entry(200, "1.1.0", ALP_UPDATE_STATUS_CONFIRMED);
    zassert_equal(ulog_engine_append(&s, &c, &ent0), ALP_OK);
    zassert_equal(ulog_engine_append(&s, &c, &ent1), ALP_OK);

    /* counter advanced to 2; meta.count == 2 */
    zassert_equal(t.counter, 2);
    uint8_t metabuf[ULOG_META_WIRE_LEN]; size_t mlen = 0;
    zassert_equal(td_get(&t, "ulog.meta", metabuf, sizeof(metabuf), &mlen), ALP_OK);
    struct ulog_meta m; zassert_equal(ulog_meta_decode(metabuf, mlen, &m), ALP_OK);
    zassert_equal(m.count, 2);

    /* entry 0 has zero prev_hash; entry 1's prev_hash == sha256(entry0 wire) */
    uint8_t e0[ULOG_ENTRY_WIRE_LEN]; size_t n0 = 0;
    zassert_equal(td_get(&t, "ulog.0", e0, sizeof(e0), &n0), ALP_OK);
    alp_update_log_entry_t d1; uint8_t prev1[32]; uint8_t e1[ULOG_ENTRY_WIRE_LEN]; size_t n1 = 0;
    zassert_equal(td_get(&t, "ulog.1", e1, sizeof(e1), &n1), ALP_OK);
    zassert_equal(ulog_entry_decode(e1, n1, &d1, prev1), ALP_OK);
    uint8_t expect[32]; ulog_sha256(e0, n0, expect);
    zassert_mem_equal(prev1, expect, 32);
    zassert_equal(d1.seq, 1);
}

/* --- Task 3: verify() helpers and tests --- */

static void seed(struct td_store *t, alp_secure_store_if *s, alp_monotonic_counter_if *c, int n) {
    td_ifaces(t, s, c);
    for (int i = 0; i < n; i++) {
        alp_update_log_entry_t ent = mk_entry((uint64_t)(i+1)*10, "1.0.0", ALP_UPDATE_STATUS_CONFIRMED);
        (void)ulog_engine_append(s, c, &ent);
    }
}

ZTEST(alp_update_log, test_verify_ok)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c;
    seed(&t, &s, &c, 3);
    alp_update_log_verdict_t v; uint64_t bad = 999;
    zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
    zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK);
}

ZTEST(alp_update_log, test_verify_detects_mutation)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c;
    seed(&t, &s, &c, 3);
    int i = td_find(&t, "ulog.1"); t.s[i].buf[12] ^= 0xFF;   /* flip a timestamp byte in entry 1 */
    alp_update_log_verdict_t v; uint64_t bad = 0;
    zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
    zassert_equal(v, ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN);
    zassert_equal(bad, 2);   /* entry 2's prev_hash no longer matches mutated entry 1 */
}

ZTEST(alp_update_log, test_verify_detects_truncation)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c;
    seed(&t, &s, &c, 3);
    (void)td_erase(&t, "ulog.2");   /* drop the tail; counter still says 3 */
    alp_update_log_verdict_t v; uint64_t bad = 0;
    zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
    zassert_equal(v, ALP_UPDATE_LOG_VERIFY_TRUNCATED);
}

ZTEST(alp_update_log, test_verify_detects_rollback)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c;
    seed(&t, &s, &c, 3);
    t.counter = 5;   /* counter advanced past the stored meta.count (=3): store regressed */
    alp_update_log_verdict_t v; uint64_t bad = 0;
    zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
    zassert_equal(v, ALP_UPDATE_LOG_VERIFY_ROLLED_BACK);
}

ZTEST(alp_update_log, test_verify_detects_reorder)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c;
    seed(&t, &s, &c, 3);
    int a = td_find(&t, "ulog.0"), b = td_find(&t, "ulog.1");
    uint8_t tmp[TD_BLOB];
    memcpy(tmp, t.s[a].buf, TD_BLOB);
    memcpy(t.s[a].buf, t.s[b].buf, TD_BLOB);
    memcpy(t.s[b].buf, tmp, TD_BLOB);  /* swap contents */
    alp_update_log_verdict_t v; uint64_t bad = 0;
    zassert_equal(ulog_engine_verify(&s, &c, &v, &bad), ALP_OK);
    zassert_equal(v, ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN);
}

ZTEST(alp_update_log, test_count_and_get)
{
    struct td_store t; alp_secure_store_if s; alp_monotonic_counter_if c;
    td_ifaces(&t, &s, &c);
    alp_update_log_entry_t ent0 = mk_entry(10, "a", ALP_UPDATE_STATUS_CONFIRMED);
    alp_update_log_entry_t ent1 = mk_entry(20, "b", ALP_UPDATE_STATUS_ROLLED_BACK);
    (void)ulog_engine_append(&s, &c, &ent0);
    (void)ulog_engine_append(&s, &c, &ent1);

    uint64_t n = 0; zassert_equal(ulog_engine_count(&s, &c, &n), ALP_OK); zassert_equal(n, 2);

    alp_update_log_entry_t e; zassert_equal(ulog_engine_get(&s, 1, &e), ALP_OK);
    zassert_equal(e.seq, 1); zassert_equal(e.status, ALP_UPDATE_STATUS_ROLLED_BACK);
    zassert_equal(strcmp(e.fw_version, "b"), 0);
    uint8_t want[32]; memset(want, 20, 32); zassert_mem_equal(e.image_hash, want, 32);

    zassert_equal(ulog_engine_get(&s, 9, &e), ALP_ERR_NOT_FOUND);
}
