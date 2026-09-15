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
 * z_start(): if TX and nothing is queued yet, set tx_pending_start and
 * return ALP_OK without touching hardware (write-then-start already
 * queues first, so it still triggers immediately, unchanged).
 *
 * z_write(): after a block is genuinely queued (i2s_write() succeeded),
 * set tx_block_queued.  If tx_pending_start, retry the real START --
 * clear tx_pending_start (and set tx_started) ONLY on success; a write
 * into a stream whose START keeps failing must never return ALP_OK,
 * so a still-failing retry returns that failure from THIS write() call,
 * and the very next write() retries again (tx_pending_start stays set).
 *
 * z_stop(): if TX and not tx_started -- covers both "start() was
 * deferred and never fired" and "start() was never called at all" --
 * DRAIN would -EIO despite real blocks possibly sitting in the queue
 * holding slab memory.  Issue DROP instead, which works from READY and
 * releases them, then clear all three flags.  Otherwise (really
 * running) DRAIN as before.
 *
 * z_close(): DROP unconditionally whenever dev != NULL, regardless of
 * any of the flags above -- a pending-but-written or START-still-
 * failing handle can hold real slab-block pointers in the driver's TX
 * ring / in-flight mem_block, and this function is about to k_free()
 * the slab backing them.  Skipping DROP left those pointers dangling
 * for the NEXT open() on this device: its tx_stream_start() would
 * dequeue a pointer into freed memory, and the completion path frees
 * it into the NEW handle's slab -- corruption, not just a leak.
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i2s.h>
#include <alp/soc_caps.h>

#include "alp_errno.h"
#include "i2s_ops.h"
#include "alp_slot_claim.h"

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
	bool in_use;
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
 * site exists exactly once. */
static alp_status_t _start_trigger(const struct device *dev, alp_i2s_dir_t dir)
{
	return _errno_to_alp(i2s_trigger(dev, _to_dir(dir), I2S_TRIGGER_START));
}

static alp_status_t z_start(alp_i2s_backend_state_t *st)
{
	struct alp_i2s      *h   = CONTAINER_OF(st, struct alp_i2s, state);
	alp_z_i2s_side_t    *s   = (alp_z_i2s_side_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;
	if (s == NULL || dev == NULL) return ALP_ERR_NOT_READY;

	if (h->cfg.direction == ALP_I2S_DIR_TX && !s->tx_block_queued) {
		/* issue #2132: defer -- see the file header comment. RX is
		 * unaffected: rx_stream_start() allocates its own block from
		 * the slab instead of dequeuing a caller-filled ring
		 * (i2s_dw.c:751-787), so it has nothing to refuse here. */
		s->tx_pending_start = true;
		return ALP_OK;
	}
	alp_status_t rc = _start_trigger(dev, h->cfg.direction);
	if (rc == ALP_OK && h->cfg.direction == ALP_I2S_DIR_TX) s->tx_started = true;
	return rc;
}

static alp_status_t z_stop(alp_i2s_backend_state_t *st)
{
	struct alp_i2s      *h   = CONTAINER_OF(st, struct alp_i2s, state);
	alp_z_i2s_side_t    *s   = (alp_z_i2s_side_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;
	if (s == NULL || dev == NULL) return ALP_ERR_NOT_READY;

	if (h->cfg.direction == ALP_I2S_DIR_TX && !s->tx_started) {
		/* issue #2132: never really running -- either start() was
		 * deferred and never fired, or start() was never even called
		 * (write-only). DRAIN requires I2S_STATE_RUNNING and would
		 * -EIO here (i2s_dw.c:301-304/315-321) even though real
		 * blocks may still be queued, holding slab memory. DROP
		 * releases them from READY too (i2s_dw.c:329-338) instead of
		 * stranding them. */
		alp_status_t rc =
		    _errno_to_alp(i2s_trigger(dev, _to_dir(h->cfg.direction), I2S_TRIGGER_DROP));
		if (rc == ALP_OK) {
			s->tx_pending_start = false;
			s->tx_block_queued  = false;
		}
		return rc;
	}
	alp_status_t rc = _errno_to_alp(i2s_trigger(dev, _to_dir(h->cfg.direction), I2S_TRIGGER_DRAIN));
	if (rc == ALP_OK && h->cfg.direction == ALP_I2S_DIR_TX) {
		s->tx_started      = false;
		s->tx_block_queued = false;
	}
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

	void *slab_block = NULL;
	int   err        = k_mem_slab_alloc(&s->mem_slab, &slab_block, K_MSEC(timeout_ms));
	if (err != 0 || slab_block == NULL) {
		return _errno_to_alp(err ? err : -ETIMEDOUT);
	}
	memcpy(slab_block, block, bytes);
	err = i2s_write(dev, slab_block, bytes);
	if (err != 0) {
		k_mem_slab_free(&s->mem_slab, slab_block);
		return _errno_to_alp(err);
	}

	if (h->cfg.direction == ALP_I2S_DIR_TX) {
		s->tx_block_queued = true;
		if (s->tx_pending_start) {
			/* issue #2132: retry the deferred start now that a block
			 * is genuinely queued. Clear tx_pending_start ONLY on
			 * success -- a still-failing retry must not be swallowed
			 * (the write itself queued fine, but this stream still
			 * isn't playing), so return the START failure from THIS
			 * write() call instead of ALP_OK; the very next write()
			 * retries again since tx_pending_start stays set. */
			alp_status_t start_rc = _start_trigger(dev, h->cfg.direction);
			if (start_rc == ALP_OK) {
				s->tx_pending_start = false;
				s->tx_started       = true;
			} else {
				return start_rc;
			}
		}
	}
	return ALP_OK;
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

	/* issue #2132 (UAF): DROP unconditionally, NOT only "if (h->started)"
	 * -- a handle that only ever queued blocks (write() before start(),
	 * or a start() whose retry never fired) still holds real slab-block
	 * pointers in the driver's TX ring / in-flight mem_block even though
	 * h->started is false. i2s_dw.c accepts DROP from READY too
	 * (i2s_dw.c:329-338), and it is the only trigger that releases both
	 * the in-flight block and everything still queued (tx_stream_disable
	 * + tx_queue_drop, i2s_dw.c:886-900) regardless of run state.
	 * Skipping this for a not-yet-started handle left those pointers
	 * dangling past the k_free() below: the NEXT open() on this device's
	 * tx_stream_start() would dequeue a pointer into freed memory, and
	 * the completion path would free it into the NEW handle's slab --
	 * memory corruption, not just a leak. */
	if (dev != NULL) {
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
