/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * RZ/V2N A55 rpc_* driver-class backend (alp-sdk #683 Path B).  Binds
 * the alp_rpc dispatcher's ops vtable to userspace open-amp / libmetal
 * (UIO, ATTACH mode) instead of src/backends/rpc/yocto_drv.c's
 * `/dev/rpmsg*` chardev surface -- RZ/V2N's Yocto BSP does not carry
 * the mainline `rpmsg_char` remoteproc glue, only the raw generic-UIO
 * regions a userspace OpenAMP master can attach to directly (see
 * scratchpad/683/REFERENCE.md, this backend's design contract).
 *
 * Registered at priority 150 with an EXACT silicon_ref
 * ("renesas:rzv2n:n44", ALP_SOC_REF_STR on this SoC -- see
 * include/alp/soc_caps.h) so it outranks yocto_drv.c's
 * silicon_ref="*"/priority=100 registration on V2N builds only; every
 * other Linux target (i.MX93, DEEPX DX-M1, ...) keeps using the
 * `/dev/rpmsg*` backend unchanged (mirrors the exact pattern
 * src/backends/camera/v2n_n44_isp.c already uses to override
 * src/backends/camera/zephyr_video.c on this one SoC -- see that
 * file's header comment).
 *
 * @par Attach flow (REFERENCE.md Sec. 1 memory map + Sec. 5 attach flow)
 * The M33 (examples/multicore/rpmsg-v2n/m33_sm) boots standalone via
 * BL22 and publishes a stock OpenAMP `fw_resource_table` (one VDEV,
 * VIRTIO_ID_RPMSG, 2 vrings) at a fixed physical address, then creates
 * its own "rpmsg-service-0" endpoint and idles waiting for the host.
 * This backend is the userspace OpenAMP MASTER (VIRTIO_DEV_DRIVER
 * role) that ATTACHES to that already-running link -- no ELF load, no
 * `remoteproc_start()` -- by:
 *
 *   1. `metal_init()` (refcounted -- safe across repeated open/close)
 *      and `metal_device_open("platform", <uio-name>, &dev)` for each
 *      of the seven regions REFERENCE.md names (env-overridable, see
 *      `uio_dev_name()`): rsctbl, mhu-shm, vring-ctl0/1, vring-shm0/1,
 *      mhu-uio (the MHU-B register block, carrying the notification IRQ
 *      -- CORRECTED alp-sdk #683 from the prior "mbox-uio" ICU-page
 *      mapping, which had no SET register to actually kick with).
 *   2. `remoteproc_init(&ch->rproc, &_rproc_ops, ch)` -- the ONLY
 *      mandatory op this backend implements is `.notify` (the MHU
 *      doorbell kick, see `uio_rproc_notify()` below); every other
 *      `remoteproc_ops` slot (`init`/`remove`/`mmap`/`handle_rsc`/
 *      `config`/`start`/`stop`/`shutdown`/`get_mem`) is safely NULL
 *      because `remoteproc_add_mem()` below pre-registers every region
 *      this link ever needs, and `remoteproc_get_mem()`
 *      (lib/remoteproc/remoteproc.c, vendored on-host as a Zephyr
 *      module) checks that internal list BEFORE ever calling
 *      `ops->mmap`/`ops->get_mem` -- confirmed by reading that source
 *      directly rather than assuming the callback is required.
 *   3. `remoteproc_add_mem()` for rsctbl / vring-ctl0 / vring-ctl1 /
 *      vring-shm0 / vring-shm1, each with `da = pa + 0x50000000`
 *      (`ALP_V2N_A55_TO_M33_NS_OFFSET` -- CORRECTED alp-sdk #683 from
 *      0x20000000, the RZ/V2L offset this file was ported from; see
 *      that macro's doc comment) and the metal-mapped `io` region.
 *   4. `remoteproc_set_rsc_table()` pointed at the rsctbl UIO mapping
 *      (the M33 already wrote it there; this call PARSES, never
 *      writes, matching "attach, no load").
 *   5. `remoteproc_create_virtio(&ch->rproc, 0, VIRTIO_DEV_DRIVER,
 *      NULL)` -- reads the resource table's single `fw_rsc_vdev`
 *      entry, creates both vrings by looking up their `da` in the
 *      list from step 3 (again, no `ops->mmap` call needed).  For the
 *      DRIVER role `rproc_virtio_wait_remote_ready()`
 *      (lib/remoteproc/remoteproc_virtio.c) returns immediately
 *      (`VIRTIO_ROLE_IS_DRIVER` short-circuit) -- confirmed by reading
 *      that source; this call cannot block waiting on the M33.
 *   6. `rpmsg_virtio_init_shm_pool()` + `rpmsg_init_vdev()` (the plain,
 *      non-split-pool form) using vring-shm1 ("mst-alloc" in
 *      REFERENCE.md -- the master-owned buffer pool both directions'
 *      descriptors are drawn from) as the single shared buffer pool.
 *      **Documented assumption, bench-TBD**: vring-shm0 is opened and
 *      `remoteproc_add_mem()`-registered (so a future split-pool
 *      config can use it) but otherwise unused by this v1 -- the
 *      REFERENCE.md table lists it with a resource-table-known `da`
 *      distinct from vring-shm1's "mst-alloc" note, and confirming
 *      which pool the M33-side Zephyr OpenAMP subsystem actually
 *      expects the master to draw buffers from needs a real link, not
 *      just the stock `fw_resource_table` layout.
 *   7. The REFERENCE.md Sec. 5 "platform_info.c quirk" --
 *      `virtqueue_enable_cb(rvq)` right after `rpmsg_init_vdev()` --
 *      is applied here for a concrete, verified reason: reading
 *      lib/rpmsg/rpmsg_virtio.c's `rpmsg_init_vdev()` shows it disables
 *      only the TX-complete callback (`svq`) by default (busy-loop send
 *      is the intended send path) and otherwise leaves virtqueue
 *      callback state exactly as `virtio_create_virtqueues()` set it up
 *      -- for the DRIVER role the RX callback is armed unconditionally
 *      by that same call, so this explicit `virtqueue_enable_cb(rvq)`
 *      is a defensive no-op given the version vendored on-host, kept
 *      because REFERENCE.md calls it out explicitly as a needed step
 *      against the Renesas Multi-OS sample's `platform_info.c` (which
 *      this backend does NOT vendor -- see the file-scope note below).
 *   8. `rpmsg_create_ept(&ch->ept, rdev, cfg->name, cfg->src_ept,
 *      cfg->dst_ept, uio_ept_cb, NULL)` -- a FIXED local/remote address
 *      pair (same `fnv1a_32(name)`-derived defaults as
 *      src/backends/rpc/yocto_drv.c / zephyr_drv.c, so all three
 *      backends stay wire-compatible), NOT the dynamic
 *      name-service-announce (`ns_bind_cb`) path.  **Documented
 *      assumption, bench-TBD**: if the M33 sample's own
 *      `rpmsg_create_ept()` call instead relies on Zephyr's OpenAMP
 *      subsystem auto-announcing "rpmsg-service-0" via the NS channel
 *      (RPMSG_NS_EPT_ADDR=0x35), this fixed-address assumption needs
 *      to become an `ns_bind_cb`-driven bind once bench-validated
 *      against the real Phase-1 image.
 *
 * @par Notification transport (no thread of our own)
 * Unlike yocto_drv.c/zephyr_drv.c, this backend does NOT spawn or own
 * an rx thread/worker: libmetal's Linux system backend
 * (lib/system/linux/irq.c, vendored on-host as a Zephyr module) already
 * runs ONE process-wide poll()+read() thread that dispatches every
 * `metal_irq_register()`-ed UIO IRQ.  `uio_rproc_notify_isr()` below is
 * registered against the mhu-uio device's IRQ (`metal_irq_register`,
 * confirmed lock-free/reentrant-safe by reading that source: the
 * `linux_irq_cntr` controller's `irq_register` slot is NULL, so
 * registration/unregistration is a single plain array write, safely
 * callable from INSIDE the very callback it un-registers) and, on
 * every invocation, ACKs the MHU IRQ then calls
 * `remoteproc_get_notification(&ch->rproc, RSC_NOTIFY_ID_ANY)`, which
 * may invoke `uio_ept_cb()` (our rpmsg endpoint callback) synchronously,
 * zero or more times, on that SAME shared libmetal thread.
 *
 * @par Close protocol (GHSA-xhm8-7f87-93q5) -- SAME contract, shared-worker adaptation
 * The dispatcher (src/rpc_dispatch.c) still owns single-owner election
 * (one atomic CAS) and the active-op drain; this file's half of the
 * ops-vtable contract (see alp_rpc_ops_t in rpc_ops.h) mirrors
 * yocto_drv.c/zephyr_drv.c's `shutdown()`/`destroy()` split -- but
 * because libmetal's IRQ thread is a SHARED, per-process singleton
 * (potentially also dispatching OTHER, unrelated UIO IRQs, now or in a
 * future multi-channel build) rather than a thread THIS backend spawns
 * and owns outright, a bare thread-identity compare
 * (`pthread_equal(pthread_self(), rx_thread)`, yocto_drv.c's approach)
 * is not sufficient by itself -- see src/backends/rpc/zephyr_drv.c's
 * header comment for the identical "shared worker" defect (defect 3)
 * this design copies the fix for: `recv_active` (true ONLY while THIS
 * channel's own `uio_ept_cb()` is genuinely on some thread's call stack
 * right now) ANDed with a `recv_thread` identity compare, both read
 * under `call_mutex`.
 *
 *   - `y_shutdown()`: sets the sticky `closing` flag + cancels any
 *     pending call + broadcasts `call_cond` (identical to
 *     yocto_drv.c).  Detects self-close via `recv_active &&
 *     pthread_equal(pthread_self(), ch->recv_thread)` under
 *     `call_mutex`: external -> `metal_irq_unregister()` (safe, see
 *     above) then a bounded sleep-poll drain of `cb_active` (mirrors
 *     zephyr_drv.c's identical drain -- "sleep, never spin", see
 *     src/rpc_dispatch.c's `_rpc_drain()` doc comment) before
 *     returning `ALP_RPC_SHUTDOWN_DONE`; self -> returns
 *     `ALP_RPC_SHUTDOWN_DEFERRED` WITHOUT unregistering or draining
 *     (would deadlock waiting on its own `cb_active`).
 *   - `uio_ept_cb()`'s caller, `uio_rproc_notify_isr()`, completes the
 *     deferred teardown in its OWN epilogue -- once
 *     `remoteproc_get_notification()` returns and `cb_active` is
 *     decremented back out -- calling
 *     `alp_rpc_close_finalize(ch->owner)` exactly once, mirroring
 *     yocto_drv.c's `rpc_rx_main()` epilogue / zephyr_drv.c's
 *     `rpc_ept_recv()` epilogue.
 *   - `y_destroy()`: frees every open-amp/libmetal resource (in the
 *     order lib/rpmsg/rpmsg_virtio.c's `rpmsg_deinit_vdev()` /
 *     lib/remoteproc/remoteproc.c's `remoteproc_remove_virtio()`
 *     require -- `rpmsg_deinit_vdev()` NULLs `rvdev.vdev` as a side
 *     effect, so the heap-owning `virtio_device*` is captured into a
 *     local BEFORE that call, then handed to `remoteproc_remove_virtio()`
 *     -- confirmed by reading both functions' sources rather than
 *     guessing the order), closes every `metal_device`, and calls
 *     `metal_finish()`.  Called exactly once by the dispatcher, strictly
 *     after the active-op count has drained.
 *
 * @par Single-link limitation (documented, not a TODO)
 * REFERENCE.md's memory map describes exactly ONE physical M33 peer;
 * `g_chan_claimed` (a single atomic flag) rejects a second concurrent
 * `y_open()` with ALP_ERR_BUSY rather than pretending to support
 * multiple simultaneous UIO/OpenAMP links this hardware doesn't have.
 *
 * @par MHU doorbell -- registers resolved; A55 receive GIC SPI overlay-gated
 * `uio_rproc_notify()`/`uio_mhu_ack()` below poke the MHU channel-1
 * scratch (`mhu-shm`) + the MHU-B NS message registers inside `mhu-uio`.
 * CORRECTED (alp-sdk #683/#697 bench cycle 2): earlier revisions aimed first
 * at ICU page 0x10400000 (routes-only, no SET reg), then at a SWINT unit
 * SET (block +0x800) -- a DIFFERENT MHU-B sub-block whose IRQ is not the
 * M33's channel-5 line, so the kick never landed.  `mhu-uio` maps the real
 * MHU-B block (A55 0x10480000); the kick/ack now target the crossbar slots
 * the M33 fw's channel 5 actually uses (see the MHU-B NS register comment
 * above): KICK = MSG_INT_SET on R_MHU_NS8 (0x10480104), ACK = RSP_INT_CLR on
 * R_MHU_NS36 (0x10480494).  These offsets are silicon-authoritative from the
 * FSP headers (bsp_mhu_b.h + mhu_iodefine.h).  The one remaining TBD --
 * because meta-rz-multi-os is license-gated and not on this host -- is the
 * A55 GIC SPI the `mhu-uio` DT node must carry for the RECEIVE direction:
 * the M33 sends on its RSP interrupt (MHU_RSP5_NS_IRQn = 293+6 = 299), so
 * the A55 must be wired to rsp_ch5_ns, NOT the msg_ch5_ns line the openamp
 * UIO dtsi currently declares (that is the M33's OWN receive line).  Confirm
 * the A55 rsp_ch5 SPI against the vendor overlay or a bench IRQ-walk.
 *
 * @par What is NOT vendored here
 * The Renesas Multi-OS Package's `meta-rz-multi-os` layer (the Linux
 * UIO device-tree overlay + its own `platform_info.c` +
 * `rpmsg_sample_client`) is license-gated and NOT fetched or vendored
 * by this backend -- see ADR 0017 / [[feedback_alp_sdk_over_vendor_sdk]].
 * This file is written directly against the upstream, Apache-2.0
 * open-amp + libmetal public APIs (already vendored on-host as Zephyr
 * modules under hal/libmetal + lib/open-amp) using ONLY the memory map
 * + UIO device names REFERENCE.md documents; it assumes (does not
 * require) that a real V2N Yocto image's own device tree names the
 * platform devices exactly as REFERENCE.md's table does.
 *
 * @par Linking
 * Gated behind the SAME `ALP_SDK_HAVE_OPENAMP_USERLAND` define
 * yocto_drv.c uses (src/yocto/CMakeLists.txt's
 * `pkg_check_modules(OPENAMP_USER open-amp libmetal)` block already
 * covers both backends with one find) -- when absent, every op below
 * compiles to NOSUPPORT so the TU still links cleanly.
 *
 * STATUS: real impl against the verified open-amp/libmetal public API
 *         (headers + call sequence cross-checked against the vendored
 *         lib/remoteproc/remoteproc.c, lib/remoteproc/remoteproc_virtio.c,
 *         lib/rpmsg/rpmsg_virtio.c, lib/system/linux/irq.c sources, NOT
 *         guessed) -- BENCH-UNVERIFIED end-to-end: no real UIO nodes /
 *         M33 peer in this build environment.  Two assumptions are
 *         explicitly flagged above as needing bench confirmation
 *         (the vring-shm0/1 pool split, and the fixed-address vs.
 *         NS-announce endpoint bind); the MHU register offsets are a
 *         placeholder pending the real register map.
 */

#if defined(__linux__)

#include "alp/rpc.h"

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "rpc_ops.h"

#if defined(ALP_SDK_HAVE_OPENAMP_USERLAND)

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <metal/device.h>
#include <metal/io.h>
#include <metal/irq.h>
#include <metal/sys.h>
#include <openamp/open_amp.h>

#ifndef ALP_RPC_SUBS_PER_CHANNEL
#define ALP_RPC_SUBS_PER_CHANNEL 8
#endif

#ifndef ALP_RPC_TX_FRAME_MAX
#define ALP_RPC_TX_FRAME_MAX 1024
#endif

/* ------------------------------------------------------------------ */
/* REFERENCE.md Sec. 1 memory map: UIO region table                    */
/* ------------------------------------------------------------------ */

/* A55-side physical base of each named region; `da` (the M33 non-secure
 * device address the resource table / vring descriptors reference) is
 * always `pa + ALP_V2N_A55_TO_M33_NS_OFFSET`.
 *
 * CORRECTED (alp-sdk #683, address root-cause fix): this used to be
 * 0x20000000 (the RZ/V2L offset), which put every region below the A55
 * DRAM base (0x48000000) -- unbacked memory on V2N. The authoritative
 * V2N map (Renesas FSP
 * drivers/rz/fsp/src/rzv/bsp/mcu/rzv2n/bsp_slave_address.h) is CM33-secure
 * 0x80000000 / CM33-non-secure 0x90000000 / A55 0x40000000, so
 * da = pa + 0x50000000. */
#define ALP_V2N_A55_TO_M33_NS_OFFSET 0x50000000u

enum uio_region_id {
	UIO_RSCTBL = 0,
	UIO_MHU_SHM,
	UIO_VRING_CTL0,
	UIO_VRING_CTL1,
	UIO_VRING_SHM0,
	UIO_VRING_SHM1,
	UIO_MHU, /* MHU-B register block -- carries the notification IRQ */
	UIO_REGION_COUNT
};

struct uio_region_def {
	const char *dt_name; /* default sysfs "platform" device name */
	const char *env_var; /* ALP_UIO_* override */
	uintptr_t   pa;      /* A55 physical base */
	size_t      size;
};

/* Addresses corrected (alp-sdk #683, address root-cause fix) to the
 * authoritative V2N map -- see ALP_V2N_A55_TO_M33_NS_OFFSET's comment
 * above and the board overlay's matching CM33-side nodes
 * (zephyr/boards/alp/e1m_v2n101_m33_sm/...cm33.dts). UIO_MHU also
 * changed WHICH register block it maps -- see this file's header
 * comment's doorbell-fix note. */
static const struct uio_region_def g_uio_regions[UIO_REGION_COUNT] = {
	[UIO_RSCTBL]     = { "4f700000.rsctbl", "ALP_UIO_RSCTBL", 0x4F700000u, 0x1000u },
	[UIO_MHU_SHM]    = { "4f701000.mhu-shm", "ALP_UIO_MHU_SHM", 0x4F701000u, 0x1000u },
	[UIO_VRING_CTL0] = { "4f800000.vring-ctl0", "ALP_UIO_VRING_CTL0", 0x4F800000u, 0x50000u },
	[UIO_VRING_CTL1] = { "4f850000.vring-ctl1", "ALP_UIO_VRING_CTL1", 0x4F850000u, 0x50000u },
	[UIO_VRING_SHM0] = { "4f900000.vring-shm0", "ALP_UIO_VRING_SHM0", 0x4F900000u, 0x300000u },
	[UIO_VRING_SHM1] = { "4fc00000.vring-shm1", "ALP_UIO_VRING_SHM1", 0x4FC00000u, 0x300000u },
	[UIO_MHU]        = { "10480000.mhu-uio", "ALP_UIO_MHU", 0x10480000u, 0x1000u },
};

/* "platform" is libmetal's Linux bus name for generic-uio (uio_pdrv_genirq)
 * devices -- REFERENCE.md's device names match the DT node name libmetal's
 * sysfs lookup (lib/system/linux/device.c: sysfs_open_device(bus_name,
 * dev_name)) expects there. Override for a bench sysfs layout that differs
 * from the DT node name (e.g. a udev-renamed uio class). */
static const char *uio_bus_name(void)
{
	const char *env = getenv("ALP_UIO_BUS");
	return env ? env : "platform";
}

static const char *uio_dev_name(enum uio_region_id id)
{
	const char *env = getenv(g_uio_regions[id].env_var);
	return env ? env : g_uio_regions[id].dt_name;
}

/* ------------------------------------------------------------------ */
/* MHU doorbell (alp-sdk #683, doorbell fix)                            */
/* ------------------------------------------------------------------ */

/* Placeholder channel-1 scratch layout inside mhu-shm (REFERENCE.md:
 * "ch1 scratch @ +0x08"; RSP_TXD@0x0 / MSG_TXD@0x4 within that 8-byte
 * pair) -- confirmed field NAMES, unconfirmed absolute byte offsets
 * beyond REFERENCE.md's own note; see the file-scope TBD comment. */
#define ALP_MHU_SHM_CH1_OFFSET 0x08u
#define ALP_MHU_SHM_RSP_TXD    0x00u
#define ALP_MHU_SHM_MSG_TXD    0x04u

/*
 * MHU-B NS message registers inside the MHU-B block that UIO_MHU maps
 * (A55 0x10480000; M33-view 0x50480000, view offset 0x40000000).  Per
 * hal_renesas mhu_iodefine.h's R_MHU0_Type, each R_MHU_NSn crossbar slot is
 * 0x20 bytes: MSG_INT_STS/SET/CLR @ +0x00/04/08, RSP_INT_STS/SET/CLR @
 * +0x0C/10/14.
 *
 * The M33 fw runs its CA55<->CM33 link on logical channel 5, which the
 * MHU-B crossbar maps NON-linearly (bsp_mhu_b.h R_BSP_MHU_B_NS_REG_PAIR_BODY
 * entry {36, R_MHU_NS36, 8, R_MHU_NS8}): the M33 SENDS via R_MHU_NS36 (slot
 * offset 0x480) and RECEIVES via R_MHU_NS8 (slot offset 0x100), with
 * send_type = RSP -- derived from its declared rx_irq MHU_MSG5_NS_IRQn(293),
 * which bsp_mhu_b.h lists in the RSP send-type table.  So on this A55 peer
 * (the mirror, send_type = MSG):
 *   - KICK the M33 (raise its MHU_MSG5_NS_IRQn) by asserting MSG_INT_SET on
 *     R_MHU_NS8            -> A55 0x10480104.
 *   - RECEIVE/ack the M33's send (RSP_INT on R_MHU_NS36, MHU_RSP5_NS_IRQn=
 *     299) by clearing RSP_INT_CLR on R_MHU_NS36 -> A55 0x10480494.
 * CORRECTED (alp-sdk #683/#697 bench cycle 2): the previous revision wrote a
 * SWINT unit SET (block +0x800) -- a DIFFERENT MHU-B sub-block whose IRQ is
 * not MHU_MSG5_NS, so the kick never raised the M33's channel-5 IRQ.  These
 * register offsets are silicon-authoritative from the FSP headers; the one
 * remaining bench/overlay-gated unknown is the A55 GIC SPI the mhu-uio DT
 * node must carry for the RECEIVE direction (rsp_ch5_ns), which lives in the
 * license-gated Renesas meta-rz-multi-os overlay -- see the openamp UIO
 * dtsi's mhu-uio node. */
#define ALP_MHU_NS_SLOT_MSG_INT_STS 0x00u
#define ALP_MHU_NS_SLOT_MSG_INT_SET 0x04u
#define ALP_MHU_NS_SLOT_MSG_INT_CLR 0x08u
#define ALP_MHU_NS_SLOT_RSP_INT_STS 0x0Cu
#define ALP_MHU_NS_SLOT_RSP_INT_SET 0x10u
#define ALP_MHU_NS_SLOT_RSP_INT_CLR 0x14u

/* Channel-5 crossbar slot offsets within the A55-mapped MHU-B block. */
#define ALP_MHU_NS_CH5_KICK_SLOT 0x100u /* R_MHU_NS8  -- M33 RX; the A55 kick target */
#define ALP_MHU_NS_CH5_RECV_SLOT 0x480u /* R_MHU_NS36 -- M33 TX; the A55 recv source */

static volatile uint32_t *
mhu_ns_reg(struct metal_io_region *mhu, uint32_t slot_offset, uint32_t reg_offset)
{
	return (volatile uint32_t *)((uint8_t *)mhu->virt + slot_offset + reg_offset);
}

/* ------------------------------------------------------------------ */
/* Backend-owned per-channel state (reached via state->be_data)        */
/* ------------------------------------------------------------------ */

struct alp_rpc_sub {
	uint32_t            method_hash;
	char                method[ALP_RPC_METHOD_MAX_LEN];
	alp_rpc_method_cb_t cb;
	void               *user;
};

struct rpc_be {
	char     name[ALP_RPC_METHOD_MAX_LEN];
	uint32_t src_ept;
	uint32_t dst_ept;
	bool     metal_ready;

	struct metal_device  *dev[UIO_REGION_COUNT];
	struct remoteproc_mem mem[UIO_REGION_COUNT]; /* only rsctbl/ctl0/ctl1/shm0/shm1 used */

	struct remoteproc            rproc;
	struct rpmsg_virtio_shm_pool shpool;
	struct rpmsg_virtio_device   rvdev;
	struct rpmsg_endpoint        ept;
	bool                         ept_created;
	bool                         vdev_created;
	/* Set the instant remoteproc_init() returns successfully -- gates
	 * rpc_be_teardown()'s remoteproc_shutdown()/remoteproc_remove()
	 * calls (alp-sdk #683 bench fix): both dereference `rproc->ops`
	 * unconditionally once `rproc->state == RPROC_OFFLINE` (true for a
	 * calloc()'d, never-initialised struct remoteproc too, since
	 * RPROC_OFFLINE == 0 -- confirmed against
	 * lib/remoteproc/remoteproc.c's remoteproc_remove()), so calling
	 * them on a `ch->rproc` that never went through remoteproc_init()
	 * (y_open() failing at the metal_init()/metal_device_open() steps
	 * above it, before remoteproc_init() ever runs) is a NULL-pointer
	 * dereference through `rproc->ops->remove`, not a graceful no-op --
	 * reproduced live by tests/yocto/rpc_uio_bench_main.c under
	 * qemu-aarch64 (no real UIO devices -> metal_init() fails -> this
	 * exact early-`goto err` path). */
	bool rproc_ready;
	int  mhu_irq; /* fd cast from dev[UIO_MHU]->irq_info */

	pthread_mutex_t    tx_mutex;
	pthread_mutex_t    sub_mutex;
	struct alp_rpc_sub subs[ALP_RPC_SUBS_PER_CHANNEL];

	uint8_t tx_scratch[ALP_RPC_TX_FRAME_MAX];

	/* Close protocol (GHSA-xhm8-7f87-93q5), shared-libmetal-worker
	 * adaptation -- see this file's header comment. All guarded by
	 * `call_mutex` except `cb_active` (atomic, decrement-only outside
	 * the lock, matching zephyr_drv.c's `cb_active`). */
	pthread_mutex_t call_mutex;
	pthread_cond_t  call_cond;
	char            call_method[ALP_RPC_METHOD_MAX_LEN];
	void           *call_resp_buf;
	size_t          call_resp_cap;
	size_t          call_resp_len;
	alp_status_t    call_result;
	bool            call_pending;
	bool            closing;
	bool            recv_active; /* true only while uio_ept_cb() runs for THIS channel */
	pthread_t       recv_thread; /* identity of the CURRENT recv (valid iff recv_active) */
	atomic_int      cb_active;   /* drain count -- external y_shutdown() waits for 0 */
	bool            close_from_worker;

	void *owner;
};

static inline struct rpc_be *rpc_be_data_load(alp_rpc_backend_state_t *st)
{
	return (struct rpc_be *)__atomic_load_n(&st->be_data, __ATOMIC_ACQUIRE);
}

static inline void rpc_be_data_store(alp_rpc_backend_state_t *st, struct rpc_be *ch)
{
	__atomic_store_n(&st->be_data, (void *)ch, __ATOMIC_RELEASE);
}

/* Single-link limitation -- see this file's header comment. */
static atomic_int g_chan_claimed;

/* ------------------------------------------------------------------ */
/* Helpers (byte-compatible wire framing -- mirrors yocto_drv.c/        */
/* zephyr_drv.c's fnv1a_32 + frame_build + frame_parse)                 */
/* ------------------------------------------------------------------ */

static uint32_t fnv1a_32(const char *s)
{
	uint32_t h = 0x811c9dc5u;
	for (; *s; ++s) {
		h ^= (uint8_t)*s;
		h *= 0x01000193u;
	}
	return h;
}

static bool method_valid(const char *m)
{
	if (m == NULL || m[0] == '\0') {
		return false;
	}
	size_t n = strnlen(m, ALP_RPC_METHOD_MAX_LEN);
	return n < ALP_RPC_METHOD_MAX_LEN;
}

static int
frame_build(uint8_t *out, size_t cap, const char *method, const void *payload, size_t payload_len)
{
	size_t method_len = strnlen(method, ALP_RPC_METHOD_MAX_LEN);
	if (method_len == ALP_RPC_METHOD_MAX_LEN) {
		return -EINVAL;
	}
	size_t total;
	if (!alp_rpc_frame_size(method_len, payload_len, cap, &total)) {
		return -ENOMEM;
	}
	memcpy(out, method, method_len);
	out[method_len] = '\0';
	if (payload_len > 0) {
		memcpy(out + method_len + 1u, payload, payload_len);
	}
	return (int)total;
}

static const char *
frame_parse(const void *data, size_t len, const void **payload_out, size_t *payload_len_out)
{
	if (data == NULL || len == 0) {
		return NULL;
	}
	const char *bytes      = (const char *)data;
	size_t      cap        = len < ALP_RPC_METHOD_MAX_LEN ? len : ALP_RPC_METHOD_MAX_LEN;
	size_t      method_len = 0;
	while (method_len < cap && bytes[method_len] != '\0') {
		method_len++;
	}
	if (method_len == cap) {
		return NULL;
	}
	*payload_out     = (const void *)(bytes + method_len + 1u);
	*payload_len_out = len - method_len - 1u;
	return bytes;
}

/* ------------------------------------------------------------------ */
/* remoteproc_ops -- only `.notify` is required, see this file's        */
/* header comment for why every other slot is safely NULL              */
/* ------------------------------------------------------------------ */

static int uio_rproc_notify(struct remoteproc *rproc, uint32_t id)
{
	struct rpc_be *ch = (struct rpc_be *)rproc->priv;
	if (ch == NULL || ch->dev[UIO_MHU_SHM] == NULL || ch->dev[UIO_MHU] == NULL) {
		return -1;
	}

	struct metal_io_region *shm = metal_device_io_region(ch->dev[UIO_MHU_SHM], 0);
	struct metal_io_region *mhu = metal_device_io_region(ch->dev[UIO_MHU], 0);
	if (shm == NULL || mhu == NULL) {
		return -1;
	}

	/* Write the vring notify id into the ch1 scratch word, then raise the
	 * A55->CM33 doorbell by asserting MSG_INT_SET on the M33's channel-5
	 * receive register R_MHU_NS8 (alp-sdk #683/#697 doorbell fix -- see the
	 * MHU-B NS register comment above). */
	volatile uint32_t *scratch =
	    (volatile uint32_t *)((uint8_t *)shm->virt + ALP_MHU_SHM_CH1_OFFSET + ALP_MHU_SHM_MSG_TXD);
	*scratch = id;

	volatile uint32_t *set_reg =
	    mhu_ns_reg(mhu, ALP_MHU_NS_CH5_KICK_SLOT, ALP_MHU_NS_SLOT_MSG_INT_SET);
	*set_reg = 1u;
	return 0;
}

static const struct remoteproc_ops g_rproc_ops = {
	.notify = uio_rproc_notify,
};

/* ------------------------------------------------------------------ */
/* Notification worker -- runs on libmetal's SHARED linux IRQ thread    */
/* ------------------------------------------------------------------ */

/* Ack the CM33->A55 doorbell by clearing RSP_INT on the M33's channel-5 send
 * register R_MHU_NS36 -- alp-sdk #683/#697 doorbell fix, see the MHU-B NS
 * register comment above. */
static void uio_mhu_ack(struct rpc_be *ch)
{
	struct metal_io_region *mhu = metal_device_io_region(ch->dev[UIO_MHU], 0);
	if (mhu == NULL) {
		return;
	}
	volatile uint32_t *clr_reg =
	    mhu_ns_reg(mhu, ALP_MHU_NS_CH5_RECV_SLOT, ALP_MHU_NS_SLOT_RSP_INT_CLR);
	*clr_reg = 1u;
}

/* Enter uio_ept_cb(): check `closing` and count this recv in ONE
 * call_mutex critical section (mirrors zephyr_drv.c's rpc_recv_enter(),
 * GHSA-xhm8-7f87-93q5 defect 2 -- see this file's header comment). */
static bool rpc_recv_enter(struct rpc_be *ch)
{
	pthread_mutex_lock(&ch->call_mutex);
	if (ch->closing) {
		pthread_mutex_unlock(&ch->call_mutex);
		return false;
	}
	ch->recv_thread = pthread_self();
	ch->recv_active = true;
	atomic_fetch_add(&ch->cb_active, 1);
	pthread_mutex_unlock(&ch->call_mutex);
	return true;
}

static void rpc_recv_leave(struct rpc_be *ch)
{
	pthread_mutex_lock(&ch->call_mutex);
	ch->recv_active = false;
	pthread_mutex_unlock(&ch->call_mutex);
	atomic_fetch_sub(&ch->cb_active, 1);
}

/* rpmsg endpoint rx callback -- invoked synchronously by
 * remoteproc_get_notification() -> rproc_virtio_notified() ->
 * virtqueue_notification() from INSIDE uio_rproc_notify_isr() below,
 * on libmetal's shared IRQ thread. Mirrors yocto_drv.c's rpc_rx_main()
 * per-frame body / zephyr_drv.c's rpc_ept_recv(). */
static int uio_ept_cb(struct rpmsg_endpoint *ept, void *data, size_t len, uint32_t src, void *priv)
{
	(void)ept;
	(void)src;
	struct rpc_be *ch = (struct rpc_be *)priv;
	if (ch == NULL) {
		return RPMSG_SUCCESS;
	}
	if (!rpc_recv_enter(ch)) {
		return RPMSG_SUCCESS;
	}

	const void *payload     = NULL;
	size_t      payload_len = 0;
	const char *method      = frame_parse(data, len, &payload, &payload_len);
	if (method == NULL) {
		goto epilogue;
	}

	{
		pthread_mutex_lock(&ch->call_mutex);
		bool consumed_by_call = false;
		if (ch->call_pending && strncmp(method, ch->call_method, ALP_RPC_METHOD_MAX_LEN) == 0) {
			if (ch->call_resp_buf != NULL && ch->call_resp_cap > 0) {
				size_t copy_n = payload_len <= ch->call_resp_cap ? payload_len : ch->call_resp_cap;
				memcpy(ch->call_resp_buf, payload, copy_n);
			}
			ch->call_resp_len = payload_len;
			ch->call_result   = (payload_len > ch->call_resp_cap && ch->call_resp_buf != NULL)
			                        ? ALP_ERR_NOMEM
			                        : ALP_OK;
			ch->call_pending  = false;
			pthread_cond_signal(&ch->call_cond);
			consumed_by_call = true;
		}
		pthread_mutex_unlock(&ch->call_mutex);
		if (consumed_by_call) {
			goto epilogue;
		}
	}

	{
		uint32_t h = fnv1a_32(method);
		pthread_mutex_lock(&ch->sub_mutex);
		struct alp_rpc_sub *match = NULL;
		for (size_t i = 0; i < ALP_RPC_SUBS_PER_CHANNEL; ++i) {
			struct alp_rpc_sub *s = &ch->subs[i];
			if (s->cb != NULL && s->method_hash == h &&
			    strncmp(s->method, method, ALP_RPC_METHOD_MAX_LEN) == 0) {
				match = s;
				break;
			}
		}
		alp_rpc_method_cb_t cb   = match ? match->cb : NULL;
		void               *user = match ? match->user : NULL;
		pthread_mutex_unlock(&ch->sub_mutex);
		if (cb != NULL) {
			/* GHSA-xhm8-7f87-93q5: cb() may call alp_rpc_close() on THIS
			 * channel -- y_shutdown() detects that via recv_active +
			 * recv_thread (see this file's header comment) and returns
			 * ALP_RPC_SHUTDOWN_DEFERRED; the epilogue in
			 * uio_rproc_notify_isr() below completes the teardown. */
			cb(payload, payload_len, user);
		}
	}

epilogue:
	/* Read close_from_worker while still counted in cb_active -- same
	 * ordering rationale as zephyr_drv.c's rpc_ept_recv() epilogue. */
	{
		pthread_mutex_lock(&ch->call_mutex);
		bool close_from_worker = ch->close_from_worker;
		pthread_mutex_unlock(&ch->call_mutex);
		rpc_recv_leave(ch);
		if (close_from_worker) {
			/* Propagate up to uio_rproc_notify_isr()'s own epilogue via
			 * the SAME flag: it re-checks close_from_worker once
			 * remoteproc_get_notification() returns and finalizes
			 * exactly once. Nothing to do here beyond leaving the flag
			 * set. */
		}
	}
	return RPMSG_SUCCESS;
}

/* Registered via metal_irq_register() against the mhu-uio device's IRQ;
 * invoked by libmetal's shared Linux IRQ thread (lib/system/linux/irq.c)
 * every time the MHU doorbell fires. */
static int uio_rproc_notify_isr(int irq, void *arg)
{
	(void)irq;
	struct rpc_be *ch = (struct rpc_be *)arg;
	if (ch == NULL) {
		return METAL_IRQ_HANDLED;
	}

	uio_mhu_ack(ch);
	/* May invoke uio_ept_cb() synchronously, zero or more times, on
	 * THIS thread -- see this file's header comment. */
	(void)remoteproc_get_notification(&ch->rproc, RSC_NOTIFY_ID_ANY);

	/* GHSA-xhm8-7f87-93q5 DEFERRED epilogue -- mirrors yocto_drv.c's
	 * rpc_rx_main() epilogue / zephyr_drv.c's rpc_ept_recv() epilogue:
	 * if a callback dispatched above closed its OWN channel,
	 * y_shutdown() set close_from_worker (under call_mutex) and
	 * returned DEFERRED without unregistering/draining anything.
	 * Finish that deferred teardown here, exactly once, now that
	 * remoteproc_get_notification() has fully returned and this
	 * invocation's cb_active count has already been decremented back
	 * to 0 by uio_ept_cb()'s own epilogue above. */
	pthread_mutex_lock(&ch->call_mutex);
	bool close_from_worker = ch->close_from_worker;
	pthread_mutex_unlock(&ch->call_mutex);
	if (close_from_worker) {
		metal_irq_unregister(ch->mhu_irq);
		alp_rpc_close_finalize(ch->owner);
	}
	return METAL_IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Teardown                                                             */
/* ------------------------------------------------------------------ */

static void rpc_be_teardown(struct rpc_be *ch)
{
	if (ch->ept_created) {
		rpmsg_destroy_ept(&ch->ept);
	}
	if (ch->vdev_created) {
		/* rpmsg_deinit_vdev() NULLs rvdev.vdev as a side effect -- the
		 * heap-owning virtio_device* must be captured BEFORE that call
		 * so remoteproc_remove_virtio() (which frees it) still has a
		 * valid pointer -- confirmed against
		 * lib/rpmsg/rpmsg_virtio.c / lib/remoteproc/remoteproc.c. */
		struct virtio_device *vdev = ch->rvdev.vdev;
		rpmsg_deinit_vdev(&ch->rvdev);
		remoteproc_remove_virtio(&ch->rproc, vdev);
	}
	if (ch->rproc_ready) {
		(void)remoteproc_shutdown(&ch->rproc);
		(void)remoteproc_remove(&ch->rproc);
	}

	for (int i = 0; i < UIO_REGION_COUNT; ++i) {
		if (ch->dev[i] != NULL) {
			metal_device_close(ch->dev[i]);
		}
	}
	if (ch->metal_ready) {
		metal_finish();
	}

	pthread_mutex_destroy(&ch->tx_mutex);
	pthread_mutex_destroy(&ch->sub_mutex);
	pthread_cond_destroy(&ch->call_cond);
	pthread_mutex_destroy(&ch->call_mutex);
	free(ch);
	atomic_store(&g_chan_claimed, 0);
}

/* ------------------------------------------------------------------ */
/* Ops                                                                 */
/* ------------------------------------------------------------------ */

static alp_status_t
y_open(const alp_rpc_config_t *cfg, alp_rpc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (caps_out != NULL) caps_out->flags = 0u;
	if (cfg == NULL || cfg->name == NULL || cfg->name[0] == '\0') {
		return ALP_ERR_INVAL;
	}
	if (strnlen(cfg->name, ALP_RPC_METHOD_MAX_LEN) == ALP_RPC_METHOD_MAX_LEN) {
		return ALP_ERR_INVAL;
	}

	int expected = 0;
	if (!atomic_compare_exchange_strong(&g_chan_claimed, &expected, 1)) {
		/* Single-link limitation -- see this file's header comment. */
		return ALP_ERR_BUSY;
	}

	struct rpc_be *ch = (struct rpc_be *)calloc(1, sizeof(*ch));
	if (ch == NULL) {
		atomic_store(&g_chan_claimed, 0);
		return ALP_ERR_NOMEM;
	}

	strncpy(ch->name, cfg->name, sizeof(ch->name) - 1);
	/* Local endpoint src: default to RPMSG_ADDR_ANY so OpenAMP auto-allocates
	 * a valid dynamic address in [1024,1151].  The previous fnv1a-derived
	 * value (0x400 | hash & 0xFF) could land in [1152,1279], which
	 * rpmsg_create_ept() rejects with RPMSG_ERR_PARAM -- the OpenAMP address
	 * bitmap is only 128 wide above the 1024 reserved base, so any src >= 1152
	 * is unconditionally refused, breaking attach for ~half of all service
	 * names.  Silicon-root-caused on e1mx-v2n-m1-01 (#683/#697 bench cycle 2,
	 * 2026-07-11).  The M33 endpoint (src=1024, dst=ANY, NS-announce) learns
	 * our src from the first frame, so ANY binds cleanly. */
	ch->src_ept = cfg->src_ept != 0u ? cfg->src_ept : RPMSG_ADDR_ANY;
	/* dst MUST be the M33's known service address.  This backend passes
	 * ns_bind_cb = NULL (no name-service path to LEARN dst), and under the
	 * RPMSG_ADDR_ANY src default an unset dst would poison to 0 (was src+1).
	 * Require the caller to name the destination explicitly. */
	if (cfg->dst_ept == 0u) {
		free(ch);
		atomic_store(&g_chan_claimed, 0);
		return ALP_ERR_INVAL;
	}
	ch->dst_ept = cfg->dst_ept;
	ch->mhu_irq = -1;

	pthread_mutex_init(&ch->tx_mutex, NULL);
	pthread_mutex_init(&ch->sub_mutex, NULL);
	pthread_mutex_init(&ch->call_mutex, NULL);
	pthread_cond_init(&ch->call_cond, NULL);
	ch->owner = st->owner;

	static const struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	if (metal_init(&metal_params) != 0) {
		fprintf(stderr, "alp_rpc: metal_init() failed\n");
		goto err;
	}
	ch->metal_ready = true;

	for (int i = 0; i < UIO_REGION_COUNT; ++i) {
		if (metal_device_open(uio_bus_name(), uio_dev_name((enum uio_region_id)i), &ch->dev[i]) !=
		    0) {
			fprintf(stderr,
			        "alp_rpc: metal_device_open(%s,%s) failed\n",
			        uio_bus_name(),
			        uio_dev_name((enum uio_region_id)i));
			goto err;
		}
	}

	if (remoteproc_init(&ch->rproc, &g_rproc_ops, ch) != &ch->rproc) {
		fprintf(stderr, "alp_rpc: remoteproc_init() failed\n");
		goto err;
	}
	ch->rproc_ready = true;

	/* Pre-register every da/pa-addressable region so
	 * remoteproc_create_virtio()'s internal remoteproc_mmap() calls
	 * resolve WITHOUT needing an ops->mmap callback -- see this file's
	 * header comment, step 3. */
	static const enum uio_region_id mem_ids[] = {
		UIO_RSCTBL, UIO_VRING_CTL0, UIO_VRING_CTL1, UIO_VRING_SHM0, UIO_VRING_SHM1,
	};
	for (size_t i = 0; i < sizeof(mem_ids) / sizeof(mem_ids[0]); ++i) {
		enum uio_region_id      id = mem_ids[i];
		struct metal_io_region *io = metal_device_io_region(ch->dev[id], 0);
		if (io == NULL) {
			fprintf(stderr, "alp_rpc: no io region for %s\n", g_uio_regions[id].dt_name);
			goto err;
		}
		metal_phys_addr_t pa = (metal_phys_addr_t)g_uio_regions[id].pa;
		/* Register the device-address EQUAL to the A55 physical address.
		 * remoteproc_create_virtio() resolves each vring by looking its
		 * resource-table `da` up in these registered regions, and the M33
		 * resource table publishes vring DAs in A55-physical space
		 * (VRING_*_ADDR_A55 = 0x4f8xxxxx, resource_table.c).  The
		 * ALP_V2N_A55_TO_M33_NS_OFFSET (+0x50000000) is the CM33-NS<->A55
		 * view translation for the M33's OWN addressing (resource_table.h,
		 * m33_sm/main.c) -- it must NOT be applied to the master-side DA
		 * registration, or the vring lookup (da=0x4f8xxxxx) matches no
		 * region (all at 0x9f8xxxxx) -> remoteproc_create_virtio() returns
		 * NULL -> ALP_ERR_NOT_READY.  Silicon-root-caused on e1mx-v2n-m1-01
		 * (#683/#697 bench, 2026-07-11). */
		metal_phys_addr_t da = pa;
		remoteproc_init_mem(
		    &ch->mem[id], g_uio_regions[id].dt_name, pa, da, g_uio_regions[id].size, io);
		remoteproc_add_mem(&ch->rproc, &ch->mem[id]);
	}

	{
		struct metal_io_region *rsctbl_io = metal_device_io_region(ch->dev[UIO_RSCTBL], 0);
		if (rsctbl_io == NULL || remoteproc_set_rsc_table(&ch->rproc,
		                                                  (struct resource_table *)rsctbl_io->virt,
		                                                  g_uio_regions[UIO_RSCTBL].size) != 0) {
			fprintf(stderr, "alp_rpc: remoteproc_set_rsc_table() failed\n");
			goto err;
		}
	}

	if (remoteproc_config(&ch->rproc, NULL) != 0) {
		fprintf(stderr, "alp_rpc: remoteproc_config() failed\n");
		goto err;
	}

	struct virtio_device *vdev = remoteproc_create_virtio(&ch->rproc, 0, VIRTIO_DEV_DRIVER, NULL);
	if (vdev == NULL) {
		fprintf(stderr, "alp_rpc: remoteproc_create_virtio() failed\n");
		goto err;
	}
	ch->vdev_created = true;

	{
		struct metal_io_region *shm1_io = metal_device_io_region(ch->dev[UIO_VRING_SHM1], 0);
		if (shm1_io == NULL) {
			fprintf(stderr, "alp_rpc: no io region for vring-shm1\n");
			goto err;
		}
		rpmsg_virtio_init_shm_pool(&ch->shpool, shm1_io->virt, g_uio_regions[UIO_VRING_SHM1].size);
		if (rpmsg_init_vdev(&ch->rvdev, vdev, NULL, shm1_io, &ch->shpool) != 0) {
			fprintf(stderr, "alp_rpc: rpmsg_init_vdev() failed\n");
			goto err;
		}
	}

	/* REFERENCE.md Sec. 5 platform_info.c quirk -- see this file's
	 * header comment, step 7. */
	if (ch->rvdev.rvq != NULL) {
		(void)virtqueue_enable_cb(ch->rvdev.rvq);
	}

	{
		struct rpmsg_device *rdev = rpmsg_virtio_get_rpmsg_device(&ch->rvdev);
		if (rdev == NULL ||
		    rpmsg_create_ept(
		        &ch->ept, rdev, ch->name, ch->src_ept, ch->dst_ept, uio_ept_cb, NULL) != 0) {
			fprintf(stderr, "alp_rpc: rpmsg_create_ept() failed\n");
			goto err;
		}
		ch->ept.priv    = ch;
		ch->ept_created = true;
	}

	{
		struct metal_io_region *mhu_io = metal_device_io_region(ch->dev[UIO_MHU], 0);
		if (mhu_io == NULL || ch->dev[UIO_MHU]->irq_num == 0) {
			fprintf(stderr, "alp_rpc: mhu-uio has no IRQ\n");
			goto err;
		}
		ch->mhu_irq = (int)(intptr_t)ch->dev[UIO_MHU]->irq_info;
		if (metal_irq_register(ch->mhu_irq, uio_rproc_notify_isr, ch) != 0) {
			fprintf(stderr, "alp_rpc: metal_irq_register() failed\n");
			goto err;
		}
		metal_irq_enable((unsigned int)ch->mhu_irq);
	}

	rpc_be_data_store(st, ch);
	return ALP_OK;

err:
	rpc_be_teardown(ch);
	return ALP_ERR_NOT_READY;
}

static alp_status_t y_unsubscribe(alp_rpc_backend_state_t *st, const char *method);

static alp_status_t
y_subscribe(alp_rpc_backend_state_t *st, const char *method, alp_rpc_method_cb_t cb, void *user)
{
	if (!method_valid(method)) return ALP_ERR_INVAL;
	if (cb == NULL) {
		return y_unsubscribe(st, method);
	}

	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;

	uint32_t h = fnv1a_32(method);

	pthread_mutex_lock(&ch->sub_mutex);
	struct alp_rpc_sub *slot = NULL;
	for (size_t i = 0; i < ALP_RPC_SUBS_PER_CHANNEL; ++i) {
		struct alp_rpc_sub *s = &ch->subs[i];
		if (s->cb != NULL && s->method_hash == h &&
		    strncmp(s->method, method, ALP_RPC_METHOD_MAX_LEN) == 0) {
			slot = s;
			break;
		}
	}
	if (slot == NULL) {
		for (size_t i = 0; i < ALP_RPC_SUBS_PER_CHANNEL; ++i) {
			if (ch->subs[i].cb == NULL) {
				slot              = &ch->subs[i];
				slot->method_hash = h;
				strncpy(slot->method, method, sizeof(slot->method) - 1);
				slot->method[sizeof(slot->method) - 1] = '\0';
				break;
			}
		}
	}
	alp_status_t rc;
	if (slot == NULL) {
		rc = ALP_ERR_NOMEM;
	} else {
		slot->cb   = cb;
		slot->user = user;
		rc         = ALP_OK;
	}
	pthread_mutex_unlock(&ch->sub_mutex);
	return rc;
}

static alp_status_t y_unsubscribe(alp_rpc_backend_state_t *st, const char *method)
{
	if (!method_valid(method)) return ALP_ERR_INVAL;

	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;

	uint32_t h = fnv1a_32(method);
	pthread_mutex_lock(&ch->sub_mutex);
	alp_status_t rc = ALP_ERR_INVAL;
	for (size_t i = 0; i < ALP_RPC_SUBS_PER_CHANNEL; ++i) {
		struct alp_rpc_sub *s = &ch->subs[i];
		if (s->cb != NULL && s->method_hash == h &&
		    strncmp(s->method, method, ALP_RPC_METHOD_MAX_LEN) == 0) {
			s->cb          = NULL;
			s->user        = NULL;
			s->method[0]   = '\0';
			s->method_hash = 0u;
			rc             = ALP_OK;
			break;
		}
	}
	pthread_mutex_unlock(&ch->sub_mutex);
	return rc;
}

static alp_status_t
y_send(alp_rpc_backend_state_t *st, const char *method, const void *payload, size_t len)
{
	if (!method_valid(method)) return ALP_ERR_INVAL;
	if (payload == NULL && len > 0) return ALP_ERR_INVAL;

	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;

	pthread_mutex_lock(&ch->tx_mutex);
	int          built = frame_build(ch->tx_scratch, sizeof ch->tx_scratch, method, payload, len);
	alp_status_t rc;
	if (built < 0) {
		rc = (built == -ENOMEM) ? ALP_ERR_NOMEM : ALP_ERR_INVAL;
	} else {
		/* rpmsg_trysend (non-blocking) mirrors yocto_drv.c's O_NONBLOCK
		 * write() semantics: no free tx buffer maps to ALP_ERR_BUSY,
		 * exactly like that backend's EAGAIN mapping. */
		int w = rpmsg_trysend(&ch->ept, ch->tx_scratch, (size_t)built);
		if (w == RPMSG_ERR_NO_BUFF) {
			rc = ALP_ERR_BUSY;
		} else if (w < 0) {
			rc = ALP_ERR_IO;
		} else {
			rc = ALP_OK;
		}
	}
	pthread_mutex_unlock(&ch->tx_mutex);
	return rc;
}

/* Test-only late-staging hook -- mirrors yocto_drv.c's
 * g_y_call_test_late_staging_hook (see tests/yocto/rpc_yocto_self_close.c). */
static void (*g_y_call_test_late_staging_hook)(void) = NULL;

static alp_status_t y_call(alp_rpc_backend_state_t *st,
                           const char              *method,
                           const void              *req,
                           size_t                   req_len,
                           void                    *resp,
                           size_t                  *resp_len,
                           uint32_t                 timeout_ms)
{
	if (!method_valid(method)) return ALP_ERR_INVAL;
	if (req == NULL && req_len > 0) return ALP_ERR_INVAL;
	if (resp != NULL && resp_len == NULL) return ALP_ERR_INVAL;

	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) return ALP_ERR_NOT_READY;

	if (g_y_call_test_late_staging_hook != NULL) {
		g_y_call_test_late_staging_hook();
	}

	pthread_mutex_lock(&ch->tx_mutex);

	pthread_mutex_lock(&ch->call_mutex);
	if (ch->closing) {
		pthread_mutex_unlock(&ch->call_mutex);
		pthread_mutex_unlock(&ch->tx_mutex);
		return ALP_ERR_NOT_READY;
	}
	strncpy(ch->call_method, method, sizeof(ch->call_method) - 1);
	ch->call_method[sizeof(ch->call_method) - 1] = '\0';
	ch->call_resp_buf                            = resp;
	ch->call_resp_cap = (resp != NULL && resp_len != NULL) ? *resp_len : 0u;
	ch->call_resp_len = 0u;
	ch->call_result   = ALP_ERR_TIMEOUT;
	ch->call_pending  = true;
	pthread_mutex_unlock(&ch->call_mutex);

	int          built = frame_build(ch->tx_scratch, sizeof ch->tx_scratch, method, req, req_len);
	alp_status_t s     = ALP_OK;
	if (built < 0) {
		s = (built == -ENOMEM) ? ALP_ERR_NOMEM : ALP_ERR_INVAL;
	} else {
		int w = rpmsg_trysend(&ch->ept, ch->tx_scratch, (size_t)built);
		if (w == RPMSG_ERR_NO_BUFF) {
			s = ALP_ERR_BUSY;
		} else if (w < 0) {
			s = ALP_ERR_IO;
		}
	}

	if (s != ALP_OK) {
		pthread_mutex_lock(&ch->call_mutex);
		ch->call_pending = false;
		pthread_mutex_unlock(&ch->call_mutex);
		pthread_mutex_unlock(&ch->tx_mutex);
		return s;
	}

	pthread_mutex_lock(&ch->call_mutex);
	int rc = 0;
	if (timeout_ms == UINT32_MAX) {
		while (ch->call_pending && !ch->closing) {
			rc = pthread_cond_wait(&ch->call_cond, &ch->call_mutex);
			if (rc != 0) break;
		}
	} else {
		struct timespec deadline;
		if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
			ch->call_pending = false;
			pthread_mutex_unlock(&ch->call_mutex);
			pthread_mutex_unlock(&ch->tx_mutex);
			return ALP_ERR_IO;
		}
		uint64_t add_s  = (uint64_t)(timeout_ms / 1000u);
		uint64_t add_ns = (uint64_t)(timeout_ms % 1000u) * 1000000u;
		deadline.tv_sec += (time_t)add_s;
		deadline.tv_nsec += (long)add_ns;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec += deadline.tv_nsec / 1000000000L;
			deadline.tv_nsec %= 1000000000L;
		}
		while (ch->call_pending && !ch->closing) {
			rc = pthread_cond_timedwait(&ch->call_cond, &ch->call_mutex, &deadline);
			if (rc != 0) break;
		}
	}

	if (ch->closing) {
		ch->call_pending = false;
		s                = ALP_ERR_NOT_READY;
	} else if (rc == ETIMEDOUT) {
		ch->call_pending = false;
		s                = ALP_ERR_TIMEOUT;
	} else if (rc != 0) {
		ch->call_pending = false;
		s                = ALP_ERR_IO;
	} else {
		s = ch->call_result;
		if (resp_len != NULL && (s == ALP_OK || s == ALP_ERR_NOMEM)) {
			*resp_len = ch->call_resp_len;
		}
	}
	pthread_mutex_unlock(&ch->call_mutex);

	pthread_mutex_unlock(&ch->tx_mutex);
	return s;
}

static alp_rpc_shutdown_result_t y_shutdown(alp_rpc_backend_state_t *st)
{
	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) {
		return ALP_RPC_SHUTDOWN_DONE;
	}

	pthread_mutex_lock(&ch->call_mutex);
	bool from_worker = ch->recv_active && pthread_equal(pthread_self(), ch->recv_thread);
	ch->closing      = true;
	if (ch->call_pending) {
		ch->call_result   = ALP_ERR_NOT_READY;
		ch->call_resp_len = 0;
		ch->call_pending  = false;
	}
	if (from_worker) {
		ch->close_from_worker = true;
	}
	pthread_cond_broadcast(&ch->call_cond);
	pthread_mutex_unlock(&ch->call_mutex);

	if (from_worker) {
		/* DEFERRED: uio_rproc_notify_isr()'s epilogue (this SAME
		 * thread, once remoteproc_get_notification() unwinds) finishes
		 * the teardown -- see this file's header comment. */
		return ALP_RPC_SHUTDOWN_DEFERRED;
	}

	/* External close: unregister so no NEW notification is dispatched
	 * (safe to call from a foreign thread -- see this file's header
	 * comment on metal_irq_register()'s lock-free implementation), then
	 * wait for any recv ALREADY in flight to finish before returning
	 * DONE. */
	if (ch->mhu_irq >= 0) {
		metal_irq_unregister(ch->mhu_irq);
	}
	while (atomic_load(&ch->cb_active) != 0) {
		/* Sleep, never spin -- see src/rpc_dispatch.c's _rpc_drain() doc
		 * comment for the priority-inversion trap a busy spin would
		 * reintroduce here. */
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
		nanosleep(&ts, NULL);
	}
	return ALP_RPC_SHUTDOWN_DONE;
}

static void y_destroy(alp_rpc_backend_state_t *st)
{
	struct rpc_be *ch = rpc_be_data_load(st);
	if (ch == NULL) {
		return;
	}
	rpc_be_data_store(st, NULL);
	rpc_be_teardown(ch);
}

#else /* !ALP_SDK_HAVE_OPENAMP_USERLAND */

/** @brief NOSUPPORT open() -- no OpenAMP user-space libraries linked. */
static alp_status_t
y_open(const alp_rpc_config_t *cfg, alp_rpc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	(void)cfg;
	(void)st;
	if (caps_out != NULL) caps_out->flags = 0u;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT subscribe() -- no OpenAMP user-space libraries linked. */
static alp_status_t
y_subscribe(alp_rpc_backend_state_t *st, const char *method, alp_rpc_method_cb_t cb, void *user)
{
	(void)st;
	(void)method;
	(void)cb;
	(void)user;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT unsubscribe() -- no OpenAMP user-space libraries linked. */
static alp_status_t y_unsubscribe(alp_rpc_backend_state_t *st, const char *method)
{
	(void)st;
	(void)method;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT send() -- no OpenAMP user-space libraries linked. */
static alp_status_t
y_send(alp_rpc_backend_state_t *st, const char *method, const void *payload, size_t len)
{
	(void)st;
	(void)method;
	(void)payload;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT call() -- no OpenAMP user-space libraries linked. */
static alp_status_t y_call(alp_rpc_backend_state_t *st,
                           const char              *method,
                           const void              *req,
                           size_t                   req_len,
                           void                    *resp,
                           size_t                  *resp_len,
                           uint32_t                 timeout_ms)
{
	(void)st;
	(void)method;
	(void)req;
	(void)req_len;
	(void)resp;
	(void)resp_len;
	(void)timeout_ms;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT shutdown() -- no worker of any kind exists, so a
 *         self-close is impossible -- always DONE. */
static alp_rpc_shutdown_result_t y_shutdown(alp_rpc_backend_state_t *st)
{
	(void)st;
	return ALP_RPC_SHUTDOWN_DONE;
}

/** @brief NOSUPPORT destroy() -- no OpenAMP user-space libraries linked. */
static void y_destroy(alp_rpc_backend_state_t *st)
{
	(void)st;
}

#endif /* ALP_SDK_HAVE_OPENAMP_USERLAND */

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const alp_rpc_ops_t _ops = {
	.open        = y_open,
	.subscribe   = y_subscribe,
	.unsubscribe = y_unsubscribe,
	.send        = y_send,
	.call        = y_call,
	.shutdown    = y_shutdown,
	.destroy     = y_destroy,
};

/* silicon_ref is the EXACT ALP_SOC_REF_STR this SoC generates
 * (include/alp/soc_caps.h) -- an exact match beats yocto_drv.c's
 * silicon_ref="*" wildcard at equal priority per src/backend.c's
 * selector, but priority=150 (> yocto_drv.c's 100) makes the override
 * explicit regardless of that tiebreak, mirroring
 * src/backends/camera/v2n_n44_isp.c's identical pattern. */
ALP_BACKEND_REGISTER(rpc,
                     yocto_uio_drv,
                     {
                         .silicon_ref = "renesas:rzv2n:n44",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 150,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* __linux__ */
