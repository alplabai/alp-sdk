/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr backend for the <alp/mproc.h> surface.  Owns all
 * three primitives behind one registry entry:
 *
 *   - mbox:  Zephyr MBOX driver class (Alif's MHU controller is
 *            registered as a Zephyr mbox device on AEN).  The
 *            studio-supplied alp_mbox_config.channel is resolved
 *            through the alp-mboxN DT aliases into a
 *            (const struct device *, channel_id) pair.
 *
 *   - shmem: DT-anchored carve-outs.  alp_shmem_open(cfg) matches
 *            cfg->name strictly against names "alp_shmem0".."N"
 *            derived from the alp-shmemN DT aliases (a static
 *            lookup table built at compile time from
 *            DT_ALIAS(alp_shmemN)).  base + size come from the
 *            aliased node's `reg` property.  Unknown names yield
 *            NOT_READY -- the wrapper does not synthesise regions.
 *
 *   - hwsem: Intra-core k_sem fallback.  alp_hwsem_open(id) maps
 *            an integer hwsem_id directly to one of
 *            CONFIG_ALP_SDK_MPROC_HWSEM_COUNT k_sem slots (count=1,
 *            mutex semantics).  This serialises access WITHIN one
 *            Zephyr image only -- cross-core / per-SoC HWSEM-block
 *            wiring (AEN HWSEM, ST HSEM, etc.) lands per-SoC in a
 *            follow-on track.  The single-core limitation is the
 *            documented behaviour.
 *
 * Registers as silicon_ref="*" at priority 100 -- mirrors the
 * design spec Section 2 backend matrix (zephyr_drv wins on every
 * SoC unless a more specific backend registers).
 *
 * Gated on CONFIG_ALP_SDK_MPROC -- when OFF the I/O ops return
 * NOSUPPORT but the registry entry still links so the dispatcher
 * picks it ahead of sw_fallback on real silicon builds with the
 * MBOX driver class in the device tree.
 *
 * Frame serialisation: when CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING is
 * on, the mbox payload is wrapped in the placeholder envelope from
 * src/common/proto/alp_mproc_frame.h before handing it to Zephyr's
 * mbox driver; inbound frames are unwrapped before invoking the
 * user callback.  Both ends must agree on the flag value.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/mproc.h>
#include <alp/peripheral.h>

#include "alp_errno.h"
#include "alp_slot_claim.h"
#include "mproc_ops.h"

#if defined(CONFIG_ALP_SDK_MPROC)
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util_macro.h>
#endif

#if defined(CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING)
#include "proto/alp_mproc_frame.h"
#ifndef CONFIG_ALP_SDK_MPROC_FRAME_MAX_BYTES
#define CONFIG_ALP_SDK_MPROC_FRAME_MAX_BYTES 512
#endif
#endif

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ALP_SDK_MPROC)
struct shmem_be {
	void  *base;
	size_t size;
};

/* Doorbell-only controllers carry no payload, so the bytes ride in the
 * last ALP_MBOX_DB_SLOT_BYTES of alp-shmem0 (one slot, one message
 * in flight: request/response use, not a queue).  The region must be
 * non-cacheable or cache-maintained by the app, same as any alp_shmem. */
#define ALP_MBOX_DB_SLOT_BYTES 64u
struct alp_mbox_db_slot {
	volatile uint32_t len;
	volatile uint8_t  data[ALP_MBOX_DB_SLOT_BYTES - sizeof(uint32_t)];
};
#define ALP_MBOX_DB_PAYLOAD_MAX (sizeof(((struct alp_mbox_db_slot *)0)->data))

struct mbox_be {
	const struct device *dev;
	int                      mtu;     /* mbox_mtu_get(dev); 0 = doorbell-only, <0 = unknown */
	struct alp_mbox_db_slot *db_slot; /* non-NULL only for a doorbell-only controller */
	const struct device     *rx_dev;  /* == dev unless an alp-mbox-rxN alias splits TX/RX */
	uint32_t             channel;
	alp_mbox_msg_cb_t    cb;
	void                *cb_user;
#if defined(CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING)
	uint32_t tx_sequence; /* monotonic outbound counter */
	uint8_t  tx_scratch[CONFIG_ALP_SDK_MPROC_FRAME_MAX_BYTES];
#endif
};

struct hwsem_be {
	uint32_t hwsem_id;
	bool     held;
};

#ifndef CONFIG_ALP_SDK_MAX_SHMEM_HANDLES
#define CONFIG_ALP_SDK_MAX_SHMEM_HANDLES 2
#endif
#ifndef CONFIG_ALP_SDK_MAX_MBOX_HANDLES
#define CONFIG_ALP_SDK_MAX_MBOX_HANDLES 4
#endif
#ifndef CONFIG_ALP_SDK_MAX_HWSEM_HANDLES
#define CONFIG_ALP_SDK_MAX_HWSEM_HANDLES 4
#endif

static struct shmem_be _shmem_be_pool[CONFIG_ALP_SDK_MAX_SHMEM_HANDLES];
static bool            _shmem_be_in_use[CONFIG_ALP_SDK_MAX_SHMEM_HANDLES];

static struct mbox_be _mbox_be_pool[CONFIG_ALP_SDK_MAX_MBOX_HANDLES];
static bool           _mbox_be_in_use[CONFIG_ALP_SDK_MAX_MBOX_HANDLES];

static struct hwsem_be _hwsem_be_pool[CONFIG_ALP_SDK_MAX_HWSEM_HANDLES];
static bool            _hwsem_be_in_use[CONFIG_ALP_SDK_MAX_HWSEM_HANDLES];

static struct shmem_be *_shmem_be_alloc(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_shmem_be_pool); ++i) {
		/* Atomic claim (src/common/alp_slot_claim.h, issue #1115):
		 * a compare-exchange, so exactly one concurrent opener wins the
		 * slot.  in_use lives in a parallel array rather than inside the
		 * slot struct, so the winner may zero the whole slot afterwards --
		 * no offsetof form is needed here. */
		if (alp_slot_try_claim(&_shmem_be_in_use[i])) {
			memset(&_shmem_be_pool[i], 0, sizeof(_shmem_be_pool[i]));
			return &_shmem_be_pool[i];
		}
	}
	return NULL;
}

static void _shmem_be_free(struct shmem_be *p)
{
	if (p == NULL) return;
	for (size_t i = 0; i < ARRAY_SIZE(_shmem_be_pool); ++i) {
		if (&_shmem_be_pool[i] == p) {
			alp_slot_release(&_shmem_be_in_use[i]);
			return;
		}
	}
}

static struct mbox_be *_mbox_be_alloc(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_mbox_be_pool); ++i) {
		/* Atomic claim (src/common/alp_slot_claim.h, issue #1115):
		 * a compare-exchange, so exactly one concurrent opener wins the
		 * slot.  in_use lives in a parallel array rather than inside the
		 * slot struct, so the winner may zero the whole slot afterwards --
		 * no offsetof form is needed here. */
		if (alp_slot_try_claim(&_mbox_be_in_use[i])) {
			memset(&_mbox_be_pool[i], 0, sizeof(_mbox_be_pool[i]));
			return &_mbox_be_pool[i];
		}
	}
	return NULL;
}

static void _mbox_be_free(struct mbox_be *p)
{
	if (p == NULL) return;
	for (size_t i = 0; i < ARRAY_SIZE(_mbox_be_pool); ++i) {
		if (&_mbox_be_pool[i] == p) {
			alp_slot_release(&_mbox_be_in_use[i]);
			return;
		}
	}
}

static struct hwsem_be *_hwsem_be_alloc(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_hwsem_be_pool); ++i) {
		/* Atomic claim (src/common/alp_slot_claim.h, issue #1115):
		 * a compare-exchange, so exactly one concurrent opener wins the
		 * slot.  in_use lives in a parallel array rather than inside the
		 * slot struct, so the winner may zero the whole slot afterwards --
		 * no offsetof form is needed here. */
		if (alp_slot_try_claim(&_hwsem_be_in_use[i])) {
			memset(&_hwsem_be_pool[i], 0, sizeof(_hwsem_be_pool[i]));
			return &_hwsem_be_pool[i];
		}
	}
	return NULL;
}

static void _hwsem_be_free(struct hwsem_be *p)
{
	if (p == NULL) return;
	for (size_t i = 0; i < ARRAY_SIZE(_hwsem_be_pool); ++i) {
		if (&_hwsem_be_pool[i] == p) {
			alp_slot_release(&_hwsem_be_in_use[i]);
			return;
		}
	}
}

static alp_status_t errno_to_alp(int err)
{
	/* Delegates to the shared negative-errno baseline (issue #1638).
	 * This switch was one of 27 hand-copied copies that had drifted; the
	 * arms it carried all agreed with the baseline, so the mapping it
	 * produced for them is unchanged. */
	return alp_status_from_zephyr_errno(err);
}
#endif /* CONFIG_ALP_SDK_MPROC */

/* ================================================================== */
/* Shared memory                                                       */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_MPROC)

struct alp_shmem_region {
	const char *name; /* matches alp_shmem_open's cfg->name */
	void       *base;
	size_t      size;
};

/* Build a const lookup table from DT_ALIAS(alp_shmemN).  IF_ENABLED
 * skips the entry when the alias isn't defined OR isn't `okay` -- a
 * present-but-disabled alp-shmemN drops out of the table rather than
 * yielding a live region on an uninstantiated carve-out.  Up to 4
 * regions supported -- raise the upper bound here if a SoM needs more. */
#define ALP_SHMEM_REGION_ENTRY_IF(idx) \
	IF_ENABLED(DT_NODE_HAS_STATUS(DT_ALIAS(_CONCAT(alp_shmem, idx)), okay), \
	           ({ \
	                .name = "alp_shmem" #idx, \
	                .base = (void *)DT_REG_ADDR(DT_ALIAS(_CONCAT(alp_shmem, idx))), \
	                .size = (size_t)DT_REG_SIZE(DT_ALIAS(_CONCAT(alp_shmem, idx))), \
	            }, ))

static const struct alp_shmem_region alp_shmem_regions[] = { ALP_SHMEM_REGION_ENTRY_IF(
	0) ALP_SHMEM_REGION_ENTRY_IF(1) ALP_SHMEM_REGION_ENTRY_IF(2) ALP_SHMEM_REGION_ENTRY_IF(3) };
#define ALP_SHMEM_REGION_COUNT (sizeof(alp_shmem_regions) / sizeof(alp_shmem_regions[0]))

#endif /* CONFIG_ALP_SDK_MPROC */

static alp_status_t z_shmem_open(const alp_shmem_config_t  *cfg,
                                 alp_shmem_backend_state_t *state,
                                 alp_capabilities_t        *caps_out)
{
	(void)caps_out;
#if defined(CONFIG_ALP_SDK_MPROC)
	/* Resolve cfg->name against the DT-alias lookup table. */
	const struct alp_shmem_region *region = NULL;
	for (size_t i = 0; i < ALP_SHMEM_REGION_COUNT; ++i) {
		if (strcmp(alp_shmem_regions[i].name, cfg->name) == 0) {
			region = &alp_shmem_regions[i];
			break;
		}
	}
	if (region == NULL) return ALP_ERR_NOT_READY;
	struct shmem_be *be = _shmem_be_alloc();
	if (be == NULL) return ALP_ERR_NOMEM;
	be->base       = region->base;
	be->size       = region->size;
	state->be_data = be;
	return ALP_OK;
#else
	(void)cfg;
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t
z_shmem_view(alp_shmem_backend_state_t *state, void **base_out, size_t *size_out)
{
#if defined(CONFIG_ALP_SDK_MPROC)
	struct shmem_be *be = (struct shmem_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	if (base_out != NULL) *base_out = be->base;
	if (size_out != NULL) *size_out = be->size;
	return ALP_OK;
#else
	(void)state;
	(void)base_out;
	(void)size_out;
	return ALP_ERR_NOSUPPORT;
#endif
}

static void z_shmem_close(alp_shmem_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_MPROC)
	struct shmem_be *be = (struct shmem_be *)state->be_data;
	if (be == NULL) return;
	_shmem_be_free(be);
	state->be_data = NULL;
#else
	(void)state;
#endif
}

/* ================================================================== */
/* Mailbox                                                             */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_MPROC)

#define ALP_MBOX_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_ALIAS(_CONCAT(alp_mbox, idx)), okay), \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_mbox, idx)))), \
	            (NULL))

static const struct device *const alp_mbox_devs[] = {
	ALP_MBOX_DEV_OR_NULL(0),
	ALP_MBOX_DEV_OR_NULL(1),
	ALP_MBOX_DEV_OR_NULL(2),
	ALP_MBOX_DEV_OR_NULL(3),
};

/* Optional receive-side device per channel (alp-mbox-rxN).  MBOX frames
 * that are one-directional in hardware (Alif MHUv2: a frame is either a
 * sender or a receiver) cannot both send on and take callbacks from one
 * node, so such boards alias the sender frame as alp-mboxN and the
 * receiver frame as alp-mbox-rxN.  Absent alias -> same device both ways. */
#define ALP_MBOX_RX_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_ALIAS(_CONCAT(alp_mbox_rx, idx)), okay), \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_mbox_rx, idx)))), \
	            (NULL))

static const struct device *const alp_mbox_rx_devs[] = {
	ALP_MBOX_RX_DEV_OR_NULL(0),
	ALP_MBOX_RX_DEV_OR_NULL(1),
	ALP_MBOX_RX_DEV_OR_NULL(2),
	ALP_MBOX_RX_DEV_OR_NULL(3),
};

static void mbox_rx_cb(const struct device *dev,
                       mbox_channel_id_t    channel_id,
                       void                *user_data,
                       struct mbox_msg     *data)
{
	(void)dev;
	struct mbox_be *be = (struct mbox_be *)user_data;
	if (be == NULL || be->cb == NULL) return;

	const void *cb_data = data ? data->data : NULL;
	size_t      cb_len  = data ? data->size : 0;

	/* Doorbell-only controller: the ISR hands us NULL, the bytes are in
	 * the slot.  Clamp the peer-written length to the slot. */
	if (data == NULL && be->db_slot != NULL) {
		barrier_dmem_fence_full();
		cb_len = be->db_slot->len;
		if (cb_len > ALP_MBOX_DB_PAYLOAD_MAX) return;
		cb_data = (const void *)be->db_slot->data;
	}

#if defined(CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING)
	/* Unwrap the placeholder envelope.  Malformed frames (magic
     * mismatch, declared length overflow) are dropped silently --
     * the peer is expected to retry on Sequence-gap detection at
     * the application layer.  Dropping silently here matches the
     * intended eventual nanopb behaviour (deferred, no committed
     * version): a decoder failure surfaces as "no message arrived"
     * rather than a corrupted payload. */
	const uint8_t *payload     = NULL;
	size_t         payload_len = 0;
	if (cb_data != NULL) {
		alp_status_t s =
		    alp_mproc_frame_decode((const uint8_t *)cb_data, cb_len, NULL, &payload, &payload_len);
		if (s != ALP_OK) {
			return;
		}
		cb_data = payload;
		cb_len  = payload_len;
	}
#endif

	be->cb((uint32_t)channel_id, cb_data, cb_len, be->cb_user);
}

#endif /* CONFIG_ALP_SDK_MPROC */

static alp_status_t z_mbox_open(const alp_mbox_config_t  *cfg,
                                alp_mbox_backend_state_t *state,
                                alp_capabilities_t       *caps_out)
{
	(void)caps_out;
#if defined(CONFIG_ALP_SDK_MPROC)
	if (cfg->channel >= ARRAY_SIZE(alp_mbox_devs)) return ALP_ERR_INVAL;
	const struct device *dev = alp_mbox_devs[cfg->channel];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;
	const struct device     *rx_dev  = alp_mbox_rx_devs[cfg->channel];
	int                      mtu     = mbox_mtu_get(dev);
	struct alp_mbox_db_slot *db_slot = NULL;
	if (mtu == 0) {
		for (size_t i = 0; i < ALP_SHMEM_REGION_COUNT; ++i) {
			if (strcmp(alp_shmem_regions[i].name, "alp_shmem0") == 0 &&
			    alp_shmem_regions[i].size >= 2u * sizeof(*db_slot)) {
				db_slot = (struct alp_mbox_db_slot *)((uint8_t *)alp_shmem_regions[i].base +
				                                      alp_shmem_regions[i].size - sizeof(*db_slot));
				break;
			}
		}
		/* Doorbell-only and nowhere to carry the bytes: refuse at open. */
		if (db_slot == NULL) return ALP_ERR_NOT_READY;
	}
	if (rx_dev == NULL) rx_dev = dev;
	if (!device_is_ready(rx_dev)) return ALP_ERR_NOT_READY;
	struct mbox_be *be = _mbox_be_alloc();
	if (be == NULL) return ALP_ERR_NOMEM;
	be->dev        = dev;
	be->mtu        = mtu;
	be->db_slot    = db_slot;
	be->rx_dev     = rx_dev;
	be->channel    = cfg->channel;
	state->be_data = be;
	return ALP_OK;
#else
	(void)cfg;
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

#if defined(CONFIG_ALP_SDK_MPROC)
/* Send `len` bytes.  Payload-capable controllers (mtu > 0) take the bytes
 * in the mbox message; an oversize message is refused rather than
 * truncated.  Doorbell-only controllers (mtu == 0, e.g. Alif MHUv2, whose
 * send rejects any non-NULL message) get the bytes through the doorbell
 * slot in the shared region and a NULL-message ring. */
static alp_status_t mbox_tx(struct mbox_be *be, const void *data, size_t len)
{
	if (be->db_slot != NULL) {
		if (len > ALP_MBOX_DB_PAYLOAD_MAX) return ALP_ERR_OUT_OF_RANGE;
		struct alp_mbox_db_slot *slot = be->db_slot;
		/* Payload first, then the length that publishes it, then the ring. */
		if (len > 0u) memcpy((void *)slot->data, data, len);
		barrier_dmem_fence_full();
		slot->len = (uint32_t)len;
		barrier_dmem_fence_full();
		return errno_to_alp(mbox_send(be->dev, be->channel, NULL));
	}
	if (be->mtu > 0 && len > (size_t)be->mtu) return ALP_ERR_OUT_OF_RANGE;
	struct mbox_msg msg = {
		.data = data,
		.size = len,
	};
	return errno_to_alp(mbox_send(be->dev, be->channel, &msg));
}
#endif /* CONFIG_ALP_SDK_MPROC */

static alp_status_t
z_mbox_send(alp_mbox_backend_state_t *state, const void *data, size_t len, uint32_t timeout_ms)
{
	(void)timeout_ms;
#if defined(CONFIG_ALP_SDK_MPROC)
	struct mbox_be *be = (struct mbox_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING)
	/* Encode into the per-handle scratch buffer.  Sequence is bumped
     * only on a successful encode + queue so failed sends don't leave
     * gaps the peer interprets as packet loss.  First successful send
     * carries sequence=1 (sequence==0 doubles as "no message yet" in
     * peer-side debugging). */
	uint32_t     next_seq   = be->tx_sequence + 1u;
	size_t       framed_len = 0;
	alp_status_t s          = alp_mproc_frame_encode(
	    next_seq, data, len, be->tx_scratch, sizeof(be->tx_scratch), &framed_len);
	if (s != ALP_OK) {
		return s;
	}
	s = mbox_tx(be, be->tx_scratch, framed_len);
	if (s == ALP_OK) {
		be->tx_sequence = next_seq;
	}
	return s;
#else
	return mbox_tx(be, data, len);
#endif
#else
	(void)state;
	(void)data;
	(void)len;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t
z_mbox_set_callback(alp_mbox_backend_state_t *state, alp_mbox_msg_cb_t cb, void *user)
{
#if defined(CONFIG_ALP_SDK_MPROC)
	struct mbox_be *be = (struct mbox_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	be->cb      = cb;
	be->cb_user = user;
	int err     = mbox_register_callback(be->rx_dev, be->channel, cb ? mbox_rx_cb : NULL, be);
	if (err == 0) {
		err = mbox_set_enabled(be->rx_dev, be->channel, cb ? true : false);
	}
	return errno_to_alp(err);
#else
	(void)state;
	(void)cb;
	(void)user;
	return ALP_ERR_NOSUPPORT;
#endif
}

static void z_mbox_close(alp_mbox_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_MPROC)
	struct mbox_be *be = (struct mbox_be *)state->be_data;
	if (be == NULL) return;
	/* #1644: disable first, then unregister the callback, then free --
	 * z_mbox_set_callback() registers `be` itself as the driver's
	 * user_data, so freeing it first would leave that registration
	 * pointing at freed memory. Not a demonstrated crash (the channel is
	 * already disabled above, so the stale registration is not known to
	 * be reachable) -- this restores the documented invariant that
	 * backend callbacks stop before their slot is released. */
	(void)mbox_set_enabled(be->rx_dev, be->channel, false);
	(void)mbox_register_callback(be->rx_dev, be->channel, NULL, NULL);
	_mbox_be_free(be);
	state->be_data = NULL;
#else
	(void)state;
#endif
}

/* ================================================================== */
/* Hardware semaphore                                                  */
/* ================================================================== */

/* Intra-core fallback: an array of k_sem indexed by hwsem_id, with
 * count=1 (mutex semantics).  This serializes access WITHIN one
 * Zephyr image but does NOT cross cores -- a peer Zephyr / Linux
 * image on the same SoM uses a DIFFERENT k_sem array and won't see
 * the lock.  Per-SoC real-HWSEM (AEN HWSEM block, ST HSEM, etc.)
 * land per-SoC in a follow-on track; until then call sites that
 * need cross-core mutex must use a real shared-state primitive
 * (DT-anchored memory + atomic ops) instead.
 *
 * hwsem_id range: 0..15.  Bump CONFIG_ALP_SDK_MPROC_HWSEM_COUNT
 * if a SoM needs more. */

#ifndef CONFIG_ALP_SDK_MPROC_HWSEM_COUNT
#define CONFIG_ALP_SDK_MPROC_HWSEM_COUNT 16
#endif

#if defined(CONFIG_ALP_SDK_MPROC)
static struct k_sem      alp_hwsem_kobjs[CONFIG_ALP_SDK_MPROC_HWSEM_COUNT];
static bool              alp_hwsem_kobjs_initialised;
static struct k_spinlock alp_hwsem_init_lock;

static void hwsem_kobjs_init_once(void)
{
	k_spinlock_key_t key = k_spin_lock(&alp_hwsem_init_lock);
	if (!alp_hwsem_kobjs_initialised) {
		for (size_t i = 0; i < CONFIG_ALP_SDK_MPROC_HWSEM_COUNT; ++i) {
			k_sem_init(&alp_hwsem_kobjs[i], 1, 1);
		}
		alp_hwsem_kobjs_initialised = true;
	}
	k_spin_unlock(&alp_hwsem_init_lock, key);
}
#endif

static alp_status_t
z_hwsem_open(uint32_t hwsem_id, alp_hwsem_backend_state_t *state, alp_capabilities_t *caps_out)
{
	(void)caps_out;
#if defined(CONFIG_ALP_SDK_MPROC)
	if (hwsem_id >= CONFIG_ALP_SDK_MPROC_HWSEM_COUNT) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	hwsem_kobjs_init_once();
	struct hwsem_be *be = _hwsem_be_alloc();
	if (be == NULL) return ALP_ERR_NOMEM;
	be->hwsem_id   = hwsem_id;
	be->held       = false;
	state->be_data = be;
	return ALP_OK;
#else
	(void)hwsem_id;
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_hwsem_try_lock(alp_hwsem_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_MPROC)
	struct hwsem_be *be = (struct hwsem_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	/* k_sem_take with K_NO_WAIT returns -EBUSY when the count is 0. */
	int rc = k_sem_take(&alp_hwsem_kobjs[be->hwsem_id], K_NO_WAIT);
	if (rc == -EBUSY) return ALP_ERR_BUSY;
	if (rc != 0) return errno_to_alp(rc);
	be->held = true;
	return ALP_OK;
#else
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_hwsem_lock(alp_hwsem_backend_state_t *state, uint32_t timeout_ms)
{
#if defined(CONFIG_ALP_SDK_MPROC)
	struct hwsem_be *be = (struct hwsem_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	k_timeout_t to = (timeout_ms == UINT32_MAX) ? K_FOREVER : K_MSEC(timeout_ms);
	int         rc = k_sem_take(&alp_hwsem_kobjs[be->hwsem_id], to);
	if (rc == -EAGAIN) return ALP_ERR_TIMEOUT;
	if (rc != 0) return errno_to_alp(rc);
	be->held = true;
	return ALP_OK;
#else
	(void)state;
	(void)timeout_ms;
	return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_hwsem_unlock(alp_hwsem_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_MPROC)
	struct hwsem_be *be = (struct hwsem_be *)state->be_data;
	if (be == NULL) return ALP_ERR_NOT_READY;
	if (!be->held) return ALP_ERR_INVAL;
	k_sem_give(&alp_hwsem_kobjs[be->hwsem_id]);
	be->held = false;
	return ALP_OK;
#else
	(void)state;
	return ALP_ERR_NOSUPPORT;
#endif
}

static void z_hwsem_close(alp_hwsem_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_MPROC)
	struct hwsem_be *be = (struct hwsem_be *)state->be_data;
	if (be == NULL) return;
	/* If still held when closed, give the kobj back so the next opener
     * can take it -- not the caller's bug to leak the lock. */
	if (be->held) {
		k_sem_give(&alp_hwsem_kobjs[be->hwsem_id]);
		be->held = false;
	}
	_hwsem_be_free(be);
	state->be_data = NULL;
#else
	(void)state;
#endif
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const alp_mproc_ops_t _ops = {
	.shmem_open        = z_shmem_open,
	.shmem_view        = z_shmem_view,
	.shmem_close       = z_shmem_close,
	.mbox_open         = z_mbox_open,
	.mbox_send         = z_mbox_send,
	.mbox_set_callback = z_mbox_set_callback,
	.mbox_close        = z_mbox_close,
	.hwsem_open        = z_hwsem_open,
	.hwsem_try_lock    = z_hwsem_try_lock,
	.hwsem_lock        = z_hwsem_lock,
	.hwsem_unlock      = z_hwsem_unlock,
	.hwsem_close       = z_hwsem_close,
};

ALP_BACKEND_REGISTER(mproc,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
