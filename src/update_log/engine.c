/* SPDX-License-Identifier: Apache-2.0
 * Pure update-log engine. No Zephyr/PSA includes -- builds on host. */
#include <string.h>

#include "update_log/engine.h"
#include "update_log/sha256.h"

static void put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}
static void put_u32(uint8_t *p, uint32_t v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
static void put_u64(uint8_t *p, uint64_t v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (uint8_t)(v >> (8 * i));
}
static uint16_t get_u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t get_u32(const uint8_t *p)
{
	uint32_t v = 0;
	for (int i = 0; i < 4; i++)
		v |= (uint32_t)p[i] << (8 * i);
	return v;
}
static uint64_t get_u64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (8 * i);
	return v;
}

/* Bounded string length -- strnlen is POSIX, not C99, and the engine must
 * stay libc-variant-portable (Zephyr's minimal libc omits it). */
static size_t bounded_len(const char *s, size_t cap)
{
	size_t n = 0;
	while (n < cap && s[n] != '\0')
		n++;
	return n;
}

alp_status_t ulog_entry_encode(const alp_update_log_entry_t *e,
                               const uint8_t                 prev_hash[32],
                               uint8_t                       out[ULOG_ENTRY_WIRE_LEN])
{
	if (e == NULL || prev_hash == NULL || out == NULL) return ALP_ERR_INVAL;
	size_t vlen = bounded_len(e->fw_version, ALP_UPDATE_LOG_FWVER_MAX + 1);
	if (vlen > ALP_UPDATE_LOG_FWVER_MAX) return ALP_ERR_INVAL;
	memset(out, 0, ULOG_ENTRY_WIRE_LEN);
	put_u16(out + 0, (uint16_t)ULOG_VERSION);
	put_u64(out + 2, e->seq);
	out[10] = (uint8_t)e->status;
	put_u64(out + 11, e->timestamp);
	out[19] = (uint8_t)vlen;
	memcpy(out + 20, e->fw_version, vlen);
	memcpy(out + 51, prev_hash, 32);
	memcpy(out + 83, e->image_hash, 32);
	return ALP_OK;
}

alp_status_t ulog_entry_decode(const uint8_t          *buf,
                               size_t                  len,
                               alp_update_log_entry_t *e_out,
                               uint8_t                 prev_hash_out[32])
{
	if (buf == NULL || e_out == NULL) return ALP_ERR_INVAL;
	if (len < ULOG_ENTRY_WIRE_LEN) return ALP_ERR_INVAL;
	if (get_u16(buf + 0) != ULOG_VERSION) return ALP_ERR_VERSION;
	uint8_t vlen = buf[19];
	if (vlen > ALP_UPDATE_LOG_FWVER_MAX) return ALP_ERR_INVAL;
	memset(e_out, 0, sizeof(*e_out));
	e_out->seq       = get_u64(buf + 2);
	e_out->status    = (alp_update_status_t)buf[10];
	e_out->timestamp = get_u64(buf + 11);
	memcpy(e_out->fw_version, buf + 20, vlen); /* zero-padded -> NUL-terminated */
	memcpy(e_out->image_hash, buf + 83, 32);
	if (prev_hash_out != NULL) memcpy(prev_hash_out, buf + 51, 32);
	return ALP_OK;
}

alp_status_t ulog_meta_encode(const struct ulog_meta *m, uint8_t out[ULOG_META_WIRE_LEN])
{
	if (m == NULL || out == NULL) return ALP_ERR_INVAL;
	memset(out, 0, ULOG_META_WIRE_LEN);
	put_u16(out + 0, (uint16_t)ULOG_VERSION);
	put_u32(out + 2, ULOG_META_MAGIC);
	put_u64(out + 6, m->count);
	memcpy(out + 14, m->head_hash, 32);
	return ALP_OK;
}

alp_status_t ulog_meta_decode(const uint8_t *buf, size_t len, struct ulog_meta *m_out)
{
	if (buf == NULL || m_out == NULL) return ALP_ERR_INVAL;
	if (len < ULOG_META_WIRE_LEN) return ALP_ERR_INVAL;
	if (get_u16(buf + 0) != ULOG_VERSION) return ALP_ERR_VERSION;
	if (get_u32(buf + 2) != ULOG_META_MAGIC) return ALP_ERR_INVAL;
	m_out->count = get_u64(buf + 6);
	memcpy(m_out->head_hash, buf + 14, 32);
	return ALP_OK;
}

static void kbuf(char *out, size_t cap, uint64_t seq)
{
	/* "ulog.<seq>" decimal. */
	int      n = 0;
	char     tmp[24];
	uint64_t v = seq;
	do {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	} while (v && n < 20);
	size_t      pos = 0;
	const char *pfx = "ulog.";
	while (*pfx && pos < cap - 1)
		out[pos++] = *pfx++;
	while (n > 0 && pos < cap - 1)
		out[pos++] = tmp[--n];
	out[pos] = 0;
}

/*
 * ---------------------------------------------------------------------
 * Crash- and concurrency-safe append (GHSA-r236-29pg-w694).
 *
 * ulog_engine_append() durably mutates three things: the entry blob
 * ("ulog.<seq>"), the metadata cache ("ulog.meta"), and the monotonic
 * counter. Only the FIRST of these is a real commit point:
 *
 *   - The entry write is the transaction's sole durable commit. A single
 *     store->put()/counter->increment() call is assumed atomic against
 *     power loss (both the NVS record layer and PSA Protected Storage
 *     guarantee this for one asset) -- what is NOT atomic is the
 *     SEQUENCE of three separate calls.
 *   - meta and the counter are DERIVED caches of the committed entries,
 *     never a second independent authority. Recovery re-derives them by
 *     hashing the actual previous entry in the store -- never by
 *     trusting a possibly-stale copy of meta -- so "meta says N" can
 *     never outrank "entry N-1 hashes to X" as the source of truth.
 *
 * ulog_recover() runs at the start of every engine entry point that
 * takes the counter seam (append/verify/count) and repairs exactly one
 * class of anomaly: an entry already durably written at the counter's
 * current value (seq == hw) with a chain that validates against the
 * PRECEDING entry. That shape can only arise from a crash between the
 * entry write and the meta/counter catching up, so recovery always
 * COMMITS it forward (re-derives meta, advances the counter) -- it
 * never discards, because a write-once entry cannot be un-written
 * anyway, and treating the mutable (NVS/RAM) and write-once (secure)
 * tiers identically keeps one tested invariant for both. Any entry that
 * exists but does NOT chain validly is left untouched: that is real
 * corruption/tamper, not a torn append, and ulog_engine_verify() must
 * still surface it as CHAIN_BROKEN rather than have recovery paper over
 * it.
 *
 *   - Crash after the entry write, before meta: recovery finds entry hw
 *     already present, re-derives meta + advances the counter. No wedge
 *     -- the entry is never rewritten, only the (mutable) cache around
 *     it is repaired.
 *   - Crash after meta, before the counter increment ("meta ahead of
 *     counter"): recovery does not trust the stale-vs-fresh meta content
 *     at all; it independently re-hashes entry hw-1, confirms entry hw
 *     chains from it, and advances the counter to match. verify() no
 *     longer reports a false ROLLED_BACK, and the next append correctly
 *     continues at hw+1 instead of re-chaining against seq hw.
 *
 * g_engine_lock serializes every call below across append/verify/count
 * (and, without needing counter access, get()) so two concurrent callers
 * can never interleave the three mutations, and a reader can never
 * observe a state midway through an append. Compiler-builtin atomics
 * (GCC/Clang __atomic_*), not an OS mutex, keep this file dependency-free
 * across host/baremetal/Zephyr/yocto builds -- the same rationale as
 * src/common/alp_slot_claim.h. The critical sections here are short and
 * synchronous (a handful of store calls), so a busy-wait spin is
 * appropriate, matching the pattern already used by
 * alp_handle_begin_close() in that header.
 * ---------------------------------------------------------------------
 */
static uint8_t g_engine_lock; /* 0 = free, 1 = held. */

static void ulog_engine_lock(void)
{
	uint8_t expected;
	do {
		expected = 0;
	} while (!__atomic_compare_exchange_n(
	    &g_engine_lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
}

static void ulog_engine_unlock(void)
{
	__atomic_store_n(&g_engine_lock, 0, __ATOMIC_RELEASE);
}

/* Re-derive meta + the counter from the newest committed entry, forward-
 * committing exactly one interrupted append. Idempotent: a no-op once
 * meta/counter already agree with the store. Must be called with
 * g_engine_lock held. */
static alp_status_t ulog_recover(const alp_secure_store_if      *store,
                                 const alp_monotonic_counter_if *ctr)
{
	for (;;) {
		uint64_t     hw = 0;
		alp_status_t rc = ctr->read(ctr->ctx, 0, &hw);
		if (rc != ALP_OK) return rc;
		if (hw == UINT64_MAX) return ALP_ERR_NOMEM; /* never let the counter wrap */

		char key[24];
		kbuf(key, sizeof(key), hw);
		uint8_t wire[ULOG_ENTRY_WIRE_LEN];
		size_t  n = 0;
		rc        = store->get(store->ctx, key, wire, sizeof(wire), &n);
		if (rc == ALP_ERR_NOT_FOUND) return ALP_OK; /* nothing to recover */
		if (rc != ALP_OK) return rc;

		/* An entry already exists at the counter's value: a prior append
		 * committed it but crashed before meta and/or the counter caught
		 * up. Validate the chain independently of meta before committing
		 * forward -- hash the actual preceding entry, don't trust a
		 * possibly-stale meta blob. */
		alp_update_log_entry_t e;
		uint8_t                got_prev[32];
		if (ulog_entry_decode(wire, n, &e, got_prev) != ALP_OK || e.seq != hw) {
			/* Not the torn-append shape: real corruption. Leave it for
			 * ulog_engine_verify() to report as CHAIN_BROKEN. */
			return ALP_OK;
		}

		uint8_t expect_prev[32];
		memset(expect_prev, 0, sizeof(expect_prev));
		if (hw > 0) {
			char    pkey[24];
			uint8_t pwire[ULOG_ENTRY_WIRE_LEN];
			size_t  pn = 0;
			kbuf(pkey, sizeof(pkey), hw - 1);
			rc = store->get(store->ctx, pkey, pwire, sizeof(pwire), &pn);
			if (rc == ALP_ERR_NOT_FOUND) return ALP_OK; /* truncated tail: not our case */
			if (rc != ALP_OK) return rc;
			ulog_sha256(pwire, pn, expect_prev);
		}
		if (memcmp(got_prev, expect_prev, 32) != 0) {
			/* Doesn't chain from the actual preceding entry either: real
			 * corruption, not a torn append. Leave for verify(). */
			return ALP_OK;
		}

		struct ulog_meta nm;
		nm.count = hw + 1u;
		ulog_sha256(wire, n, nm.head_hash);
		uint8_t metaout[ULOG_META_WIRE_LEN];
		(void)ulog_meta_encode(&nm, metaout);
		rc = store->put(store->ctx, "ulog.meta", metaout, sizeof(metaout));
		if (rc != ALP_OK) return rc;

		uint64_t newhw = 0;
		rc             = ctr->increment(ctr->ctx, 0, &newhw);
		if (rc != ALP_OK) return rc;
		/* Loop again: defensive against more than one orphaned entry.
		 * Cheap and idempotent -- terminates as soon as ulog.<hw> is
		 * absent. */
	}
}

alp_status_t ulog_engine_append(const alp_secure_store_if      *store,
                                const alp_monotonic_counter_if *ctr,
                                const alp_update_log_entry_t   *entry)
{
	if (store == NULL || ctr == NULL || entry == NULL) return ALP_ERR_INVAL;

	ulog_engine_lock();

	alp_status_t rc = ulog_recover(store, ctr);
	if (rc != ALP_OK) {
		ulog_engine_unlock();
		return rc;
	}

	uint64_t hw = 0;
	rc          = ctr->read(ctr->ctx, 0, &hw);
	if (rc != ALP_OK) {
		ulog_engine_unlock();
		return rc;
	}
	if (hw > ULOG_SEQ_MAX) {
		/* Sequence space exhausted: refuse rather than risk a truncated
		 * "ulog.<seq>" key or an eventual counter wrap. The existing
		 * chain stays intact and verifiable, same contract as a full
		 * store. */
		ulog_engine_unlock();
		return ALP_ERR_NOMEM;
	}

	uint8_t prev[32];
	memset(prev, 0, sizeof(prev));
	if (hw > 0) {
		/* Recovery above guarantees meta already agrees with hw, so this
		 * cached head hash is trustworthy here. */
		uint8_t metabuf[ULOG_META_WIRE_LEN];
		size_t  mlen = 0;
		rc           = store->get(store->ctx, "ulog.meta", metabuf, sizeof(metabuf), &mlen);
		if (rc != ALP_OK) {
			ulog_engine_unlock();
			return rc;
		}
		struct ulog_meta m;
		rc = ulog_meta_decode(metabuf, mlen, &m);
		if (rc != ALP_OK) {
			ulog_engine_unlock();
			return rc;
		}
		memcpy(prev, m.head_hash, 32);
	}

	alp_update_log_entry_t e = *entry;
	e.seq                    = hw;
	uint8_t wire[ULOG_ENTRY_WIRE_LEN];
	rc = ulog_entry_encode(&e, prev, wire);
	if (rc != ALP_OK) {
		ulog_engine_unlock();
		return rc;
	}

	/* The durable commit point. If the process crashes right after this
	 * put() succeeds, the entry is safely written (write-once tiers never
	 * need to rewrite it) and ulog_recover() will catch meta/the counter
	 * up on the next call. */
	char key[24];
	kbuf(key, sizeof(key), hw);
	rc = store->put(store->ctx, key, wire, sizeof(wire));
	if (rc != ALP_OK) {
		ulog_engine_unlock();
		return rc;
	}

	struct ulog_meta nm;
	nm.count = hw + 1u;
	ulog_sha256(wire, sizeof(wire), nm.head_hash);
	uint8_t metaout[ULOG_META_WIRE_LEN];
	(void)ulog_meta_encode(&nm, metaout);
	rc = store->put(store->ctx, "ulog.meta", metaout, sizeof(metaout));
	if (rc != ALP_OK) {
		/* meta didn't catch up -- not fatal to the chain. ulog_recover()
		 * re-derives it (and the counter) from the entry itself next time
		 * around, never from this stale attempt. */
		ulog_engine_unlock();
		return rc;
	}

	uint64_t newhw = 0;
	rc             = ctr->increment(ctr->ctx, 0, &newhw);
	ulog_engine_unlock();
	return rc;
}

alp_status_t ulog_engine_verify(const alp_secure_store_if      *store,
                                const alp_monotonic_counter_if *ctr,
                                alp_update_log_verdict_t       *verdict_out,
                                uint64_t                       *bad_seq_out)
{
	if (store == NULL || ctr == NULL || verdict_out == NULL) return ALP_ERR_INVAL;
	if (bad_seq_out) *bad_seq_out = 0;

	ulog_engine_lock();

	alp_status_t rc = ulog_recover(store, ctr);
	if (rc != ALP_OK) {
		ulog_engine_unlock();
		return rc;
	}

	uint64_t hw = 0;
	rc          = ctr->read(ctr->ctx, 0, &hw);
	if (rc != ALP_OK) {
		ulog_engine_unlock();
		return ALP_ERR_IO;
	}

	struct ulog_meta m;
	m.count = 0;
	memset(m.head_hash, 0, 32);
	if (hw > 0) {
		uint8_t metabuf[ULOG_META_WIRE_LEN];
		size_t  mlen = 0;
		rc           = store->get(store->ctx, "ulog.meta", metabuf, sizeof(metabuf), &mlen);
		if (rc == ALP_ERR_NOT_FOUND) {
			*verdict_out = ALP_UPDATE_LOG_VERIFY_ROLLED_BACK;
			rc           = ALP_OK;
			goto out;
		}
		if (rc != ALP_OK) {
			rc = ALP_ERR_IO;
			goto out;
		}
		if (ulog_meta_decode(metabuf, mlen, &m) != ALP_OK) {
			rc = ALP_ERR_IO;
			goto out;
		}
		/* The counter is the trusted anchor. If the stored meta disagrees, the
         * store (or the counter) was rolled back. */
		if (m.count != hw) {
			*verdict_out = ALP_UPDATE_LOG_VERIFY_ROLLED_BACK;
			rc           = ALP_OK;
			goto out;
		}
	}

	uint8_t prev[32];
	memset(prev, 0, sizeof(prev));
	uint8_t cur_hash[32];
	memset(cur_hash, 0, sizeof(cur_hash));
	for (uint64_t i = 0; i < hw; i++) {
		char key[24];
		kbuf(key, sizeof(key), i);
		uint8_t wire[ULOG_ENTRY_WIRE_LEN];
		size_t  n = 0;
		rc        = store->get(store->ctx, key, wire, sizeof(wire), &n);
		if (rc == ALP_ERR_NOT_FOUND) {
			*verdict_out = ALP_UPDATE_LOG_VERIFY_TRUNCATED;
			if (bad_seq_out) *bad_seq_out = i;
			rc = ALP_OK;
			goto out;
		}
		if (rc != ALP_OK) {
			rc = ALP_ERR_IO;
			goto out;
		}

		alp_update_log_entry_t e;
		uint8_t                got_prev[32];
		if (ulog_entry_decode(wire, n, &e, got_prev) != ALP_OK) {
			*verdict_out = ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN;
			if (bad_seq_out) *bad_seq_out = i;
			rc = ALP_OK;
			goto out;
		}
		if (e.seq != i || memcmp(got_prev, prev, 32) != 0) {
			*verdict_out = ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN;
			if (bad_seq_out) *bad_seq_out = i;
			rc = ALP_OK;
			goto out;
		}
		ulog_sha256(wire, n, cur_hash);
		memcpy(prev, cur_hash, 32);
	}

	if (hw > 0 && memcmp(cur_hash, m.head_hash, 32) != 0) {
		*verdict_out = ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN;
		if (bad_seq_out) *bad_seq_out = hw - 1;
		rc = ALP_OK;
		goto out;
	}

	*verdict_out = ALP_UPDATE_LOG_VERIFY_OK;
	rc           = ALP_OK;
out:
	ulog_engine_unlock();
	return rc;
}

alp_status_t ulog_engine_count(const alp_secure_store_if      *store,
                               const alp_monotonic_counter_if *ctr,
                               uint64_t                       *count_out)
{
	if (store == NULL || ctr == NULL || count_out == NULL) return ALP_ERR_INVAL;

	ulog_engine_lock();

	alp_status_t rc = ulog_recover(store, ctr);
	if (rc == ALP_OK) {
		rc = ctr->read(ctr->ctx, 0, count_out);
	}

	ulog_engine_unlock();
	return rc;
}

alp_status_t
ulog_engine_get(const alp_secure_store_if *store, uint64_t seq, alp_update_log_entry_t *e_out)
{
	if (store == NULL || e_out == NULL) return ALP_ERR_INVAL;

	/* No counter seam here, so this cannot run ulog_recover() -- but a
	 * raw keyed lookup does not depend on meta/counter consistency at
	 * all: any entry that is durably present (including one still
	 * awaiting recovery's meta/counter catch-up) is already valid to
	 * return. Still take the engine lock so a read here can never
	 * interleave with another caller's in-flight 3-phase append. */
	ulog_engine_lock();

	char key[24];
	kbuf(key, sizeof(key), seq);
	uint8_t      wire[ULOG_ENTRY_WIRE_LEN];
	size_t       n  = 0;
	alp_status_t rc = store->get(store->ctx, key, wire, sizeof(wire), &n);
	if (rc == ALP_OK) {
		uint8_t prev[32];
		rc = ulog_entry_decode(wire, n, e_out, prev);
	}

	ulog_engine_unlock();
	return rc; /* ALP_ERR_NOT_FOUND propagates */
}
