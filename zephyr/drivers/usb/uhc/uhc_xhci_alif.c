/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble USB host (xHCI USB-2.0 dual-role, DWC3-family) UHC driver.
 *
 * Implemented: the xHCI register map (CAP/op/runtime/doorbell @ 0x48200000),
 * the standard xHCI ring/context structures (per the xHCI spec §5), the DWC3
 * G*-register host-mode init (GCTL @ 0xC110, PrtCapDir=host), command-ring /
 * event-ring processing (bounded polling, §4.9), root-hub port reset +
 * single-device enumeration, the full uhc_api (enqueue/dequeue, bus
 * reset/suspend/resume, disable/shutdown), and the event-ring IRQ (hotplug
 * connect/disconnect notification via Port Status Change).
 *
 * NOT VALIDATED ON SILICON beyond the bring-up path noted per-function below.
 * What was actually proven on an E8 EVK, 2026-07-04 (see the timing comments
 * in uhc_xhci_alif_first_light()) is the *_enable() controller bring-up
 * itself: DWC3 host-mode init, xHCI HCRST/CNR settling, the command ring ->
 * doorbell -> event ring round trip (No-Op + Enable Slot commands both
 * completed), and root-hub port power-up.  uhc_xhci_alif_enumerate()'s
 * device-dependent stages beyond that (port reset with a real device
 * attached, Address Device, the GET_DESCRIPTOR control transfer) did NOT
 * reach completion on that bench session -- the EVK's D+/D- signal path
 * blocks a real device from being seen, independent of this driver's
 * software state (issue #388's triage comments).  Everything added after
 * that bench session (ep_enqueue/ep_dequeue, bus_reset/suspend/resume,
 * disable/shutdown, the ISR) follows the same register sequencing but has
 * NOT itself been run against the live controller -- needs a bench pass per
 * issue #388's tracking comment before it can be called validated.
 *
 * Grounded from the Alif DFP soc.h (AE402FA0E5597):
 *   USB_BASE         0x48200000  (DFP soc.h line 3578)
 *   USB_IRQ_IRQn     101         (DFP soc.h line 181)
 *   DWC3 GCTL        @ 0xC110   (DFP USB_Type struct member)
 *
 * DWC3 G*-register host-mode init (HWRM §14.10.5.3 / Table 14-168):
 *   1. Assert soft reset: DCTL.CoreSoftReset (0xC704 bit 30); poll clear.
 *   2. Set GCTL.PrtCapDir (bits 13:12) = 0b01 (host).
 *   3. Program GUSB2PHYCFG0 (0xC200) for the embedded HS PHY parameters.
 *   4. Size the TX/RX FIFOs: GTXFIFOSIZ0/GRXFIFOSIZ0 (0xC300/0xC380+).
 *   5. Set GCTL.U2RSTECN (bit 16) for USB-2.0 reset control.
 *   Steps 1-5 are TODO(aen401-bench) -- bench bring-up provides the exact
 *   field values and sequencing for Alif silicon.
 *
 * xHCI register semantics from the xHCI specification (Intel, rev 1.2):
 *   §5.3  Capability registers   (base + 0x0)
 *   §5.4  Operational registers  (base + CAPLENGTH)
 *   §5.5  Runtime registers      (base + RTSOFF)
 *   §5.6  Doorbell registers     (base + DBOFF)
 *   §6    xHCI data structures (DCBAA, command ring, event ring, TRBs)
 */

#define DT_DRV_COMPAT alif_xhci_uhc

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/cache.h>
#include <zephyr/irq.h>
#include <errno.h>
#include <string.h>
#include "uhc_common.h"
#include "xhci_core.h"

LOG_MODULE_REGISTER(uhc_xhci_alif, CONFIG_UHC_DRIVER_LOG_LEVEL);

/* The xHCI DMA master is a SYSTOP system-bus master: it CANNOT reach the M55's
 * local TCM aliases.  Every DMA structure the CPU allocates in TCM (DCBAA,
 * command/event rings, ERST, scratchpad) must be handed to the controller --
 * and referenced from INSIDE the structures (Link TRB, DCBAA[0], ERST base,
 * scratchpad-array entry) -- by its GLOBAL alias.  Addresses already global
 * pass through.
 *
 * The alias window is PER-CORE (issue #752): M55-HP's ITCM/DTCM alias at
 * 0x50000000/0x50800000, M55-HE's at 0x58000000/0x58800000 -- see the board
 * DTS `&itcm`/`&dtcm` `global_base` overrides (e.g.
 * boards/alp/e1m_aen401_m55_hp/..._rtss_hp.dts, boards/alp/e1m_aen801_m55_he/
 * ..._rtss_he.dts), which are themselves transcribed from the Alif DFP /
 * zephyr_alif fork ensemble_rtss_hp.dtsi / ensemble_rtss_he.dtsi.  Never
 * hardcode one core's aliases here -- resolve the ACTIVE core's own
 * itcm/dtcm DT nodes (same source hal_alif's local_to_global(),
 * soc_memory_map.h, reads via DT_NODELABEL(itcm)/DT_NODELABEL(dtcm)) and feed
 * them into the arch-neutral, host-unit-tested xhci_local_to_global()
 * (xhci_core.c). */
static const struct xhci_tcm_map xhci_tcm = {
	.itcm_base        = DT_REG_ADDR(DT_NODELABEL(itcm)),
	.itcm_size        = DT_REG_SIZE(DT_NODELABEL(itcm)),
	.itcm_global_base = DT_PROP(DT_NODELABEL(itcm), global_base),
	.dtcm_base        = DT_REG_ADDR(DT_NODELABEL(dtcm)),
	.dtcm_size        = DT_REG_SIZE(DT_NODELABEL(dtcm)),
	.dtcm_global_base = DT_PROP(DT_NODELABEL(dtcm), global_base),
};

static uint64_t xhci_l2g(const void *p)
{
	return xhci_local_to_global(&xhci_tcm, p);
}

/* The xHCI controller is an AXI-master DMA engine: it reads/writes system RAM
 * directly, bypassing the M55's D-cache.  Every ring/context/data buffer the
 * CPU hands to it needs explicit cache maintenance at the handoff:
 *   - CPU produced data the xHC will read (TRBs we enqueue, DCBAA/ERST,
 *     input contexts, OUT transfer data) -- flush (clean) before ringing the
 *     doorbell, or the xHC can read stale/partial data still sitting in cache.
 *   - Data the xHC produced that the CPU will read (event-ring TRBs, IN
 *     transfer data) -- invalidate before reading, or the CPU can read its
 *     own stale cached copy instead of what the DMA engine wrote.
 * sys_cache_data_{flush,invd}_range() are safe no-ops when
 * CONFIG_CACHE_MANAGEMENT/CONFIG_DCACHE aren't set, so these calls are
 * unconditional.  UNVERIFIED ON SILICON: whether the M55's cache is enabled
 * for the region backing these buffers (TCM is typically cached; the exact
 * line size / MPU attributes are a bench question, not a software one). */
static void xhci_alif_flush(const void *addr, size_t len)
{
	sys_cache_data_flush_range((void *)addr, len);
}

static void xhci_alif_invd(const volatile void *addr, size_t len)
{
	sys_cache_data_invd_range((void *)(uintptr_t)addr, len);
}

/* ---------------------------------------------------------------------------
 * DWC3 global registers (offsets from the USB controller base).
 * Source: Alif DFP USB_Type struct; HWRM §14.10.5.3.
 * ---------------------------------------------------------------------------
 */
#define DWC3_GCTL                 0xC110u /* Global Core Control Register */
#define DWC3_GCTL_PRTCAPDIR_SHIFT 12u
#define DWC3_GCTL_PRTCAPDIR_MASK  (3u << DWC3_GCTL_PRTCAPDIR_SHIFT)
#define DWC3_GCTL_PRTCAPDIR_HOST  (1u << DWC3_GCTL_PRTCAPDIR_SHIFT)
/* DFP drivers/include/usbd.h: GCTL.DisableClockGating bit0, CoreSoftReset bit11,
 * PrtCapDir host = 1<<12 (matches PRTCAPDIR_HOST above). */
#define DWC3_GCTL_DSBLCLKGTNG   (1u << 0)
#define DWC3_GCTL_CORESOFTRESET (1u << 11)

/* Global USB2 PHY config (DFP soc.h USB_Type @ 0xC200; bits from usbd.h). */
#define DWC3_GUSB2PHYCFG0               0xC200u
#define DWC3_GUSB2PHYCFG_PHYSOFTRST     (1u << 31)
#define DWC3_GUSB2PHYCFG_SUSPHY         (1u << 6)
#define DWC3_GUSB2PHYCFG_ULPI_UTMI      (1u << 4)
#define DWC3_GUSB2PHYCFG_ULPIAUTORES    (1u << 15)
#define DWC3_GUSB2PHYCFG_PHYIF_MASK     (1u << 3)    /* PHYIF pos 3 */
#define DWC3_GUSB2PHYCFG_USBTRDTIM_MASK (0xFu << 10) /* USBTRDTIM pos 10 */
/* The E8 HS PHY is 16-bit UTMI+ (UTMIW) -- the DFP hard-defaults hsphy_mode to
 * UTMIW (usbd_initialize.c:276): PHYIF=1 (16-bit), USBTRDTIM=5, SUSPHY set.
 * An 8-bit config (PHYIF=0, USBTRDTIM=9) mismatches the PHY data width so the
 * UTMI clock is wrong and the xHCI core stays frozen (HCRST never completes). */
#define DWC3_GUSB2PHYCFG_PHYIF_16BIT     (1u << 3)  /* UTMI_PHYIF_16_BIT */
#define DWC3_GUSB2PHYCFG_USBTRDTIM_16BIT (5u << 10) /* USBTRDTIM_UTMI_16_BIT */

/* Global SoC bus config (0xC100), user control (0xC12C), frame-length adjust
 * (0xC630) -- DFP soc.h USB_Type offsets; bits from usbd.h. */
#define DWC3_GSBUSCFG0               0xC100u
#define DWC3_GSBUSCFG0_INCRBRSTENA   (1u << 0)
#define DWC3_GSBUSCFG0_INCR16BRSTENA (1u << 3)
#define DWC3_GUCTL                   0xC12Cu
#define DWC3_GUCTL_HSTINAUTORETRY    (1u << 14)
#define DWC3_GFLADJ                  0xC630u
#define DWC3_GFLADJ_30MHZ_MASK       0x3Fu
#define DWC3_GFLADJ_30MHZ_SDBND_SEL  (1u << 7)
#define DWC3_GFLADJ_30MHZ_DEFAULT    0x20u

/* USB clock + PHY power-on-reset live in CLKCTL_PER_MST (0x4903F000), NOT the
 * controller window -- DFP drivers/include/sys_ctrl_usb.h.  Transcribed here
 * so the driver is self-contained (no hal_alif USB module exists). */
#define CLKCTL_PER_MST_BASE          0x4903F000u
#define CLKCTL_PERIPH_CLK_ENA        (CLKCTL_PER_MST_BASE + 0x0Cu)
#define CLKCTL_PERIPH_CLK_ENA_USB    (1u << 20) /* PERIPH_CLK_ENA_USB_CKEN */
#define CLKCTL_USB_CTRL1             (CLKCTL_PER_MST_BASE + 0xA8u)
#define CLKCTL_USB_CTRL1_PERM_ATTACH (1u << 1) /* HUB_PORT_PERM_ATTACH (SVD) */
#define CLKCTL_USB_CTRL2             (CLKCTL_PER_MST_BASE + 0xACu)
#define CLKCTL_USB_CTRL2_PHY_POR     (1u << 8)    /* USB_CTRL2 bit8 = POR_RST_MASK (SVD) */
#define CLKCTL_USB_CTRL2_FLADJ_MASK  (0x3Fu << 0) /* USB_CTRL2 FLADJ_30MHZ_REG [5:0] */
#define CLKCTL_USB_CTRL2_FLADJ_30MHZ (0x20u << 0) /* SoC-wrapper 30MHz frame-len adjust */

/* xHCI operational registers (xHCI spec §5.4, at controller base + CAPLENGTH). */
#define XHCI_OP_USBCMD    0x00u
#define XHCI_OP_USBSTS    0x04u
#define XHCI_USBCMD_HCRST (1u << 1)  /* Host Controller Reset (§5.4.1) */
#define XHCI_USBCMD_INTE  (1u << 2)  /* Interrupter Enable (§5.4.1)    */
#define XHCI_USBSTS_CNR   (1u << 11) /* Controller Not Ready (§5.4.2)  */
#define XHCI_USBSTS_HCH   (1u << 0)  /* HCHalted (§5.4.2)              */
#define XHCI_USBSTS_EINT  (1u << 3)  /* Event Interrupt, RW1C (§5.4.2) */
#define XHCI_CRCR_CA      (1u << 2)  /* Command Ring Abort (§5.4.5)    */
#define XHCI_CRCR_CRR     (1u << 3)  /* Command Ring Running, RO (§5.4.5) */

/* Primary interrupter management register (IMAN, §5.5.2.1) at RT+0x20. */
#define XHCI_IMAN_IP (1u << 0) /* Interrupt Pending (write-1-clear) */
#define XHCI_IMAN_IE (1u << 1) /* Interrupt Enable */

/* PORTSC (§5.4.8) link-state fields used by bus_reset/suspend/resume. */
#define XHCI_PORTSC_CCS         (1u << 0)  /* Current Connect Status */
#define XHCI_PORTSC_PP          (1u << 9)  /* Port Power */
#define XHCI_PORTSC_LWS         (1u << 16) /* Link State Write Strobe */
#define XHCI_PORTSC_PLS_SHIFT   5u
#define XHCI_PORTSC_PLS_MASK    (0xFu << XHCI_PORTSC_PLS_SHIFT)
#define XHCI_PORTSC_PLS_U0      (0u << XHCI_PORTSC_PLS_SHIFT)
#define XHCI_PORTSC_PLS_U3      (3u << XHCI_PORTSC_PLS_SHIFT)
#define XHCI_PORTSC_PLS_RESUME  (15u << XHCI_PORTSC_PLS_SHIFT)
#define XHCI_PORTSC_PLC         (1u << 22) /* Port Link State Change (RW1C) */
#define XHCI_PORTSC_PRC         (1u << 21) /* Port Reset Change (RW1C) */
#define XHCI_PORTSC_PR          (1u << 4)  /* Port Reset */
#define XHCI_PORTSC_SPEED_SHIFT 10u
#define XHCI_PORTSC_SPEED_MASK  (0xFu << XHCI_PORTSC_SPEED_SHIFT)
/* RW1C change bits [23:17] plus PED[1]: never write these back as 1 by
 * accident when updating PLS/PR/PP, or a real pending change gets eaten. */
#define XHCI_PORTSC_RW1C_MASK ((0x7Fu << 17) | (1u << 1))

/* ---------------------------------------------------------------------------
 * xHCI capability registers (at controller base; xHCI spec §5.3).
 * ---------------------------------------------------------------------------
 */
struct xhci_cap_regs {
	uint8_t  caplength; /* Capability Registers Length (§5.3.1) */
	uint8_t  rsvd;
	uint16_t hciversion; /* Interface Version Number (§5.3.2) */
	uint32_t hcsparams1; /* Structural Parameters 1 (§5.3.3) */
	uint32_t hcsparams2; /* Structural Parameters 2 (§5.3.4) */
	uint32_t hcsparams3; /* Structural Parameters 3 (§5.3.5) */
	uint32_t hccparams1; /* Capability Parameters 1 (§5.3.6) */
	uint32_t dboff;      /* Doorbell Array Offset (§5.3.7) */
	uint32_t rtsoff;     /* Runtime Register Space Offset (§5.3.8) */
	uint32_t hccparams2; /* Capability Parameters 2 (§5.3.9) */
};

/* TODO(aen401-bench): add the full operational register map (§5.4):
 *   USBCMD (0x00): R/S, HCRST, INTE, HSEE, ...
 *   USBSTS (0x04): HCH, HSE, EINT, PCD, ...
 *   PAGESIZE (0x08): supported page sizes
 *   DNCTRL (0x14): device notification bitmap
 *   CRCR   (0x18): command ring control (RCS, CS, CA, CRR, ptr)
 *   DCBAAP (0x30): device context base address array pointer
 *   CONFIG (0x38): MaxSlotsEn
 * Runtime registers (§5.5) at base+RTSOFF:
 *   MFINDEX, primary interrupter (IMAN, IMOD, ERSTSZ, ERSTBA, ERDP)
 * Doorbell registers (§5.6) at base+DBOFF:
 *   DB[0] = host controller doorbell (command ring)
 *   DB[slot] = device slot doorbell (endpoint streams)
 * Data structures (§6):
 *   DCBAA: 64-bit array of device-context pointers (up to MaxSlots+1)
 *   Command ring: 16-byte TRBs (Enable/Disable Slot, Address Device, ...)
 *   Event ring: 16-byte TRBs (Command Completion, Transfer, Port SC Change)
 *   Transfer rings: per-endpoint TRB rings (Normal, Setup, Data, Status)
 *   Segment Table (ERST): event ring segment table
 */

/* ---------------------------------------------------------------------------
 * Driver config + data structs
 * ---------------------------------------------------------------------------
 */

struct uhc_xhci_alif_config {
	/** Controller register base address (0x48200000 from DT). */
	uintptr_t base;
	/** Called during init to connect the SoC interrupt line (IRQ 101). */
	void (*irq_config)(void);
	/**
	 * IRQ number (DT_INST_IRQN), used to mask the event-ring IRQ around
	 * every synchronous command/transfer poll below (uhc_xhci_alif_wait_event())
	 * so the ISR never races a thread-context poll over the shared
	 * event_deq/event_cycle consumer state on this single-core M55.
	 */
	unsigned int irqn;
};

struct uhc_xhci_alif_data {
	/**
	 * MUST be first: the Zephyr UHC subsystem casts dev->data to
	 * struct uhc_data * (uhc_lock_internal / uhc_unlock_internal rely on
	 * this layout).  Driver-private fields follow after common.
	 */
	struct uhc_data common;
	/** Mapped capability register block (used only after bench init). */
	volatile struct xhci_cap_regs *cap;
	/** xhci_core command ring bookkeeping (initialised in *_init). */
	struct xhci_ring cmd_ring;
	/**
	 * Command ring TRB storage: 31 usable TRBs + 1 Link TRB.
	 * 64-byte alignment satisfies the xHCI base-address alignment
	 * requirement (spec §4.9.1 / §6.4.4.1).
	 */
	struct xhci_trb cmd_ring_seg[32] __aligned(64);
	/**
	 * Device Context Base Address Array: slot 0 reserved (spec §6.1) +
	 * 8 device slots.  TODO(aen401-bench): size from HCSPARAMS1[7:0].
	 * 64-byte alignment required by spec §6.1.
	 */
	uint64_t dcbaa[9] __aligned(64);
	/**
	 * Operational-register image built by the host-validated xhci_core path.
	 * TODO(aen401-bench): after HCRST completes + USBSTS.CNR clears, copy these
	 * fields out to the real op-reg block at (cfg->base + CAPLENGTH) with volatile
	 * writes (DCBAAP, CRCR), then set CONFIG.MaxSlotsEn and USBCMD.R/S at enable.
	 */
	struct xhci_op_regs op_image;
	/** Register block bases resolved in *_init (base + CAPLENGTH/RTSOFF/DBOFF). */
	uintptr_t op_base;
	uintptr_t rt_base;
	uintptr_t db_base;
	/** Event ring: 16 TRBs (no Link -- a single ERST segment, spec §4.9.4). */
	struct xhci_trb event_ring_seg[16] __aligned(64);
	/** Event Ring Segment Table: one entry (base + size), 64-byte aligned (§6.5). */
	struct {
		uint32_t base_lo;
		uint32_t base_hi;
		uint32_t size; /* segment size in TRBs (low 16 bits) */
		uint32_t rsvd;
	} erst[1] __aligned(64);
	/** Scratchpad (HCSPARAMS2 says MaxScratchpadBufs=1): a 1-entry pointer array
	 *  (DCBAA[0], §6.6) + one page the xHC owns for internal state. */
	uint64_t scratchpad_array[1] __aligned(64);
	uint8_t  scratchpad_buf[4096] __aligned(4096);
	/*
	 * Enumeration + endpoint contexts: 64-byte contexts (HCCPARAMS1.CSZ=1).
	 * Position layout (spec §6.2.1, Table 6-1): [0]=input-control (input
	 * context only), [1]=slot, [1+dci]=endpoint context for Device Context
	 * Index `dci`.  Sized for XHCI_MAX_DCI (EP0 control + endpoints 1 and 2,
	 * each direction -- e.g. one bulk-only mass-storage interface's OUT+IN
	 * pair) -- a device using endpoint numbers beyond that needs this raised.
	 * All in SRAM0/TCM (DMA-reachable via xhci_l2g()).
	 */
#define XHCI_MAX_DCI 5 /* EP0(1) + EP1 OUT(2)/IN(3) + EP2 OUT(4)/IN(5) */
	uint32_t         input_ctx[(2 + XHCI_MAX_DCI) * 16] __aligned(64);
	uint32_t         device_ctx[(1 + XHCI_MAX_DCI) * 16] __aligned(64);
	struct xhci_trb  ep0_ring[16] __aligned(64);
	struct xhci_ring ep0;
	uint8_t          descriptor_buf[64] __aligned(64);
	/**
	 * Non-control endpoint rings (bulk/interrupt), allocated on first use by
	 * uhc_xhci_alif_ep_ctx_get().  `dci` is the Device Context Index this
	 * slot is bound to once `used`.  ep_enqueue() is synchronous (one
	 * transfer processed to completion per call, spec-legal but
	 * ponytail: no per-endpoint transfer pipelining -- add a real
	 * pending-transfer queue, drained from uhc_xhci_alif_isr(), if a
	 * consumer needs more than one transfer in flight per endpoint).
	 */
	struct {
		uint8_t          used;
		uint8_t          dci;
		struct xhci_ring ring;
		struct xhci_trb  seg[16] __aligned(64);
	} ep_ctx[4]; /* covers every non-EP0 DCI up to XHCI_MAX_DCI (2..5) at once */
	/** Highest DCI configured so far (drives the slot context's
	 * ContextEntries field on each Configure Endpoint command). */
	uint32_t max_dci_configured;
	/**
	 * First-light snapshot: the xHCI capability registers read after the DWC3
	 * host-mode init + xHCI HCRST.  Populated by uhc_xhci_alif_first_light() so a
	 * bench read (SWD or LOG) confirms the controller is alive and reports the
	 * real HCSPARAMS the ring/slot TODOs must be sized from.  fl_ok = the reset
	 * settled (USBSTS.CNR cleared) and HCIVERSION looks sane (>= 0x0100).
	 */
	struct {
		uint32_t magic;     /* 0x58484349 ("XHCI") once populated */
		int32_t  fl_status; /* 0 = ok, negative = reset/probe failure */
		uint8_t  caplength;
		uint16_t hciversion;
		uint32_t hcsparams1; /* [7:0]=MaxSlots [18:8]=MaxIntrs [31:24]=MaxPorts */
		uint32_t hcsparams2;
		uint32_t hccparams1;
		uint32_t dboff;
		uint32_t rtsoff;
		uint32_t usbsts; /* post-reset status snapshot */
		/* Run + No-Op milestone (set in *_enable): run_hch=0 means the
		 * controller started (USBSTS.HCH cleared after USBCMD.R/S); noop_cc is
		 * the completion code of a No-Op command driven round-trip through the
		 * command ring -> doorbell -> event ring (1 = SUCCESS = the ring/event
		 * machinery works end-to-end without any device attached). */
		uint32_t run_hch;
		uint32_t noop_cc;
		uint32_t run_usbsts;
		/* Enable Slot command result + the root-hub port status.  slot_cc=1 +
		 * slot_id>0 means the xHC allocated a device slot (a real command, not a
		 * No-Op, round-tripped).  portsc: CCS(bit0)=a device is attached,
		 * PP(bit9)=port powered, PLS(bits8:5), speed(bits13:10). */
		uint32_t slot_cc;
		uint32_t slot_id;
		uint32_t portsc;
		/* Enumeration results.  enum_stage: 0=no device on port, 1=port reset +
		 * enabled, 2=Address Device SUCCESS, 3=device descriptor read.  desc0/desc1
		 * = the first 8 device-descriptor bytes (bLength, bDescriptorType, bcdUSB,
		 * bDeviceClass/SubClass/Proto, bMaxPacketSize0); idVendor follows at [8]. */
		uint32_t enum_stage;
		uint32_t port_speed;
		uint32_t addr_cc;
		uint32_t xfer_cc;
		uint32_t desc0;
		uint32_t desc1;
	} fl;
	/* Event-ring consumer state (dequeue index + cycle, spec §4.9.4). */
	uint32_t event_deq;
	uint8_t  event_cycle;
	/**
	 * Software gate for uhc_xhci_alif_isr(): set only once *_enable()
	 * finishes arming IMAN.IE/USBCMD.INTE and enabling the NVIC line;
	 * cleared by *_disable() before the NVIC line is masked again.
	 * Defense-in-depth against a spurious/late IRQ landing between
	 * IRQ_CONNECT (at driver init, POST_KERNEL) and the controller
	 * actually being enabled -- the ISR bails immediately if this is
	 * clear instead of touching registers on a not-yet-running
	 * controller.
	 */
	uint8_t armed;
	/**
	 * Set by uhc_xhci_alif_wait_event() when it drains a Port Status
	 * Change event while polling for a different event type (a command
	 * or transfer completion) -- that connect/disconnect notification
	 * would otherwise be silently dropped instead of merely deferred.
	 * Drained by uhc_xhci_alif_handle_port_status() once the caller's
	 * IRQ mask lifts.
	 */
	uint8_t psc_pending;
};

/* ---------------------------------------------------------------------------
 * uhc_api lock / unlock
 *
 * uhc_lock_internal / uhc_unlock_internal are inline helpers in uhc_common.h
 * (Zephyr 4.4 confirmed).  They cast dev->data to struct uhc_data * and
 * wrap k_mutex_lock/unlock on common.mutex -- valid because common is first.
 * ---------------------------------------------------------------------------
 */

static int uhc_xhci_alif_lock(const struct device *dev)
{
	return uhc_lock_internal(dev, K_FOREVER);
}

static int uhc_xhci_alif_unlock(const struct device *dev)
{
	return uhc_unlock_internal(dev);
}

/* ---------------------------------------------------------------------------
 * uhc_api operations
 * ---------------------------------------------------------------------------
 */

/* ---------------------------------------------------------------------------
 * First light: clock + PHY + DWC3 host-mode core init + xHCI reset, then read
 * the capability registers.  Transcribed from the Alif DFP USB device driver
 * (drivers/source/usb/usbd_initialize.c + drivers/include/{usbd.h,sys_ctrl_usb.h})
 * -- the clock/PHY/core-reset/GCTL path is shared device<->host; only
 * GCTL.PrtCapDir differs (host=1).  Per usbd_core_soft_reset(), host mode must
 * NOT assert the DCTL device soft-reset (bit30) -- the xHCI USBCMD.HCRST resets
 * the host block instead.  The exact PHY-POR polarity/settle and FIFO sizing are
 * the fields the TODOs mean by "bench provides values"; the reset+cap-read below
 * is the milestone that proves the controller is reachable and reports the real
 * HCSPARAMS the ring/slot code must size from.
 * ---------------------------------------------------------------------------
 */
static int uhc_xhci_alif_first_light(const struct device *dev)
{
	const struct uhc_xhci_alif_config *cfg  = dev->config;
	struct uhc_xhci_alif_data         *data = dev->data;
	const uintptr_t                    base = cfg->base;
	uint32_t                           reg;
	int                                timeout;

	data->fl.magic     = 0u;
	data->fl.fl_status = -EIO;

	/* 1. Ungate the USB peripheral clock (CLKCTL_PER_MST.PERIPH_CLK_ENA bit20).
	 *    Without this every controller-window read bus-faults (the PL330 lesson). */
	sys_set_bits(CLKCTL_PERIPH_CLK_ENA, CLKCTL_PERIPH_CLK_ENA_USB);

	/* Treat the USB2 root port as PERMANENTLY ATTACHED (USB_CTRL1 bit1).  On this
	 * EVK the DWC3 VBUS-valid sense is not wired to the connector, so the port
	 * never reports a connect even with VBUS + a device present.  Perm-attach
	 * bypasses that so the host can reset + address the physically-connected
	 * device.  A carrier that wires USB2_VBUS to the PHY sense can drop this and
	 * use real connect detection. */
	sys_set_bits(CLKCTL_USB_CTRL1, CLKCTL_USB_CTRL1_PERM_ATTACH);

	/* 2. Release the HS PHY from power-on-reset.  The DFP usbd_initialize() just
	 *    CLEARS USB_CTRL2.PHY_POR (the cold boot leaves it asserted) -- do NOT
	 *    re-assert it: a fresh POR pulse needs a long PLL-relock settle, and the
	 *    earlier set-then-clear left the PHY without a stable clock so the xHCI
	 *    HCRST hung.  Follow with the 5 ms pre-reset settle usbd_initialize uses. */
	reg = sys_read32(CLKCTL_USB_CTRL2);
	reg &= ~(CLKCTL_USB_CTRL2_PHY_POR | CLKCTL_USB_CTRL2_FLADJ_MASK);
	reg |= CLKCTL_USB_CTRL2_FLADJ_30MHZ; /* SoC-wrapper 30MHz adjust (was 0 -- the
	                                      * suspend/power-down clock timing needs it;
	                                      * DWC3 GFLADJ alone is not enough) */
	sys_write32(reg, CLKCTL_USB_CTRL2);
	k_busy_wait(5000);

	/* 2b. Select HOST port capability BEFORE the core comes out of soft-reset, so
	 *     the DWC3 core initialises the host (xHCI) block on reset-release.  DWC3
	 *     databook: GCTL.PrtCapDir must be set going INTO the core reset -- setting
	 *     it after (as usbd, which is device-mode, does) leaves the host block
	 *     unconnected and USBCMD.HCRST never completes. */
	reg = sys_read32(base + DWC3_GCTL);
	reg &= ~DWC3_GCTL_PRTCAPDIR_MASK;
	reg |= DWC3_GCTL_PRTCAPDIR_HOST;
	sys_write32(reg, base + DWC3_GCTL);

	/* 3. DWC3 core + PHY soft-reset, matching the DFP usbd_phy_reset() timing
	 *    EXACTLY: assert core+PHY reset together, hold 50 ms, release the PHY,
	 *    wait another 50 ms for the PHYs to stabilise, THEN release the core.
	 *    Bench (E8, 2026-07-04): the earlier 100 us holds were ~500x too short --
	 *    the core never finished resetting so the xHCI HCRST hung (USBCMD stuck at
	 *    0x02 with USBSTS.CNR already clear). */
	reg = sys_read32(base + DWC3_GCTL);
	reg |= DWC3_GCTL_CORESOFTRESET;
	sys_write32(reg, base + DWC3_GCTL);
	reg = sys_read32(base + DWC3_GUSB2PHYCFG0);
	reg |= DWC3_GUSB2PHYCFG_PHYSOFTRST;
	sys_write32(reg, base + DWC3_GUSB2PHYCFG0);
	k_busy_wait(50000);
	reg = sys_read32(base + DWC3_GUSB2PHYCFG0);
	reg &= ~DWC3_GUSB2PHYCFG_PHYSOFTRST;
	sys_write32(reg, base + DWC3_GUSB2PHYCFG0);
	k_busy_wait(50000);
	reg = sys_read32(base + DWC3_GCTL);
	reg &= ~DWC3_GCTL_CORESOFTRESET;
	sys_write32(reg, base + DWC3_GCTL);

	/* 4. PHY setup (DFP usbd_phy_setup, UTMIW/16-bit path): clear ULPIAutoRes,
	 *    ULPI_UTMI, PHYIF + USBTRDTIM, then set PHYIF=16-bit + USBTRDTIM=16-bit +
	 *    SUSPHY -- matching the E8's 16-bit UTMI+ PHY.  (An 8-bit config froze the
	 *    xHCI core: caps read on APB but HCRST never completed -- bench 2026-07-04.) */
	reg = sys_read32(base + DWC3_GUSB2PHYCFG0);
	reg &=
	    ~(DWC3_GUSB2PHYCFG_ULPIAUTORES | DWC3_GUSB2PHYCFG_ULPI_UTMI | DWC3_GUSB2PHYCFG_PHYIF_MASK |
	      DWC3_GUSB2PHYCFG_USBTRDTIM_MASK | DWC3_GUSB2PHYCFG_SUSPHY);
	reg |= DWC3_GUSB2PHYCFG_PHYIF_16BIT | DWC3_GUSB2PHYCFG_USBTRDTIM_16BIT;
	sys_write32(reg, base + DWC3_GUSB2PHYCFG0);
	/* SUSPHY left CLEARED: during the xHCI HCRST the controller is HALTED, and a
	 * set SUSPHY would let the PHY suspend (stop its clock) so HCRST -- a
	 * core-clock operation -- never completes.  (The DFP sets SUSPHY, but that is
	 * the device path where the controller is running; host reset needs it off.) */

	/* 5. Global control (usbd_setup_global_control): disable clock gating; in HOST
	 *    mode set GUCTL.HSTINAUTORETRY. */
	reg = sys_read32(base + DWC3_GCTL);
	reg |= DWC3_GCTL_DSBLCLKGTNG;
	sys_write32(reg, base + DWC3_GCTL);
	reg = sys_read32(base + DWC3_GUCTL);
	reg |= DWC3_GUCTL_HSTINAUTORETRY;
	sys_write32(reg, base + DWC3_GUCTL);

	/* 6. Frame-length adjustment (usbd_frame_length_adjustment): 30 MHz GFLADJ to
	 *    the default 0x20 with SDBND select, so SOF/ITP timing tracks the ref
	 *    clock -- part of what the host block needs before HCRST can complete. */
	reg = sys_read32(base + DWC3_GFLADJ);
	reg &= ~DWC3_GFLADJ_30MHZ_MASK;
	reg |= DWC3_GFLADJ_30MHZ_SDBND_SEL | DWC3_GFLADJ_30MHZ_DEFAULT;
	sys_write32(reg, base + DWC3_GFLADJ);

	/* 7. AXI burst config (usbd_set_incr_burst_type): undefined-length + INCR16. */
	reg = sys_read32(base + DWC3_GSBUSCFG0);
	reg |= DWC3_GSBUSCFG0_INCRBRSTENA | DWC3_GSBUSCFG0_INCR16BRSTENA;
	sys_write32(reg, base + DWC3_GSBUSCFG0);

	/* 8. Select HOST port capability (usbd_set_mode -> PrtCapDir=host). */
	reg = sys_read32(base + DWC3_GCTL);
	reg &= ~DWC3_GCTL_PRTCAPDIR_MASK;
	reg |= DWC3_GCTL_PRTCAPDIR_HOST;
	sys_write32(reg, base + DWC3_GCTL);
	k_busy_wait(1000);

	/* 5. xHCI host-block reset: USBCMD.HCRST at (base + CAPLENGTH), then poll
	 *    HCRST self-clear + USBSTS.CNR clear (xHCI spec §4.2). */
	uint8_t         caplength  = sys_read8(base);       /* §5.3.1 */
	uint16_t        hciversion = sys_read16(base + 2u); /* §5.3.2 */
	const uintptr_t op         = base + caplength;

	/* Do NOT assert the xHCI USBCMD.HCRST.  On this DWC3 that bit is hardware-
	 * cleared on reset completion but never clears (quirk), and while it stays
	 * asserted the controller is held IN RESET: the operational registers read 0
	 * and drop writes (DCBAAP/CRCR/CONFIG), and USBCMD.R/S is ignored -- so the
	 * controller can never be programmed or run.  The DWC3 core soft-reset done
	 * above already resets the host block; just wait for it to be ready:
	 * USBSTS.CNR clear (xHCI spec §4.2) + HCSPARAMS readable (reads 0 mid-reset). */
	for (timeout = 200000; timeout > 0; timeout--) {
		if ((sys_read32(op + XHCI_OP_USBSTS) & XHCI_USBSTS_CNR) == 0u &&
		    sys_read32(base + 0x04u) != 0u) {
			break;
		}
		k_busy_wait(1);
	}

	/* 6. Snapshot the capability registers (the first-light observable). */
	data->fl.caplength  = caplength;
	data->fl.hciversion = hciversion;
	data->fl.hcsparams1 = sys_read32(base + 0x04u);
	data->fl.hcsparams2 = sys_read32(base + 0x08u);
	data->fl.hccparams1 = sys_read32(base + 0x10u);
	data->fl.dboff      = sys_read32(base + 0x14u);
	data->fl.rtsoff     = sys_read32(base + 0x18u);
	data->fl.usbsts     = sys_read32(op + XHCI_OP_USBSTS);
	data->fl.magic      = 0x58484349u; /* "XHCI" */

	if (timeout == 0) {
		LOG_ERR("xhci: HCRST/CNR did not settle (USBSTS=0x%08x)", data->fl.usbsts);
		data->fl.fl_status = -ETIMEDOUT;
		return -ETIMEDOUT;
	}
	/* Sanity: a live xHCI reports HCIVERSION >= 0x0100 and a nonzero CAPLENGTH. */
	if (caplength == 0u || caplength == 0xFFu || hciversion < 0x0100u) {
		LOG_ERR(
		    "xhci: implausible caps (CAPLENGTH=0x%02x HCIVERSION=0x%04x)", caplength, hciversion);
		data->fl.fl_status = -ENODEV;
		return -ENODEV;
	}
	data->fl.fl_status = 0;
	LOG_INF("xhci first light: CAPLENGTH=0x%02x HCIVERSION=0x%04x "
	        "HCSPARAMS1=0x%08x (MaxSlots=%u MaxPorts=%u) DBOFF=0x%x RTSOFF=0x%x",
	        caplength,
	        hciversion,
	        data->fl.hcsparams1,
	        data->fl.hcsparams1 & 0xFFu,
	        (data->fl.hcsparams1 >> 24) & 0xFFu,
	        data->fl.dboff,
	        data->fl.rtsoff);
	return 0;
}

static int uhc_xhci_alif_init(const struct device *dev)
{
	const struct uhc_xhci_alif_config *cfg  = dev->config;
	struct uhc_xhci_alif_data         *data = dev->data;

	data->cap = (volatile struct xhci_cap_regs *)cfg->base;

	/* First light: bring the DWC3 core to host mode, reset the xHCI block, and
	 * read the capability registers.  A failure here means the controller is not
	 * reachable/clocked -- report it rather than proceeding into the (still
	 * TODO(aen401-bench)) ring/slot/enumeration path on a dead controller. */
	int fl = uhc_xhci_alif_first_light(dev);

	if (fl != 0) {
		return fl;
	}

	/* Resolve the register-block bases now that CAPLENGTH/DBOFF/RTSOFF are known
	 * (op = base + CAPLENGTH §5.4; runtime = base + RTSOFF §5.5; doorbell = base +
	 * DBOFF §5.6).  No MMU on the M55, so VA == PA == bus address for the DMA
	 * structures below (DCBAA/rings/ERST take bus addresses, §6.1/§5.4.8). */
	data->op_base = cfg->base + data->fl.caplength;
	data->rt_base = cfg->base + (data->fl.rtsoff & ~0x1Fu);
	data->db_base = cfg->base + (data->fl.dboff & ~0x3u);

	/* Command ring (31 usable TRBs + 1 Link TRB).  xhci_ring_init sets the Link
	 * TRB to the ring's CPU (local) address -- the xHC follows it via DMA, so
	 * rewrite it to the ring's GLOBAL alias. */
	xhci_ring_init(&data->cmd_ring, data->cmd_ring_seg, ARRAY_SIZE(data->cmd_ring_seg));
	{
		uint64_t         g    = xhci_l2g(data->cmd_ring_seg);
		struct xhci_trb *link = &data->cmd_ring_seg[ARRAY_SIZE(data->cmd_ring_seg) - 1u];

		link->param_lo = (uint32_t)g;
		link->param_hi = (uint32_t)(g >> 32);
	}
	xhci_init_sequence(&data->op_image, 0u, 0u,
			   data->fl.hcsparams1 & 0xFFu /* real MaxSlots (DCBAAP/CRCR
			   * are written with global addresses in *_enable) */);

	/* Scratchpad (§6.6): HCSPARAMS2 says the xHC needs scratchpad pages before it
	 * can run.  DCBAA[0] -> the scratchpad-pointer array (global); array[0] -> one
	 * page (global) the xHC owns. */
	data->scratchpad_array[0] = xhci_l2g(data->scratchpad_buf);
	data->dcbaa[0]            = xhci_l2g(data->scratchpad_array);

	return 0;
}

/* Primary interrupter (interrupter 0) register offsets from the runtime base
 * (xHCI spec §5.5.2): the interrupter array starts at RT + 0x20. */
#define XHCI_IR0          0x20u
#define XHCI_IR_ERSTSZ    0x08u
#define XHCI_IR_ERSTBA_LO 0x10u
#define XHCI_IR_ERSTBA_HI 0x14u
#define XHCI_IR_ERDP_LO   0x18u
#define XHCI_IR_ERDP_HI   0x1Cu

/* Operational register offsets used at enable (spec §5.4). */
#define XHCI_OP_CRCR_LO   0x18u
#define XHCI_OP_CRCR_HI   0x1Cu
#define XHCI_OP_DCBAAP_LO 0x30u
#define XHCI_OP_DCBAAP_HI 0x34u
#define XHCI_OP_CONFIG    0x38u
#define XHCI_OP_PORTSC(p) (0x400u + (p) * 0x10u) /* port 0 PORTSC (§5.4.8) */
#define XHCI_ERDP_EHB     (1u << 3)              /* Event Handler Busy (write-1-clear) */

/* xHCI endpoint-context Endpoint Type values (spec Table 6-9). */
#define XHCI_EP_TYPE_BULK_OUT 2u
#define XHCI_EP_TYPE_CONTROL  4u
#define XHCI_EP_TYPE_BULK_IN  6u
#define XHCI_EP_TYPE_INT_OUT  3u
#define XHCI_EP_TYPE_INT_IN   7u

/* Speed the driver maps a USB 2.0-only xHCI port to a UHC connect event.
 * uhc_event_type has no Super-Speed entry (this controller is DWC3
 * host-mode, USB2 PHY only), so an unexpected code is logged and reported
 * as FS rather than silently mis-mapped to LS/HS. */
static enum uhc_event_type uhc_xhci_alif_speed_to_connect_evt(uint32_t speed)
{
	switch (speed) {
	case 1u:
		return UHC_EVT_DEV_CONNECTED_FS;
	case 2u:
		return UHC_EVT_DEV_CONNECTED_LS;
	case 3u:
		return UHC_EVT_DEV_CONNECTED_HS;
	default:
		LOG_WRN("xhci: unexpected port speed id %u, reporting FS", speed);
		return UHC_EVT_DEV_CONNECTED_FS;
	}
}

/* Report a Port Status Change to the UHC layer and RW1C the PORTSC change
 * bits it represents (spec §5.4.8) -- write back the value exactly as read:
 * every set RW1C bit clears, every control bit (PP, PLS, ...) is written
 * back unchanged.  Leaving those bits set would starve a later, genuine
 * change of its own event on some implementations, and (via
 * uhc_xhci_alif_irq_unmask()) is also how a Port Status Change event that
 * arrived while a synchronous command/transfer wait had the IRQ masked
 * (and so got silently drained+skipped by uhc_xhci_alif_wait_event()) still
 * reaches the host stack instead of being lost. */
static void uhc_xhci_alif_handle_port_status(const struct device *dev)
{
	struct uhc_xhci_alif_data *data   = dev->data;
	uint32_t                   portsc = sys_read32(data->op_base + XHCI_OP_PORTSC(0));
	enum uhc_event_type        t;

	if ((portsc & XHCI_PORTSC_CCS) != 0u) {
		t = uhc_xhci_alif_speed_to_connect_evt((portsc & XHCI_PORTSC_SPEED_MASK) >>
		                                       XHCI_PORTSC_SPEED_SHIFT);
	} else {
		t = UHC_EVT_DEV_REMOVED;
	}
	sys_write32(portsc, data->op_base + XHCI_OP_PORTSC(0));
	uhc_submit_event(dev, t, 0);
}

static unsigned int uhc_xhci_alif_irqn(const struct device *dev)
{
	return ((const struct uhc_xhci_alif_config *)dev->config)->irqn;
}

/* Mask/unmask the controller's NVIC line around every enqueue+doorbell+wait
 * critical section below: the same event_deq/event_cycle consumer state (and,
 * once IMAN.IE is armed by *_enable(), uhc_xhci_alif_isr()) must never be
 * touched by both a synchronous poll and the ISR at once on this single-core
 * M55.  Masking has to happen BEFORE the TRB is enqueued and the doorbell is
 * rung -- ringing the doorbell first leaves a window where the ISR (already
 * armed) can steal the very completion event the poll is about to wait for,
 * so every caller below masks first, then builds+enqueues the TRB(s), then
 * rings the doorbell, then waits. */
static void uhc_xhci_alif_irq_mask(const struct device *dev)
{
	irq_disable(uhc_xhci_alif_irqn(dev));
}

static void uhc_xhci_alif_irq_unmask(const struct device *dev)
{
	struct uhc_xhci_alif_data *data = dev->data;

	irq_enable(uhc_xhci_alif_irqn(dev));
	if (data->psc_pending) {
		data->psc_pending = 0u;
		uhc_xhci_alif_handle_port_status(dev);
	}
}

/* Consume the next event of `want` type from the event ring, advancing the
 * dequeue pointer + ERDP (spec §4.9.4).  ASSUMES the caller already masked
 * the controller IRQ (uhc_xhci_alif_irq_mask()) before enqueuing the TRB and
 * ringing the doorbell -- this function only polls/drains, it does not touch
 * the IRQ itself.
 *
 * A Port Status Change seen while waiting for something else is latched
 * (data->psc_pending) rather than silently dropped -- uhc_xhci_alif_irq_unmask()
 * reports it once the caller's critical section ends.  Any OTHER event type
 * seen first is logged and dropped (nobody is waiting for it).
 *
 * `match_ptr`, if non-zero, is the global address of the TRB this wait is
 * for (the command or transfer TRB the caller just enqueued).  An event of
 * `want`'s type whose own TRB Pointer field doesn't match is a STALE
 * completion -- e.g. the eventual (very late) completion of an earlier
 * operation this same function already gave up on after a timeout -- and is
 * skipped rather than wrongly accepted as this wait's result.
 *
 * Returns the completion code (0 -- an otherwise-unused/"Invalid" code per
 * spec Table 6-90 -- on timeout, doubling as the timeout sentinel); *slot_id
 * gets the event's Slot ID, *residual (if non-NULL) gets the Transfer
 * Event's untransferred byte count (0 for a full transfer / non-Transfer
 * Event). */
static uint8_t uhc_xhci_alif_wait_event(const struct device *dev,
                                        uint8_t              want,
                                        uint64_t             match_ptr,
                                        uint8_t             *slot_id,
                                        uint32_t            *residual)
{
	struct uhc_xhci_alif_data *data    = dev->data;
	const uintptr_t            ir      = data->rt_base + XHCI_IR0;
	int                        timeout = 500000;
	uint8_t                    cc      = 0u;

	if (slot_id != NULL) {
		*slot_id = 0u;
	}
	if (residual != NULL) {
		*residual = 0u;
	}
	while (timeout-- > 0) {
		volatile struct xhci_trb *evt = &data->event_ring_seg[data->event_deq];

		xhci_alif_invd(evt, sizeof(*evt));
		if ((evt->control & XHCI_TRB_CYCLE) != (uint32_t)data->event_cycle) {
			k_busy_wait(1);
			continue;
		}
		uint8_t  etype   = XHCI_TRB_GET_TYPE(evt->control);
		uint8_t  got_cc  = XHCI_TRB_GET_CC(evt->status);
		uint8_t  slot    = XHCI_TRB_GET_SLOT(evt->control);
		uint32_t resid   = XHCI_TRB_GET_RESIDUAL(evt->status);
		uint64_t evt_ptr = ((uint64_t)evt->param_hi << 32) | evt->param_lo;

		data->event_deq++;
		if (data->event_deq >= ARRAY_SIZE(data->event_ring_seg)) {
			data->event_deq = 0u;
			data->event_cycle ^= 1u;
		}
		uint64_t erdp = xhci_l2g(&data->event_ring_seg[data->event_deq]);

		sys_write32((uint32_t)erdp | XHCI_ERDP_EHB, ir + XHCI_IR_ERDP_LO);
		sys_write32((uint32_t)(erdp >> 32), ir + XHCI_IR_ERDP_HI);
		if (etype == XHCI_TRB_TYPE_PORT_STATUS) {
			data->psc_pending = 1u;
			continue;
		}
		if (etype == want) {
			if (match_ptr != 0u && evt_ptr != match_ptr) {
				continue; /* stale completion -- not this wait's TRB */
			}
			if (slot_id != NULL) {
				*slot_id = slot;
			}
			if (residual != NULL) {
				*residual = resid;
			}
			cc = got_cc;
			break;
		}
		LOG_WRN("xhci: unclaimed event type %u while waiting for %u", etype, want);
	}
	return cc;
}

/* Enqueue a command TRB, ring DB[0], wait for its Command Completion Event
 * (matched by TRB pointer -- finding: a timed-out command's late completion
 * must never be mistaken for a later command's).  On timeout, abort the
 * command ring (spec §5.4.5 CRCR.CA) so the aborted command can't leave an
 * armed TRB a later submit_cmd() might otherwise race. */
static uint8_t
uhc_xhci_alif_submit_cmd(const struct device *dev, const struct xhci_trb *cmd, uint8_t *slot_id)
{
	struct uhc_xhci_alif_data *data       = dev->data;
	uint32_t                   enq_before = data->cmd_ring.enqueue;
	uint64_t                   trb_g;
	uint8_t                    cc;

	uhc_xhci_alif_irq_mask(dev);
	trb_g = xhci_l2g(&data->cmd_ring_seg[enq_before]);
	xhci_ring_enqueue(&data->cmd_ring, cmd);
	xhci_alif_flush(data->cmd_ring_seg, sizeof(data->cmd_ring_seg));
	sys_write32(0u, data->db_base); /* DB[0] = command doorbell */
	cc = uhc_xhci_alif_wait_event(dev, XHCI_TRB_TYPE_CMD_COMPLETION, trb_g, slot_id, NULL);
	if (cc == 0u) {
		uintptr_t op = data->op_base;
		int       t;

		LOG_WRN("xhci: command timed out, aborting the command ring");
		sys_write32(sys_read32(op + XHCI_OP_CRCR_LO) | XHCI_CRCR_CA, op + XHCI_OP_CRCR_LO);
		for (t = 100000; t > 0; t--) {
			if ((sys_read32(op + XHCI_OP_CRCR_LO) & XHCI_CRCR_CRR) == 0u) {
				break;
			}
			k_busy_wait(1);
		}
	}
	uhc_xhci_alif_irq_unmask(dev);
	return cc;
}

/* Set TR Dequeue Pointer (spec §4.6.10 / §6.4.3.9): resync the xHC's hardware
 * dequeue position on `ring`'s DCI to the driver's own producer (enqueue)
 * slot, with the matching Dequeue Cycle State, so the NEXT transfer on this
 * endpoint starts from where software actually is instead of wherever a
 * halted/stopped/timed-out ring was left. */
static void uhc_xhci_alif_set_tr_dequeue(const struct device *dev,
                                         uint8_t              slot,
                                         uint8_t              dci,
                                         struct xhci_ring    *ring)
{
	struct xhci_trb trb   = { 0 };
	uint64_t        deq_g = xhci_l2g(&ring->seg[ring->enqueue]);

	trb.param_lo = (uint32_t)(deq_g & ~0xFu) | (ring->cycle ? 1u : 0u); /* DCS */
	trb.param_hi = (uint32_t)(deq_g >> 32);
	trb.control =
	    XHCI_TRB_TYPE(XHCI_TRB_TYPE_SET_TR_DEQUEUE) | XHCI_SLOT_ID(slot) | XHCI_EP_ID(dci);
	(void)uhc_xhci_alif_submit_cmd(dev, &trb, NULL);
}

/* Halted-endpoint recovery (spec §4.10.2.1): a STALL, transaction error, or
 * babble leaves an endpoint Halted, where the xHC accepts no further
 * transfers until software issues Reset Endpoint (clears Halted) followed
 * by Set TR Dequeue Pointer. */
static void uhc_xhci_alif_ep_reset_halted(const struct device *dev,
                                          uint8_t              slot,
                                          uint8_t              dci,
                                          struct xhci_ring    *ring)
{
	struct xhci_trb trb = { 0 };

	trb.control =
	    XHCI_TRB_TYPE(XHCI_TRB_TYPE_RESET_ENDPOINT) | XHCI_SLOT_ID(slot) | XHCI_EP_ID(dci);
	(void)uhc_xhci_alif_submit_cmd(dev, &trb, NULL);
	uhc_xhci_alif_set_tr_dequeue(dev, slot, dci, ring);
}

/* Timeout recovery: uhc_xhci_alif_wait_event() gave up on a transfer that
 * may still complete later on the hardware.  Stop Endpoint halts it
 * deterministically, then Set TR Dequeue Pointer resyncs the hardware
 * dequeue to our software enqueue slot so the next transfer on this
 * endpoint doesn't inherit the abandoned TRB. */
static void uhc_xhci_alif_ep_stop_and_resync(const struct device *dev,
                                             uint8_t              slot,
                                             uint8_t              dci,
                                             struct xhci_ring    *ring)
{
	struct xhci_trb trb = { 0 };

	trb.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_STOP_ENDPOINT) | XHCI_SLOT_ID(slot) | XHCI_EP_ID(dci);
	(void)uhc_xhci_alif_submit_cmd(dev, &trb, NULL);
	uhc_xhci_alif_set_tr_dequeue(dev, slot, dci, ring);
}

/* Address (or re-Address) the device on root-hub port 0's slot: build the
 * input slot+EP0 contexts and submit Address Device (spec §4.6.5).  Shared
 * by uhc_xhci_alif_enumerate() (first addressing, during *_enable()) and
 * uhc_xhci_alif_bus_reset() (re-addressing after a PORTSC.PR the usbh stack
 * requested on an already-addressed device) so both stay in lock-step. */
static uint8_t
uhc_xhci_alif_address_device(const struct device *dev, uint8_t slot, uint32_t speed, uint32_t mps)
{
	struct uhc_xhci_alif_data *data = dev->data;
	struct xhci_trb            addr = { 0 };
	uint64_t                   ing;

	xhci_ring_init(&data->ep0, data->ep0_ring, ARRAY_SIZE(data->ep0_ring));
	{
		uint64_t         g    = xhci_l2g(data->ep0_ring);
		struct xhci_trb *link = &data->ep0_ring[ARRAY_SIZE(data->ep0_ring) - 1u];

		link->param_lo = (uint32_t)g;
		link->param_hi = (uint32_t)(g >> 32);
	}

	memset(data->input_ctx, 0, sizeof(data->input_ctx));
	memset(data->device_ctx, 0, sizeof(data->device_ctx));
	data->input_ctx[1]  = 0x3u; /* Add flags: A0 (slot) | A1 (EP0) */
	data->input_ctx[16] = (speed << 20) | (1u << 27);
	data->input_ctx[17] = (1u << 16); /* root-hub port = 1 */
	xhci_build_ep_context(
	    &data->input_ctx[32], XHCI_EP_TYPE_CONTROL, mps, xhci_l2g(data->ep0_ring), 1, 0u, mps);
	data->dcbaa[slot] = xhci_l2g(data->device_ctx);
	memset(data->ep_ctx, 0, sizeof(data->ep_ctx));
	data->max_dci_configured = 1u; /* EP0 only so far */

	xhci_alif_flush(data->input_ctx, sizeof(data->input_ctx));
	xhci_alif_flush(data->device_ctx, sizeof(data->device_ctx));
	xhci_alif_flush(data->dcbaa, sizeof(data->dcbaa));

	ing           = xhci_l2g(data->input_ctx);
	addr.param_lo = (uint32_t)ing;
	addr.param_hi = (uint32_t)(ing >> 32);
	addr.control  = XHCI_TRB_TYPE(XHCI_TRB_TYPE_ADDRESS_DEVICE) | XHCI_SLOT_ID(slot);
	return uhc_xhci_alif_submit_cmd(dev, &addr, NULL);
}

/* Enumerate the device on root-hub port 0 (spec §4.3): reset the port, Address
 * Device (build the input slot+EP0 contexts, slot already Enabled), then a
 * control GET_DESCRIPTOR(device, 8) over EP0 -- the core of USB enumeration.
 * Records how far it got + the first descriptor bytes in fl.  No-op if no device
 * is attached (PORTSC.CCS=0). */
static void uhc_xhci_alif_enumerate(const struct device *dev, uintptr_t op)
{
	struct uhc_xhci_alif_data *data = dev->data;
	const uint8_t              slot = (uint8_t)data->fl.slot_id;
	uint32_t                   portsc;
	int                        t;

	/* Wait briefly for a connect (PORTSC.CCS).  On this EVK VBUS-valid is not
	 * sensed by the PHY, so CCS may never set even with a device present -- we run
	 * with PERM_ATTACH (USB_CTRL1) and proceed with the port reset + Address
	 * Device regardless; the physically-connected device responds. */
	for (t = 2000; t > 0; t--) {
		portsc = sys_read32(op + XHCI_OP_PORTSC(0));
		if (portsc & 1u) { /* CCS */
			break;
		}
		k_busy_wait(1000);
	}
	portsc          = sys_read32(op + XHCI_OP_PORTSC(0));
	data->fl.portsc = portsc;
	if (slot == 0u) {
		data->fl.enum_stage = 0u;
		return;
	}

	/* 1. Reset the port (PORTSC.PR).  Preserve PP, do NOT write-1-clear the change
	 *    bits [23:17] or PED [1].  Wait for PRC (reset complete), read the speed. */
	sys_write32((portsc & ~((0x7Fu << 17) | (1u << 1))) | (1u << 4) | (1u << 9),
	            op + XHCI_OP_PORTSC(0));
	for (t = 500000; t > 0; t--) {
		portsc = sys_read32(op + XHCI_OP_PORTSC(0));
		if (portsc & (1u << 21)) { /* PRC: Port Reset Change */
			break;
		}
		k_busy_wait(1);
	}
	data->fl.port_speed = (portsc >> 10) & 0xFu;
	data->fl.enum_stage = 1u;

	uint32_t speed = data->fl.port_speed;

	if (speed == 0u) {
		speed = 1u; /* PERM_ATTACH: port trained no speed -> assume Full Speed (the
			     * FTDI is FS) so the slot context is valid + Address Device
			     * passes; the EP0 transfer then proves the D+/D- path. */
	}
	/* Initial EP0 max-packet-size (xHCI port-speed IDs: 1=FS 2=LS 3=HS 4=SS).
	 * HS EP0 is always 64; FS/LS use 8 for the first 8-byte GET_DESCRIPTOR (the
	 * real bMaxPacketSize0 is byte 7 of that descriptor, applied on a later
	 * Evaluate Context).  SS would be 512, but this port is USB2 (FS device). */
	uint32_t mps = (speed == 3u) ? 64u : (speed == 4u) ? 512u : 8u;

	/* 2+3+4. EP0 ring + input context + Address Device command. */
	data->fl.addr_cc = uhc_xhci_alif_address_device(dev, slot, speed, mps);
	if (data->fl.addr_cc != XHCI_CC_SUCCESS) {
		return;
	}
	data->fl.enum_stage = 2u;

	/* 5. GET_DESCRIPTOR(device, 8) over EP0: Setup (immediate data) + Data-IN +
	 *    Status-OUT stage TRBs, ring DB[slot] target EP0 (DCI 1), wait for the
	 *    Data stage's own Transfer Event (matched by TRB pointer -- its residual
	 *    is the real byte count; the Status stage's Transfer Event carries no
	 *    residual and would misreport a short read as a full one if used
	 *    instead).  Setup packet 80 06 00 01 00 00 08 00. */
	memset(data->descriptor_buf, 0, sizeof(data->descriptor_buf));

	uint32_t        enq_setup = data->ep0.enqueue;
	struct xhci_trb setup     = { 0 };

	setup.param_lo = 0x01000680u; /* bmReqType=80 bReq=06 wValue=0100 */
	setup.param_hi = 0x00080000u; /* wIndex=0000 wLength=0008 */
	setup.status   = 8u;
	setup.control  = XHCI_TRB_TYPE(XHCI_TRB_TYPE_SETUP) | XHCI_TRB_IDT | XHCI_TRB_TRT_IN;

	uint32_t        enq_dstage = xhci_ring_next_index(&data->ep0, enq_setup);
	struct xhci_trb dstage     = { 0 };
	uint64_t        bufg       = xhci_l2g(data->descriptor_buf);

	dstage.param_lo = (uint32_t)bufg;
	dstage.param_hi = (uint32_t)(bufg >> 32);
	dstage.status   = 8u;
	dstage.control  = XHCI_TRB_TYPE(XHCI_TRB_TYPE_DATA) | XHCI_TRB_DIR_IN | XHCI_TRB_IOC;

	uint32_t        enq_sstage = xhci_ring_next_index(&data->ep0, enq_dstage);
	struct xhci_trb sstage     = { 0 };

	sstage.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_STATUS) | XHCI_TRB_IOC;

	uint64_t dstage_g = xhci_l2g(&data->ep0_ring[enq_dstage]);
	uint64_t sstage_g = xhci_l2g(&data->ep0_ring[enq_sstage]);
	uint8_t  xfer_cc;

	uhc_xhci_alif_irq_mask(dev);
	xhci_ring_enqueue(&data->ep0, &setup);
	xhci_ring_enqueue(&data->ep0, &dstage);
	xhci_ring_enqueue(&data->ep0, &sstage);
	xhci_alif_flush(data->descriptor_buf, sizeof(data->descriptor_buf)); /* IN: flush
		before the doorbell too, or a dirty line's later write-back could
		clobber what the xHC DMA's in */
	xhci_alif_flush(data->ep0_ring, sizeof(data->ep0_ring));
	sys_write32(1u, data->db_base + (uint32_t)slot * 4u); /* DB[slot] EP0 = DCI 1 */
	xfer_cc = uhc_xhci_alif_wait_event(dev, XHCI_TRB_TYPE_TRANSFER_EVENT, dstage_g, NULL, NULL);
	if (xfer_cc == XHCI_CC_SUCCESS || xfer_cc == XHCI_CC_SHORT_PACKET) {
		/* Drain the Status stage's own completion too, so a later wait
		 * never mistakes it for something else's event. */
		(void)uhc_xhci_alif_wait_event(dev, XHCI_TRB_TYPE_TRANSFER_EVENT, sstage_g, NULL, NULL);
	}
	uhc_xhci_alif_irq_unmask(dev);
	data->fl.xfer_cc = xfer_cc;

	xhci_alif_invd(data->descriptor_buf, sizeof(data->descriptor_buf));
	data->fl.desc0 = sys_read32((uintptr_t)&data->descriptor_buf[0]);
	data->fl.desc1 = sys_read32((uintptr_t)&data->descriptor_buf[4]);
	if (data->fl.xfer_cc == XHCI_CC_SUCCESS) {
		data->fl.enum_stage = 3u;
	}
}

static int uhc_xhci_alif_enable(const struct device *dev)
{
	const struct uhc_xhci_alif_config *cfg  = dev->config;
	struct uhc_xhci_alif_data         *data = dev->data;
	const uintptr_t                    op   = data->op_base;
	const uintptr_t                    rt   = data->rt_base;
	const uintptr_t                    ir   = rt + XHCI_IR0;
	int                                timeout;

	/* Re-init the command ring's software producer state + drop any
	 * endpoint-ring bindings from a previous *_enable() session.
	 * *_disable() already told the xHC to forget the old slot (Disable
	 * Slot) -- without this, a disable()->enable() cycle would reuse a
	 * stale producer index/cycle against the freshly-programmed CRCR
	 * below (hardware always starts consuming at index 0/cycle 1), and
	 * uhc_xhci_alif_ep_ctx_get() could hand out a ring still marked
	 * `used` for a device slot ID the xHC no longer recognises. */
	xhci_ring_init(&data->cmd_ring, data->cmd_ring_seg, ARRAY_SIZE(data->cmd_ring_seg));
	{
		uint64_t         g    = xhci_l2g(data->cmd_ring_seg);
		struct xhci_trb *link = &data->cmd_ring_seg[ARRAY_SIZE(data->cmd_ring_seg) - 1u];

		link->param_lo = (uint32_t)g;
		link->param_hi = (uint32_t)(g >> 32);
	}
	memset(data->ep_ctx, 0, sizeof(data->ep_ctx));
	data->max_dci_configured = 0u;

	/* 1. Program the operational registers (spec §4.2): DCBAAP, CRCR (RCS=1),
	 *    CONFIG.MaxSlotsEn -- all with GLOBAL (DMA-reachable) addresses. */
	uint64_t dcbaa_g = xhci_l2g(data->dcbaa);
	uint64_t cmdr_g  = xhci_l2g(data->cmd_ring_seg);

	xhci_alif_flush(data->dcbaa, sizeof(data->dcbaa));
	xhci_alif_flush(data->cmd_ring_seg, sizeof(data->cmd_ring_seg));
	sys_write32((uint32_t)(dcbaa_g & 0xFFFFFFC0u), op + XHCI_OP_DCBAAP_LO);
	sys_write32((uint32_t)(dcbaa_g >> 32), op + XHCI_OP_DCBAAP_HI);
	sys_write32((uint32_t)(cmdr_g & 0xFFFFFFC0u) | XHCI_CRCR_RCS, op + XHCI_OP_CRCR_LO);
	sys_write32((uint32_t)(cmdr_g >> 32), op + XHCI_OP_CRCR_HI);
	sys_write32(data->op_image.config, op + XHCI_OP_CONFIG);

	/* 2. Event ring: one ERST segment -> event_ring_seg (global); program the
	 *    primary interrupter's ERSTSZ / ERSTBA / ERDP (spec §4.9.4, §5.5.2). */
	uint64_t evtr_g = xhci_l2g(data->event_ring_seg);
	uint64_t erst_g = xhci_l2g(data->erst);

	memset(data->event_ring_seg, 0, sizeof(data->event_ring_seg));
	data->erst[0].base_lo = (uint32_t)evtr_g;
	data->erst[0].base_hi = (uint32_t)(evtr_g >> 32);
	data->erst[0].size    = ARRAY_SIZE(data->event_ring_seg);
	data->erst[0].rsvd    = 0u;
	xhci_alif_flush(data->event_ring_seg, sizeof(data->event_ring_seg));
	xhci_alif_flush(data->erst, sizeof(data->erst));
	sys_write32(1u, ir + XHCI_IR_ERSTSZ); /* 1 segment */
	sys_write32((uint32_t)evtr_g, ir + XHCI_IR_ERDP_LO);
	sys_write32((uint32_t)(evtr_g >> 32), ir + XHCI_IR_ERDP_HI);
	sys_write32((uint32_t)erst_g, ir + XHCI_IR_ERSTBA_LO);
	sys_write32((uint32_t)(erst_g >> 32), ir + XHCI_IR_ERSTBA_HI);

	/* 3. Run: set USBCMD.R/S, wait for USBSTS.HCH to clear (controller running). */
	sys_write32(sys_read32(op + XHCI_OP_USBCMD) | XHCI_USBCMD_RS, op + XHCI_OP_USBCMD);
	for (timeout = 100000; timeout > 0; timeout--) {
		if ((sys_read32(op + XHCI_OP_USBSTS) & XHCI_USBSTS_HCH) == 0u) {
			break;
		}
		k_busy_wait(1);
	}
	data->fl.run_hch = sys_read32(op + XHCI_OP_USBSTS) & XHCI_USBSTS_HCH;

	/* Event-ring consumer starts at index 0, cycle 1 (the xHC writes the first
	 * event with cycle=1). */
	data->event_deq   = 0u;
	data->event_cycle = 1u;

	/* 4. No-Op command: command ring -> doorbell -> event ring round-trip (no
	 *    device needed). */
	struct xhci_trb noop = { 0 };

	noop.control     = XHCI_TRB_TYPE(XHCI_TRB_TYPE_NOOP_CMD);
	data->fl.noop_cc = uhc_xhci_alif_submit_cmd(dev, &noop, NULL);

	/* 5. Enable Slot command: a REAL command -- the xHC allocates a device slot
	 *    and returns its Slot ID in the completion event (still no device needed;
	 *    the slot is provisioned for the device we would then Address). */
	struct xhci_trb en_slot = { 0 };
	uint8_t         slot_id = 0u;

	en_slot.control  = XHCI_TRB_TYPE(XHCI_TRB_TYPE_ENABLE_SLOT);
	data->fl.slot_cc = uhc_xhci_alif_submit_cmd(dev, &en_slot, &slot_id);
	data->fl.slot_id = slot_id;

	/* 6. Root-hub port: apply power (PORTSC.PP, bit 9) and snapshot the status
	 *    (CCS bit0 = a device is attached; PLS bits8:5; speed bits13:10). */
	uint32_t portsc = sys_read32(op + XHCI_OP_PORTSC(0));

	sys_write32(portsc | (1u << 9), op + XHCI_OP_PORTSC(0));
	k_busy_wait(100000); /* let PP settle + connect debounce */
	data->fl.portsc = sys_read32(op + XHCI_OP_PORTSC(0));

	/* 7. If a device is attached (PORTSC.CCS), enumerate it: port reset ->
	 *    Address Device -> control GET_DESCRIPTOR over EP0. */
	uhc_xhci_alif_enumerate(dev, op);

	data->fl.run_usbsts = sys_read32(op + XHCI_OP_USBSTS);

	LOG_INF("xhci: run HCH=%u No-Op=%u Slot cc=%u id=%u PORTSC=0x%08x",
	        data->fl.run_hch,
	        data->fl.noop_cc,
	        data->fl.slot_cc,
	        data->fl.slot_id,
	        data->fl.portsc);
	LOG_INF("xhci enum: stage=%u speed=%u Addr cc=%u Xfer cc=%u desc=%08x %08x",
	        data->fl.enum_stage,
	        data->fl.port_speed,
	        data->fl.addr_cc,
	        data->fl.xfer_cc,
	        data->fl.desc0,
	        data->fl.desc1);

	/* 8. Steady state: arm this interrupter (IMAN.IE) + the controller-level
	 * interrupter enable (USBCMD.INTE) so a later connect/disconnect on an
	 * otherwise-idle root port reaches uhc_xhci_alif_isr() instead of needing
	 * a poller (the synchronous bring-up above intentionally ran with both
	 * clear, so it never raced uhc_xhci_alif_isr() over the shared
	 * event_deq/event_cycle consumer state -- see uhc_xhci_alif_wait_event()). */
	sys_write32(sys_read32(ir) | XHCI_IMAN_IE, ir);
	sys_write32(sys_read32(op + XHCI_OP_USBCMD) | XHCI_USBCMD_INTE, op + XHCI_OP_USBCMD);

	/* Arm the ISR gate + the NVIC line LAST, only once the controller is
	 * actually running and the interrupter is armed -- irq_config() at
	 * driver init only connects the vector, it never enables it (finding:
	 * the NVIC line must not be live before there is anything to service). */
	data->armed = 1u;
	irq_enable(cfg->irqn);

	/* If a device is already attached (PORTSC.CCS), tell the host stack now:
	 * uhc_xhci_alif_enumerate() above only updates this driver's own fl.*
	 * bench snapshot, it never reports a UHC connect event, so without this
	 * the usbh layer above would never learn a device is present on an
	 * otherwise-idle root port (no later PORTSC change is coming to trigger
	 * uhc_xhci_alif_isr() for it). */
	if ((data->fl.portsc & XHCI_PORTSC_CCS) != 0u) {
		uhc_submit_event(dev, uhc_xhci_alif_speed_to_connect_evt(data->fl.port_speed), 0);
	}

	/* Controller-level bring-up PASS = running + command/event ring proven (No-Op
	 * + Enable Slot).  Enumeration (enum_stage 3) only completes with a device on
	 * the port; report it separately in fl. */
	return (data->fl.run_hch == 0u && data->fl.noop_cc == XHCI_CC_SUCCESS &&
	        data->fl.slot_cc == XHCI_CC_SUCCESS && data->fl.slot_id != 0u)
	           ? 0
	           : -EIO;
}

/* Find the endpoint-ring slot already bound to `dci`, or configure a new one
 * (Configure Endpoint Command, spec §4.6.6) if none exists yet.  On failure,
 * returns NULL and sets *err to a code that distinguishes *why* (checked by
 * the caller instead of always reporting -ENOMEM): -ENODEV (slot not
 * addressed / dci out of range), -ENOMEM (endpoint pool exhausted), -EIO
 * (Configure Endpoint command itself failed -- cc is logged).
 *
 * ponytail: XHCI_MAX_DCI + ARRAY_SIZE(data->ep_ctx) bound the endpoints this
 * driver can track per device to control (EP0) + 3 more -- enough for one
 * bulk-only mass-storage interface (BOT: EP0 + one bulk OUT + one bulk IN).
 * A device needing more concurrently-active endpoints needs both raised.
 * UNVERIFIED ON SILICON: this whole command path (never bench-run). */
static struct xhci_ring *uhc_xhci_alif_ep_ctx_get(const struct device *dev,
                                                  uint8_t              dci,
                                                  uint32_t             ep_type,
                                                  uint16_t             mps,
                                                  uint16_t             interval,
                                                  int                 *err)
{
	struct uhc_xhci_alif_data *data = dev->data;
	uint8_t                    slot = (uint8_t)data->fl.slot_id;
	size_t                     i, free_i = ARRAY_SIZE(data->ep_ctx);
	uint64_t                   ring_g, ing;
	uint32_t                   entries;
	uint8_t                    cc;
	struct xhci_trb           *link;
	struct xhci_trb            cfg_trb = { 0 };

	*err = 0;
	if (slot == 0u || dci == 0u || dci > XHCI_MAX_DCI) {
		*err = -ENODEV;
		return NULL;
	}
	for (i = 0; i < ARRAY_SIZE(data->ep_ctx); i++) {
		if (data->ep_ctx[i].used && data->ep_ctx[i].dci == dci) {
			return &data->ep_ctx[i].ring;
		}
		if (!data->ep_ctx[i].used && free_i == ARRAY_SIZE(data->ep_ctx)) {
			free_i = i;
		}
	}
	if (free_i == ARRAY_SIZE(data->ep_ctx)) {
		*err = -ENOMEM; /* pool exhausted */
		return NULL;
	}

	xhci_ring_init(
	    &data->ep_ctx[free_i].ring, data->ep_ctx[free_i].seg, ARRAY_SIZE(data->ep_ctx[free_i].seg));
	ring_g         = xhci_l2g(data->ep_ctx[free_i].seg);
	link           = &data->ep_ctx[free_i].seg[ARRAY_SIZE(data->ep_ctx[free_i].seg) - 1u];
	link->param_lo = (uint32_t)ring_g;
	link->param_hi = (uint32_t)(ring_g >> 32);

	/* New endpoint: Configure Endpoint (add flags A0|A(dci), an updated
	 * slot ContextEntries, and the new EP's context) so the xHC accepts
	 * transfers on it (spec §4.6.6). */
	entries = (dci > data->max_dci_configured) ? dci : data->max_dci_configured;
	memset(data->input_ctx, 0, sizeof(data->input_ctx));
	data->input_ctx[1] = (1u << 0) | (1u << dci); /* Add flags: A0 (slot) | A(dci) */
	xhci_build_slot_context(&data->input_ctx[16],
	                        0u /* route: root-hub attached */,
	                        data->fl.port_speed ? data->fl.port_speed : 1u,
	                        entries);
	data->input_ctx[17] = (1u << 16); /* root-hub port = 1 */
	xhci_build_ep_context(
	    &data->input_ctx[(1u + dci) * 16u], ep_type, mps, ring_g, 1, interval, mps);
	xhci_alif_flush(data->input_ctx, sizeof(data->input_ctx));

	ing              = xhci_l2g(data->input_ctx);
	cfg_trb.param_lo = (uint32_t)ing;
	cfg_trb.param_hi = (uint32_t)(ing >> 32);
	cfg_trb.control  = XHCI_TRB_TYPE(XHCI_TRB_TYPE_CONFIGURE_ENDPOINT) | XHCI_SLOT_ID(slot);
	cc               = uhc_xhci_alif_submit_cmd(dev, &cfg_trb, NULL);
	if (cc != XHCI_CC_SUCCESS) {
		LOG_WRN("xhci: Configure Endpoint (dci=%u) failed, cc=%u", dci, cc);
		*err = -EIO;
		return NULL;
	}

	data->max_dci_configured  = entries;
	data->ep_ctx[free_i].used = 1u;
	data->ep_ctx[free_i].dci  = dci;
	return &data->ep_ctx[free_i].ring;
}

/* A control transfer over EP0 (DCI 1): Setup (immediate data) + an optional
 * Data stage (direction from bmRequestType bit7) + Status (opposite
 * direction, IOC set).  Reuses data->ep0/ep0_ring, already addressed by
 * uhc_xhci_alif_enumerate() during *_enable() -- this driver supports one
 * device per root port, addressed once at enable time.
 *
 * The Data stage's OWN Transfer Event (matched by TRB pointer), not the
 * Status stage's, is what carries the true residual byte count -- the
 * Status stage moves no data, so waiting on it (as an earlier version of
 * this function did) always reports a full wLength transfer even on a
 * short IN read. */
static int
uhc_xhci_alif_ep0_transfer(const struct device *dev, uint8_t slot, struct uhc_transfer *xfer)
{
	struct uhc_xhci_alif_data *data    = dev->data;
	struct xhci_trb            setup   = { 0 };
	struct xhci_trb            sstage  = { 0 };
	bool                       data_in = (xfer->setup_pkt[0] & 0x80u) != 0u;
	uint16_t wlen      = (uint16_t)xfer->setup_pkt[6] | ((uint16_t)xfer->setup_pkt[7] << 8);
	uint32_t enq_setup = data->ep0.enqueue;
	uint32_t enq_dstage, enq_sstage;
	uint64_t dstage_g = 0u, sstage_g;
	uint8_t  cc;
	uint32_t residual = 0u;

	if (wlen != 0u) {
		if (xfer->buf == NULL) {
			return -EINVAL;
		}
		size_t room = data_in ? net_buf_tailroom(xfer->buf) : xfer->buf->len;

		if (xhci_validate_xfer_len(wlen, room) != 0) {
			return -EINVAL;
		}
	}

	memcpy(&setup.param_lo, &xfer->setup_pkt[0], 4u);
	memcpy(&setup.param_hi, &xfer->setup_pkt[4], 4u);
	setup.status  = 8u;
	setup.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_SETUP) | XHCI_TRB_IDT |
	                (wlen == 0u ? 0u : (data_in ? XHCI_TRB_TRT_IN : (2u << 16)));

	enq_dstage = xhci_ring_next_index(&data->ep0, enq_setup);
	if (wlen != 0u) {
		dstage_g   = xhci_l2g(&data->ep0_ring[enq_dstage]);
		enq_sstage = xhci_ring_next_index(&data->ep0, enq_dstage);
	} else {
		enq_sstage = enq_dstage;
	}
	sstage_g       = xhci_l2g(&data->ep0_ring[enq_sstage]);
	sstage.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_STATUS) | XHCI_TRB_IOC |
	                 ((wlen != 0u && data_in) ? 0u : XHCI_TRB_DIR_IN);

	uhc_xhci_alif_irq_mask(dev);
	xhci_ring_enqueue(&data->ep0, &setup);
	if (wlen != 0u) {
		struct xhci_trb dstage = { 0 };
		uint64_t        bufg   = xhci_l2g(xfer->buf->data);

		/* Flush BOTH directions before the doorbell: an OUT buffer's
		 * dirty lines must reach memory before the xHC reads them; an
		 * IN buffer's dirty lines (if any, e.g. from a prior use of
		 * this net_buf) must be pushed out too, or a later write-back
		 * could clobber what the xHC's DMA writes -- invalidate (not
		 * flush) is what happens AFTER, below. */
		xhci_alif_flush(xfer->buf->data, wlen);
		dstage.param_lo = (uint32_t)bufg;
		dstage.param_hi = (uint32_t)(bufg >> 32);
		dstage.status   = wlen;
		dstage.control =
		    XHCI_TRB_TYPE(XHCI_TRB_TYPE_DATA) | XHCI_TRB_IOC | (data_in ? XHCI_TRB_DIR_IN : 0u);
		xhci_ring_enqueue(&data->ep0, &dstage);
	}
	xhci_ring_enqueue(&data->ep0, &sstage);

	xhci_alif_flush(data->ep0_ring, sizeof(data->ep0_ring));
	sys_write32(1u, data->db_base + (uint32_t)slot * 4u); /* DB[slot] EP0 = DCI 1 */
	if (wlen != 0u) {
		cc = uhc_xhci_alif_wait_event(dev, XHCI_TRB_TYPE_TRANSFER_EVENT, dstage_g, NULL, &residual);
		if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
			/* Drain the Status stage's completion too, so it can
			 * never be mistaken for a later transfer's event. */
			(void)uhc_xhci_alif_wait_event(dev, XHCI_TRB_TYPE_TRANSFER_EVENT, sstage_g, NULL, NULL);
		}
	} else {
		cc = uhc_xhci_alif_wait_event(dev, XHCI_TRB_TYPE_TRANSFER_EVENT, sstage_g, NULL, NULL);
	}
	uhc_xhci_alif_irq_unmask(dev);

	if (wlen != 0u && data_in) {
		size_t got = (residual <= wlen) ? (size_t)(wlen - residual) : 0u;

		xhci_alif_invd(xfer->buf->data, wlen);
		(void)net_buf_add(xfer->buf, got);
	}

	if (cc == 0u) {
		uhc_xhci_alif_ep_stop_and_resync(dev, slot, 1u, &data->ep0);
		return -ETIMEDOUT;
	}
	if (cc == XHCI_CC_STALL || cc == XHCI_CC_TRB_ERROR || cc == XHCI_CC_BABBLE) {
		uhc_xhci_alif_ep_reset_halted(dev, slot, 1u, &data->ep0);
		return (cc == XHCI_CC_STALL) ? -EPIPE : -EIO;
	}
	return (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) ? 0 : -EIO;
}

/* A bulk or interrupt transfer: one Normal TRB (IOC set) on the endpoint's
 * own ring.  ponytail: one TRB per xfer, so one transfer is bounded to a
 * single TRB's addressable length -- ample for the block sizes this driver's
 * consumer (usb-host-storage) moves; a caller needing to chain multiple TRBs
 * per transfer needs this extended. */
static int uhc_xhci_alif_bulk_transfer(const struct device *dev,
                                       uint8_t              slot,
                                       uint8_t              dci,
                                       struct xhci_ring    *ring,
                                       struct uhc_transfer *xfer)
{
	struct uhc_xhci_alif_data *data = dev->data;
	bool                       in   = USB_EP_DIR_IS_IN(xfer->ep);
	size_t                     len;
	struct xhci_trb            normal = { 0 };
	uint64_t                   bufg, trb_g;
	uint8_t                    cc;
	uint32_t                   residual = 0u;

	if (xfer->buf == NULL) {
		return -EINVAL;
	}
	len = in ? net_buf_tailroom(xfer->buf) : xfer->buf->len;
	if (xhci_validate_xfer_len(len, len) != 0) {
		return -EINVAL;
	}
	bufg            = xhci_l2g(in ? net_buf_tail(xfer->buf) : xfer->buf->data);
	trb_g           = xhci_l2g(&ring->seg[ring->enqueue]);
	normal.param_lo = (uint32_t)bufg;
	normal.param_hi = (uint32_t)(bufg >> 32);
	normal.status   = (uint32_t)len;
	normal.control  = XHCI_TRB_TYPE(XHCI_TRB_TYPE_NORMAL) | XHCI_TRB_IOC;

	uhc_xhci_alif_irq_mask(dev);
	/* Flush regardless of direction: an IN buffer needs its dirty lines
	 * pushed out before the xHC's DMA write too (see the ep0_transfer
	 * comment above) -- invalidate (below, after the wait) is the other
	 * half of the pair. */
	xhci_alif_flush(in ? net_buf_tail(xfer->buf) : xfer->buf->data, len);
	xhci_ring_enqueue(ring, &normal);
	xhci_alif_flush(ring->seg, (size_t)ring->size * sizeof(struct xhci_trb));

	sys_write32(dci, data->db_base + (uint32_t)slot * 4u); /* DB[slot] target DCI */
	cc = uhc_xhci_alif_wait_event(dev, XHCI_TRB_TYPE_TRANSFER_EVENT, trb_g, NULL, &residual);
	uhc_xhci_alif_irq_unmask(dev);

	if (in) {
		size_t got = (residual <= len) ? (len - residual) : 0u;

		xhci_alif_invd(net_buf_tail(xfer->buf), got);
		(void)net_buf_add(xfer->buf, got);
	}

	if (cc == 0u) {
		uhc_xhci_alif_ep_stop_and_resync(dev, slot, dci, ring);
		return -ETIMEDOUT;
	}
	if (cc == XHCI_CC_STALL || cc == XHCI_CC_TRB_ERROR || cc == XHCI_CC_BABBLE) {
		uhc_xhci_alif_ep_reset_halted(dev, slot, dci, ring);
		return (cc == XHCI_CC_STALL) ? -EPIPE : -EIO;
	}
	return (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) ? 0 : -EIO;
}

/* Event-ring ISR: reached only once *_enable() arms IMAN.IE + USBCMD.INTE +
 * the NVIC line (hotplug notification while otherwise idle -- see the
 * comment there).  `armed` is a software gate, defense-in-depth against a
 * spurious/late IRQ landing before the controller is actually enabled or
 * after *_disable() has masked it again.  Drains every pending event; a
 * Port Status Change reports connect/disconnect to the host stack (and RW1Cs
 * the PORTSC change bits it represents).  Any other event type here means it
 * arrived while nobody was synchronously polling for it -- logged and
 * dropped, never blocked on, since uhc_xhci_alif_wait_event() masks this same
 * IRQ for the duration of every synchronous wait and so cannot be the one
 * racing this handler. */
static void uhc_xhci_alif_isr(const struct device *dev)
{
	struct uhc_xhci_alif_data *data = dev->data;
	const uintptr_t            ir   = data->rt_base + XHCI_IR0;

	if (!data->armed) {
		return;
	}

	sys_write32(sys_read32(ir) | XHCI_IMAN_IP, ir);                /* ack (write-1-clear) */
	sys_write32(XHCI_USBSTS_EINT, data->op_base + XHCI_OP_USBSTS); /* ack (RW1C) */

	for (;;) {
		volatile struct xhci_trb *evt = &data->event_ring_seg[data->event_deq];
		uint8_t                   etype;
		uint64_t                  erdp;

		xhci_alif_invd(evt, sizeof(*evt));
		if ((evt->control & XHCI_TRB_CYCLE) != (uint32_t)data->event_cycle) {
			break; /* ring drained */
		}
		etype = XHCI_TRB_GET_TYPE(evt->control);

		data->event_deq++;
		if (data->event_deq >= ARRAY_SIZE(data->event_ring_seg)) {
			data->event_deq = 0u;
			data->event_cycle ^= 1u;
		}
		erdp = xhci_l2g(&data->event_ring_seg[data->event_deq]);
		sys_write32((uint32_t)erdp | XHCI_ERDP_EHB, ir + XHCI_IR_ERDP_LO);
		sys_write32((uint32_t)(erdp >> 32), ir + XHCI_IR_ERDP_HI);

		if (etype == XHCI_TRB_TYPE_PORT_STATUS) {
			uhc_xhci_alif_handle_port_status(dev);
		} else {
			LOG_WRN("xhci isr: unclaimed event type %u", etype);
		}
	}
}

static int uhc_xhci_alif_disable(const struct device *dev)
{
	const struct uhc_xhci_alif_config *cfg  = dev->config;
	struct uhc_xhci_alif_data         *data = dev->data;
	const uintptr_t                    op   = data->op_base;
	const uintptr_t                    ir   = data->rt_base + XHCI_IR0;
	int                                timeout;

	/* Release the device slot *_enable() allocated (spec §4.6.4) WHILE the
	 * controller is still running -- Disable Slot needs the command ring
	 * processed, which needs USBCMD.R/S=1, so this must happen before
	 * that bit is cleared below.  Without it, a later *_enable() would
	 * reprogram a fresh command ring/DCBAA against an xHC that still
	 * thinks the old slot is allocated. */
	if (data->fl.slot_id != 0u) {
		struct xhci_trb dis = { 0 };

		dis.control =
		    XHCI_TRB_TYPE(XHCI_TRB_TYPE_DISABLE_SLOT) | XHCI_SLOT_ID((uint8_t)data->fl.slot_id);
		(void)uhc_xhci_alif_submit_cmd(dev, &dis, NULL);
		data->fl.slot_id = 0u;
		data->fl.slot_cc = 0u;
	}

	/* Mask the interrupter first so a hotplug event mid-teardown can't
	 * land on a controller we're about to stop, then clear USBCMD.R/S (+
	 * INTE) and poll for USBSTS.HCH to set (spec §5.4.1). */
	sys_write32(sys_read32(ir) & ~XHCI_IMAN_IE, ir);
	sys_write32(sys_read32(op + XHCI_OP_USBCMD) & ~(XHCI_USBCMD_RS | XHCI_USBCMD_INTE),
	            op + XHCI_OP_USBCMD);
	for (timeout = 100000; timeout > 0; timeout--) {
		if ((sys_read32(op + XHCI_OP_USBSTS) & XHCI_USBSTS_HCH) != 0u) {
			break;
		}
		k_busy_wait(1);
	}

	/* Mask the NVIC line + drop the ISR gate LAST, once nothing above
	 * still needs uhc_xhci_alif_submit_cmd()'s own mask/unmask pair to
	 * function normally. */
	data->armed = 0u;
	irq_disable(cfg->irqn);
	return (timeout > 0) ? 0 : -ETIMEDOUT;
}

static int uhc_xhci_alif_shutdown(const struct device *dev)
{
	int ret = uhc_xhci_alif_disable(dev);

	/* Power down: gate the peripheral clock only.  Do NOT re-assert
	 * USB_CTRL2.PHY_POR -- see the bench note in
	 * uhc_xhci_alif_first_light(): a fresh POR pulse needs a long
	 * PLL-relock settle, and a set-then-clear here would leave the PHY
	 * without a stable clock on the next uhc_xhci_alif_init(), hanging
	 * HCRST exactly like the very bug that comment documents.  (An
	 * earlier version of this function re-asserted PHY_POR, directly
	 * contradicting that comment -- this is the fix.)  *_disable() above
	 * already masked the NVIC line before we get here. */
	sys_clear_bits(CLKCTL_PERIPH_CLK_ENA, CLKCTL_PERIPH_CLK_ENA_USB);
	return ret;
}

static int uhc_xhci_alif_bus_reset(const struct device *dev)
{
	struct uhc_xhci_alif_data *data   = dev->data;
	const uintptr_t            op     = data->op_base;
	uint32_t                   portsc = sys_read32(op + XHCI_OP_PORTSC(0));
	uint8_t                    slot   = (uint8_t)data->fl.slot_id;
	uint32_t                   speed, mps;
	int                        t;

	sys_write32((portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_PP | XHCI_PORTSC_PR,
	            op + XHCI_OP_PORTSC(0));
	for (t = 500000; t > 0; t--) {
		portsc = sys_read32(op + XHCI_OP_PORTSC(0));
		if ((portsc & XHCI_PORTSC_PRC) != 0u) {
			break;
		}
		k_busy_wait(1);
	}
	if (t == 0) {
		return -ETIMEDOUT;
	}
	sys_write32((portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_PP | XHCI_PORTSC_PRC,
	            op + XHCI_OP_PORTSC(0)); /* ack PRC only */
	speed               = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
	data->fl.port_speed = speed;

	/* The electrical port reset above just made the device forget its
	 * address (and every endpoint beyond EP0).  A device slot Addressed
	 * during *_enable()'s enumeration is now out of sync with the xHC's
	 * own slot state unless we re-address it: Reset Device (spec
	 * §4.6.11) returns the slot to Default without deallocating it, then
	 * Address Device (again) assigns it a fresh address -- exactly what
	 * a real USB host does after any bus reset on an already-enumerated
	 * device.  No slot yet (first reset before any device was ever
	 * addressed) -- nothing to resync. */
	if (slot != 0u) {
		struct xhci_trb rst_dev = { 0 };
		uint32_t        s       = speed ? speed : 1u;

		rst_dev.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_RESET_DEVICE) | XHCI_SLOT_ID(slot);
		(void)uhc_xhci_alif_submit_cmd(dev, &rst_dev, NULL);

		mps              = (s == 3u) ? 64u : (s == 4u) ? 512u : 8u;
		data->fl.addr_cc = uhc_xhci_alif_address_device(dev, slot, s, mps);
	}
	return uhc_submit_event(dev, UHC_EVT_RESETED, 0);
}

static int uhc_xhci_alif_sof_enable(const struct device *dev)
{
	/*
	 * xHCI schedules SOF automatically once USBCMD.R/S is set and a port
	 * is enabled -- there is no explicit SOF-enable register (unlike
	 * OHCI/UHCI/EHCI).  Nothing to do: SOF generation resumes on its own
	 * once bus_resume() re-requests U0.  This is the real, final answer
	 * for this op, not a placeholder.
	 */
	ARG_UNUSED(dev);
	return 0;
}

static int uhc_xhci_alif_bus_suspend(const struct device *dev)
{
	struct uhc_xhci_alif_data *data = dev->data;
	uintptr_t                  op   = data->op_base;
	uint32_t                   portsc;
	uint8_t                    slot = (uint8_t)data->fl.slot_id;
	int                        t;
	size_t                     i;

	/* Stop every endpoint this driver has addressed before dropping link
	 * power (spec §4.19.1.2.4 implies nothing should be mid-transfer
	 * across a U0->U3 transition).  Best-effort: ep_enqueue()'s
	 * synchronous design means nothing should genuinely be in flight, so
	 * a Stop Endpoint here is normally a no-op on an already-idle
	 * endpoint. */
	if (slot != 0u) {
		struct xhci_trb stop = { 0 };

		stop.control =
		    XHCI_TRB_TYPE(XHCI_TRB_TYPE_STOP_ENDPOINT) | XHCI_SLOT_ID(slot) | XHCI_EP_ID(1u);
		(void)uhc_xhci_alif_submit_cmd(dev, &stop, NULL);
		for (i = 0; i < ARRAY_SIZE(data->ep_ctx); i++) {
			if (!data->ep_ctx[i].used) {
				continue;
			}
			stop.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_STOP_ENDPOINT) | XHCI_SLOT_ID(slot) |
			               XHCI_EP_ID(data->ep_ctx[i].dci);
			(void)uhc_xhci_alif_submit_cmd(dev, &stop, NULL);
		}
	}

	portsc = sys_read32(op + XHCI_OP_PORTSC(0));
	sys_write32((portsc & ~(XHCI_PORTSC_RW1C_MASK | XHCI_PORTSC_PLS_MASK)) | XHCI_PORTSC_PP |
	                XHCI_PORTSC_PLS_U3 | XHCI_PORTSC_LWS,
	            op + XHCI_OP_PORTSC(0));
	/* Confirm via PLS itself, not PLC: PLC is not guaranteed to latch for
	 * every link-state transition on every implementation, but PLS
	 * reading back U0 is the authoritative state (spec §4.19.1.2.4). */
	for (t = 100000; t > 0; t--) {
		portsc = sys_read32(op + XHCI_OP_PORTSC(0));
		if ((portsc & XHCI_PORTSC_PLS_MASK) == XHCI_PORTSC_PLS_U3) {
			break;
		}
		k_busy_wait(1);
	}
	if (t == 0) {
		return -ETIMEDOUT;
	}
	if ((portsc & XHCI_PORTSC_PLC) != 0u) {
		sys_write32((portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_PP | XHCI_PORTSC_PLC,
		            op + XHCI_OP_PORTSC(0)); /* ack PLC if it did latch */
	}
	return uhc_submit_event(dev, UHC_EVT_SUSPENDED, 0);
}

static int uhc_xhci_alif_bus_resume(const struct device *dev)
{
	uintptr_t op     = ((struct uhc_xhci_alif_data *)dev->data)->op_base;
	uint32_t  portsc = sys_read32(op + XHCI_OP_PORTSC(0));
	int       t;

	/* Drive Resume (K-state) for >= 20 ms (spec §4.19.1.2.4), then U0. */
	sys_write32((portsc & ~(XHCI_PORTSC_RW1C_MASK | XHCI_PORTSC_PLS_MASK)) | XHCI_PORTSC_PP |
	                XHCI_PORTSC_PLS_RESUME | XHCI_PORTSC_LWS,
	            op + XHCI_OP_PORTSC(0));
	k_busy_wait(20000);
	portsc = sys_read32(op + XHCI_OP_PORTSC(0));
	sys_write32((portsc & ~(XHCI_PORTSC_RW1C_MASK | XHCI_PORTSC_PLS_MASK)) | XHCI_PORTSC_PP |
	                XHCI_PORTSC_PLS_U0 | XHCI_PORTSC_LWS,
	            op + XHCI_OP_PORTSC(0));
	for (t = 100000; t > 0; t--) {
		portsc = sys_read32(op + XHCI_OP_PORTSC(0));
		if ((portsc & XHCI_PORTSC_PLC) != 0u) {
			break;
		}
		k_busy_wait(1);
	}
	if (t == 0) {
		return -ETIMEDOUT;
	}
	sys_write32((portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_PP | XHCI_PORTSC_PLC,
	            op + XHCI_OP_PORTSC(0)); /* ack PLC */
	return uhc_submit_event(dev, UHC_EVT_RESUMED, 0);
}

static int uhc_xhci_alif_ep_enqueue(const struct device *dev, struct uhc_transfer *const xfer)
{
	struct uhc_xhci_alif_data *data = dev->data;
	uint8_t                    slot = (uint8_t)data->fl.slot_id;
	uint8_t                    dci  = xhci_dci_for_ep(xfer->ep);
	int                        err;

	if (slot == 0u) {
		return -ENODEV; /* no device was addressed during *_enable() */
	}
	uhc_xfer_append(dev, xfer);

	if (dci == 1u) {
		/*
		 * SET_ADDRESS is already implicit in the Address Device
		 * Command *_enable() issued (spec §4.6.5) -- do not re-send
		 * it on the wire, which would re-address an already-addressed
		 * device.  UNVERIFIED: whether the Zephyr host stack's own
		 * enumeration sequence ever routes a SET_ADDRESS through
		 * ep_enqueue given this driver's Address-Device-in-enable()
		 * design has not been exercised against a live usbh/class-
		 * driver stack.
		 */
		if (xfer->setup_pkt[1] == USB_SREQ_SET_ADDRESS) {
			uhc_xfer_return(dev, xfer, 0);
			return 0;
		}
		err = uhc_xhci_alif_ep0_transfer(dev, slot, xfer);
	} else {
		struct xhci_ring *ring;
		uint32_t          ep_type;

		switch (xfer->type) {
		case USB_EP_TYPE_BULK:
			ep_type = USB_EP_DIR_IS_IN(xfer->ep) ? XHCI_EP_TYPE_BULK_IN : XHCI_EP_TYPE_BULK_OUT;
			break;
		case USB_EP_TYPE_INTERRUPT:
			/*
			 * This driver's ep_enqueue()/ep0_transfer()/bulk_transfer() design is
			 * fully SYNCHRONOUS (blocks until the Transfer Event
			 * arrives).  That is spec-legal for control/bulk, but
			 * an interrupt endpoint needs periodic, asynchronous
			 * completion (polled from the ISR, pipelined) to be a
			 * real interrupt-IN implementation -- claiming support
			 * without that would just be a blocking bulk transfer
			 * wearing an interrupt endpoint's clothes.  Dropped
			 * rather than shipped half-true; -ENOTSUP like the
			 * isochronous case below.  See Kconfig.xhci_alif /
			 * the example README for the same correction.
			 */
			uhc_xfer_return(dev, xfer, -ENOTSUP);
			return 0;
		default:
			uhc_xfer_return(dev, xfer, -ENOTSUP); /* isochronous: out of scope */
			return 0;
		}

		ring = uhc_xhci_alif_ep_ctx_get(dev, dci, ep_type, xfer->mps, xfer->interval, &err);
		if (ring == NULL) {
			uhc_xfer_return(dev, xfer, err);
			return 0;
		}
		err = uhc_xhci_alif_bulk_transfer(dev, slot, dci, ring, xfer);
	}

	uhc_xfer_return(dev, xfer, err);
	return 0;
}

static int uhc_xhci_alif_ep_dequeue(const struct device *dev, struct uhc_transfer *const xfer)
{
	struct uhc_xhci_alif_data *data = dev->data;
	uint8_t                    slot = (uint8_t)data->fl.slot_id;
	uint8_t                    dci  = xhci_dci_for_ep(xfer->ep);
	struct xhci_trb            stop = { 0 };
	uint8_t                    cc;

	if (slot == 0u) {
		return -ENODEV;
	}
	/*
	 * Best-effort: ep_enqueue() above is synchronous (it blocks until the
	 * transfer's Transfer Event arrives, then returns), so by the time
	 * ep_dequeue() could run on another thread the transfer has either
	 * already completed (xfer->queued is already clear -- nothing to do,
	 * checked below) or is genuinely mid-flight on the hardware with no
	 * software-side queue entry left to remove.  Stop Endpoint halts
	 * whatever the xHC is doing on this DCI's ring (spec §4.6.9), then Set
	 * TR Dequeue Pointer resyncs the hardware dequeue to our software
	 * enqueue slot, so a later transfer on this endpoint never starts from
	 * wherever the stopped ring was actually left.  UNVERIFIED ON
	 * SILICON: this whole command path, and whether Stop Endpoint on an
	 * idle endpoint is accepted as SUCCESS (spec says it should be).
	 */
	stop.control =
	    XHCI_TRB_TYPE(XHCI_TRB_TYPE_STOP_ENDPOINT) | XHCI_SLOT_ID(slot) | XHCI_EP_ID(dci);
	cc = uhc_xhci_alif_submit_cmd(dev, &stop, NULL);
	if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_STOPPED) {
		LOG_WRN("xhci: Stop Endpoint (dci=%u) failed, cc=%u", dci, cc);
	}
	if (dci == 1u) {
		uhc_xhci_alif_set_tr_dequeue(dev, slot, dci, &data->ep0);
	} else {
		size_t i;

		for (i = 0; i < ARRAY_SIZE(data->ep_ctx); i++) {
			if (data->ep_ctx[i].used && data->ep_ctx[i].dci == dci) {
				uhc_xhci_alif_set_tr_dequeue(dev, slot, dci, &data->ep_ctx[i].ring);
				break;
			}
		}
	}

	if (xfer->queued) {
		uhc_xfer_return(dev, xfer, -ECONNABORTED);
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 * uhc_api table
 * ---------------------------------------------------------------------------
 */

static const struct uhc_api uhc_xhci_alif_api = {
	.lock        = uhc_xhci_alif_lock,
	.unlock      = uhc_xhci_alif_unlock,
	.init        = uhc_xhci_alif_init,
	.enable      = uhc_xhci_alif_enable,
	.disable     = uhc_xhci_alif_disable,
	.shutdown    = uhc_xhci_alif_shutdown,
	.bus_reset   = uhc_xhci_alif_bus_reset,
	.sof_enable  = uhc_xhci_alif_sof_enable,
	.bus_suspend = uhc_xhci_alif_bus_suspend,
	.bus_resume  = uhc_xhci_alif_bus_resume,
	.ep_enqueue  = uhc_xhci_alif_ep_enqueue,
	.ep_dequeue  = uhc_xhci_alif_ep_dequeue,
};

/* ---------------------------------------------------------------------------
 * Device initialization + instantiation macro
 * ---------------------------------------------------------------------------
 */

static int uhc_xhci_alif_driver_init(const struct device *dev)
{
	const struct uhc_xhci_alif_config *cfg  = dev->config;
	struct uhc_xhci_alif_data         *data = dev->data;

	/* Initialize the UHC subsystem mutex (required before any API call).
	 * common is first in uhc_xhci_alif_data so the cast in
	 * uhc_lock_internal / uhc_unlock_internal is valid. */
	k_mutex_init(&data->common.mutex);

	/* Connect the SoC IRQ (IRQ 101, DFP soc.h USB_IRQ_IRQn) to the ISR now,
	 * but do NOT enable it at the NVIC yet: this runs at POST_KERNEL, long
	 * before *_enable() ever arms IMAN.IE/USBCMD.INTE or sets data->armed.
	 * *_enable() enables the NVIC line itself, at the very end, once the
	 * controller is actually running -- see the comment there. */
	cfg->irq_config();
	return 0;
}

#define UHC_XHCI_ALIF_INIT(n) \
	static void uhc_xhci_alif_irq_config_##n(void) \
	{ \
		IRQ_CONNECT(DT_INST_IRQN(n), \
		            DT_INST_IRQ(n, priority), \
		            uhc_xhci_alif_isr, \
		            DEVICE_DT_INST_GET(n), \
		            0); \
	} \
\
	static const struct uhc_xhci_alif_config uhc_xhci_alif_cfg_##n = { \
		.base       = DT_INST_REG_ADDR(n), \
		.irq_config = uhc_xhci_alif_irq_config_##n, \
		.irqn       = DT_INST_IRQN(n), \
	}; \
\
	static struct uhc_xhci_alif_data uhc_xhci_alif_data_##n; \
\
	DEVICE_DT_INST_DEFINE(n, \
	                      uhc_xhci_alif_driver_init, \
	                      NULL, \
	                      &uhc_xhci_alif_data_##n, \
	                      &uhc_xhci_alif_cfg_##n, \
	                      POST_KERNEL, \
	                      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, \
	                      &uhc_xhci_alif_api);

DT_INST_FOREACH_STATUS_OKAY(UHC_XHCI_ALIF_INIT)
