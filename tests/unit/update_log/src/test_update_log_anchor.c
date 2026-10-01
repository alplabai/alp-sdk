/* SPDX-License-Identifier: Apache-2.0
 * Issue #111 P2: the optional external counter-anchor seam. Host-only; the
 * "anchor" is a fake floor that, unlike the store, survives a store reset --
 * exactly the property a hardware anchor (SE NV counter / OTP floor) adds. */
#include <string.h>
#include <zephyr/ztest.h>

#include <alp/update_log.h>

#include "../../../../src/update_log/engine.h"
#include "../../../../src/update_log/store.h"

ZTEST_SUITE(alp_update_log_anchor, NULL, NULL, NULL, NULL, NULL);

#define FS_SLOTS 16
struct fake {
	struct {
		bool    used;
		char    key[24];
		uint8_t buf[128];
		size_t  len;
	} s[FS_SLOTS];
	uint64_t counter;
	bool     fail_meta_put; /* inject: crash after the entry commit, before meta */
};
struct fake_anchor {
	uint64_t floor;
	bool     fail_advance; /* inject: crash after the counter, before the anchor */
};

static int fs_find(struct fake *f, const char *key)
{
	for (int i = 0; i < FS_SLOTS; i++) {
		if (f->s[i].used && strcmp(f->s[i].key, key) == 0) return i;
	}
	return -1;
}
static alp_status_t fs_put(void *c, const char *key, const uint8_t *b, size_t n)
{
	struct fake *f = c;
	if (f->fail_meta_put && strcmp(key, "ulog.meta") == 0) return ALP_ERR_IO;
	int i = fs_find(f, key);
	for (int k = 0; i < 0 && k < FS_SLOTS; k++) {
		if (!f->s[k].used) i = k;
	}
	if (i < 0 || n > sizeof(f->s[i].buf)) return ALP_ERR_NOMEM;
	f->s[i].used = true;
	strncpy(f->s[i].key, key, sizeof(f->s[i].key) - 1);
	memcpy(f->s[i].buf, b, n);
	f->s[i].len = n;
	return ALP_OK;
}
static alp_status_t fs_get(void *c, const char *key, uint8_t *b, size_t cap, size_t *out)
{
	struct fake *f = c;
	int          i = fs_find(f, key);
	if (i < 0) return ALP_ERR_NOT_FOUND;
	if (f->s[i].len > cap) return ALP_ERR_NOMEM;
	memcpy(b, f->s[i].buf, f->s[i].len);
	if (out) *out = f->s[i].len;
	return ALP_OK;
}
static alp_status_t fs_erase(void *c, const char *key)
{
	struct fake *f = c;
	int          i = fs_find(f, key);
	if (i < 0) return ALP_ERR_NOT_FOUND;
	f->s[i].used = false;
	return ALP_OK;
}
static alp_status_t fc_read(void *c, uint32_t id, uint64_t *v)
{
	(void)id;
	*v = ((struct fake *)c)->counter;
	return ALP_OK;
}
static alp_status_t fc_inc(void *c, uint32_t id, uint64_t *v)
{
	(void)id;
	*v = ++((struct fake *)c)->counter;
	return ALP_OK;
}
static alp_status_t fa_read(void *c, uint64_t *floor)
{
	*floor = ((struct fake_anchor *)c)->floor;
	return ALP_OK;
}
static alp_status_t fa_advance(void *c, uint64_t count)
{
	struct fake_anchor *a = c;
	if (a->fail_advance) return ALP_ERR_IO;
	if (count > a->floor) a->floor = count;
	return ALP_OK;
}

struct rig {
	struct fake              f;
	struct fake_anchor       a;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;
	alp_counter_anchor_if    an;
};
static void rig_init(struct rig *r)
{
	memset(r, 0, sizeof(*r));
	r->s  = (alp_secure_store_if){ fs_put, fs_get, fs_erase, &r->f };
	r->c  = (alp_monotonic_counter_if){ fc_read, fc_inc, &r->f };
	r->an = (alp_counter_anchor_if){ fa_read, fa_advance, &r->a };
}
static alp_status_t add(struct rig *r, int i)
{
	alp_update_log_entry_t e = { 0 };
	e.timestamp              = (uint64_t)i;
	e.status                 = ALP_UPDATE_STATUS_CONFIRMED;
	strcpy(e.fw_version, "1.0.0");
	return ulog_engine_append_anchored(&r->s, &r->c, &r->an, &e);
}
static alp_update_log_verdict_t vfy(struct rig *r, bool use_anchor)
{
	alp_update_log_verdict_t v   = ALP_UPDATE_LOG_VERIFY_OK;
	uint64_t                 bad = 0;
	zassert_equal(ulog_engine_verify_anchored(&r->s, &r->c, use_anchor ? &r->an : NULL, &v, &bad),
	              ALP_OK);
	return v;
}

ZTEST(alp_update_log_anchor, test_normal_appends_ok_and_anchor_tracks_count)
{
	struct rig r;
	rig_init(&r);
	for (int i = 0; i < 3; i++)
		zassert_equal(add(&r, i), ALP_OK);
	zassert_equal(r.a.floor, 3);
	zassert_equal(vfy(&r, true), ALP_UPDATE_LOG_VERIFY_OK);
}

ZTEST(alp_update_log_anchor, test_reflash_rolls_back)
{
	struct rig r;
	rig_init(&r);
	for (int i = 0; i < 3; i++)
		zassert_equal(add(&r, i), ALP_OK);
	/* Full reflash: store and its counter rewind together, anchor survives. */
	memset(&r.f, 0, sizeof(r.f));
	zassert_equal(vfy(&r, true), ALP_UPDATE_LOG_VERIFY_ROLLED_BACK);
	/* Without the anchor the rewound store is indistinguishable from fresh. */
	zassert_equal(vfy(&r, false), ALP_UPDATE_LOG_VERIFY_OK);
}

ZTEST(alp_update_log_anchor, test_tail_truncation_rolls_back)
{
	struct rig r;
	rig_init(&r);
	for (int i = 0; i < 2; i++)
		zassert_equal(add(&r, i), ALP_OK);
	struct fake snap = r.f; /* self-consistent 2-entry image */
	zassert_equal(add(&r, 2), ALP_OK);
	r.f = snap; /* attacker restores the older image: tail entry gone */
	zassert_equal(vfy(&r, true), ALP_UPDATE_LOG_VERIFY_ROLLED_BACK);
	zassert_equal(vfy(&r, false), ALP_UPDATE_LOG_VERIFY_OK);
}

/* Crash order 1: entry committed, meta/counter/anchor never ran. The anchor
 * is still at n, recovery adopts the orphan, no false ROLLED_BACK. */
ZTEST(alp_update_log_anchor, test_torn_append_entry_only_no_false_rollback)
{
	struct rig r;
	rig_init(&r);
	for (int i = 0; i < 2; i++)
		zassert_equal(add(&r, i), ALP_OK);
	r.f.fail_meta_put = true; /* entry put lands, meta put "crashes" */
	zassert_equal(add(&r, 2), ALP_ERR_IO);
	r.f.fail_meta_put = false;
	zassert_equal(r.a.floor, 2, "anchor must not lead the store");
	zassert_equal(vfy(&r, true), ALP_UPDATE_LOG_VERIFY_OK);
	zassert_equal(r.a.floor, 3, "recovery adopts the orphan and raises the floor");
	zassert_equal(add(&r, 3), ALP_OK);
	zassert_equal(vfy(&r, true), ALP_UPDATE_LOG_VERIFY_OK);
}

/* Crash order 2: entry, meta and counter committed, anchor advance lost. The
 * anchor lags by one; verify must be OK and the next append catches it up. */
ZTEST(alp_update_log_anchor, test_torn_append_anchor_lag_no_false_rollback)
{
	struct rig r;
	rig_init(&r);
	for (int i = 0; i < 2; i++)
		zassert_equal(add(&r, i), ALP_OK);
	r.a.fail_advance = true;
	zassert_equal(add(&r, 2), ALP_ERR_IO);
	r.a.fail_advance = false;
	zassert_equal(r.f.counter, 3);
	zassert_equal(r.a.floor, 2);
	zassert_equal(vfy(&r, true), ALP_UPDATE_LOG_VERIFY_OK);
	zassert_equal(add(&r, 3), ALP_OK);
	zassert_equal(r.a.floor, 4);
	zassert_equal(vfy(&r, true), ALP_UPDATE_LOG_VERIFY_OK);
}

ZTEST(alp_update_log_anchor, test_no_anchor_behaviour_unchanged)
{
	struct rig r;
	rig_init(&r);
	alp_update_log_entry_t e = { 0 };
	strcpy(e.fw_version, "1.0.0");
	for (int i = 0; i < 3; i++)
		zassert_equal(ulog_engine_append(&r.s, &r.c, &e), ALP_OK);
	zassert_equal(r.a.floor, 0, "plain append never touches an anchor");
	alp_update_log_verdict_t v   = ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN;
	uint64_t                 bad = 0;
	zassert_equal(ulog_engine_verify(&r.s, &r.c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK);
	/* Reflash is invisible without an anchor (pre-P2 behaviour). */
	memset(&r.f, 0, sizeof(r.f));
	zassert_equal(ulog_engine_verify(&r.s, &r.c, &v, &bad), ALP_OK);
	zassert_equal(v, ALP_UPDATE_LOG_VERIFY_OK);
}
