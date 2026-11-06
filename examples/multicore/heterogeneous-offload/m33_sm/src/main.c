/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * heterogeneous-offload / m33_sm -- Cortex-M33-SM CMSIS-DSP FFT worker.
 *
 * Serves the `fft` method of the A55's <alp/rpc.h> alp_rpc_call() over the
 * RAW OpenAMP transport, NOT through <alp/rpc.h> on this side.  The generic
 * Zephyr backend (src/backends/rpc/zephyr_drv.c) rides on ipc_service with its
 * own generated resource table; it publishes no AMP beacon and has no V2N
 * bring-up, so the A55 backend (src/backends/rpc/yocto_uio_drv.c) refuses to
 * talk to it.  The bench-proven bring-up of ../../rpmsg-v2n/m33_sm/src/main.c
 * is copied here instead: resource table in DDR, MHU doorbell, the rpmsg
 * endpoint at the fixed address APP_EPT_ADDR, and the version-2 AMP beacon
 * (<alp/protocol/amp_beacon.h>) with its attach-epoch handshake.  Only the
 * responder differs: rpmsg-v2n echoes, this file runs the FFT.
 * resource_table.[ch] are NOT copied: CMakeLists.txt builds rpmsg-v2n's.
 * The transport code duplicated here (and in rpmsg-v2n and microros) is meant
 * to go away once the generic Zephyr RPC backend serves V2N: tracked in #3067.
 *
 * Wire contract (the A55 requester in ../linux/src/main.c agrees):
 *   frame    : <method>\0<payload>   (include/alp/rpc.h framing)
 *   request  : "fft\0" + 64 x float32 (256 B)
 *   response : "fft\0" + 33 x float32 (132 B)  -- same method name; the
 *              A55 alp_rpc_call() matches its reply by method name.
 * Both frames fit one 496 B RPMsg buffer (512 B minus the 16 B header).  A
 * 1024-point FFT (4096 B) would not; split it into several calls.
 */

#include <math.h>
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
#include <alp/protocol/v2n_mhu_doorbell.h>

#include "resource_table.h"

LOG_MODULE_REGISTER(hetero_offload_m33_sm, LOG_LEVEL_INF);

#define SHM_DEVICE_NAME "shm"

#if !DT_HAS_CHOSEN(zephyr_ipc_shm)
#error "m33_sm requires `chosen { zephyr,ipc_shm = ...; }` (see the board overlay)"
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
 * `devmem` of the beacon's A55 alias (see README.md) read after this app boots proves,
 * in one shot, that the M33 is alive, the DDR window is backed, and the
 * CM33<->A55 address translation in resource_table.h is correct -- and the
 * heartbeat word lets a re-read tell "alive" apart from "wrote once, then
 * faulted".
 *
 * Version 2 (#2586) adds the attach-epoch word (end-0x04 of the rsctbl page), which publishes
 * whether this firmware is bound to an A55 session: ODD = bound (bumped
 * after rpmsg_init_vdev() returned), EVEN = waiting for an attach (starts
 * at 0, bumped again after cleanup_system() on an A55 attach reset; see
 * rpmsg_link_reset()).  The A55 backend (src/backends/rpc/yocto_uio_drv.c,
 * uio_attach_reset()) requests a reset whenever it reads ODD, whatever
 * vdev.status says, and waits for EVEN.  It refuses a re-attach outright
 * when the version is below 2.
 */
#define RSCTBL_BEACON_VERSION (2U)

/* The beacon: the top 16 bytes of the rsctbl page, laid out by
 * <alp/protocol/amp_beacon.h> (shared with the A55 backend and the stock
 * shim).  DT_REG_SIZE of `rsctbl` is that page's size, from the SoC
 * metadata via the generated board .dts. */
#define RSCTBL_BEACON ALP_AMP_BEACON_AT(RSC_TABLE_ADDR, DT_REG_SIZE(DT_NODELABEL(rsctbl)))

/* 3072 (rpmsg-v2n uses 2048): the responder thread holds a struct sc_frame
 * (~520 B) plus the 136 B reply and calls the CMSIS FFT and rpmsg_send(),
 * whose OpenAMP call chain is deep; 1 KiB of margin over rpmsg-v2n. */
#define APP_TASK_STACK_SIZE (3072)

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

static struct metal_io_region      *shm_io;
static struct rpmsg_virtio_shm_pool shpool;

static struct metal_io_region    *rsc_io;
static struct rpmsg_virtio_device rvdev;

static void                *rsc_table;
static struct rpmsg_device *rpdev;

static struct rpmsg_endpoint sc_ept;

#define FFT_METHOD     "fft"
#define FFT_METHOD_LEN (sizeof(FFT_METHOD)) /* includes the NUL */

/* CMSIS-DSP pulled in via the `cmsis_dsp` library knob.  Available
 * under both Zephyr (cmsis-dsp module) and the alp-sdk wrappers. */
#if __has_include("arm_math.h")
#include "arm_math.h"
#else
#error "CMSIS-DSP required (board.yaml cmsis-dsp library; build with generated/alp.conf)"
#endif

#define FFT_LEN  64u
#define MAGS_LEN (FFT_LEN / 2u + 1u)

static float g_samples[FFT_LEN];
static float g_complex[FFT_LEN * 2u]; /* CMSIS interleaved scratch */
static float g_mags[MAGS_LEN];

/* Run a single FFT + magnitude pass on `g_samples`. */
static void run_fft(void)
{
	arm_rfft_fast_instance_f32 fft;
	/* The fixed-size init links only the 64-point tables. The generic
	 * arm_rfft_fast_init_f32() keeps every table up to 4096 points (~79 KB)
	 * and pushes the image past the 0x30000 CM33 boot slot. */
	BUILD_ASSERT(FFT_LEN == 64u, "arm_rfft_fast_init_64_f32 needs FFT_LEN 64");
	arm_rfft_fast_init_64_f32(&fft);
	arm_rfft_fast_f32(&fft, g_samples, g_complex, /*ifft=*/0);

	/* CMSIS packs DC + Nyquist into bin 0 (DC.real, Nyquist.real).
     * We split them into separate bins for the response. */
	g_mags[0]            = fabsf(g_complex[0]);
	g_mags[FFT_LEN / 2u] = fabsf(g_complex[1]);
	for (uint32_t i = 1; i < FFT_LEN / 2u; ++i) {
		const float r  = g_complex[2u * i];
		const float im = g_complex[2u * i + 1u];
		g_mags[i]      = sqrtf(r * r + im * im);
	}
}

/* #707: echo RX frames via a QUEUE, not a single buffer + binary sem.  The old
 * single sc_msg buffer + K_SEM(...,1) dropped the second of two rapid frames --
 * the RX callback overwrote sc_msg before the echo thread ran -- so the A55
 * GHSA-xhm8 self-close test (a fast "close_me" trigger + a "slow_..." blocked
 * call, back to back) never opened its race window on silicon (#697 cycle 13).
 * The queue echoes both, in order. */
struct sc_frame {
	size_t len;
	char   data[512];
};
K_MSGQ_DEFINE(sc_frame_q, sizeof(struct sc_frame), 8, 4);

static K_SEM_DEFINE(data_sem, 0, 1);
/* data_sc_sem: "sc_ept is up, start echoing" handoff from the manager thread
 * (which creates sc_ept itself, before its receive loop) to the responder
 * thread, once per A55 attach; per-frame delivery goes through sc_frame_q
 * above.  responder_stopped_sem is the reverse handoff on an attach reset. */
static K_SEM_DEFINE(data_sc_sem, 0, 1);
static K_SEM_DEFINE(responder_stopped_sem, 0, 1);

/* sc_frame.len value that tells the responder to stop (#2586). */
#define SC_FRAME_STOP ((size_t)-1)

/* Set while rpmsg_link_reset() tears the link down: the responder drops a
 * frame it was still holding instead of sending it onto the old vring. */
static atomic_t link_resetting;

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
	struct sc_frame f;
	f.len = len < sizeof(f.data) ? len : sizeof(f.data);
	memcpy(f.data, data, f.len);
	/* Never block the OpenAMP RX path.  If the queue is full the frame is
	 * dropped WITHOUT a reply (rpmsg_send() from this callback could stall
	 * on a TX buffer); the A55 serialises calls, so at most one request is
	 * in flight and this cannot happen in normal use. */
	(void)k_msgq_put(&sc_frame_q, &f, K_NO_WAIT);
	return RPMSG_SUCCESS;
}

/* Returns false when the A55 asked for a link reset: it wrote vdev.status = 0
 * (no DRIVER_OK) and kicked, ahead of re-creating its virtio driver and
 * zeroing both vrings (#2586).  OpenAMP's rproc_virtio_notified() never looks
 * at the status, so the check has to happen here, before it walks a ring the
 * A55 is about to reset. */
static bool receive_message(void)
{
	if (k_sem_take(&data_sem, K_FOREVER) != 0) {
		return true;
	}
	uint8_t status = 0;
	(void)virtio_get_status(rvdev.vdev, &status);
	if ((status & VIRTIO_CONFIG_STATUS_DRIVER_OK) == 0U) {
		return false;
	}
	rproc_virtio_notified(rvdev.vdev, VRING1_ID);
	return true;
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
 * SWINT units 12-15 (-> INTID 436-439 = GIC_SPI 404-407); the RSP interrupt of
 * NS slot 5, which the FSP/mbox send raises, routes to NO A55 GIC line, so a
 * reply sent that way is never delivered.  Ring the A55 by writing the
 * doorbell's SET register directly instead.  (Forward A55->M33 is unaffected:
 * it uses R_MHU_NS5.MSG -> the M33's own NVIC IRQ 293.)  Which register that
 * is -- SWINT unit 12 SET (default) or RSP_INT_SET of NS slot 8 with
 * CONFIG_ALP_V2N_DOORBELL_RSP_CH8 -- and its offset come from
 * <alp/protocol/v2n_mhu_doorbell.h>; the A55 must be built for the same line. */
#define ALP_M33_DOORBELL_SET (ALP_V2N_MHU_B_CM33_BASE + ALP_V2N_DOORBELL_SET_OFF)

int mailbox_notify(void *priv, uint32_t id)
{
	ARG_UNUSED(priv);
	ARG_UNUSED(id);
	*(volatile uint32_t *)ALP_M33_DOORBELL_SET = 1U;
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

static void rsctbl_attach_epoch_bump(void);

static void cleanup_system(void)
{
	/* rpmsg_deinit_vdev() NULLs rvdev.vdev, so take it first or
	 * rproc_virtio_remove_vdev() frees nothing (#2586: this path now runs
	 * once per A55 re-attach, so the leak would add up). */
	struct virtio_device *vdev = rvdev.vdev;

	rpmsg_deinit_vdev(&rvdev);
	rproc_virtio_remove_vdev(vdev);
	/* vdev.status is deliberately NOT written here: on the reset path the
	 * A55 already wrote 0 before kicking, and a store from this side could
	 * overwrite a DRIVER_OK the A55 has since set for its next attach. */
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

	rsctbl_attach_epoch_bump(); /* even -> odd: bound */
	return rpmsg_virtio_get_rpmsg_device(&rvdev);

failed:
	rproc_virtio_remove_vdev(vdev);
	return NULL;
}

/* Serves "fft" requests: <method>\0 + 64 float32 in, <method>\0 + 33 float32
 * out.  Runs on this thread, not in the RX callback, so the OpenAMP RX path
 * is never blocked by the transform. */
static void app_rpmsg_client_sample(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	/* One pass per A55 attach; see rpmsg_mng_task() for the handoff. */
	for (;;) {
		k_sem_take(&data_sc_sem, K_FOREVER);
		LOG_INF("heterogeneous-offload/m33_sm: FFT worker started");

		for (;;) {
			struct sc_frame f;
			if (k_msgq_get(&sc_frame_q, &f, K_FOREVER) != 0) {
				continue;
			}
			if (f.len == SC_FRAME_STOP) {
				break;
			}
			if (f.len != FFT_METHOD_LEN + sizeof g_samples ||
			    memcmp(f.data, FFT_METHOD, FFT_METHOD_LEN) != 0) {
				/* Dropped without a reply, so the A55 call times out. An empty
				 * "fft" reply here stopped the image from booting on E1M-V2M103
				 * (layout-sensitive, root cause not found: #3068), so it is not
				 * sent. */
				LOG_WRN("dropped %u B frame (not an fft request)", (unsigned)f.len);
				continue;
			}
			memcpy(g_samples, f.data + FFT_METHOD_LEN, sizeof g_samples);
			run_fft();
			if (atomic_get(&link_resetting) != 0) {
				continue; /* the A55 is resetting the link: drop it */
			}
			uint8_t reply[FFT_METHOD_LEN + sizeof g_mags];
			memcpy(reply, FFT_METHOD, FFT_METHOD_LEN);
			memcpy(reply + FFT_METHOD_LEN, g_mags, sizeof g_mags);
			if (rpmsg_send(&sc_ept, reply, sizeof reply) < 0) {
				LOG_ERR("rpmsg_send failed");
			}
		}

		LOG_INF("heterogeneous-offload/m33_sm: FFT worker stopped");
		k_sem_give(&responder_stopped_sem);
	}
}

/* The A55 asked for a link reset (#2586): stop the responder, drop queued
 * frames, tear the virtio device down and acknowledge, so the next attach
 * starts from fresh ring indices on both sides. */
static void rpmsg_link_reset(void)
{
	struct sc_frame stop = { .len = SC_FRAME_STOP };

	atomic_set(&link_resetting, 1);
	k_msgq_purge(&sc_frame_q);
	(void)k_msgq_put(&sc_frame_q, &stop, K_FOREVER);
	k_sem_take(&responder_stopped_sem, K_FOREVER);

	/* An empty name skips the name-service destroy announcement
	 * rpmsg_destroy_ept() would otherwise queue onto the old A55 ring (the
	 * A55 is about to zero it); with no free TX buffer there rpmsg_send()
	 * busy-waits up to 15 s and the A55 gives up on the ack. */
	sc_ept.name[0] = '\0';
	cleanup_system();
	atomic_set(&link_resetting, 0);
	rsctbl_attach_epoch_bump(); /* odd -> even: waiting; the A55 may proceed */
}

static void rpmsg_mng_task(void *arg1, void *arg2, void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	/* One pass per A55 attach (#2586): the virtio device and endpoint are
	 * re-created after every attach reset, because the A55 zeroes both vrings
	 * whenever it re-attaches and OpenAMP keeps the ring indices in the
	 * virtqueue objects. */
	for (;;) {
		LOG_INF("heterogeneous-offload/m33_sm: bringing up the rpmsg virtio device");

		rpdev = platform_create_rpmsg_vdev(0, VIRTIO_DEV_DEVICE, NULL, new_service_cb);
		if (!rpdev) {
			LOG_ERR("failed to create rpmsg virtio device");
			return;
		}

		/* Create the responder endpoint HERE -- before the receive_message()
		 * loop -- not in the responder thread.  The A55's first request sits on
		 * the RX vring until receive_message() processes it below; if sc_ept
		 * doesn't exist yet the rpmsg RX path drops that frame for want of a
		 * matching endpoint, and the A55's first alp_rpc_call() times out
		 * (#697: echo[1-byte] on a cold attach).  The old code created sc_ept
		 * in app_rpmsg_client_sample() via data_sc_sem, which raced this loop
		 * and lost.  Creating it inline, before the loop starts, guarantees
		 * the endpoint is live for the very first frame. */
		if (rpmsg_create_ept(&sc_ept,
		                     rpdev,
		                     "rpmsg-service-0",
		                     APP_EPT_ADDR,
		                     RPMSG_ADDR_ANY,
		                     rpmsg_recv_cs_callback,
		                     NULL) != 0) {
			LOG_ERR("failed to create responder endpoint");
			cleanup_system();
			rsctbl_attach_epoch_bump(); /* odd -> even: no longer bound */
			return;
		}

		/* Release the responder thread's echo loop now that sc_ept exists. */
		k_sem_give(&data_sc_sem);
		while (receive_message()) {
		}

		LOG_INF("heterogeneous-offload/m33_sm: A55 reset the link; waiting for the next attach");
		rpmsg_link_reset();
	}
}

/* Writes the one-shot magic+version half of the beacon -- see the
 * header comment above. */
static void rsctbl_beacon_publish(void)
{
	RSCTBL_BEACON->attach_epoch = 0U;
	RSCTBL_BEACON->version      = RSCTBL_BEACON_VERSION;
	/* Magic last: the A55 trusts epoch + version only once it reads the magic. */
	barrier_dsync_fence_full();
	RSCTBL_BEACON->magic = ALP_AMP_BEACON_MAGIC;
	barrier_dsync_fence_full();
}

/* Flips the attach epoch's parity (even <-> odd).  Called after the virtio
 * device is bound (-> odd) and after it is gone (-> even). */
static void rsctbl_attach_epoch_bump(void)
{
	barrier_dsync_fence_full();
	RSCTBL_BEACON->attach_epoch = RSCTBL_BEACON->attach_epoch + 1U;
	barrier_dsync_fence_full();
}

/* ~1 Hz heartbeat -- see the header comment above. */
static uint32_t rsctbl_heartbeat_count;

static void rsctbl_heartbeat_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	rsctbl_heartbeat_count++;
	RSCTBL_BEACON->heartbeat = rsctbl_heartbeat_count;
	barrier_dsync_fence_full();
}

K_TIMER_DEFINE(rsctbl_heartbeat_timer, rsctbl_heartbeat_expiry, NULL);

int main(void)
{
	LOG_INF("heterogeneous-offload/m33_sm: starting");

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
