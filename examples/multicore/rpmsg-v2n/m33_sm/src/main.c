/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rpmsg-v2n / m33_sm -- Cortex-M33 OpenAMP rpmsg SLAVE endpoint.
 *
 * alp-sdk #683 "Path B": this is a from-scratch firmware, NOT the native_sim
 * placeholder this file used to be.  It proves the RAW OpenAMP transport --
 * resource table, vrings, mailbox doorbell, rpmsg endpoint -- against REAL
 * RZ/V2N devicetree + the Renesas MHU mailbox driver, so the A55/Linux side
 * (an OpenAMP `rpmsg_sample_client` over UIO) has a live peer to attach to.
 * BENCH-PROVEN on E1M-X V2N-M1 silicon (#697): attach + echo round-trip
 * (1/4/16/64 B) + the GHSA-xhm8 external concurrent-close all pass end-to-end.
 * The doorbell is asymmetric -- forward A55->M33 via R_MHU_NS5.MSG (M33 NVIC
 * IRQ 293), reverse M33->A55 via a CA55-routed MHU-B SWINT unit (12 -> GIC_SPI
 * 404); see mailbox_notify() below and yocto_uio_drv.c for the register map.
 *
 * Adapted near-verbatim from Renesas's own sample --
 * zephyr/samples/boards/renesas/openamp_linux_zephyr/src/main_remote.c --
 * which is itself the reference the RZ/V2L port of this exact link ships.
 * Nothing here reimplements OpenAMP/libmetal/the FSP MHU driver; it only
 * wires the CM33-side half of the protocol those libraries already
 * implement (`applying-the-alp-sdk-c-house-style` + the "ride over vendor,
 * don't rewrite vendor drivers" rule) to this board's devicetree.
 *
 * Deliberately bypasses <alp/rpc.h> (the SDK's portable framed-RPC-over-
 * OpenAMP surface, src/backends/rpc/zephyr_drv.c) for this phase: that
 * backend rides on Zephyr's `subsys/ipc/ipc_service` and its OWN generated
 * resource table, whereas the A55 side here is the Renesas Multi-OS
 * Package's `rpmsg_sample_client` + `platform_info.c` (license-gated, not
 * an alp-sdk component) -- fixed, vendor userspace tooling that dials a
 * resource table shaped EXACTLY like this file's resource_table.c, not
 * whatever `ipc_service` would generate.  Reconciling the two -- most
 * likely a v2n-specific `<alp/rpc.h>` backend once this raw transport is
 * bench-proven -- is follow-up, not this phase.
 *
 * Echo behaviour kept from the vendor sample on purpose: the A55 side's
 * `rpmsg_sample_client` sends a growing message and checks the SAME bytes
 * come back, so keeping the echo (rather than trimming it to
 * temperature-sample style) gives the Linux side a live, verifiable peer.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>

#include <metal/device.h>
#include <openamp/open_amp.h>

#include "resource_table.h"

LOG_MODULE_REGISTER(rpmsg_v2n_m33_sm, LOG_LEVEL_INF);

#define SHM_DEVICE_NAME "shm"

#if !DT_HAS_CHOSEN(zephyr_ipc_shm)
#error "rpmsg-v2n/m33_sm requires `chosen { zephyr,ipc_shm = ...; }` -- see the board overlay"
#endif

/* Endpoint address the A55 side's rpmsg_sample_client dials; matches the
 * vendor sample -- this is NOT the alp_rpc_config_t src_ept convention, see
 * the file header. */
#define APP_EPT_ADDR (1024)

/* Shared-memory geometry, read straight from the board overlay's
 * `chosen { zephyr,ipc_shm = &vring_shm0; }` -- see resource_table.h for
 * why the vring TX/RX addresses come from a DIFFERENT pair of nodes
 * (vring_ctrl0/1, allocated by the rpmsg MASTER). */
#define SHM_NODE       DT_CHOSEN(zephyr_ipc_shm)
#define SHM_START_ADDR DT_REG_ADDR(SHM_NODE)
#define SHM_SIZE       DT_REG_SIZE(SHM_NODE)

#define RSC_TABLE_ADDR DT_REG_ADDR(DT_NODELABEL(rsctbl))

/*
 * Liveness beacon (alp-sdk #683, address root-cause fix): the resource
 * table itself gets memcpy'd into RSC_TABLE_ADDR by platform_init() below,
 * so it owns the low end of the `rsctbl` region -- the beacon lives at the
 * TOP of the region instead, out of the resource table's way.  A Linux-side
 * `devmem 0x4F700FF0` (rsctbl's A55 alias) read after this app boots proves,
 * in one shot, that the M33 is alive, the DDR window is backed, and the
 * CM33<->A55 address translation in resource_table.h is correct -- and the
 * heartbeat word lets a re-read tell "alive" apart from "wrote once, then
 * faulted".
 */
#define RSCTBL_BEACON_MAGIC_OFFSET     (0xFF0)
#define RSCTBL_BEACON_VERSION_OFFSET   (0xFF4)
#define RSCTBL_BEACON_HEARTBEAT_OFFSET (0xFF8)

#define RSCTBL_BEACON_MAGIC   (0xA10D0683U) /* "Alp Lab, #683" -- arbitrary, just distinctive */
#define RSCTBL_BEACON_VERSION (1U)

#define APP_TASK_STACK_SIZE (1024)

K_THREAD_STACK_DEFINE(thread_mng_stack, APP_TASK_STACK_SIZE);
K_THREAD_STACK_DEFINE(thread_rp_client_stack, APP_TASK_STACK_SIZE);

static struct k_thread thread_mng_data;
static struct k_thread thread_rp_client_data;

/* `chosen { zephyr,ipc = &mbox_consumer; }` in the board overlay resolves
 * this pair to the MHU channel-1 TX / channel-0 RX halves declared there --
 * see the overlay's KNOWN GAP comment for why this link isn't bench-proven
 * yet. */
static const struct mbox_dt_spec tx_channel = MBOX_DT_SPEC_GET(DT_CHOSEN(zephyr_ipc), tx);
static const struct mbox_dt_spec rx_channel = MBOX_DT_SPEC_GET(DT_CHOSEN(zephyr_ipc), rx);

static metal_phys_addr_t shm_physmap = CM33_TO_A55_ADDR_NS(SHM_START_ADDR);
static metal_phys_addr_t rsc_physmap = RSC_TABLE_ADDR;

/* One libmetal "generic" device carrying two regions: the rpmsg shared
 * buffer pool (region 0) and the resource table (region 1).  Needs
 * METAL_MAX_DEVICE_REGIONS=2 (default is 1) -- set in this app's
 * CMakeLists.txt. */
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

struct rpmsg_rcv_msg {
	void  *data;
	size_t len;
};

static struct metal_io_region      *shm_io;
static struct rpmsg_virtio_shm_pool shpool;

static struct metal_io_region    *rsc_io;
static struct rpmsg_virtio_device rvdev;

static void                *rsc_table;
static struct rpmsg_device *rpdev;

static char                  rx_sc_msg[512];
static struct rpmsg_endpoint sc_ept;
static struct rpmsg_rcv_msg  sc_msg = { .data = rx_sc_msg };

static K_SEM_DEFINE(data_sem, 0, 1);
static K_SEM_DEFINE(data_sc_sem, 0, 1);

static volatile int finish;

/* Doorbell RX: the A55 side kicked our mailbox after touching a vring --
 * just wake the manager thread, which figures out which vring via
 * rproc_virtio_notified() below. */
static void platform_mbox_callback(const struct device *dev,
                                   mbox_channel_id_t    channel_id,
                                   void                *user_data,
                                   struct mbox_msg     *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(user_data);
	ARG_UNUSED(data);
	k_sem_give(&data_sem);
}

static int
rpmsg_recv_cs_callback(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv)
{
	ARG_UNUSED(ept);
	ARG_UNUSED(src);
	ARG_UNUSED(priv);
	memcpy(sc_msg.data, data, len);
	sc_msg.len = len;
	k_sem_give(&data_sc_sem);
	return RPMSG_SUCCESS;
}

static void receive_message(void)
{
	if (k_sem_take(&data_sem, K_FOREVER) == 0) {
		rproc_virtio_notified(rvdev.vdev, VRING1_ID);
	}
}

static void new_service_cb(struct rpmsg_device *rdev, const char *name, uint32_t src)
{
	ARG_UNUSED(rdev);
	ARG_UNUSED(src);
	LOG_INF("%s: message received from service %s", __func__, name);
}

/* OpenAMP calls this whenever a vring needs the peer's attention; on this
 * link that means "ring the MHU doorbell" (the actual payload is already
 * in shared memory -- the mailbox only ever carries a wakeup, never
 * data). */
/* #697 cycle 10 (GIC-measured): the A55 GIC only receives the MHU-B CA55-routed
 * SWINT units 12-15 (-> INTID 436-439 = GIC_SPI 404-407); the NS-channel RSP
 * interrupt the FSP/mbox send raises (R_MHU_NS5.RSP) routes to NO A55 GIC line,
 * so a reply sent that way is never delivered.  Ring the A55 by asserting SWINT
 * unit 12's SET directly instead.  (Forward A55->M33 is unaffected: it uses
 * R_MHU_NS5.MSG -> the M33's own NVIC IRQ 293.)  SWINT units live in the MHU-B
 * block at +0x800, 0x10 stride, STS/SET/CLR @ +0x00/04/08; unit 12 SET =
 * CM33 0x50480000 + 0x800 + 12*0x10 + 0x04. */
#define ALP_M33_MHU_SWINT12_SET (0x504808C4U)

int mailbox_notify(void *priv, uint32_t id)
{
	ARG_UNUSED(priv);
	ARG_UNUSED(id);
	*(volatile uint32_t *)ALP_M33_MHU_SWINT12_SET = 1U;
	return 0;
}

int platform_init(void)
{
	void                    *rsc_tab_addr;
	int                      rsc_size;
	struct metal_device     *device;
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	int                      status;

	status = metal_init(&metal_params);
	if (status) {
		LOG_ERR("metal_init failed: %d", status);
		return -1;
	}

	status = metal_register_generic_device(&shm_device);
	if (status) {
		LOG_ERR("failed to register shared memory: %d", status);
		return -1;
	}

	status = metal_device_open("generic", SHM_DEVICE_NAME, &device);
	if (status) {
		LOG_ERR("metal_device_open failed: %d", status);
		return -1;
	}

	/* Region 0: the rpmsg buffer pool, shared verbatim with the A55. */
	metal_io_init(&device->regions[0], (void *)SHM_START_ADDR, &shm_physmap, SHM_SIZE, -1, 0, NULL);
	shm_io = metal_device_io_region(device, 0);
	if (!shm_io) {
		LOG_ERR("failed to get shm_io region");
		return -1;
	}

	/* Region 1: our resource table -- copied into place at RSC_TABLE_ADDR
	 * (the `rsctbl` DT node) so the A55's remoteproc attach can read it
	 * back over the matching UIO device.  This copy is deliberate, not
	 * link-placement: BL22 loads only this app's SRAM raw-bin, so a
	 * `.resource_table` section linked AT the DDR rsctbl address (0x9F7xxxxx)
	 * would simply never be loaded.  Keeping the const table in the SRAM
	 * image (resource_table.c's `.resource_table` section) and memcpy'ing it
	 * here is what actually gets it into DDR (alp-sdk #683 address fix). */
	rsc_table_get(&rsc_tab_addr, &rsc_size);
	memcpy((void *)RSC_TABLE_ADDR, rsc_tab_addr, rsc_size);
	rsc_table = (struct fw_resource_table *)RSC_TABLE_ADDR;

	metal_io_init(&device->regions[1], (void *)RSC_TABLE_ADDR, &rsc_physmap, rsc_size, -1, 0, NULL);
	rsc_io = metal_device_io_region(device, 1);
	if (!rsc_io) {
		LOG_ERR("failed to get rsc_io region");
		return -1;
	}

	if (!mbox_is_ready_dt(&tx_channel)) {
		LOG_ERR("mailbox TX channel is not ready");
		return -1;
	}
	if (!mbox_is_ready_dt(&rx_channel)) {
		LOG_ERR("mailbox RX channel is not ready");
		return -1;
	}
	mbox_register_callback_dt(&rx_channel, platform_mbox_callback, NULL);
	status = mbox_set_enabled_dt(&rx_channel, true);
	if (status) {
		LOG_ERR("mbox_set_enabled_dt failed: %d", status);
		return -1;
	}

	return 0;
}

static void platform_deinit(void)
{
	mbox_set_enabled_dt(&rx_channel, false);
	metal_finish();
}

static void cleanup_system(void)
{
	struct fw_resource_table *rsc_tbl = (struct fw_resource_table *)RSC_TABLE_ADDR;

	rpmsg_deinit_vdev(&rvdev);
	rproc_virtio_remove_vdev(rvdev.vdev);
	/* The master doesn't always clear this on its own teardown path --
	 * clear it here so a re-attach doesn't see a stale "live" vdev. */
	rsc_tbl->vdev.status = 0;
}

struct rpmsg_device *platform_create_rpmsg_vdev(unsigned int vdev_index,
                                                unsigned int role,
                                                void (*rst_cb)(struct virtio_device *vdev),
                                                rpmsg_ns_bind_cb ns_cb)
{
	ARG_UNUSED(vdev_index);
	ARG_UNUSED(rst_cb);

	struct fw_rsc_vdev_vring *vring_rsc;
	struct virtio_device     *vdev;
	int                       ret;

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

	/* Block until the A55 side's remoteproc attach has published its own
	 * half of the handshake -- this is the "who goes first" resolution
	 * for an ATTACH-mode (not rproc-load) link: the M33 is already
	 * running (booted by BL22 before Linux even starts), so it waits
	 * here for Linux to catch up. */
	rproc_virtio_wait_remote_ready(vdev);

	vring_rsc = rsc_table_get_vring0(rsc_table);
	ret       = rproc_virtio_init_vring(vdev,
	                                    0,
	                                    vring_rsc->notifyid,
	                                    (void *)VRING_TX_ADDR_CM33,
	                                    rsc_io,
	                                    vring_rsc->num,
	                                    vring_rsc->align);
	if (ret) {
		LOG_ERR("failed to init vring 0: %d", ret);
		goto failed;
	}

	vring_rsc = rsc_table_get_vring1(rsc_table);
	ret       = rproc_virtio_init_vring(vdev,
	                                    1,
	                                    vring_rsc->notifyid,
	                                    (void *)VRING_RX_ADDR_CM33,
	                                    rsc_io,
	                                    vring_rsc->num,
	                                    vring_rsc->align);
	if (ret) {
		LOG_ERR("failed to init vring 1: %d", ret);
		goto failed;
	}

	/* rproc_virtio_create_vdev() makes a VIRTIO_DEV_DEVICE (not
	 * _DRIVER), so gfeatures is never set for us; rpmsg_init_vdev()
	 * below asserts gfeatures == dfeatures, so set it by hand -- we
	 * accept every remote feature the A55 offers. */
	virtio_set_features(vdev, 0x1);

	rpmsg_virtio_init_shm_pool(&shpool, NULL, SHM_SIZE);
	ret = rpmsg_init_vdev(&rvdev, vdev, ns_cb, shm_io, &shpool);
	if (ret) {
		LOG_ERR("rpmsg_init_vdev failed: %d", ret);
		goto failed;
	}

	return rpmsg_virtio_get_rpmsg_device(&rvdev);

failed:
	rproc_virtio_remove_vdev(vdev);
	return NULL;
}

/* Echoes every message it receives back to the same endpoint address --
 * lets the A55's rpmsg_sample_client validate the round trip byte-for-byte
 * (see the file header for why the echo is kept, not trimmed). */
static void app_rpmsg_client_sample(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	k_sem_take(&data_sc_sem, K_FOREVER);
	LOG_INF("rpmsg-v2n/m33_sm: Linux responder started");

	rpmsg_create_ept(&sc_ept,
	                 rpdev,
	                 "rpmsg-service-0",
	                 APP_EPT_ADDR,
	                 RPMSG_ADDR_ANY,
	                 rpmsg_recv_cs_callback,
	                 NULL);

	while (!finish) {
		k_sem_take(&data_sc_sem, K_FOREVER);
		rpmsg_send(&sc_ept, sc_msg.data, sc_msg.len);
	}

	rpmsg_destroy_ept(&sc_ept);
	k_sem_reset(&data_sc_sem);
	LOG_INF("rpmsg-v2n/m33_sm: Linux responder ended");
}

static void rpmsg_mng_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	LOG_INF("rpmsg-v2n/m33_sm: bringing up the rpmsg virtio device");

	rpdev = platform_create_rpmsg_vdev(0, VIRTIO_DEV_DEVICE, NULL, new_service_cb);
	if (!rpdev) {
		LOG_ERR("failed to create rpmsg virtio device");
		goto task_end;
	}

	k_sem_give(&data_sc_sem);
	while (!finish) {
		receive_message();
	}

task_end:
	cleanup_system();
	LOG_INF("rpmsg-v2n/m33_sm: demo ended");
}

/* Writes the one-shot magic+version half of the beacon -- see the
 * RSCTBL_BEACON_* macros' header comment. */
static void rsctbl_beacon_publish(void)
{
	volatile uint32_t *magic = (volatile uint32_t *)(RSC_TABLE_ADDR + RSCTBL_BEACON_MAGIC_OFFSET);
	volatile uint32_t *version =
	    (volatile uint32_t *)(RSC_TABLE_ADDR + RSCTBL_BEACON_VERSION_OFFSET);

	*magic   = RSCTBL_BEACON_MAGIC;
	*version = RSCTBL_BEACON_VERSION;
	/* Make the writes visible to the A55 side before anything else runs. */
	barrier_dsync_fence_full();
}

/* ~1 Hz heartbeat -- see the RSCTBL_BEACON_* macros' header comment. */
static uint32_t rsctbl_heartbeat_count;

static void rsctbl_heartbeat_expiry(struct k_timer *timer)
{
	volatile uint32_t *heartbeat =
	    (volatile uint32_t *)(RSC_TABLE_ADDR + RSCTBL_BEACON_HEARTBEAT_OFFSET);

	ARG_UNUSED(timer);
	rsctbl_heartbeat_count++;
	*heartbeat = rsctbl_heartbeat_count;
	barrier_dsync_fence_full();
}

K_TIMER_DEFINE(rsctbl_heartbeat_timer, rsctbl_heartbeat_expiry, NULL);

int main(void)
{
	LOG_INF("rpmsg-v2n/m33_sm: starting (alp-sdk #683, Path B Phase 1)");

	/* CRITICAL: the liveness beacon is the VERY FIRST action, ahead of
	 * OpenAMP/libmetal/driver init below -- so a bench `devmem` read that
	 * catches this firmware mid-fault still sees "alive, once" rather than
	 * nothing at all. */
	rsctbl_beacon_publish();
	k_timer_start(&rsctbl_heartbeat_timer, K_SECONDS(1), K_SECONDS(1));

	if (platform_init() != 0) {
		LOG_ERR("platform_init failed");
		return -1;
	}

	finish = 0;
	k_thread_create(&thread_mng_data,
	                thread_mng_stack,
	                APP_TASK_STACK_SIZE,
	                (k_thread_entry_t)rpmsg_mng_task,
	                NULL,
	                NULL,
	                NULL,
	                K_PRIO_COOP(8),
	                0,
	                K_NO_WAIT);
	k_thread_create(&thread_rp_client_data,
	                thread_rp_client_stack,
	                APP_TASK_STACK_SIZE,
	                (k_thread_entry_t)app_rpmsg_client_sample,
	                NULL,
	                NULL,
	                NULL,
	                K_PRIO_COOP(7),
	                0,
	                K_NO_WAIT);

	k_thread_join(&thread_mng_data, K_FOREVER);
	k_thread_join(&thread_rp_client_data, K_FOREVER);

	platform_deinit();
	return 0;
}
