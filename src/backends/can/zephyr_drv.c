/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr can_* driver-class backend.  Used on any SoC
 * unless a vendor-specific backend registers a more specific
 * silicon_ref match.  Pooling lives in src/can_dispatch.c; the
 * backend's open resolves the alp-canN DT alias, configures the
 * controller's bitrate + mode, and reserves a per-handle Zephyr
 * sidecar for RX-callback dispatch.
 *
 * The portable handle in src/backends/can/can_ops.h carries no
 * Zephyr types -- the RX-callback table needs the slot index to be
 * carried as user_data into the can_add_rx_filter trampoline, which
 * cannot live on the portable handle without dragging
 * <zephyr/drivers/can.h> into translation units that should stay
 * backend-agnostic.  The sidecar (alp_z_can_side_t) holds the
 * MAX_FILTERS cb_table; state->be_data carries the per-handle
 * pointer set at open() time.  The sidecar pool is sized by
 * CONFIG_ALP_SDK_MAX_CAN_HANDLES so every active dispatcher slot
 * has a matching sidecar.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/can.h>
#include <alp/soc_caps.h>

#include "can_ops.h"

#define ALP_CAN_DEV_OR_NULL(idx)                                                                   \
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_can, idx))),                                   \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_can, idx)))),                                  \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_CAN_DEV_OR_NULL(0), ALP_CAN_DEV_OR_NULL(1), ALP_CAN_DEV_OR_NULL(2),
	ALP_CAN_DEV_OR_NULL(3), ALP_CAN_DEV_OR_NULL(4), ALP_CAN_DEV_OR_NULL(5),
};

#ifndef CONFIG_ALP_SDK_MAX_CAN_HANDLES
#define CONFIG_ALP_SDK_MAX_CAN_HANDLES 4
#endif

#define MAX_FILTERS 16

typedef struct {
	alp_can_rx_cb_t cb;
	void           *user;
} cb_ctx_t;

/* Per-handle Zephyr sidecar.  Holds the MAX_FILTERS-sized cb_table so
 * the can_add_rx_filter trampoline can dispatch from its user_data
 * (slot index) back into the caller's alp_can_rx_cb_t.  state->be_data
 * carries the per-handle pointer set at open() time. */
typedef struct {
	cb_ctx_t cb_table[MAX_FILTERS];
	bool     in_use;
} alp_z_can_side_t;

static alp_z_can_side_t _sides[CONFIG_ALP_SDK_MAX_CAN_HANDLES];

static alp_z_can_side_t *_alloc_side(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_sides); ++i) {
		if (!_sides[i].in_use) {
			_sides[i]        = (alp_z_can_side_t){ 0 };
			_sides[i].in_use = true;
			return &_sides[i];
		}
	}
	return NULL;
}

static void _free_side(alp_z_can_side_t *s)
{
	if (s != NULL) s->in_use = false;
}

static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -EAGAIN:
	case -ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case -EIO:
		return ALP_ERR_IO;
	case -ENOTSUP:
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	case -ENOMEM:
		return ALP_ERR_NOMEM;
	default:
		return ALP_ERR_IO;
	}
}

/* RX trampoline: the slot index travels in user_data so the
 * trampoline can look up the caller's cb + user.  The sidecar
 * pointer is recovered by walking back through the alp_can_t
 * dispatcher handle the slot lives on -- but the trampoline only
 * receives the slot index, not the handle, so we encode the
 * sidecar + slot in a single pointer below. */
typedef struct {
	alp_z_can_side_t *side;
	uint8_t           slot;
} trampoline_key_t;

/* Trampoline keys are allocated alongside their cb_table slot.  We
 * keep one per (sidecar, slot) so the encoded user_data pointer is
 * stable for the lifetime of the filter. */
static trampoline_key_t _keys[CONFIG_ALP_SDK_MAX_CAN_HANDLES][MAX_FILTERS];

static void _rx_trampoline(const struct device *dev, struct can_frame *frame, void *user_data)
{
	(void)dev;
	trampoline_key_t *key = (trampoline_key_t *)user_data;
	if (key == NULL || key->side == NULL) return;
	if (key->slot >= MAX_FILTERS) return;
	cb_ctx_t *ctx = &key->side->cb_table[key->slot];
	if (ctx->cb == NULL) return;

	alp_can_frame_t out = {
		.id     = frame->id,
		.ext_id = (frame->flags & CAN_FRAME_IDE) != 0,
		.rtr    = (frame->flags & CAN_FRAME_RTR) != 0,
		.fd     = (frame->flags & CAN_FRAME_FDF) != 0,
		.brs    = (frame->flags & CAN_FRAME_BRS) != 0,
		.dlc    = can_dlc_to_bytes(frame->dlc),
	};
	if (out.dlc > sizeof out.data) out.dlc = sizeof out.data;
	memcpy(out.data, frame->data, out.dlc);
	ctx->cb(&out, ctx->user);
}

static size_t _side_index(alp_z_can_side_t *s)
{
	return (size_t)(s - &_sides[0]);
}

static alp_status_t
z_open(const alp_can_config_t *cfg, alp_can_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg->bus_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
	if (cfg->bus_id >= ALP_SOC_CAN_COUNT) return ALP_ERR_OUT_OF_RANGE;
	if (cfg->mode == ALP_CAN_MODE_FD && !ALP_SOC_CAN_FD_SUPPORTED) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	const struct device *dev = _devs[cfg->bus_id];
	if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;

	alp_z_can_side_t *s = _alloc_side();
	if (s == NULL) return ALP_ERR_NOMEM;

	int err = can_set_bitrate(dev, cfg->bitrate_nominal_hz);
	if (err != 0) {
		_free_side(s);
		return _errno_to_alp(err);
	}

	if (cfg->mode == ALP_CAN_MODE_FD && cfg->bitrate_data_hz > 0u) {
#if defined(CONFIG_CAN_FD_MODE)
		err = can_set_bitrate_data(dev, cfg->bitrate_data_hz);
		if (err != 0) {
			_free_side(s);
			return _errno_to_alp(err);
		}
		err = can_set_mode(dev, CAN_MODE_FD | (cfg->loopback ? CAN_MODE_LOOPBACK : 0));
#else
		err = -ENOTSUP;
#endif
	} else {
		err = can_set_mode(dev, cfg->loopback ? CAN_MODE_LOOPBACK : CAN_MODE_NORMAL);
	}
	if (err != 0) {
		_free_side(s);
		return _errno_to_alp(err);
	}

	st->dev         = (void *)dev;
	st->bus_id      = cfg->bus_id;
	st->be_data     = s;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t z_start(alp_can_backend_state_t *st)
{
	const struct device *dev = (const struct device *)st->dev;
	return _errno_to_alp(can_start(dev));
}

static alp_status_t z_stop(alp_can_backend_state_t *st)
{
	const struct device *dev = (const struct device *)st->dev;
	return _errno_to_alp(can_stop(dev));
}

static alp_status_t
z_send(alp_can_backend_state_t *st, const alp_can_frame_t *frame, uint32_t timeout_ms)
{
	const struct device *dev = (const struct device *)st->dev;
	struct can_frame     zf  = {
		.id    = frame->id,
		.dlc   = can_bytes_to_dlc(frame->dlc),
		.flags = (frame->ext_id ? CAN_FRAME_IDE : 0) | (frame->rtr ? CAN_FRAME_RTR : 0) |
		         (frame->fd ? CAN_FRAME_FDF : 0) | (frame->brs ? CAN_FRAME_BRS : 0),
	};
	memcpy(zf.data, frame->data, frame->dlc);
	return _errno_to_alp(can_send(dev, &zf, K_MSEC(timeout_ms), NULL, NULL));
}

static alp_status_t z_add_filter(alp_can_backend_state_t *st,
                                 const alp_can_filter_t  *filter,
                                 alp_can_rx_cb_t          cb,
                                 void                    *user,
                                 int32_t                 *filter_id_out)
{
	alp_z_can_side_t    *s   = (alp_z_can_side_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;
	if (s == NULL || dev == NULL) return ALP_ERR_NOT_READY;

	/* Reserve a slot in cb_table; encode (side, slot) into the
     * trampoline key so the ISR thunk can dispatch back into cb. */
	int slot = -1;
	for (int i = 0; i < MAX_FILTERS; ++i) {
		if (s->cb_table[i].cb == NULL) {
			slot = i;
			break;
		}
	}
	if (slot < 0) return ALP_ERR_NOMEM;
	s->cb_table[slot].cb   = cb;
	s->cb_table[slot].user = user;

	size_t            side_idx = _side_index(s);
	trampoline_key_t *key      = &_keys[side_idx][slot];
	key->side                  = s;
	key->slot                  = (uint8_t)slot;

	struct can_filter zf = {
		.id    = filter->id,
		.mask  = filter->mask,
		.flags = filter->ext_id ? CAN_FILTER_IDE : 0,
	};
	int fid = can_add_rx_filter(dev, _rx_trampoline, key, &zf);
	if (fid < 0) {
		s->cb_table[slot] = (cb_ctx_t){ 0 };
		return _errno_to_alp(fid);
	}
	if (filter_id_out != NULL) *filter_id_out = fid;
	return ALP_OK;
}

static alp_status_t z_remove_filter(alp_can_backend_state_t *st, int32_t filter_id)
{
	const struct device *dev = (const struct device *)st->dev;
	if (dev == NULL) return ALP_ERR_NOT_READY;
	can_remove_rx_filter(dev, filter_id);
	/* The slot lookup-by-fid is best-effort; when zephyr's filter
     * id is opaque the matching cb_table entry stays -- leak is
     * bounded by MAX_FILTERS and the v0.3 OpenAMP-friendly rewrite
     * gives this a proper id->slot map. */
	return ALP_OK;
}

static void z_close(alp_can_backend_state_t *st)
{
	alp_z_can_side_t    *s   = (alp_z_can_side_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;
	struct alp_can      *h   = CONTAINER_OF(st, struct alp_can, state);

	/* If the dispatcher latched started=true without a matching stop,
     * issue can_stop before releasing the sidecar so the controller
     * doesn't reference a stale RX dispatch context. */
	if (h->started && dev != NULL) {
		(void)can_stop(dev);
	}
	if (s != NULL) _free_side(s);
	st->be_data = NULL;
}

static const alp_can_ops_t _ops = {
	.open          = z_open,
	.start         = z_start,
	.stop          = z_stop,
	.send          = z_send,
	.add_filter    = z_add_filter,
	.remove_filter = z_remove_filter,
	.close         = z_close,
};

ALP_BACKEND_REGISTER(can,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
