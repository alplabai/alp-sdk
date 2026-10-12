/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rpmsg_link -- OpenAMP/RPMsg plumbing for the micro-ROS transport.
 *
 * The bring-up half of this file (metal device, resource table copy, MHU
 * doorbell, vdev creation) is the BENCH-PROVEN sequence from
 * ../../rpmsg-v2n/m33_sm/src/main.c (alp-sdk #683/#697/#2374), trimmed to
 * what a datagram pipe needs: no liveness beacon, no self-close test
 * harness, no echo.  It is duplicated on purpose rather than refactored
 * out of rpmsg-v2n so the proven example stays untouched; see README.md
 * ("Maintainer decisions") for the follow-up to share one platform file.
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
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <metal/device.h>
#include <openamp/open_amp.h>

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

static void mbox_rx_cb(const struct device *dev, mbox_channel_id_t ch, void *u, struct mbox_msg *m)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ch);
	ARG_UNUSED(u);
	ARG_UNUSED(m);
	k_sem_give(&doorbell_sem);
}

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
	return rpmsg_virtio_get_rpmsg_device(&rvdev);

failed:
	LOG_ERR("vring / rpmsg vdev init failed");
	rproc_virtio_remove_vdev(vdev);
	return NULL;
}

/* Manager thread: bring the vdev up, create the endpoint, then turn every
 * A55 doorbell into an rproc_virtio_notified() so ept_rx_cb() runs. */
static void link_task(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	rpdev = create_rpmsg_vdev();
	if (!rpdev) {
		return;
	}
	/* Create the endpoint BEFORE the receive loop so the A55's first frame is
	 * not dropped for want of a matching endpoint (rpmsg-v2n #697). */
	if (rpmsg_create_ept(
	        &ept, rpdev, "rpmsg-service-0", APP_EPT_ADDR, RPMSG_ADDR_ANY, ept_rx_cb, NULL)) {
		LOG_ERR("failed to create endpoint");
		return;
	}
	LOG_INF("rpmsg link up, waiting for the A55 bridge");
	for (;;) {
		if (k_sem_take(&doorbell_sem, K_FOREVER) == 0) {
			rproc_virtio_notified(rvdev.vdev, VRING1_ID);
		}
	}
}

int rpmsg_link_start(void)
{
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
	return rpmsg_send(&ept, frame, XRCE_METHOD_LEN + len) > 0 ? len : 0;
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
