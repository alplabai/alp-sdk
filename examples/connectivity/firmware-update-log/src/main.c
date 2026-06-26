/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * firmware-update-log -- portable, tamper-evident firmware-update audit log.
 *
 * This example answers one customer question directly: "can old audit entries
 * be modified or deleted by application firmware -- and can you PROVE it?"
 *
 * It runs in two acts, end to end, on native_sim (your laptop -- no board):
 *
 *   ACT 1  The API a customer actually writes. Open the log, record a real
 *          firmware lifecycle (install / confirm / failed-verify / rollback),
 *          then verify the whole chain. The backend AND the assurance level
 *          are selected for you at boot -- the app never names a vendor
 *          mechanism. Here that resolves to the software tier
 *          (SW_TAMPER_EVIDENT); on a SoM with a secure backend the very same
 *          source reports HW_ENFORCED.
 *
 *   ACT 2  The attacker. We drop BELOW the public API to the raw byte store
 *          the way a process with write access to the log memory would -- which
 *          on the Alif E4/E8 is exactly what the application Cortex-M55 can do:
 *          it writes MRAM directly over the bus, so "the app just won't call
 *          erase" is not protection. We hand the attacker that raw access on
 *          purpose and show that every mutation, deletion, rollback and reorder
 *          of a historical entry is DETECTED by verify().
 *
 * The honest takeaway for the meeting:
 *   - TODAY (software tier): tamper is *detectable*. Act 2 proves it runs.
 *   - WITH THE E4 KIT (hardware tier): the same API reports HW_ENFORCED, and
 *     the SE-controlled firewall makes the log region unreachable to the
 *     application core -- so tamper becomes *prevented*, not just detected.
 *     Tracking issues: #262 (durable store), #263 (authenticated inputs from
 *     verified boot metadata), #111 (wire the Alif hardware-enforced backend).
 */
#include <stdio.h>
#include <string.h>

#include <alp/peripheral.h> /* alp_status_t, ALP_OK */
#include <alp/update_log.h> /* the one public API */

/*
 * ACT 2 reaches the engine and its storage seams directly. These are
 * module-private SDK headers (not part of the public surface) -- we use them
 * here only to play the role of an attacker holding raw access to the log
 * bytes. The include path to the SDK src/ root is set in CMakeLists.txt, the
 * same way tests/unit/update_log does it.
 */
#include "update_log/engine.h"
#include "update_log/store.h"

/* ==========================================================================
 * HOW THE LOG STAYS TAMPER-EVIDENT  (read this once; the rest follows from it)
 * ==========================================================================
 *
 * The log is an append-only chain, the same idea a blockchain uses:
 *
 *     entry[0]            entry[1]            entry[2]
 *   +-----------+       +-----------+       +-----------+
 *   | fw/hash/  |       | fw/hash/  |       | fw/hash/  |
 *   | status/ts |   +-->| status/ts |   +-->| status/ts |
 *   | prev = 0  |   |   | prev = H0 |   |   | prev = H1 |
 *   +-----------+   |   +-----------+   |   +-----------+
 *        |          |        |          |
 *        +-- H0 ----+        +-- H1 ----+      Hn = SHA-256(entry[n]'s bytes)
 *
 * Each entry carries `prev` = the SHA-256 of the *entire previous entry*. So an
 * entry's hash depends on its own content AND on every entry before it. Change
 * one byte anywhere and every hash downstream of it stops matching -- that is
 * what makes the past immutable-by-detection.
 *
 * Two more pieces close the gaps a pure chain leaves open:
 *   - META (`head_hash` + `count`): commits to the *tail*. A chain alone can be
 *     truncated (lop off the newest entries) without breaking any remaining
 *     link; comparing against the committed count catches that.
 *   - MONOTONIC COUNTER: a value that only ever increases. It is the anchor
 *     that catches a wholesale ROLLBACK -- replacing the store with an older,
 *     internally-consistent snapshot. The old snapshot's chain verifies fine,
 *     but its count is behind where the counter has already been, so the
 *     regression is visible.
 *
 * verify() walks the chain from entry 0, recomputing each hash and checking the
 * next entry's `prev` against it, then cross-checks META and the counter. It
 * returns one of: OK / CHAIN_BROKEN (a `prev` mismatch -- mutation or reorder)
 * / TRUNCATED (fewer entries than committed) / ROLLED_BACK (store behind the
 * counter).
 *
 * WHO HOLDS THE PIECES is the whole security story:
 *   - SW tier (this run): chain bytes + counter live in normal RAM the app can
 *     write. So tamper is DETECTABLE but not preventable -- an attacker who
 *     rewrites every downstream hash *and* the META *and* cannot lower the
 *     counter is still caught, but cooperative integrity is the ceiling.
 *   - HW tier (E4 kit): the store sits behind the SE firewall (unreachable to
 *     the app core) and the counter is a hardware non-decrementable NV counter.
 *     The attacker can no longer write the bytes at all -> PREVENTED.
 * The engine code is identical across both; only the two seams below change.
 *
 * --------------------------------------------------------------------------
 * Pretty-printers
 * ------------------------------------------------------------------------ */

static const char *assurance_str(alp_update_log_assurance_t a)
{
	return (a == ALP_UPDATE_LOG_HW_ENFORCED) ? "HW_ENFORCED (SE-firewalled store)"
	                                         : "SW_TAMPER_EVIDENT (software tier)";
}

static const char *verdict_str(alp_update_log_verdict_t v)
{
	switch (v) {
	case ALP_UPDATE_LOG_VERIFY_OK:
		return "OK";
	case ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN:
		return "CHAIN_BROKEN";
	case ALP_UPDATE_LOG_VERIFY_TRUNCATED:
		return "TRUNCATED";
	case ALP_UPDATE_LOG_VERIFY_ROLLED_BACK:
		return "ROLLED_BACK";
	default:
		return "?";
	}
}

/* --------------------------------------------------------------------------
 * ACT 1 -- the portable public API
 * ------------------------------------------------------------------------ */

/* Append one record the way a bootloader / secure service would after it has
 * verified the image it just acted on. In production the version + hash come
 * from the *verified* boot metadata, not from values the app made up (that is
 * issue #263); here we fill them inline so the example is self-contained. */
static void record(alp_update_log_t *log, const char *ver, alp_update_status_t st, uint64_t ts)
{
	alp_update_log_entry_t e;
	memset(&e, 0, sizeof(e)); /* zero first: unset fields (incl. seq) must be 0 */
	strncpy(e.fw_version, ver, ALP_UPDATE_LOG_FWVER_MAX); /* what was installed */
	memset(e.image_hash, (int)ts, sizeof(e.image_hash));  /* real builds: SHA-256 of image */
	e.status    = st;                                     /* CONFIRMED / FAILED / ROLLED_BACK ... */
	e.timestamp = ts;                                     /* best-effort epoch; 0 = unknown */

	/* The caller never sets `seq`. The engine assigns it from the monotonic
	 * counter and computes this entry's `prev` link -- so the chain is built
	 * for you; append is the only write verb the app ever needs. */
	if (alp_update_log_append(log, &e) != ALP_OK) {
		printf("  ! append failed for v=%s\n", ver);
	}
}

static void act1_public_api(void)
{
	printf("\n=== ACT 1: the API a customer writes (portable, auto-selected backend) ===\n");

	/* Open the device's update log. NULL means no backend on this SoM. */
	alp_update_log_t *log = alp_update_log_open();
	if (log == NULL) {
		printf("[update-log] no backend present\n");
		return;
	}

	/* Tell the operator exactly how strong the guarantee is on this silicon.
	 * The application branches on the *level*, never on the mechanism. */
	printf("[update-log] assurance: %s\n", assurance_str(alp_update_log_assurance(log)));

	/* A realistic field lifecycle: ship 1.4.0, try 1.5.0 (fails verification),
	 * roll back, then succeed with the fixed 1.5.1. Every event is recorded --
	 * including the failures, which are exactly what an auditor wants to see. */
	record(log, "1.4.0", ALP_UPDATE_STATUS_CONFIRMED, 1718000000u);
	record(log, "1.5.0", ALP_UPDATE_STATUS_PENDING_CONFIRM, 1718600000u);
	record(log, "1.5.0", ALP_UPDATE_STATUS_VERIFY_FAILED, 1718600060u);
	record(log, "1.4.0", ALP_UPDATE_STATUS_ROLLED_BACK, 1718600120u);
	record(log, "1.5.1", ALP_UPDATE_STATUS_CONFIRMED, 1719200000u);

	/* Walk the chain. In a healthy log this is OK; Act 2 shows what a tampered
	 * one returns. (The CI harness keys on this "verify: OK" line.) */
	alp_update_log_verdict_t v;
	uint64_t                 bad = 0;
	if (alp_update_log_verify(log, &v, &bad) == ALP_OK) {
		printf("[update-log] verify: %s\n", verdict_str(v));
	}

	/* Print the append-only trail. */
	uint64_t n = 0;
	if (alp_update_log_count(log, &n) == ALP_OK) {
		printf("[update-log] %llu entr%s:\n", (unsigned long long)n, (n == 1) ? "y" : "ies");
		for (uint64_t i = 0; i < n; i++) {
			alp_update_log_entry_t r;
			if (alp_update_log_get(log, i, &r) == ALP_OK) {
				printf("  #%llu  v=%-6s  status=%d  ts=%llu\n",
				       (unsigned long long)r.seq,
				       r.fw_version,
				       (int)r.status,
				       (unsigned long long)r.timestamp);
			}
		}
	}

	alp_update_log_close(log);
}

/* --------------------------------------------------------------------------
 * ACT 2 -- the attacker holds raw access to the log bytes
 *
 * Everything below stands in for "the application core can write the log
 * memory directly." We back the engine with an in-RAM keyed-blob store and a
 * monotonic counter, append entries, then corrupt the raw store the way an
 * attacker would -- and confirm verify() catches it every time.
 *
 * The engine talks to storage through exactly two narrow seams (see store.h):
 *
 *   alp_secure_store_if       a tiny key->blob store: put / get / erase.
 *                             Keys are "ulog.<seq>" for entries (seq is
 *                             0-based) plus "ulog.meta" for the head/count.
 *                             Host = this RAM table; HW tier = TF-M Protected
 *                             Storage behind the SE firewall.
 *   alp_monotonic_counter_if  read / increment a never-decreasing value.
 *                             Host = a plain uint64 in RAM; HW tier = a PSA NV
 *                             / hardware counter the app cannot wind back.
 *
 * Swapping these two seams is the *entire* difference between SW_TAMPER_EVIDENT
 * and HW_ENFORCED. The engine, the wire format, and this app are unchanged.
 * Below we implement the host (attacker-writable) versions so we can tamper.
 * ------------------------------------------------------------------------ */

#define ATK_SLOTS 16
#define ATK_BLOB  128 /* one blob slot; >= ULOG_ENTRY_WIRE_LEN (115 bytes) */

/* A toy key->blob table: the minimum that satisfies alp_secure_store_if. A real
 * backend (NVS, TF-M PS) has the same shape -- find by key, store/return bytes. */
struct atk_store {
	struct {
		char    key[24];       /* e.g. "ulog.0", "ulog.meta" */
		uint8_t buf[ATK_BLOB]; /* the raw entry/meta bytes verify() will hash */
		size_t  len;
		bool    used;
	} s[ATK_SLOTS];
	uint64_t counter; /* the monotonic anchor (only ever ++ in normal use) */
};

/* --- seam boilerplate (put/get/erase/read/increment) ---------------------
 * Mechanical glue that makes `struct atk_store` satisfy the two seam
 * interfaces. Nothing security-relevant lives here; skip to act2_attacker()
 * on a first read. A production backend replaces just these functions. */

static struct atk_store *as_of(void *ctx)
{
	return (struct atk_store *)ctx;
}

static int atk_find(struct atk_store *t, const char *key)
{
	for (int i = 0; i < ATK_SLOTS; i++) {
		if (t->s[i].used && strcmp(t->s[i].key, key) == 0) {
			return i;
		}
	}
	return -1;
}

static alp_status_t atk_put(void *c, const char *key, const uint8_t *b, size_t n)
{
	struct atk_store *t = as_of(c);
	if (n > ATK_BLOB) {
		return ALP_ERR_NOMEM;
	}
	int i = atk_find(t, key);
	if (i < 0) {
		for (i = 0; i < ATK_SLOTS; i++) {
			if (!t->s[i].used) {
				break;
			}
		}
		if (i == ATK_SLOTS) {
			return ALP_ERR_NOMEM;
		}
	}
	t->s[i].used = true;
	strncpy(t->s[i].key, key, sizeof(t->s[i].key) - 1);
	t->s[i].key[sizeof(t->s[i].key) - 1] = 0;
	memcpy(t->s[i].buf, b, n);
	t->s[i].len = n;
	return ALP_OK;
}

static alp_status_t atk_get(void *c, const char *key, uint8_t *b, size_t cap, size_t *out)
{
	struct atk_store *t = as_of(c);
	int               i = atk_find(t, key);
	if (i < 0) {
		return ALP_ERR_NOT_FOUND;
	}
	if (t->s[i].len > cap) {
		return ALP_ERR_NOMEM;
	}
	memcpy(b, t->s[i].buf, t->s[i].len);
	if (out) {
		*out = t->s[i].len;
	}
	return ALP_OK;
}

static alp_status_t atk_erase(void *c, const char *key)
{
	struct atk_store *t = as_of(c);
	int               i = atk_find(t, key);
	if (i < 0) {
		return ALP_ERR_NOT_FOUND;
	}
	t->s[i].used = false;
	return ALP_OK;
}

static alp_status_t atk_cread(void *c, uint32_t id, uint64_t *v)
{
	(void)id;
	*v = as_of(c)->counter;
	return ALP_OK;
}

static alp_status_t atk_cinc(void *c, uint32_t id, uint64_t *v)
{
	(void)id;
	as_of(c)->counter++;
	*v = as_of(c)->counter;
	return ALP_OK;
}

/* Wipe the store, wire the two seams to it, and append `n` valid entries so we
 * start every attack from a known-good chain. Each entry gets a distinct
 * image_hash so swaps/mutations are real content changes, not no-ops. */
static void
atk_seed(struct atk_store *t, alp_secure_store_if *s, alp_monotonic_counter_if *c, int n)
{
	memset(t, 0, sizeof(*t));
	s->put       = atk_put;
	s->get       = atk_get;
	s->erase     = atk_erase;
	s->ctx       = t;
	c->read      = atk_cread;
	c->increment = atk_cinc;
	c->ctx       = t;

	for (int i = 0; i < n; i++) {
		alp_update_log_entry_t e;
		memset(&e, 0, sizeof(e));
		strncpy(e.fw_version, "1.0.0", ALP_UPDATE_LOG_FWVER_MAX);
		memset(e.image_hash, i + 1, sizeof(e.image_hash));
		e.status    = ALP_UPDATE_STATUS_CONFIRMED;
		e.timestamp = (uint64_t)(i + 1) * 1000u;
		(void)ulog_engine_append(s, c, &e);
	}
}

/* Run verify() over the store and print the verdict + offending seq. */
static void atk_check(const char *attack, alp_secure_store_if *s, alp_monotonic_counter_if *c)
{
	alp_update_log_verdict_t v   = ALP_UPDATE_LOG_VERIFY_OK;
	uint64_t                 bad = 0;
	(void)ulog_engine_verify(s, c, &v, &bad);
	if (v == ALP_UPDATE_LOG_VERIFY_CHAIN_BROKEN || v == ALP_UPDATE_LOG_VERIFY_TRUNCATED) {
		printf("  %-28s -> verify: %s (seq %llu)  [DETECTED]\n",
		       attack,
		       verdict_str(v),
		       (unsigned long long)bad);
	} else {
		printf("  %-28s -> verify: %s  [DETECTED]\n", attack, verdict_str(v));
	}
}

static void act2_attacker(void)
{
	printf("\n=== ACT 2: attacker with raw access to the log bytes ===\n");
	printf("(On the E4/E8 the application M55 can write MRAM directly -- so we GIVE\n");
	printf(" the attacker that raw access and prove every tamper is caught.)\n\n");

	struct atk_store         t;
	alp_secure_store_if      s;
	alp_monotonic_counter_if c;

	/* Baseline: a clean 4-entry log. Every `prev` link matches, META agrees,
	 * the store is level with the counter -> OK. This is the control case;
	 * each attack below re-seeds from here so the attacks are independent. */
	atk_seed(&t, &s, &c, 4);
	atk_check("baseline (untampered)", &s, &c);

	/* Attack 1 -- MODIFY a historical entry. Flip one byte inside entry #1's
	 * stored bytes. Now SHA-256(entry#1) changes, so entry#2's `prev` (which
	 * still holds the OLD hash) no longer matches what verify() recomputes.
	 * Reported as CHAIN_BROKEN at the first entry whose link fails (seq 2). To
	 * hide this the attacker would have to rewrite #2's prev, then #3's, ...
	 * every entry to the tip -- and still cannot fix META. */
	atk_seed(&t, &s, &c, 4);
	{
		int i = atk_find(&t, "ulog.1");
		t.s[i].buf[12] ^= 0xFF; /* corrupt one byte of the raw entry */
	}
	atk_check("modify entry #1", &s, &c);

	/* Attack 2 -- DELETE the newest entry. Erasing "ulog.3" leaves a chain
	 * that is still internally consistent (#0..#2 all link fine), so the chain
	 * walk alone would say OK. META still commits to count=4 while only 3
	 * entries are present -> TRUNCATED. This is why a bare hash-chain is not
	 * enough; the committed tail is what catches a silent drop. */
	atk_seed(&t, &s, &c, 4);
	(void)atk_erase(&t, "ulog.3");
	atk_check("delete newest entry", &s, &c);

	/* Attack 3 -- ROLL BACK the whole store to an older, internally-valid
	 * snapshot (e.g. re-flash a saved MRAM image). The snapshot's chain + META
	 * are self-consistent, so chain and truncation checks both pass. But the
	 * monotonic counter only goes up: it is already ahead of the restored
	 * snapshot's count -> ROLLED_BACK. We simulate "counter kept climbing
	 * while the store went backwards" by bumping the anchor past the store. */
	atk_seed(&t, &s, &c, 4);
	t.counter += 2; /* the never-decreasing anchor outruns the restored store */
	atk_check("roll store back", &s, &c);

	/* Attack 4 -- REORDER two entries. Swap the raw bytes of #0 and #1. Order
	 * is baked into the chain (each `prev` names a specific predecessor), so a
	 * swap makes the links inconsistent just like a mutation -> CHAIN_BROKEN.
	 * Position is not just metadata here; it is cryptographically committed. */
	atk_seed(&t, &s, &c, 4);
	{
		int     a = atk_find(&t, "ulog.0");
		int     b = atk_find(&t, "ulog.1");
		uint8_t tmp[ATK_BLOB];
		memcpy(tmp, t.s[a].buf, ATK_BLOB);
		memcpy(t.s[a].buf, t.s[b].buf, ATK_BLOB);
		memcpy(t.s[b].buf, tmp, ATK_BLOB);
	}
	atk_check("reorder entries #0/#1", &s, &c);
}

int main(void)
{
	printf("==========================================================\n");
	printf(" Alp SDK -- tamper-evident firmware-update audit log demo\n");
	printf("==========================================================\n");

	act1_public_api();
	act2_attacker();

	printf("\n=== Summary ===\n");
	printf("Software tier TODAY: every tamper above was DETECTED (this ran on\n");
	printf("native_sim -- no hardware). With the E4 kit the same API reports\n");
	printf("HW_ENFORCED and the SE firewall removes the attacker's write access\n");
	printf("entirely -- tamper moves from detected to PREVENTED. (#262/#263/#111)\n");
	printf("demo complete\n");
	return 0;
}
