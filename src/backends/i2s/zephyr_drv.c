/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr i2s_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend registers a more specific
 * silicon_ref match.  Pooling lives in src/i2s_dispatch.c; the
 * backend's open resolves the alp-i2sN DT alias, allocates the
 * 2-block ping-pong slab + its backing buffer in a per-handle
 * Zephyr sidecar, and configures the i2s controller.
 *
 * The portable handle in src/backends/i2s/i2s_ops.h carries no
 * Zephyr types -- k_mem_slab is a Zephyr-typed object, so it
 * cannot live inside struct alp_i2s without contaminating the
 * portable surface.  The sidecar (alp_z_i2s_side_t) holds the
 * slab + slab_buf; state->be_data carries the per-handle pointer.
 * The sidecar pool is sized by CONFIG_ALP_SDK_MAX_I2S_HANDLES so
 * every active dispatcher slot has a matching sidecar.
 *
 * TX deferred-start state machine (issue #2132).  Every caller that
 * routes through <alp/i2s.h> OR <alp/audio.h> lands here, so the fix
 * lives at this one shared layer, not in the audio backend above it.
 *
 * The Alif DesignWare I2S driver (zephyr/drivers/i2s/i2s_dw.c) refuses
 * to trigger TX START on an empty ring buffer: tx_stream_start()
 * dequeues a block via queue_get() and returns -ENOMEM if none is
 * queued (i2s_dw.c:144-147/794).  The natural open->start->write order
 * every in-tree caller uses hits that -ENOMEM before anything is ever
 * written.  STOP/DRAIN separately refuse a trigger unless the stream
 * already reached I2S_STATE_RUNNING (-EIO, i2s_dw.c:301-304/315-321),
 * while DROP works from READY too and releases both the in-flight
 * block and everything still queued (i2s_dw.c:329-338, 886-900).
 *
 * Three TX-only sidecar flags carry this across calls:
 *   - tx_block_queued  a write() has queued a block since open() or
 *                      the last full release (stop's success path).
 *   - tx_pending_start  z_start() was called while tx_block_queued was
 *                      false, so the real START trigger was deferred.
 *   - tx_started       the real hardware START trigger has actually
 *                      succeeded and (as far as this backend knows) is
 *                      still running.
 *
 * INVARIANT (enforced by construction, not just by the individual call
 * sites getting it right): tx_started => !tx_pending_start. Every site
 * that can set tx_started true routes through _mark_tx_started(), which
 * clears tx_pending_start in the same assignment. Two DIFFERENT sites can
 * clear tx_started back to false, because there are two different idle
 * shapes to land in (issue #2137 review round 2, finding 2):
 * _mark_tx_stopped() for FULLY idle (all three flags false, a genuine
 * stop), and _mark_tx_needs_restart() for idle-but-owed-a-restart
 * (tx_pending_start ends up true, not false, so it cannot reuse
 * _mark_tx_stopped()) -- both clear tx_started, so the invariant holds via
 * either. See finding #2132-MAJOR-2: z_start()'s immediate-trigger success
 * path used to set tx_started directly without touching tx_pending_start,
 * so a start() that raced ahead of a still-pending retry (reachable before
 * the MAJOR-1 fix below closed off the specific sequence that exposed it)
 * could leave both flags true, and the next write() would retry a START on
 * an ALREADY-RUNNING stream and get i2s_dw's -EIO (i2s_dw.c:279-283)
 * instead of just writing.
 *
 * z_start(): if TX and nothing is queued yet, set tx_pending_start and
 * return ALP_OK without touching hardware (write-then-start already
 * queues first, so it still triggers immediately, unchanged). On the
 * immediate-trigger path, a real START that fails ALP_ERR_NOMEM is
 * treated as "tx_block_queued lied" rather than a hard failure (issue
 * #2132 review round 4) -- see that branch's own comment for the race
 * this closes, and (issue #2137 review round 2) for why this branch now
 * also clears tx_started via _mark_tx_needs_restart() rather than leaving
 * it stale. A real START that fails ALP_ERR_IO is retried ONCE after a
 * PREPARE (issue #2137, both directions) -- see below.
 *
 * z_write(): after a block is genuinely queued (i2s_write() succeeded),
 * set tx_block_queued.  If tx_pending_start, retry the real START --
 * on success, _mark_tx_started() clears tx_pending_start too; a write
 * into a stream whose START keeps failing must never return ALP_OK, so
 * a still-failing retry DROPs the block THIS write just queued (finding
 * #2132-MAJOR-1: while a start is pending, nothing else can have queued
 * ahead of it, so DROP removes exactly this write's block, never a
 * sibling's) and returns that failure from THIS write() call -- an I2S
 * write that returns an error has NEVER left a block queued, which is
 * what lets the audio layer above trust out_frames again. The very next
 * write() retries again since tx_pending_start stays set. The initial
 * i2s_write() itself failing ALP_ERR_IO gets its own PREPARE-and-retry
 * recovery first (issue #2137, TX only -- see below) before any of this
 * runs; that retry now runs even when PREPARE itself was refused (issue
 * #2137 review round 2, finding 4) -- see that paragraph below.
 *
 * z_stop(): for TX, if tx_started already says the stream is NOT
 * genuinely running, skip DRAIN entirely and go straight to DROP (issue
 * #2137 review round 2, findings 2/3 -- avoids i2s_dw's own LOG_ERR on a
 * DRAIN refusal this backend already knows is coming); otherwise, and
 * always for RX, DRAIN is tried first -- covers the running case exactly
 * as before. If DRAIN fails or was skipped (issue #2132: "start() was
 * deferred and never fired" or "start() was never called at all", both
 * still READY; issue #2137: the stream underran/overran into
 * I2S_STATE_ERROR), fall back to DROP, which works from READY, RUNNING,
 * AND ERROR alike and releases whatever was queued instead of stranding
 * it -- gated on h->started for RX only (issue #2137 review round 2,
 * finding 8: DROP touches real RX hardware teardown, so a never-started
 * RX handle must not reach it, matching z_close()'s own gate). Either
 * trigger succeeding runs _mark_tx_stopped() (TX) and returns ALP_OK;
 * both failing (or DROP being skipped) returns DRAIN's failure (the
 * primary, expected trigger).
 *
 * z_close(): DROP whenever dev != NULL for TX always (a pending-but-
 * written or START-still-failing TX handle can hold real slab-block
 * pointers in the driver's TX ring / in-flight mem_block, and this
 * function is about to k_free() the slab backing them -- skipping DROP
 * left those pointers dangling for the NEXT open() on this device to
 * dequeue into freed memory: corruption, not just a leak); for RX only
 * when h->started, matching i2s_dw's own RX behaviour byte-for-byte
 * (rx_stream_disable() -- including its clock teardown -- only ever ran
 * when RX had actually been triggered, pre-#2132 and after). DROP
 * recovers from ERROR too (issue #2137), so this needs no separate
 * PREPARE handling of its own.
 *
 * issue #2137 -- I2S_STATE_ERROR recovery (a stream that started, then
 * went quiet long enough to underrun (TX, i2s_dw.c:516-524) or overrun
 * (RX, i2s_dw.c:636-647)). From ERROR, DRAIN/STOP -EIO (need RUNNING,
 * i2s_dw.c:301-321), START -EIOs (needs READY, i2s_dw.c:278-283), and
 * TX write() -EIOs (needs RUNNING or READY, i2s_dw.c:390-394) -- the
 * SAME state that lets a pending-but-never-started stream's DRAIN fail
 * also describes a genuinely-underran one, which is exactly why z_stop()
 * above needs no separate ERROR branch: DROP already covers both.
 * I2S_TRIGGER_PREPARE is the ONLY trigger valid FROM ERROR
 * (i2s_dw.c:340-347); it moves to READY and drops the ring (normally
 * empty for a TX underrun; PREPARE frees any block that raced in --
 * review round 2, finding 7, since i2s_dw_write() checks state
 * separately from queue_put(), i2s_dw.c:390-402). z_start() and
 * z_write() each retry their OWN failed operation after a successful
 * PREPARE; a failed PREPARE leaves z_start()'s ORIGINAL -EIO as the
 * reported failure (not PREPARE's own) and z_start() does NOT retry
 * further -- but z_write() DOES still retry once even when its OWN
 * PREPARE was refused (issue #2137 review round 2, finding 4): a
 * concurrent call may have already taken the stream out of ERROR between
 * this write()'s own trigger failing and its own PREPARE running, so a
 * refused PREPARE here does not prove the stream is still stuck, only
 * that it was not FROM ERROR at that exact instant -- if it genuinely
 * still is, the retry just gets the same -EIO and falls into the
 * existing free-and-report path, no new failure mode. z_write()'s retry
 * additionally resets the TX flags via _mark_tx_needs_restart() --
 * tx_started=false, tx_block_queued=false, tx_pending_start=true --
 * "freshly opened, a start is owed" -- UNCONDITIONALLY on entering this
 * branch, not only when its own PREPARE succeeded: the ORIGINAL
 * i2s_write() failing -EIO already proves the stream was in ERROR, not
 * RUNNING (i2s_dw_write() only -EIOs from ERROR, i2s_dw.c:390-394), so
 * tx_started is ALREADY known-stale here regardless of what THIS
 * PREPARE call itself reports -- gating the reset on this PREPARE's own
 * success would leave tx_pending_start false in exactly the concurrent-
 * recovery case above, silently stranding the retry's genuinely-queued
 * block with no START ever fired for it. Retried with the SAME slab
 * block either way (never freed before the retry, never double-freed
 * either way after it); the existing post-write path then fires the
 * now-armed deferred start, so the write that discovers the gap is also
 * the one that resumes playback. A recovery this call's OWN PREPARE
 * performed (in either function) logs one LOG_WRN (issue #2137 review
 * round 2, finding 3) -- the only bench-visible evidence a recovery
 * happened at all; a REFUSED PREPARE does not log (nothing this call
 * did fixed it), whether it stays stuck or a concurrent recovery already
 * got there first.
 *
 * The RX overrun leak (issue #2137 review round 2, finding 1): fixed at
 * its actual source, zephyr/drivers/i2s/i2s_dw.c's RX IRQ handler, not
 * here -- both its error exits (a failed k_mem_slab_alloc() for the next
 * block, and a failed queue_put() of the one just filled) now free the
 * block they would otherwise have orphaned. Neither path touches this
 * backend's own bookkeeping (RX carries no sidecar flags -- see z_start()
 * never deferring for RX), so no zephyr_drv.c change was needed for it.
 *
 * Locking: a k_spinlock in the sidecar (alp_z_i2s_side_t.lock) guards
 * the three tx_* flags and the i2s_trigger() calls that change them in
 * z_start()/z_write()/z_stop() -- including the #2137 PREPARE calls
 * above, each its own short lock/unlock bracket around a trigger call,
 * consistent with every other trigger site in this file; the RETRIED
 * i2s_write() call in z_write() stays outside the lock like the
 * original one, for the same blocking-call reason -- true even in review
 * round 2's retry-despite-refused-PREPARE case (finding 4), since the
 * lock only ever needs to protect the trigger call and the three flags,
 * never the retried i2s_write() itself. Between the lock and z_start()'s
 * NOMEM-as-stale-flag handling, both the write/stop race and the
 * start/write/stop race identified across #2132's review rounds are
 * closed -- see the lock field's own comment for exactly which
 * mechanism closes which, and what backends this lock design does not
 * generalise to.
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i2s.h>
#include <alp/soc_caps.h>

#include "alp_errno.h"
#include "i2s_ops.h"
#include "alp_slot_claim.h"

/* issue #2137 review round 2, finding 3: a PREPARE recovery is otherwise
 * silent -- LOG_WRN below is the only bench-visible evidence an underrun/
 * overrun happened at all. */
LOG_MODULE_REGISTER(alp_i2s_zephyr, CONFIG_LOG_DEFAULT_LEVEL);

#define ALP_I2S_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_ALIAS(_CONCAT(alp_i2s, idx)), okay), \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_i2s, idx)))), \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_I2S_DEV_OR_NULL(0),
	ALP_I2S_DEV_OR_NULL(1),
};

#ifndef CONFIG_ALP_SDK_MAX_I2S_HANDLES
#define CONFIG_ALP_SDK_MAX_I2S_HANDLES 2
#endif

/* Per-handle Zephyr sidecar.  Holds the k_mem_slab + its heap-backed
 * buffer so the portable handle in src/backends/i2s/i2s_ops.h stays
 * free of <zephyr/kernel.h> types.  state->be_data carries the
 * per-handle pointer set at open() time. */
typedef struct {
	struct k_mem_slab mem_slab;
	uint8_t          *slab_buf;
	size_t            slab_buf_bytes;
	/* Negotiated slab block size.  z_write() bounds the caller's byte
	 * count against this: without it a caller-supplied length was
	 * memcpy'd into a fixed-size block, corrupting the neighbouring
	 * slab block and the k_malloc heap. */
	size_t block_bytes;
	/* TX-only deferred-start state (issue #2132) -- see the file
	 * header comment for the full state machine these three drive. */
	bool tx_block_queued;
	bool tx_pending_start;
	bool tx_started;
	/* Guards the three tx_* flags above AND the i2s_trigger() calls that
	 * change them, in z_start()/z_write()/z_stop() -- see those
	 * functions. NEVER held across k_mem_slab_alloc() or the Zephyr
	 * i2s_write() API call: both can block (i2s_dw.c's i2s_dw_write()
	 * takes tx.sem with the caller's own timeout, i2s_dw.c:396-397).
	 *
	 * i2s_trigger() itself is safe to call under this raw spinlock
	 * because i2s_dw_trigger() is safe to call from ISR context: the
	 * whole call stays under its own irq_lock()/irq_unlock()
	 * (i2s_dw.c:255-354), and everything it does under that lock --
	 * queue_get(), k_sem_give(), a clock_control_set_rate() call (the
	 * Alif backend's implementation takes no mutex), register writes,
	 * k_mem_slab_free(), pm_device_busy_set()/clear() -- never
	 * reschedules while irqs are locked (Zephyr only switches when the
	 * saved key has irqs unlocked, kernel/sched.c:533-539).
	 *
	 * That "ISR-safe trigger" property is NOT universal across every
	 * i2s_* driver class member, even though this backend registers as
	 * silicon_ref="*" -- it also holds for zephyr/drivers/i2s/
	 * i2s_mcux_sai.c (whole trigger under irq_lock too), but NOT for
	 * zephyr/drivers/i2s/i2s_ambiq.c, whose STOP/DROP call
	 * k_sleep(K_MSEC(100)) (i2s_ambiq.c:383), or zephyr/drivers/i2s/
	 * i2s_silabs_siwx91x.c, whose START calls pm_device_runtime_get()
	 * (i2s_silabs_siwx91x.c:752) -- both would be illegal under this
	 * lock. Neither is E1M silicon and no code here accounts for them;
	 * a future vendor-ext backend swap onto either driver would need
	 * its own locking story, not this one.
	 *
	 * Start/write vs stop race (issue #2132 review, MINOR) -- CLOSED: a
	 * concurrent write() queuing a block while stop() DROPs it (or vice
	 * versa) is serialized by this lock once both sides are inside it,
	 * so tx_block_queued always reflects the ring's real state at the
	 * instant either call reads or writes it while holding the lock.
	 *
	 * Start vs write/stop race -- CLOSED a different way (issue #2132
	 * review, round 4): write()'s k_mem_slab_alloc()/i2s_write() run
	 * UNLOCKED (see above), so a write can genuinely queue a block, then
	 * a concurrent stop() can lock, see tx_started still false, DROP
	 * that exact block for real, and clear the flags -- all before the
	 * write reaches its own lock acquisition and stamps a now-stale
	 * tx_block_queued=true over an empty ring. z_start()'s immediate-
	 * trigger path closes this on the READ side instead: an
	 * ALP_ERR_NOMEM from the real START trigger on TX means i2s_dw's
	 * ring was empty (queue_get(), i2s_dw.c:794-798) with no other
	 * driver-state change, so z_start() treats that outcome as "the
	 * tx_block_queued flag was stale" and defers instead of propagating
	 * the error -- see z_start(). z_write()'s own retry needs no
	 * equivalent handling: it runs its whole check-and-trigger sequence
	 * under this lock, so nothing else can race its OWN retry once
	 * started, and any trigger failure it does see already goes through
	 * the existing DROP-and-report-error path (MAJOR-1). */
	struct k_spinlock lock;
	bool              in_use;
} alp_z_i2s_side_t;

static alp_z_i2s_side_t _sides[CONFIG_ALP_SDK_MAX_I2S_HANDLES];

/* issue #1115 round-2 dev review: claim atomically (in_use is the LAST
 * member; memset only the bytes ahead of it, since the previous
 * whole-struct `{0}` reset would zero the just-won in_use flag back to
 * false and race a concurrent claimant). */
static alp_z_i2s_side_t *_alloc_side(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_sides); ++i) {
		if (alp_slot_try_claim(&_sides[i].in_use)) {
			memset(&_sides[i], 0, offsetof(alp_z_i2s_side_t, in_use));
			return &_sides[i];
		}
	}
	return NULL;
}

static void _free_side(alp_z_i2s_side_t *s)
{
	if (s != NULL) alp_slot_release(&s->in_use);
}

static alp_status_t _errno_to_alp(int err)
{
	/* Delegates to the shared negative-errno baseline (issue #1638).
	 * This switch was one of 27 hand-copied copies that had drifted; the
	 * arms it carried all agreed with the baseline, so the mapping it
	 * produced for them is unchanged. */
	return alp_status_from_zephyr_errno(err);
}

static enum i2s_dir _to_dir(alp_i2s_dir_t d)
{
	switch (d) {
	case ALP_I2S_DIR_RX:
		return I2S_DIR_RX;
	case ALP_I2S_DIR_TX:
		return I2S_DIR_TX;
	case ALP_I2S_DIR_BOTH:
		return I2S_DIR_BOTH;
	default:
		return I2S_DIR_RX;
	}
}

static i2s_fmt_t _to_fmt(alp_i2s_format_t f)
{
	switch (f) {
	case ALP_I2S_FMT_I2S:
		return I2S_FMT_DATA_FORMAT_I2S;
	case ALP_I2S_FMT_LEFT_JUSTIFIED:
		return I2S_FMT_DATA_FORMAT_LEFT_JUSTIFIED;
	case ALP_I2S_FMT_RIGHT_JUSTIFIED:
		return I2S_FMT_DATA_FORMAT_RIGHT_JUSTIFIED;
	case ALP_I2S_FMT_PCM_SHORT:
		return I2S_FMT_DATA_FORMAT_PCM_SHORT;
	case ALP_I2S_FMT_PCM_LONG:
		return I2S_FMT_DATA_FORMAT_PCM_LONG;
	default:
		return I2S_FMT_DATA_FORMAT_I2S;
	}
}

static alp_status_t
z_open(const alp_i2s_config_t *cfg, alp_i2s_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg->bus_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
	if (cfg->bus_id >= ALP_SOC_I2S_COUNT) return ALP_ERR_OUT_OF_RANGE;
	const struct device *dev = _devs[cfg->bus_id];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;

	alp_z_i2s_side_t *s = _alloc_side();
	if (s == NULL) return ALP_ERR_NOMEM;

	/* Allocate a 2-block ping-pong slab.  Memory comes from the
     * caller's heap via k_malloc; on M-class targets the application
     * enables CONFIG_HEAP_MEM_POOL_SIZE.  Block bytes = frames ×
     * channels × (word_bits / 8). */
	size_t block_bytes =
	    (size_t)cfg->block_frames * (size_t)cfg->channels * (size_t)((cfg->word_bits + 7u) / 8u);
	s->slab_buf_bytes = block_bytes * 2u;
	s->block_bytes    = block_bytes;
	s->slab_buf       = k_malloc(s->slab_buf_bytes);
	if (s->slab_buf == NULL) {
		_free_side(s);
		return ALP_ERR_NOMEM;
	}
	int err = k_mem_slab_init(&s->mem_slab, s->slab_buf, block_bytes, 2);
	if (err != 0) {
		k_free(s->slab_buf);
		_free_side(s);
		return _errno_to_alp(err);
	}

	struct i2s_config zcfg = {
		.word_size      = cfg->word_bits,
		.channels       = cfg->channels,
		.format         = _to_fmt(cfg->format),
		.options        = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER,
		.frame_clk_freq = cfg->sample_rate_hz,
		.mem_slab       = &s->mem_slab,
		.block_size     = block_bytes,
		.timeout        = SYS_FOREVER_MS,
	};
	err = i2s_configure(dev, _to_dir(cfg->direction), &zcfg);
	if (err != 0) {
		k_free(s->slab_buf);
		_free_side(s);
		return _errno_to_alp(err);
	}

	st->dev         = (void *)dev;
	st->bus_id      = cfg->bus_id;
	st->be_data     = s;
	caps_out->flags = 0u;
	return ALP_OK;
}

/* Issue the real TX/RX START trigger.  Shared by z_start()'s immediate
 * path and z_write()'s pending-start retry so the i2s_trigger() call
 * site exists exactly once. Caller holds s->lock. */
static alp_status_t _start_trigger(const struct device *dev, alp_i2s_dir_t dir)
{
	return _errno_to_alp(i2s_trigger(dev, _to_dir(dir), I2S_TRIGGER_START));
}

/* The ONLY places tx_started may become true. Clears tx_pending_start in
 * the same assignment so tx_started => !tx_pending_start holds by
 * construction (issue #2132-MAJOR-2) -- caller holds s->lock. */
static void _mark_tx_started(alp_z_i2s_side_t *s)
{
	s->tx_started       = true;
	s->tx_pending_start = false;
}

/* The ONLY places a TX stream transitions back to fully idle (DRAIN or
 * DROP success). Clears all three flags together so no stale
 * tx_pending_start (or tx_block_queued) can survive a stop() regardless
 * of which branch reached it -- caller holds s->lock. */
static void _mark_tx_stopped(alp_z_i2s_side_t *s)
{
	s->tx_started       = false;
	s->tx_pending_start = false;
	s->tx_block_queued  = false;
}

/* The OTHER place tx_started clears back to false (issue #2137 review
 * round 2, finding 2): a PREPARE recovery leaves the stream idle-but-
 * owed-a-restart, not fully idle like _mark_tx_stopped() -- tx_pending_
 * start must end up TRUE here, not false, so this cannot just be a call
 * to _mark_tx_stopped(). Used by z_start()'s NOMEM branch and z_write()'s
 * PREPARE-retry-success branch; tx_started => !tx_pending_start still
 * holds by construction because both a stopped stream and a needs-
 * restart stream agree tx_started is false -- caller holds s->lock. */
static void _mark_tx_needs_restart(alp_z_i2s_side_t *s)
{
	s->tx_started       = false;
	s->tx_block_queued  = false;
	s->tx_pending_start = true;
}

static alp_status_t z_start(alp_i2s_backend_state_t *st)
{
	struct alp_i2s      *h   = CONTAINER_OF(st, struct alp_i2s, state);
	alp_z_i2s_side_t    *s   = (alp_z_i2s_side_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;
	if (s == NULL || dev == NULL) return ALP_ERR_NOT_READY;

	k_spinlock_key_t key = k_spin_lock(&s->lock);

	if (h->cfg.direction == ALP_I2S_DIR_TX && !s->tx_block_queued) {
		/* issue #2132: defer -- see the file header comment. RX is
		 * unaffected: rx_stream_start() allocates its own block from
		 * the slab instead of dequeuing a caller-filled ring
		 * (i2s_dw.c:751-787), so it has nothing to refuse here. */
		s->tx_pending_start = true;
		k_spin_unlock(&s->lock, key);
		return ALP_OK;
	}
	alp_status_t rc = _start_trigger(dev, h->cfg.direction);
	if (rc == ALP_ERR_IO) {
		/* issue #2137: START requires I2S_STATE_READY and -EIOs from
		 * ERROR too (i2s_dw.c:278-283), not just RUNNING -- a prior
		 * underrun (TX) or overrun (RX) can leave the stream there
		 * (i2s_dw.c:516-524 / :636-647). PREPARE is the ONLY trigger
		 * valid FROM ERROR (i2s_dw.c:340-347); it moves to READY and
		 * drops whatever was queued, so retrying START right after
		 * normally sees an empty ring on TX -- PREPARE frees any block
		 * that raced in ahead of it too (issue #2137 review round 2,
		 * finding 7: i2s_dw_write() checks state separately from
		 * queue_put(), i2s_dw.c:390-402, so a concurrent write is not
		 * ruled out here) -- either way this is exactly the #2132
		 * empty-queue case below, which already defers correctly (the
		 * comment inline explains why that is the right outcome, not a
		 * lingering failure). If PREPARE itself fails, `rc` is left
		 * untouched below, so the ORIGINAL START failure is what gets
		 * returned, not PREPARE's. */
		alp_status_t prepare_rc =
		    _errno_to_alp(i2s_trigger(dev, _to_dir(h->cfg.direction), I2S_TRIGGER_PREPARE));
		if (prepare_rc == ALP_OK) {
			/* issue #2137 review round 2, finding 3: otherwise-silent
			 * recovery -- the only bench-visible evidence a PREPARE
			 * ever ran. */
			LOG_WRN("i2s: recovered from I2S_STATE_ERROR on start() (dir=%d)",
			        (int)h->cfg.direction);
			rc = _start_trigger(dev, h->cfg.direction);
		}
	}
	if (h->cfg.direction == ALP_I2S_DIR_TX) {
		if (rc == ALP_OK) {
			_mark_tx_started(s);
		} else if (rc == ALP_ERR_NOMEM) {
			/* On TX, ALP_ERR_NOMEM from the real START trigger means
			 * exactly one thing on i2s_dw: queue_get() found the ring
			 * empty (i2s_dw.c:794-798), and a failed START changes no
			 * other driver state (i2s_dw.c returns before touching
			 * stream->state -- i2s_dw.c:277-291). tx_block_queued said
			 * otherwise, so treat THAT as stale rather than the
			 * hardware: clear it, defer instead of failing, and let
			 * the next write() queue for real and fire the trigger
			 * normally. Two independent, unrelated ways to reach this
			 * with tx_block_queued genuinely (not just apparently)
			 * true:
			 *   - issue #2132 review round 4: a race with a concurrent
			 *     stop() -- see the lock field's comment -- stamps a
			 *     stale tx_block_queued=true over a ring stop() just
			 *     emptied for real.
			 *   - issue #2137: the PREPARE retry just above. PREPARE
			 *     drops whatever was queued (i2s_dw.c:340-347), so a
			 *     retried START after a genuine underrun/overrun
			 *     recovery correctly finds the ring empty too -- this
			 *     is the EXPECTED outcome of that recovery, not a
			 *     race, and z_start() returning ALP_OK here (deferred)
			 *     rather than propagating -ENOMEM is exactly what lets
			 *     a caller's bare restart-after-underrun succeed.
			 *
			 * issue #2137 review round 2, finding 2: route this
			 * through _mark_tx_needs_restart(), not a manual two-
			 * field assign -- tx_started must ALSO clear here.
			 * Reachable with tx_started still true: start()
			 * (deferred) -> write() fires the real START ->
			 * underrun -> start(): the real START -EIOs, PREPARE
			 * succeeds, the retried START -ENOMEMs (ring empty, as
			 * above) -- tx_started was set true by write()'s own
			 * successful trigger and nothing had cleared it yet at
			 * this point, so leaving it alone here would violate
			 * tx_started => !tx_pending_start by construction. */
			_mark_tx_needs_restart(s);
			rc = ALP_OK;
		}
	}
	k_spin_unlock(&s->lock, key);
	return rc;
}

static alp_status_t z_stop(alp_i2s_backend_state_t *st)
{
	struct alp_i2s      *h   = CONTAINER_OF(st, struct alp_i2s, state);
	alp_z_i2s_side_t    *s   = (alp_z_i2s_side_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;
	if (s == NULL || dev == NULL) return ALP_ERR_NOT_READY;

	k_spinlock_key_t key = k_spin_lock(&s->lock);

	/* issue #2132: DRAIN requires I2S_STATE_RUNNING and -EIOs on a
	 * stream that never really started (start() deferred and never
	 * fired, or start() never called at all -- still READY).
	 * issue #2137: DRAIN also -EIOs on a stream that underran (TX,
	 * i2s_dw.c:516-524) or overran (RX, i2s_dw.c:636-647) into
	 * I2S_STATE_ERROR -- READY and ERROR both fail the SAME
	 * `state != RUNNING` check DRAIN makes (i2s_dw.c:315-321), so one
	 * fallback covers both directions and both causes: DROP is valid
	 * from any state except NOT_READY (i2s_dw.c:329-338, reachable from
	 * READY, RUNNING, AND ERROR alike), and releases queued blocks
	 * either way instead of stranding them.
	 *
	 * issue #2137 review round 2, findings 2/3: skip the DRAIN attempt
	 * entirely for a TX stream this backend KNOWS is not RUNNING --
	 * tx_started is the one bit that tracks the real hardware state (as
	 * opposed to h->started, set by the dispatcher on TX's own deferred-
	 * start ALP_OK even before the real trigger has fired -- see
	 * z_start()). Every i2s_dw DRAIN refusal logs its own LOG_ERR
	 * (i2s_dw.c:314), so issuing it here just to watch it fail on an
	 * already-known, already-handled case is pure log noise -- matches
	 * 24eb5a1c5's pre-#2137 behaviour, which used DROP unconditionally
	 * here with no DRAIN attempt at all. RX has no equivalent hardware-
	 * truth bit in the sidecar (RX never defers, see z_start()), so RX
	 * always attempts the real DRAIN first, same as before. */
	bool         tx_known_not_running = h->cfg.direction == ALP_I2S_DIR_TX && !s->tx_started;
	alp_status_t rc;
	if (tx_known_not_running) {
		/* The same status i2s_dw's own refused DRAIN would report, just
		 * without incurring its LOG_ERR side effect. */
		rc = ALP_ERR_IO;
	} else {
		rc = _errno_to_alp(i2s_trigger(dev, _to_dir(h->cfg.direction), I2S_TRIGGER_DRAIN));
	}
	if (rc == ALP_OK) {
		if (h->cfg.direction == ALP_I2S_DIR_TX) _mark_tx_stopped(s);
		k_spin_unlock(&s->lock, key);
		return ALP_OK;
	}

	/* issue #2137 review round 2, finding 8: DROP touches real hardware
	 * (i2s_dw.c's stream_disable -- for RX that includes its own
	 * i2s_clock_disable(), i2s_dw.c:834-852) and must not run for an RX
	 * handle that was never started, exactly like z_close() already
	 * gates its own DROP call on h->started (see that function's own
	 * comment). TX carries no such gate here -- DROP is well-defined,
	 * and necessary, even for a never-started TX handle, which may hold
	 * queued-but-unsent blocks that must still be released (see
	 * z_close()'s UAF comment) -- so only RX is gated. */
	if (h->cfg.direction != ALP_I2S_DIR_TX && !h->started) {
		k_spin_unlock(&s->lock, key);
		return rc;
	}

	alp_status_t drop_rc =
	    _errno_to_alp(i2s_trigger(dev, _to_dir(h->cfg.direction), I2S_TRIGGER_DROP));
	if (drop_rc == ALP_OK) {
		if (h->cfg.direction == ALP_I2S_DIR_TX) _mark_tx_stopped(s);
		k_spin_unlock(&s->lock, key);
		return ALP_OK;
	}
	/* Both refused -- surface DRAIN's failure (the primary, expected
	 * trigger). DRAIN was REFUSED here, not accepted (issue #2137 review
	 * round 2, finding 8) -- DROP's own precondition (anything but
	 * NOT_READY) is strictly wider than DRAIN's (RUNNING only), so DROP
	 * failing too on a stream whose DRAIN was already refused is not
	 * expected to happen in practice. */
	k_spin_unlock(&s->lock, key);
	return rc;
}

static alp_status_t
z_write(alp_i2s_backend_state_t *st, const void *block, size_t bytes, uint32_t timeout_ms)
{
	struct alp_i2s      *h   = CONTAINER_OF(st, struct alp_i2s, state);
	alp_z_i2s_side_t    *s   = (alp_z_i2s_side_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;
	if (s == NULL || dev == NULL) return ALP_ERR_NOT_READY;
	/* Bound the caller's length against the block negotiated at open()
	 * BEFORE claiming a block: i2s_write() below would reject an oversize
	 * length with -EINVAL, but only after the memcpy has already run. */
	if (bytes > s->block_bytes) return ALP_ERR_OUT_OF_RANGE;

	/* Both of these can block (k_mem_slab_alloc() on slab room,
	 * i2s_write()/i2s_dw_write() on its tx.sem, i2s_dw.c:396-397) --
	 * deliberately OUTSIDE s->lock; see the lock field's comment. */
	void *slab_block = NULL;
	int   err        = k_mem_slab_alloc(&s->mem_slab, &slab_block, K_MSEC(timeout_ms));
	if (err != 0 || slab_block == NULL) {
		return _errno_to_alp(err ? err : -ETIMEDOUT);
	}
	memcpy(slab_block, block, bytes);
	err = i2s_write(dev, slab_block, bytes);
	if (err == -EIO && h->cfg.direction == ALP_I2S_DIR_TX) {
		/* issue #2137: the stream underran into I2S_STATE_ERROR since
		 * the last write -- i2s_dw_write() refuses anything but
		 * RUNNING/READY (i2s_dw.c:390-394). PREPARE is the only
		 * trigger valid FROM ERROR (i2s_dw.c:340-347); it moves to
		 * READY and drops the ring (normally empty; PREPARE frees any
		 * block that raced in -- issue #2137 review round 2, finding 7:
		 * i2s_dw_write() checks state separately from queue_put(),
		 * i2s_dw.c:390-402, so a concurrent write is not ruled out
		 * here). Resetting the flags here to "open, start requested,
		 * nothing queued yet" is what #2132's own deferred-start
		 * machinery already expects at this point; the tx_started =>
		 * !tx_pending_start invariant holds because PREPARE succeeding
		 * means the stream is provably NOT running any more. */
		k_spinlock_key_t pkey = k_spin_lock(&s->lock);
		alp_status_t     prepare_rc =
		    _errno_to_alp(i2s_trigger(dev, _to_dir(h->cfg.direction), I2S_TRIGGER_PREPARE));
		/* issue #2137 review round 2, finding 4 (fixed more robustly
		 * than "only the flag reset depends on PREPARE's success" --
		 * see below): reset UNCONDITIONALLY, not only when OUR OWN
		 * PREPARE succeeded. The ORIGINAL i2s_write() already proves
		 * the stream was in I2S_STATE_ERROR, not RUNNING -- i2s_dw_
		 * write() only -EIOs from ERROR (i2s_dw.c:390-394) -- so
		 * tx_started, if true, is ALREADY known-stale the instant this
		 * branch is entered, regardless of what PREPARE itself reports.
		 * Gating the reset on prepare_rc == ALP_OK (as found on first
		 * pass) reintroduces finding 2's exact bug one level up: a
		 * CONCURRENT recovery that clears ERROR before our own PREPARE
		 * runs makes our PREPARE legitimately REFUSED (already READY,
		 * nothing to prepare from) -- but the retry below still queues
		 * a block for real. Leaving tx_pending_start false in that case
		 * silently starves the stream of the START trigger it now
		 * needs: the write reports ALP_OK, the block genuinely sits in
		 * the ring, and nothing ever plays it. Resetting unconditionally
		 * closes that gap; a still-stuck stream (PREPARE refused AND
		 * the retry below also fails) is unaffected either way, since
		 * that path frees the block and returns before ever reading
		 * tx_pending_start again. */
		_mark_tx_needs_restart(s);
		if (prepare_rc == ALP_OK) {
			/* issue #2137 review round 2, finding 3: otherwise-silent
			 * recovery -- the only bench-visible evidence a PREPARE
			 * ever ran. Logged only when OUR OWN PREPARE is what
			 * recovered it, not when a concurrent one already had. */
			LOG_WRN("i2s: recovered from I2S_STATE_ERROR on write() (dir=%d)",
			        (int)h->cfg.direction);
		}
		k_spin_unlock(&s->lock, pkey);
		/* Retry ONCE regardless of PREPARE's own result -- see above.
		 * A CONCURRENT call (another writer's own PREPARE, or a
		 * stop()'s DROP) may already have taken the stream out of ERROR
		 * between our own i2s_write() failing and our own PREPARE
		 * running, so OUR PREPARE seeing -EIO does not prove the stream
		 * is still stuck -- it proves only "not FROM ERROR right now",
		 * which is exactly the state a retry needs anyway. If the
		 * stream genuinely is still in ERROR, the retry below just gets
		 * the SAME -EIO i2s_dw.c:390-394 always returns from there, and
		 * falls into the existing free-and-report path below same as
		 * always -- no new failure mode, just one less needlessly-
		 * abandoned write. Retried with the SAME block either way --
		 * not freed and not re-allocated, so there is nothing to
		 * double-free below regardless of outcome. */
		err = i2s_write(dev, slab_block, bytes);
	}
	if (err != 0) {
		k_mem_slab_free(&s->mem_slab, slab_block);
		return _errno_to_alp(err);
	}

	if (h->cfg.direction != ALP_I2S_DIR_TX) return ALP_OK;

	k_spinlock_key_t key = k_spin_lock(&s->lock);
	s->tx_block_queued   = true;
	if (!s->tx_pending_start) {
		k_spin_unlock(&s->lock, key);
		return ALP_OK;
	}
	/* issue #2132: retry the deferred start now that a block is
	 * genuinely queued. _mark_tx_started() clears tx_pending_start ONLY
	 * on success -- a still-failing retry must not be swallowed (the
	 * write itself queued fine, but this stream still isn't playing). */
	alp_status_t start_rc = _start_trigger(dev, h->cfg.direction);
	if (start_rc == ALP_OK) {
		_mark_tx_started(s);
		k_spin_unlock(&s->lock, key);
		return ALP_OK;
	}
	/* MAJOR-1 (issue #2132 review): release the block THIS write just
	 * queued before reporting the failure. While a start was pending,
	 * nothing else could have queued ahead of it, so DROP here removes
	 * exactly this write's block, never a sibling write's -- an I2S
	 * write that returns an error must NEVER leave a block queued, or
	 * the caller (the audio layer's out_frames, or any other caller)
	 * cannot tell "nothing was queued" apart from "queued, but the
	 * deferred start still failed". tx_pending_start stays set (not
	 * cleared here) so the next write() retries again. */
	alp_status_t drop_rc =
	    _errno_to_alp(i2s_trigger(dev, _to_dir(h->cfg.direction), I2S_TRIGGER_DROP));
	if (drop_rc != ALP_OK) {
		/* issue #2132 review round 4: don't ignore DROP's own result.
		 * If DROP itself failed, we do NOT know whether the block is
		 * still queued -- clearing tx_block_queued here would be a
		 * guess, and it could silently break the "an error means
		 * nothing is queued" invariant this whole function exists to
		 * uphold. Surface DROP's failure instead of the original
		 * start_rc, leaving tx_block_queued set so the state is
		 * visibly unresolved rather than confidently wrong. */
		k_spin_unlock(&s->lock, key);
		return drop_rc;
	}
	s->tx_block_queued = false;
	k_spin_unlock(&s->lock, key);
	return start_rc;
}

static alp_status_t z_read(alp_i2s_backend_state_t *st,
                           void                    *block,
                           size_t                   bytes,
                           size_t                  *bytes_out,
                           uint32_t                 timeout_ms)
{
	(void)timeout_ms; /* upstream i2s_read has no per-call timeout */
	alp_z_i2s_side_t    *s   = (alp_z_i2s_side_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;
	if (s == NULL || dev == NULL) return ALP_ERR_NOT_READY;

	void  *slab_block = NULL;
	size_t got        = 0u;
	int    err        = i2s_read(dev, &slab_block, &got);
	if (err != 0) return _errno_to_alp(err);

	if (got > bytes) got = bytes;
	memcpy(block, slab_block, got);
	k_mem_slab_free(&s->mem_slab, slab_block);
	if (bytes_out != NULL) *bytes_out = got;
	return ALP_OK;
}

static void z_close(alp_i2s_backend_state_t *st)
{
	alp_z_i2s_side_t    *s   = (alp_z_i2s_side_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;
	struct alp_i2s      *h   = CONTAINER_OF(st, struct alp_i2s, state);

	/* issue #2132 (UAF): TX always DROPs, NOT only "if (h->started)" --
	 * a TX handle that only ever queued blocks (write() before start(),
	 * or a start() whose retry never fired) still holds real slab-block
	 * pointers in the driver's TX ring / in-flight mem_block even though
	 * h->started is false. i2s_dw.c accepts DROP from READY too
	 * (i2s_dw.c:329-338), and it is the only trigger that releases both
	 * the in-flight block and everything still queued (tx_stream_disable
	 * + tx_queue_drop, i2s_dw.c:886-900) regardless of run state.
	 * Skipping this for a not-yet-started TX handle left those pointers
	 * dangling past the k_free() below: the NEXT open() on this device's
	 * tx_stream_start() would dequeue a pointer into freed memory, and
	 * the completion path would free it into the NEW handle's slab --
	 * memory corruption, not just a leak.
	 *
	 * RX has no equivalent trap -- rx_stream_start() allocates its own
	 * block instead of dequeuing a caller-filled ring (i2s_dw.c:751-787)
	 * -- so RX stays gated on h->started exactly like before #2132,
	 * byte-identical to i2s_dw's own rx_stream_disable() (i2s_dw.c:834-
	 * 852, including its i2s_clock_disable() teardown), which only ever
	 * ran when RX had actually been triggered. */
	if (dev != NULL && (h->cfg.direction == ALP_I2S_DIR_TX || h->started)) {
		(void)i2s_trigger(dev, _to_dir(h->cfg.direction), I2S_TRIGGER_DROP);
	}
	if (s != NULL) {
		if (s->slab_buf != NULL) {
			k_free(s->slab_buf);
			s->slab_buf = NULL;
		}
		_free_side(s);
	}
	st->be_data = NULL;
}

static const alp_i2s_ops_t _ops = {
	.open  = z_open,
	.start = z_start,
	.stop  = z_stop,
	.write = z_write,
	.read  = z_read,
	.close = z_close,
};

ALP_BACKEND_REGISTER(i2s,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
