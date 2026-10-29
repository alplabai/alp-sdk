/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rpmsg_link -- OpenAMP/RPMsg plumbing for the micro-ROS transport.
 *
 * The bring-up half of this file (metal device, resource table copy, MHU
 * doorbell, vdev creation) is the BENCH-PROVEN sequence from
 * ../../rpmsg-v2n/m33_sm/src/main.c (alp-sdk #683/#697/#2374), trimmed to
 * what a datagram pipe needs: no self-close test harness, no echo.  The
 * AMP liveness beacon and the attach-reset handshake are kept: the A55 side
 * (yocto_uio_drv.c) requires both (see "Attach protocol" below).  It is
 * duplicated on purpose rather than refactored out of rpmsg-v2n so the
 * proven example stays untouched; see README.md ("Maintainer decisions")
 * for the follow-up to share one platform file.
 * The M33 endpoint address (1024) and the doorbell asymmetry are explained
 * in that file's header and are not repeated here.
 *
 * What this file adds is the datagram layer.  The A55 side is
 * <alp/rpc.h> (yocto_uio_drv.c), which frames every RPMsg payload as
 * `<method>\0<bytes>`.  We carry XRCE datagrams under the method "xrce"
 * and treat any other method (the bridge sends "hello") purely as a
 * "peer is alive" signal:
 *
 *     A55 bridge                              M33 (this file)
 *     ----------                              ---------------
 *     "hello\0"        ------------------->   rpmsg learns the A55 address
 *                                             from the frame source;
 *                                             peer_sem fires
 *     "xrce\0<XRCE>"   <------------------>   queued -> rpmsg_link_recv()
 *                                             rpmsg_link_send() -> "xrce\0<XRCE>"
 *
 * Attach protocol (same as ../../rpmsg-v2n/m33_sm/src/main.c, #683/#2586)
 *   The A55 backend (src/backends/rpc/yocto_uio_drv.c, uio_attach_reset())
 *   runs on EVERY alp_rpc_open().  It reads the beacon at the top of the
 *   rsctbl page (<alp/protocol/amp_beacon.h>) and:
 *     - no magic            -> fails the open with ALP_ERR_NOT_READY;
 *     - epoch ODD (bound)   -> writes vdev.status = 0, kicks, and waits for
 *                              EVEN, then zeroes both vrings;
 *     - epoch EVEN          -> proceeds straight to zeroing the vrings.
 *   So this firmware must (1) publish the beacon first thing, (2) bump the
 *   epoch to ODD once the virtio device is bound, and (3) on a doorbell that
 *   finds DRIVER_OK clear, tear the vdev down, bump the epoch back to EVEN
 *   and build a fresh vdev -- the A55 zeroes the vrings, and OpenAMP keeps
 *   its ring indices in the virtqueue objects, so reusing the old vdev would
 *   desync the second bridge run in one CM33 boot.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>

#include <metal/device.h>
#include <openamp/open_amp.h>

#include <alp/protocol/amp_beacon.h>

#include "resource_table.h"
#include "rpmsg_link.h"

LOG_MODULE_REGISTER(rpmsg_link, LOG_LEVEL_INF);

#define SHM_DEVICE_NAME "shm"

#if !DT_HAS_CHOSEN(zephyr_ipc_shm)
#error \
    "microros-ros2-v2n/m33_sm requires `chosen { zephyr,ipc_shm = ...; }` -- see the board overlay"
#endif

/* Fixed endpoint address the A55 bridge dials -- same constant as
 * rpmsg-v2n (APP_EPT_ADDR) so alp_rpc_open()'s dst_ept matches. */
#define APP_EPT_ADDR (1024)

#define SHM_NODE       DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE       DT_REG_SIZE(SHM_NODE)
#define RSC_TABLE_ADDR DT_REG_ADDR(DT_NODELABEL(rsctbl))

/* CM33 -> A55 doorbell: MHU-B SWINT unit 12 SET (see rpmsg-v2n main.c). */
#define ALP_M33_MHU_SWINT12_SET (0x504808C4U)

/* Beacon version 2 = attach reset supported (ALP_AMP_BEACON_VERSION_ATTACH_ACK);
 * the A55 backend refuses a re-attach below that.  The beacon is the top 16
 * bytes of the rsctbl page, out of the way of the resource table memcpy'd to
 * its low end by platform_init(). */
#define RSCTBL_BEACON_VERSION ALP_AMP_BEACON_VERSION_ATTACH_ACK
#define RSCTBL_BEACON         ALP_AMP_BEACON_AT(RSC_TABLE_ADDR, DT_REG_SIZE(DT_NODELABEL(rsctbl)))

/* <alp/rpc.h> method that carries XRCE datagrams; the length includes the NUL. */
#define XRCE_METHOD     "xrce"
#define XRCE_METHOD_LEN (sizeof(XRCE_METHOD))

#define LINK_STACK_SIZE 2048
K_THREAD_STACK_DEFINE(link_stack, LINK_STACK_SIZE);
static struct k_thread link_thread;

static const struct mbox_dt_spec tx_channel = MBOX_DT_SPEC_GET(DT_CHOSEN(zephyr_ipc), tx);
static const struct mbox_dt_spec rx_channel = MBOX_DT_SPEC_GET(DT_CHOSEN(zephyr_ipc), rx);

static metal_phys_addr_t shm_physmap = CM33_TO_A55_ADDR_NS(SHM_START_ADDR);
static metal_phys_addr_t rsc_physmap = RSC_TABLE_ADDR;

struct metal_device shm_device = {
	.name        = SHM_DEVICE_NAME,
	.num_regions = 2,
	{
	    { .virt = NULL }, /* shared memory */
	    { .virt = NULL }, /* rsc_table memory */
	},
	.node    = { NULL },
	.irq_num = 0,
};

static struct metal_io_region      *shm_io;
static struct rpmsg_virtio_shm_pool shpool;
static struct metal_io_region      *rsc_io;
static struct rpmsg_virtio_device   rvdev;
static void                        *rsc_table;
static struct rpmsg_device         *rpdev;
static struct rpmsg_endpoint        ept;

/* One received XRCE datagram, method header already stripped. */
struct datagram {
	uint16_t len;
	uint8_t  data[RPMSG_LINK_MTU];
};
K_MSGQ_DEFINE(rx_q, sizeof(struct datagram), 8, 4);

static K_SEM_DEFINE(doorbell_sem, 0, 1); /* A55 kicked the mailbox */
static K_SEM_DEFINE(peer_sem, 0, 1);     /* first frame from the A55 seen */

/* Set while link_reset() tears the link down: rpmsg_link_send() refuses to
 * touch the endpoint of a vdev that is being removed. */
static atomic_t link_resetting;

/* Serialises the sender against teardown.  rpmsg_link_send() holds it across
 * the (non-blocking) send; link_reset() takes it before rpmsg_deinit_vdev(),
 * so a publish already in flight drains first and a later one sees
 * link_resetting under the lock.  Priority inheritance lifts the preempted
 * sender above link_task while link_task waits. */
static K_MUTEX_DEFINE(send_lock);

static void mbox_rx_cb(const struct device *dev, mbox_channel_id_t ch, void *u, struct mbox_msg *m)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ch);
	ARG_UNUSED(u);
	ARG_UNUSED(m);
	k_sem_give(&doorbell_sem);
}

/* Writes the one-shot half of the beacon.  Magic goes last: the A55 trusts
 * epoch and version only once it reads the magic, and the page survives a
 * reset of this core, so every boot rewrites all of it. */
static void beacon_publish(void)
{
	RSCTBL_BEACON->heartbeat    = 0U;
	RSCTBL_BEACON->attach_epoch = 0U;
	RSCTBL_BEACON->version      = RSCTBL_BEACON_VERSION;
	barrier_dsync_fence_full();
	RSCTBL_BEACON->magic = ALP_AMP_BEACON_MAGIC;
	barrier_dsync_fence_full();
}

/* Flips the attach epoch's parity: even -> odd once the virtio device is
 * bound, odd -> even once it is gone (the A55 waits for that EVEN). */
static void beacon_epoch_bump(void)
{
	barrier_dsync_fence_full();
	RSCTBL_BEACON->attach_epoch = RSCTBL_BEACON->attach_epoch + 1U;
	barrier_dsync_fence_full();
}

/* ~1 Hz heartbeat: growth between two A55 reads = this firmware is alive. */
static uint32_t heartbeat_count;

static void heartbeat_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	heartbeat_count++;
	RSCTBL_BEACON->heartbeat = heartbeat_count;
	barrier_dsync_fence_full();
}

K_TIMER_DEFINE(heartbeat_timer, heartbeat_expiry, NULL);

/* Runs on the manager thread (inside rproc_virtio_notified). */
static int ept_rx_cb(struct rpmsg_endpoint *e, void *data, size_t len, uint32_t src, void *priv)
{
	ARG_UNUSED(e);
	ARG_UNUSED(src);
	ARG_UNUSED(priv);

	/* Any frame proves the A55 is there; open-amp has already recorded its
	 * address as our reply destination (we created the ept with RPMSG_ADDR_ANY). */
	k_sem_give(&peer_sem);

	if (len <= XRCE_METHOD_LEN || memcmp(data, XRCE_METHOD, XRCE_METHOD_LEN) != 0) {
		return RPMSG_SUCCESS; /* "hello" or anything else: liveness only */
	}

	struct datagram d;
	d.len = (uint16_t)(len - XRCE_METHOD_LEN);
	if (d.len > sizeof(d.data)) {
		return RPMSG_SUCCESS; /* cannot happen: rpmsg buffers are 496 B */
	}
	memcpy(d.data, (const uint8_t *)data + XRCE_METHOD_LEN, d.len);
	/* Never block the OpenAMP RX path; XRCE reliable streams retransmit. */
	if (k_msgq_put(&rx_q, &d, K_NO_WAIT) != 0) {
		LOG_WRN("rx queue full, dropped %u B datagram", d.len);
	}
	return RPMSG_SUCCESS;
}

int mailbox_notify(void *priv, uint32_t id)
{
	ARG_UNUSED(priv);
	ARG_UNUSED(id);
	*(volatile uint32_t *)ALP_M33_MHU_SWINT12_SET = 1U;
	return 0;
}

static int platform_init(void)
{
	void                    *rsc_tab_addr;
	int                      rsc_size;
	struct metal_device     *device;
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;

	if (metal_init(&metal_params) || metal_register_generic_device(&shm_device) ||
	    metal_device_open("generic", SHM_DEVICE_NAME, &device)) {
		LOG_ERR("libmetal init failed");
		return -1;
	}

	metal_io_init(&device->regions[0], (void *)SHM_START_ADDR, &shm_physmap, SHM_SIZE, -1, 0, NULL);
	shm_io = metal_device_io_region(device, 0);

	/* Copy the resource table into the DDR window the A55 attaches to
	 * (BL22 only loads the SRAM image; see rpmsg-v2n main.c). */
	rsc_table_get(&rsc_tab_addr, &rsc_size);
	memcpy((void *)RSC_TABLE_ADDR, rsc_tab_addr, rsc_size);
	rsc_table = (struct fw_resource_table *)RSC_TABLE_ADDR;
	metal_io_init(&device->regions[1], (void *)RSC_TABLE_ADDR, &rsc_physmap, rsc_size, -1, 0, NULL);
	rsc_io = metal_device_io_region(device, 1);
	if (!shm_io || !rsc_io) {
		LOG_ERR("failed to get libmetal io regions");
		return -1;
	}

	if (!mbox_is_ready_dt(&tx_channel) || !mbox_is_ready_dt(&rx_channel)) {
		LOG_ERR("mailbox channels not ready");
		return -1;
	}
	mbox_register_callback_dt(&rx_channel, mbox_rx_cb, NULL);
	if (mbox_set_enabled_dt(&rx_channel, true)) {
		LOG_ERR("mbox_set_enabled_dt failed");
		return -1;
	}
	return 0;
}

static struct rpmsg_device *create_rpmsg_vdev(void)
{
	struct fw_rsc_vdev_vring *vring_rsc;
	struct virtio_device     *vdev;

	vdev = rproc_virtio_create_vdev(VIRTIO_DEV_DEVICE,
	                                VDEV_ID,
	                                rsc_table_to_vdev(rsc_table),
	                                rsc_io,
	                                NULL,
	                                mailbox_notify,
	                                NULL);
	if (!vdev) {
		LOG_ERR("failed to create vdev");
		return NULL;
	}

	/* ATTACH mode: this M33 is already running, wait for Linux to catch up. */
	rproc_virtio_wait_remote_ready(vdev);

	vring_rsc = rsc_table_get_vring0(rsc_table);
	if (rproc_virtio_init_vring(vdev,
	                            0,
	                            vring_rsc->notifyid,
	                            (void *)VRING_TX_ADDR_CM33,
	                            rsc_io,
	                            vring_rsc->num,
	                            vring_rsc->align)) {
		goto failed;
	}
	vring_rsc = rsc_table_get_vring1(rsc_table);
	if (rproc_virtio_init_vring(vdev,
	                            1,
	                            vring_rsc->notifyid,
	                            (void *)VRING_RX_ADDR_CM33,
	                            rsc_io,
	                            vring_rsc->num,
	                            vring_rsc->align)) {
		goto failed;
	}

	/* VIRTIO_DEV_DEVICE never sets gfeatures; rpmsg_init_vdev() asserts them
	 * equal, so accept every feature the A55 offers. */
	virtio_set_features(vdev, 0x1);
	rpmsg_virtio_init_shm_pool(&shpool, NULL, SHM_SIZE);
	if (rpmsg_init_vdev(&rvdev, vdev, NULL, shm_io, &shpool)) {
		goto failed;
	}
	beacon_epoch_bump(); /* even -> odd: bound */
	return rpmsg_virtio_get_rpmsg_device(&rvdev);

failed:
	LOG_ERR("vring / rpmsg vdev init failed");
	rproc_virtio_remove_vdev(vdev);
	return NULL;
}

/* The A55 asked for a link reset (it wrote vdev.status = 0 and kicked, ahead
 * of re-creating its virtio driver and zeroing both vrings): tear the virtio
 * device down and acknowledge with an EVEN epoch so the next attach starts
 * from fresh ring indices on both sides. */
static void link_reset(void)
{
	/* rpmsg_deinit_vdev() NULLs rvdev.vdev, so take it first or
	 * rproc_virtio_remove_vdev() frees nothing. */
	struct virtio_device *vdev = rvdev.vdev;

	k_mutex_lock(&send_lock, K_FOREVER);
	atomic_set(&link_resetting, 1);
	/* An empty name skips the name-service destroy announcement
	 * rpmsg_destroy_ept() would queue onto the old A55 ring (the A55 is
	 * about to zero it). */
	ept.name[0] = '\0';
	rpmsg_deinit_vdev(&rvdev);
	rproc_virtio_remove_vdev(vdev);
	k_mutex_unlock(&send_lock);
	/* vdev.status is deliberately NOT written: the A55 already wrote 0, and a
	 * store from this side could overwrite a DRIVER_OK it has since set. */

	/* Datagrams and the peer token belong to the old session; the new bridge
	 * says "hello" again.  A stale doorbell would be read against the new vdev. */
	k_msgq_purge(&rx_q);
	k_sem_reset(&peer_sem);
	k_sem_reset(&doorbell_sem);
	atomic_set(&link_resetting, 0);
	beacon_epoch_bump(); /* odd -> even: waiting; the A55 may proceed */
}

/* Manager thread: one pass per A55 attach.  Bring the vdev up, create the
 * endpoint, then turn every A55 doorbell into an rproc_virtio_notified() so
 * ept_rx_cb() runs, until the A55 resets the link. */
static void link_task(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		rpdev = create_rpmsg_vdev();
		if (!rpdev) {
			return;
		}
		/* Create the endpoint BEFORE the receive loop so the A55's first frame
		 * is not dropped for want of a matching endpoint (rpmsg-v2n #697). */
		if (rpmsg_create_ept(
		        &ept, rpdev, "rpmsg-service-0", APP_EPT_ADDR, RPMSG_ADDR_ANY, ept_rx_cb, NULL)) {
			LOG_ERR("failed to create endpoint");
			link_reset(); /* odd -> even: no longer bound */
			return;
		}
		LOG_INF("rpmsg link up, waiting for the A55 bridge");
		for (;;) {
			if (k_sem_take(&doorbell_sem, K_FOREVER) != 0) {
				continue;
			}
			/* No DRIVER_OK = the A55 is resetting the link ahead of zeroing the
			 * vrings.  rproc_virtio_notified() never looks at the status, so check
			 * here, before it walks a ring the A55 is about to reset. */
			uint8_t status = 0;
			(void)virtio_get_status(rvdev.vdev, &status);
			if ((status & VIRTIO_CONFIG_STATUS_DRIVER_OK) == 0U) {
				break;
			}
			rproc_virtio_notified(rvdev.vdev, VRING1_ID);
		}
		LOG_INF("A55 reset the link; waiting for the next attach");
		link_reset();
	}
}

int rpmsg_link_start(void)
{
	/* First action, ahead of OpenAMP/libmetal init: the A55 backend refuses to
	 * open without the magic, and a bench read that catches this firmware
	 * mid-fault still sees "alive, once". */
	beacon_publish();
	k_timer_start(&heartbeat_timer, K_SECONDS(1), K_SECONDS(1));

	if (platform_init() != 0) {
		return -1;
	}
	k_thread_create(&link_thread,
	                link_stack,
	                LINK_STACK_SIZE,
	                link_task,
	                NULL,
	                NULL,
	                NULL,
	                K_PRIO_COOP(8),
	                0,
	                K_NO_WAIT);
	return 0;
}

int rpmsg_link_wait_peer(int32_t timeout_ms)
{
	k_timeout_t t = timeout_ms < 0 ? K_FOREVER : K_MSEC(timeout_ms);

	if (k_sem_take(&peer_sem, t) != 0) {
		return -1;
	}
	/* Put the token back: once the peer is known it stays known (limit 1),
	 * so re-opening the transport after an XRCE session reset returns at once. */
	k_sem_give(&peer_sem);
	return 0;
}

size_t rpmsg_link_send(const uint8_t *data, size_t len)
{
	uint8_t frame[XRCE_METHOD_LEN + RPMSG_LINK_MTU];

	if (len == 0 || len > RPMSG_LINK_MTU) {
		return 0;
	}
	memcpy(frame, XRCE_METHOD, XRCE_METHOD_LEN);
	memcpy(frame + XRCE_METHOD_LEN, data, len);

	/* rpmsg_trysend() never sleeps for a TX buffer, so the lock is held only
	 * briefly and link_reset() is never stalled past the A55 attach ack
	 * timeout.  Back-pressure is waited out here, outside the lock. */
	for (int tries = 0; tries < 1500; tries++) {
		int rc;

		k_mutex_lock(&send_lock, K_FOREVER);
		if (atomic_get(&link_resetting) != 0) {
			k_mutex_unlock(&send_lock);
			return 0;
		}
		rc = rpmsg_trysend(&ept, frame, XRCE_METHOD_LEN + len);
		k_mutex_unlock(&send_lock);
		if (rc > 0) {
			return len;
		}
		if (rc != RPMSG_ERR_NO_BUFF) {
			return 0;
		}
		k_msleep(10);
	}
	return 0;
}

size_t rpmsg_link_recv(uint8_t *buf, size_t cap, int timeout_ms)
{
	struct datagram d;

	if (k_msgq_get(&rx_q, &d, timeout_ms < 0 ? K_FOREVER : K_MSEC(timeout_ms)) != 0) {
		return 0;
	}
	if (d.len > cap) {
		LOG_WRN("datagram %u B > read buffer %u B, dropped", d.len, (unsigned)cap);
		return 0;
	}
	memcpy(buf, d.data, d.len);
	return d.len;
}
