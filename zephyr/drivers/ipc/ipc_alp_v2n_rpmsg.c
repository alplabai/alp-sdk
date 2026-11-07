/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ipc_service backend for the RZ/V2N / V2M Cortex-M33 system manager: the
 * CM33 is the OpenAMP remote of the Linux A55 master, so <alp/rpc.h>
 * (src/backends/rpc/zephyr_drv.c) runs on it unchanged against
 * src/backends/rpc/yocto_uio_drv.c.
 *
 * The transport is the one bench-proven by examples/multicore/rpmsg-v2n/m33_sm
 * (#683, #697, #2586): a vendor-shaped resource table copied into the `rsctbl`
 * DDR window, vring addresses from `vring_ctrl0/1`, the rpmsg buffer pool in
 * `chosen { zephyr,ipc_shm }`, the A55 -> CM33 doorbell on the MHU-B mbox and
 * the CM33 -> A55 doorbell as a direct register write.  The Alp AMP beacon
 * (alp_v2n_beacon.h) is published from device init so the A55 sees the CM33
 * alive before anything opens a channel; the attach epoch turns odd once the
 * virtio device is bound and even again after an A55-requested link reset.
 *
 * Endpoint slot i is created at rpmsg address 1024 + i (first free slot; a
 * freed slot is reused), with the peer address learned from the first frame the
 * A55 sends.  alp_rpc_config_t::src_ept / dst_ept do not reach this layer
 * (ipc_service has no such field), so the A55 must dial 1024 + slot.  `bound`
 * fires just before the first frame is delivered, i.e. once a send can
 * succeed; `unbound` fires when the A55 resets the link and `bound` again
 * after the next attach.  Callbacks run on the manager thread, never
 * concurrently, and sends are non-blocking (-ENOMEM when no TX buffer is
 * free).
 *
 * NOT bench-proven through <alp/rpc.h> yet -- see the changelog fragment for
 * the V2M bench steps.
 */

#define DT_DRV_COMPAT alp_ipc_v2n_rpmsg

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/ipc/ipc_service_backend.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <metal/device.h>
#include <openamp/open_amp.h>

#include "alp_v2n_beacon.h"
#include "resource_table.h"

LOG_MODULE_REGISTER(ipc_alp_v2n_rpmsg, CONFIG_LOG_DEFAULT_LEVEL);

#if DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) > 1
#error "only one alp,ipc-v2n-rpmsg instance: there is one A55 link"
#endif

#if !DT_HAS_CHOSEN(zephyr_ipc_shm)
#error "alp,ipc-v2n-rpmsg requires `chosen { zephyr,ipc_shm = ...; }` -- see the board dts"
#endif

#define SHM_NODE       DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE       DT_REG_SIZE(SHM_NODE)

#define RSC_TABLE_ADDR DT_REG_ADDR(DT_NODELABEL(rsctbl))

#define MAX_EPT   CONFIG_IPC_SERVICE_BACKEND_ALP_V2N_RPMSG_MAX_ENDPOINTS
#define EPT_ADDR0 (1024U)

#define MGR_STACK_SIZE CONFIG_IPC_SERVICE_BACKEND_ALP_V2N_RPMSG_STACK_SIZE
#define MGR_PRIO       K_PRIO_PREEMPT(1)

struct v2n_ept {
	struct rpmsg_endpoint     rept;
	const struct ipc_ept_cfg *cfg;  /* NULL = slot free */
	bool                      live; /* rpmsg endpoint exists */
	bool                      bound_pending;
	/* Teardown barrier (see v2n_deregister_endpoint()): `closing` stops new
	 * callbacks / sends from starting, `in_cb` / `tx` count the ones that
	 * did, and deregister waits for both to drain before the slot is reused.
	 * `cfg->priv` and the cfg itself belong to the caller, who frees them as
	 * soon as deregister returns. */
	atomic_t closing;
	atomic_t in_cb;
	atomic_t tx;
};

struct v2n_data {
	struct k_mutex               lock; /* recursive; slots, link state, vdev lifecycle only */
	struct k_sem                 kick; /* doorbell or endpoint change -> manager */
	struct v2n_ept               ept[MAX_EPT];
	struct metal_io_region       shm_io;
	struct metal_io_region       rsc_io;
	struct rpmsg_virtio_device   rvdev;
	struct rpmsg_virtio_shm_pool shpool;
	struct rpmsg_device         *rpdev;
	void                        *rsc_table;
	bool                         link_up;
	bool                         opened;
};

static const struct mbox_dt_spec rx_channel = MBOX_DT_SPEC_GET(DT_DRV_INST(0), rx);
static struct v2n_data           v2n;
static metal_phys_addr_t         shm_physmap = CM33_TO_A55_ADDR_NS(SHM_START_ADDR);
static metal_phys_addr_t         rsc_physmap = RSC_TABLE_ADDR;

K_THREAD_STACK_DEFINE(mgr_stack, MGR_STACK_SIZE);
static struct k_thread mgr_thread;

/* A55 -> CM33 doorbell: only wakes the manager, which finds the vring. */
static void
mbox_rx_cb(const struct device *dev, mbox_channel_id_t ch, void *user_data, struct mbox_msg *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ch);
	ARG_UNUSED(user_data);
	ARG_UNUSED(data);
	k_sem_give(&v2n.kick);
}

/* OpenAMP asks to ring the peer: the payload is already in shared memory. */
static int notify_a55(void *priv, uint32_t id)
{
	ARG_UNUSED(priv);
	ARG_UNUSED(id);
	alp_v2n_doorbell_ring();
	return 0;
}

static int
rpmsg_rx_cb(struct rpmsg_endpoint *rept, void *data, size_t len, uint32_t src, void *priv)
{
	ARG_UNUSED(src);
	ARG_UNUSED(priv);
	struct v2n_ept           *e = CONTAINER_OF(rept, struct v2n_ept, rept);
	const struct ipc_ept_cfg *cfg;

	/* Count in first, then look: deregister sets `closing` before it waits for
	 * `in_cb` to drain, so either we see `closing` and back out, or it sees
	 * our count and waits for the callback to return. */
	atomic_inc(&e->in_cb);
	cfg = e->cfg;
	if (atomic_get(&e->closing) != 0 || cfg == NULL) {
		atomic_dec(&e->in_cb);
		return RPMSG_SUCCESS;
	}
	/* The A55's first frame teaches the peer address; only now can a send
	 * succeed, so only now is the link "bound". */
	if (e->bound_pending && rept->dest_addr != RPMSG_ADDR_ANY) {
		e->bound_pending = false;
		if (cfg->cb.bound != NULL) {
			cfg->cb.bound(cfg->priv);
		}
		/* The callback may have deregistered this endpoint (on this thread
		 * deregister does not wait for us), after which `cfg` is the
		 * caller's to free: do not touch it again. */
		if (atomic_get(&e->closing) != 0 || e->cfg != cfg) {
			atomic_dec(&e->in_cb);
			return RPMSG_SUCCESS;
		}
	}
	if (cfg->cb.received != NULL) {
		cfg->cb.received(data, len, cfg->priv);
	}
	atomic_dec(&e->in_cb);
	return RPMSG_SUCCESS;
}

static void ept_create(struct v2n_data *d, unsigned int idx)
{
	struct v2n_ept *e = &d->ept[idx];

	if (rpmsg_create_ept(&e->rept,
	                     d->rpdev,
	                     e->cfg->name != NULL ? e->cfg->name : "alp-rpc",
	                     EPT_ADDR0 + idx,
	                     RPMSG_ADDR_ANY,
	                     rpmsg_rx_cb,
	                     NULL) != 0) {
		LOG_ERR("rpmsg_create_ept(%u) failed", idx);
		if (e->cfg->cb.error != NULL) {
			e->cfg->cb.error("rpmsg_create_ept failed", e->cfg->priv);
		}
		return;
	}
	e->live          = true;
	e->bound_pending = true;
}

static int vdev_up(struct v2n_data *d)
{
	struct fw_rsc_vdev_vring *vring_rsc;
	struct virtio_device     *vdev;
	int                       ret;

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE,
	                                VDEV_ID,
	                                rsc_table_to_vdev(d->rsc_table),
	                                &d->rsc_io,
	                                NULL,
	                                notify_a55,
	                                NULL);
	if (vdev == NULL) {
		LOG_ERR("failed to create vdev");
		return -ENOMEM;
	}

	/* ATTACH-mode link: the CM33 is already running, so wait here for the
	 * A55's remoteproc attach to publish its half of the handshake. */
	rproc_virtio_wait_remote_ready(vdev);

	vring_rsc = rsc_table_get_vring0(d->rsc_table);
	ret       = rproc_virtio_init_vring(vdev,
	                                    0,
	                                    vring_rsc->notifyid,
	                                    (void *)VRING_TX_ADDR_CM33,
	                                    &d->rsc_io,
	                                    vring_rsc->num,
	                                    vring_rsc->align);
	if (ret == 0) {
		vring_rsc = rsc_table_get_vring1(d->rsc_table);
		ret       = rproc_virtio_init_vring(vdev,
		                                    1,
		                                    vring_rsc->notifyid,
		                                    (void *)VRING_RX_ADDR_CM33,
		                                    &d->rsc_io,
		                                    vring_rsc->num,
		                                    vring_rsc->align);
	}
	if (ret != 0) {
		LOG_ERR("failed to init vrings: %d", ret);
		goto failed;
	}

	/* A VIRTIO_DEV_DEVICE never gets gfeatures set; rpmsg_init_vdev() asserts
	 * gfeatures == dfeatures, so accept every feature the A55 offers. */
	virtio_set_features(vdev, 0x1);

	rpmsg_virtio_init_shm_pool(&d->shpool, NULL, SHM_SIZE);
	ret = rpmsg_init_vdev(&d->rvdev, vdev, NULL, &d->shm_io, &d->shpool);
	if (ret != 0) {
		LOG_ERR("rpmsg_init_vdev failed: %d", ret);
		goto failed;
	}

	d->rpdev = rpmsg_virtio_get_rpmsg_device(&d->rvdev);
	return 0;

failed:
	rproc_virtio_remove_vdev(vdev);
	return -EIO;
}

/* Tear the virtio device down after an A55 reset.  vdev.status is NOT written:
 * the A55 already wrote 0, and a store from here could overwrite a DRIVER_OK it
 * has since set for its next attach. */
static void vdev_down(struct v2n_data *d)
{
	/* rpmsg_deinit_vdev() NULLs rvdev.vdev, so take it first. */
	struct virtio_device *vdev = d->rvdev.vdev;

	for (unsigned int i = 0; i < MAX_EPT; i++) {
		if (d->ept[i].live) {
			/* An empty name skips the ns destroy announcement, which would
			 * otherwise queue onto the ring the A55 is about to zero. */
			d->ept[i].rept.name[0] = '\0';
		}
	}
	rpmsg_deinit_vdev(&d->rvdev);
	rproc_virtio_remove_vdev(vdev);
	d->rpdev = NULL;
}

static bool wait_for_work(struct v2n_data *d)
{
	uint8_t status = 0;

	(void)k_sem_take(&d->kick, K_FOREVER);
	(void)virtio_get_status(d->rvdev.vdev, &status);
	/* The A55 asked for a link reset: vdev.status = 0 + kick, ahead of
	 * re-creating its driver and zeroing both vrings (#2586).  OpenAMP's
	 * rproc_virtio_notified() never looks at the status. */
	return (status & VIRTIO_CONFIG_STATUS_DRIVER_OK) != 0U;
}

static void manager(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	struct v2n_data *d = &v2n;

	/* One pass per A55 attach: the A55 zeroes both vrings whenever it
	 * re-attaches and OpenAMP keeps the ring indices in the virtqueues, so the
	 * virtio device is re-created every time. */
	for (;;) {
		if (vdev_up(d) != 0) {
			return;
		}
		alp_v2n_beacon_epoch_bump(); /* even -> odd: bound */

		k_mutex_lock(&d->lock, K_FOREVER);
		d->link_up = true;
		k_mutex_unlock(&d->lock);

		do {
			/* Endpoints registered since the last wake are created here, on
			 * this thread, so a (possibly blocking) ns announcement never
			 * runs on an application thread. */
			k_mutex_lock(&d->lock, K_FOREVER);
			for (unsigned int i = 0; i < MAX_EPT; i++) {
				if (d->ept[i].cfg != NULL && !d->ept[i].live &&
				    atomic_get(&d->ept[i].closing) == 0) {
					ept_create(d, i);
				}
			}
			k_mutex_unlock(&d->lock);
			/* rpmsg callbacks run here without `lock`: OpenAMP's own rdev
			 * lock serialises the rings, and a handler may send. */
			rproc_virtio_notified(d->rvdev.vdev, VRING1_ID);
		} while (wait_for_work(d));

		LOG_INF("A55 reset the link; waiting for the next attach");
		k_mutex_lock(&d->lock, K_FOREVER);
		d->link_up = false;
		k_mutex_unlock(&d->lock);
		/* Sends are non-blocking, so this drains in microseconds. */
		for (unsigned int i = 0; i < MAX_EPT; i++) {
			while (atomic_get(&d->ept[i].tx) != 0) {
				k_msleep(1);
			}
		}
		k_mutex_lock(&d->lock, K_FOREVER);
		for (unsigned int i = 0; i < MAX_EPT; i++) {
			struct v2n_ept *e = &d->ept[i];

			if (e->cfg != NULL && e->live && !e->bound_pending && e->cfg->cb.unbound != NULL) {
				e->cfg->cb.unbound(e->cfg->priv);
			}
		}
		vdev_down(d);
		for (unsigned int i = 0; i < MAX_EPT; i++) {
			d->ept[i].live          = false;
			d->ept[i].bound_pending = false;
		}
		k_mutex_unlock(&d->lock);
		alp_v2n_beacon_epoch_bump(); /* odd -> even: waiting */
	}
}

static int v2n_open_instance(const struct device *instance)
{
	ARG_UNUSED(instance);
	struct v2n_data         *d  = &v2n;
	struct metal_init_params mp = METAL_INIT_DEFAULTS;
	int                      rc;

	k_mutex_lock(&d->lock, K_FOREVER);
	if (d->opened) {
		k_mutex_unlock(&d->lock);
		return -EALREADY;
	}

	rc = metal_init(&mp);
	if (rc != 0) {
		k_mutex_unlock(&d->lock);
		return rc;
	}
	metal_io_init(&d->shm_io, (void *)SHM_START_ADDR, &shm_physmap, SHM_SIZE, -1, 0, NULL);
	metal_io_init(&d->rsc_io,
	              (void *)RSC_TABLE_ADDR,
	              &rsc_physmap,
	              sizeof(struct fw_resource_table),
	              -1,
	              0,
	              NULL);

	mbox_register_callback_dt(&rx_channel, mbox_rx_cb, NULL);
	rc = mbox_set_enabled_dt(&rx_channel, true);
	if (rc != 0) {
		metal_finish();
		k_mutex_unlock(&d->lock);
		return rc;
	}

	d->opened = true;
	k_mutex_unlock(&d->lock);

	k_thread_create(&mgr_thread,
	                mgr_stack,
	                K_THREAD_STACK_SIZEOF(mgr_stack),
	                manager,
	                NULL,
	                NULL,
	                NULL,
	                MGR_PRIO,
	                0,
	                K_NO_WAIT);
	k_thread_name_set(&mgr_thread, "alp_v2n_rpmsg");
	return 0;
}

static int
v2n_register_endpoint(const struct device *instance, void **token, const struct ipc_ept_cfg *cfg)
{
	ARG_UNUSED(instance);
	struct v2n_data *d   = &v2n;
	int              ret = -ENOMEM;

	k_mutex_lock(&d->lock, K_FOREVER);
	for (unsigned int i = 0; i < MAX_EPT; i++) {
		if (d->ept[i].cfg == NULL) {
			atomic_set(&d->ept[i].closing, 0);
			d->ept[i].cfg           = cfg;
			d->ept[i].bound_pending = false;
			*token                  = &d->ept[i];
			ret                     = 0;
			break;
		}
	}
	k_mutex_unlock(&d->lock);
	/* The manager creates the rpmsg endpoint; `bound` follows the A55's
	 * first frame. */
	k_sem_give(&d->kick);
	return ret;
}

/* Barrier: when this returns no callback or send on `token` is running or can
 * start, so the caller may free the cfg and its priv.  `lock` is NOT held
 * while waiting -- a callback may itself send, which takes it.  Called from the
 * manager thread (an endpoint closing itself or a sibling from inside a
 * callback) the in-callback wait is skipped: the only callback that could be
 * running is the caller's own, and waiting for it would deadlock; the caller
 * does not touch cfg/priv after deregister returns.
 *
 * Not covered by a native_sim test: it needs a live OpenAMP vdev + MHU-B, which
 * only the board tree provides.  The reasoning above is the check; the V2M
 * bench step is a close-while-receiving loop on the A55 side. */
static int v2n_deregister_endpoint(const struct device *instance, void *token)
{
	ARG_UNUSED(instance);
	struct v2n_data *d = &v2n;
	struct v2n_ept  *e = token;

	/* Under `lock` so a v2n_send() that already claimed (also under `lock`) is
	 * counted in `tx` before we start waiting, and later ones see `closing`. */
	k_mutex_lock(&d->lock, K_FOREVER);
	atomic_set(&e->closing, 1);
	k_mutex_unlock(&d->lock);
	while (atomic_get(&e->tx) != 0) {
		k_msleep(1); /* sends are non-blocking: microseconds */
	}
	if (k_current_get() != &mgr_thread) {
		while (atomic_get(&e->in_cb) != 0) {
			k_msleep(1);
		}
	}

	k_mutex_lock(&d->lock, K_FOREVER);
	if (e->live) {
		rpmsg_destroy_ept(&e->rept);
	}
	e->live          = false;
	e->bound_pending = false;
	e->cfg           = NULL;
	k_mutex_unlock(&d->lock);
	return 0;
}

static int v2n_send(const struct device *instance, void *token, const void *data, size_t len)
{
	ARG_UNUSED(instance);
	struct v2n_data *d   = &v2n;
	struct v2n_ept  *e   = token;
	int              ret = -EBUSY;

	/* `lock` is held only to claim the send, never across it: a send from an
	 * rpmsg callback runs on the manager thread, and a blocking wait for a TX
	 * buffer there would stall RX and link-reset handling.  The per-endpoint
	 * `tx` count lets deregister and the link-reset path wait for it. */
	k_mutex_lock(&d->lock, K_FOREVER);
	bool ok = d->link_up && e->live && atomic_get(&e->closing) == 0 &&
	          e->rept.dest_addr != RPMSG_ADDR_ANY;
	if (ok) {
		atomic_inc(&e->tx);
	}
	k_mutex_unlock(&d->lock);
	if (!ok) {
		return -EBUSY;
	}

	ret = rpmsg_trysend(&e->rept, data, len);
	atomic_dec(&e->tx);
	if (ret == RPMSG_ERR_NO_BUFF) {
		ret = -ENOMEM; /* A55 has not returned a TX buffer yet; retry later */
	} else if (ret == RPMSG_ERR_BUFF_SIZE) {
		ret = -EBADMSG;
	} else if (ret < 0) {
		ret = -EIO;
	}
	return ret;
}

static const struct ipc_service_backend v2n_ops = {
	.open_instance       = v2n_open_instance,
	.send                = v2n_send,
	.register_endpoint   = v2n_register_endpoint,
	.deregister_endpoint = v2n_deregister_endpoint,
};

/* Runs at boot, before anything opens a channel: the A55 refuses to attach
 * without the beacon, and the resource table has to be in DDR before it reads
 * the vdev entry.  The table lives in this image's SRAM (BL22 loads only the
 * SRAM raw-bin, so a section linked at the DDR address would never be loaded)
 * and is copied into the rsctbl window. */
static int v2n_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	struct v2n_data *d = &v2n;
	void            *tab;
	int              size;

	k_mutex_init(&d->lock);
	k_sem_init(&d->kick, 0, 1);

	/* No magic until the link can actually work. */
	if (!mbox_is_ready_dt(&rx_channel)) {
		LOG_ERR("MHU-B mailbox RX channel is not ready");
		return -ENODEV;
	}
	rsc_table_get(&tab, &size);
	memcpy((void *)RSC_TABLE_ADDR, tab, size);
	d->rsc_table = (void *)RSC_TABLE_ADDR;
	alp_v2n_beacon_start();
	return 0;
}

DEVICE_DT_INST_DEFINE(0,
                      v2n_init,
                      NULL,
                      NULL,
                      NULL,
                      POST_KERNEL,
                      CONFIG_IPC_SERVICE_REG_BACKEND_PRIORITY,
                      &v2n_ops);
