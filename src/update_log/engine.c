/* SPDX-License-Identifier: Apache-2.0
 * Pure update-log engine. No vendor/PSA includes -- builds on host. The only
 * OS-conditional piece is the lock's yield hook right below (Zephyr/POSIX/
 * bare-metal), selected purely off predefined compiler macros -- no vendor
 * HAL, no PSA, still links standalone in the fuzz/unit-test host build. */
#include <string.h>

#include "update_log/engine.h"
#include "update_log/sha256.h"

/* Yield the current thread/core so a runnable lock holder can actually run --
 * see the g_engine_lock block comment below for why a pure spin is wrong
 * here. Selected by the same build-tier macros already used elsewhere in the
 * tree (src/common/alp_internal.h, src/common/stub_backend.c): __ZEPHYR__ for
 * the RTOS build (this also covers native_sim, which defines __ZEPHYR__),
 * __linux__ for the yocto/A-class and plain-CMake-on-host builds, and a
 * no-op for a genuine bare-metal target with no scheduler beneath this layer
 * (a busy-wait cannot livelock there the way it can under a preemptive
 * scheduler -- the "lock holder never gets scheduled" failure mode requires
 * an OS to begin with). */
#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#define ULOG_ENGINE_LOCK_YIELD() k_yield()
#elif defined(__linux__)
#include <sched.h>
#define ULOG_ENGINE_LOCK_YIELD() ((void)sched_yield())
#else
#define ULOG_ENGINE_LOCK_YIELD() ((void)0)
#endif

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
 * ulog_recover() adopts AT MOST ONE orphan per call and never loops. A
 * functioning monotonic counter can produce at most one torn-append shape
 * between a crash and the next recovery (append always runs recovery
 * before reading the counter, so it never writes a second entry above an
 * already-orphaned one). If a SECOND validly-chained entry is still
 * sitting above the counter after that single adoption, the counter
 * itself regressed -- that is proof of rollback, not a second torn
 * append, and must not be silently adopted too (it would rewrite meta
 * and the counter forward, turning a state the pre-fix code correctly
 * called ROLLED_BACK into a false VERIFY_OK). ulog_engine_verify()'s
 * frontier check (below the main chain-walk loop) is what surfaces that
 * left-behind second entry, not ulog_recover().
 *
 * Similarly, an entry sitting at the counter's value that does NOT chain
 * validly (bad seq, or a prev_hash that does not match the actual
 * preceding entry) is real corruption/tamper, not a torn append:
 * ulog_recover() leaves it untouched -- it never discards or overwrites
 * it (a write-once tier could not anyway) -- and ulog_engine_verify()'s
 * frontier check reports that as CHAIN_BROKEN rather than VERIFY_OK. The
 * invariant that check protects: verify() == OK must imply append() can
 * make progress. Before this check existed, verify()'s chain walk only
 * covered slots [0, hw) and never looked at slot hw itself, so a
 * non-chaining entry squatting on the frontier slot was invisible to
 * verify() while append()'s subsequent store->put() to that same
 * already-occupied key would keep failing -- on a PSA Protected Storage
 * WRITE_ONCE asset (src/update_log/tfm_psa_secure_owner.c) that failure
 * is permanent: the log silently stops accepting new entries forever
 * even though its own verify() said OK. append() does not attempt to
 * force an overwrite of that slot; it surfaces the store's error (a
 * write-once conflict maps to ALP_ERR_IO) as a clear terminal failure. A
 * mutable tier (SW-NVS/RAM) instead accepts the overwrite and the next
 * append after that self-heals the frontier -- that behavioural split is
 * inherent to what the underlying tier allows, not something the engine
 * can paper over.
 *
 * g_engine_lock serializes every call below across append/verify/count
 * (and, without needing counter access, get()) so two concurrent callers
 * can never interleave the three mutations, and a reader can never
 * observe a state midway through an append. Compiler-builtin atomics
 * (GCC/Clang __atomic_*), not an OS mutex, keep this file dependency-free
 * across host/baremetal/Zephyr/yocto builds. Unlike
 * alp_handle_begin_close() in src/common/alp_slot_claim.h -- whose
 * critical section really is a handful of short, synchronous in-RAM
 * calls -- the critical section guarded here can include the SW-NVS
 * tier's nvs_write()/nvs_calc_free_space(), which can trigger NVS garbage
 * collection (a sector erase: milliseconds of flash I/O). On a
 * single-core target (each AEN M55 is single-core Zephyr) a pure busy
 * spin is therefore not safe: a low-priority thread can be holding the
 * lock mid-GC while a higher-priority thread spins on
 * ulog_engine_lock(), and on a single core that higher-priority spinner
 * never yields the CPU back to the (runnable, not blocked) lock holder --
 * livelock, not just a slow path. ulog_engine_lock() therefore yields
 * (ULOG_ENGINE_LOCK_YIELD(), defined above) between CAS attempts so a
 * runnable holder can actually be scheduled to finish and release.
 *
 * Cross-image scope: g_engine_lock is a per-IMAGE static. It does not,
 * and cannot, serialize two separate cores/images that both end up
 * touching the same secure store -- there is exactly one correct writer
 * image per store instance, by construction, not by locking across
 * images. On AEN, the HE (application) image must never itself open a
 * local store instance over the same MRAM region the HP (secure owner)
 * image writes; HE only ever reaches the log through the HP owner's MHU
 * mailbox (src/backends/update_log/aen_m55_owner.c). If aen_ready() ever
 * declines (SE/device firewall NOSUPPORT, MHU timeout) and the
 * dispatcher (src/update_log_dispatch.c) falls through to a LOCAL sw_tier
 * on the HE image, that fallback is safe ONLY if HE's devicetree does
 * NOT also carve out `alp_ulog_partition` over the HP owner's MRAM region
 * -- two unsynchronized NVS mounts over the same flash region would race
 * regardless of anything g_engine_lock does, since it has no visibility
 * across images. See CONFIG_ALP_SDK_UPDATE_LOG_AEN_M55_OWNER's Kconfig
 * help for the same requirement stated at the board-porting seam.
 * ---------------------------------------------------------------------
 */
static uint8_t g_engine_lock; /* 0 = free, 1 = held. */

static void ulog_engine_lock(void)
{
	for (;;) {
		uint8_t expected = 0;
		if (__atomic_compare_exchange_n(
		        &g_engine_lock, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
			return;
		}
		/* Not "a handful of short store calls" here -- see the block
		 * comment above. Yield so a runnable holder mid-flash-GC gets
		 * scheduled instead of losing the core to this spinner. */
		ULOG_ENGINE_LOCK_YIELD();
	}
}

static void ulog_engine_unlock(void)
{
	__atomic_store_n(&g_engine_lock, 0, __ATOMIC_RELEASE);
}

/* Re-derive meta + the counter from the newest committed entry,
 * forward-committing AT MOST ONE interrupted append (never loops -- see
 * the block comment above for why a second orphan must be left for
 * ulog_engine_verify() to report as ROLLED_BACK instead of adopted here).
 * Idempotent: a no-op once meta/counter already agree with the store.
 * Must be called with g_engine_lock held. */
static alp_status_t ulog_recover(const alp_secure_store_if      *store,
                                 const alp_monotonic_counter_if *ctr)
{
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
		 * ulog_engine_verify()'s frontier check to report as
		 * CHAIN_BROKEN. */
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
	return ctr->increment(ctr->ctx, 0, &newhw);
	/* Deliberately do not loop: adopting a second entry here would hide
	 * a counter rollback as a clean VERIFY_OK. See the block comment. */
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
	 * up on the next call.
	 *
	 * If ulog_recover() above declined to adopt a non-chaining entry
	 * already squatting on key(hw) (real corruption/tamper at the
	 * frontier, not a torn append -- see the block comment above
	 * ulog_recover()), this put() targets an already-occupied key. A
	 * mutable tier (SW-NVS/RAM) accepts the overwrite and self-heals; a
	 * write-once tier (PSA Protected Storage WRITE_ONCE) rejects it and
	 * this returns that failure as a terminal error to the caller --
	 * intentionally not force-overwritten. ulog_engine_verify() already
	 * reports CHAIN_BROKEN for that same state, so the caller is never
	 * told OK while append() cannot make progress. */
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

	/* ulog_recover() adopts at most one orphan per call and never loops
	 * (see its block comment), so the chain walk above -- which only
	 * covers [0, hw) -- never inspects slot hw itself. Whatever
	 * ulog_recover() deliberately left sitting there needs an explicit
	 * look: verify()==OK must imply append() can make progress, and
	 * silently ignoring the frontier slot broke that on a write-once
	 * tier (GHSA-r236-29pg-w694, DEFECT 1). */
	char    frontier_key[24];
	uint8_t frontier_wire[ULOG_ENTRY_WIRE_LEN];
	size_t  frontier_n = 0;
	kbuf(frontier_key, sizeof(frontier_key), hw);
	rc = store->get(store->ctx, frontier_key, frontier_wire, sizeof(frontier_wire), &frontier_n);
	if (rc != ALP_OK && rc != ALP_ERR_NOT_FOUND) {
		rc = ALP_ERR_IO;
		goto out;
	}
	if (rc == ALP_OK) {
		alp_update_log_entry_t frontier_e;
		uint8_t                frontier_prev[32];
		alp_status_t drc = ulog_entry_decode(frontier_wire, frontier_n, &frontier_e, frontier_prev);
		if (drc == ALP_OK && frontier_e.seq == hw && memcmp(frontier_prev, cur_hash, 32) == 0) {
			/* Validly chains from the recognized head: a second orphan
			 * that ulog_recover() deliberately left un-adopted (it
			 * adopts at most one per call -- a functioning counter can
			 * never produce two). Proof the counter itself regressed,
			 * not a second torn append (DEFECT 3). */
			*verdict_out = ALP_UPDATE_LOG_VERIFY_ROLLED_BACK;
		} else {
			/* Present but does not chain: real corruption/tamper
			 * squatting on the frontier slot, not a torn append. On a
			 * write-once tier append()'s subsequent store->put() to
			 * this same key can never succeed -- surface that now
			 * instead of a false OK. */
			*verdict_out = ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN;
			if (bad_seq_out) *bad_seq_out = hw;
		}
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
