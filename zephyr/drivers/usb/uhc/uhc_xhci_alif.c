/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble USB host (xHCI USB-2.0 dual-role, DWC3-family) UHC driver.
 *
 * SKELETON: the xHCI register map (CAP/op/runtime/doorbell @ 0x48200000),
 * the standard xHCI ring/context structures (per the xHCI spec §5), the DWC3
 * G*-register host-mode init (GCTL @ 0xC110, PrtCapDir=host), and the
 * uhc_api wiring are real; the ring processing / transfer completion / event
 * ISR / root-hub enumeration that need the live controller are
 * TODO(aen401-bench).  Not a validated driver.
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
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <errno.h>
#include <string.h>
#include "uhc_common.h"
#include "xhci_core.h"

LOG_MODULE_REGISTER(uhc_xhci_alif, CONFIG_UHC_DRIVER_LOG_LEVEL);

/* The xHCI DMA master is a SYSTOP system-bus master: it CANNOT reach the M55's
 * local TCM aliases (DTCM @ 0x20000000, ITCM @ 0x0).  Every DMA structure the CPU
 * allocates in TCM (DCBAA, command/event rings, ERST, scratchpad) must be handed
 * to the controller -- and referenced from INSIDE the structures (Link TRB,
 * DCBAA[0], ERST base, scratchpad-array entry) -- by its GLOBAL alias.  Same
 * remap as hal_alif local_to_global() (soc_memory_map.h): DTCM 0x20000000 ->
 * 0x58800000, ITCM 0x0 -> 0x58000000.  Addresses already global pass through. */
static uint64_t xhci_l2g(const void *p)
{
	uintptr_t a = (uintptr_t)p;

	if (a >= 0x20000000u && a < 0x20100000u) {
		return (uint64_t)(a - 0x20000000u + 0x58800000u);
	}
	if (a < 0x00100000u) {
		return (uint64_t)(a + 0x58000000u);
	}
	return (uint64_t)a;
}

/* ---------------------------------------------------------------------------
 * DWC3 global registers (offsets from the USB controller base).
 * Source: Alif DFP USB_Type struct; HWRM §14.10.5.3.
 * ---------------------------------------------------------------------------
 */
#define DWC3_GCTL               0xC110u   /* Global Core Control Register */
#define DWC3_GCTL_PRTCAPDIR_SHIFT   12u
#define DWC3_GCTL_PRTCAPDIR_MASK    (3u << DWC3_GCTL_PRTCAPDIR_SHIFT)
#define DWC3_GCTL_PRTCAPDIR_HOST    (1u << DWC3_GCTL_PRTCAPDIR_SHIFT)
/* DFP drivers/include/usbd.h: GCTL.DisableClockGating bit0, CoreSoftReset bit11,
 * PrtCapDir host = 1<<12 (matches PRTCAPDIR_HOST above). */
#define DWC3_GCTL_DSBLCLKGTNG   (1u << 0)
#define DWC3_GCTL_CORESOFTRESET (1u << 11)

/* Global USB2 PHY config (DFP soc.h USB_Type @ 0xC200; bits from usbd.h). */
#define DWC3_GUSB2PHYCFG0       0xC200u
#define DWC3_GUSB2PHYCFG_PHYSOFTRST (1u << 31)
#define DWC3_GUSB2PHYCFG_SUSPHY     (1u << 6)
#define DWC3_GUSB2PHYCFG_ULPI_UTMI  (1u << 4)
#define DWC3_GUSB2PHYCFG_ULPIAUTORES (1u << 15)
#define DWC3_GUSB2PHYCFG_PHYIF_MASK  (1u << 3)          /* PHYIF pos 3 */
#define DWC3_GUSB2PHYCFG_USBTRDTIM_MASK (0xFu << 10)    /* USBTRDTIM pos 10 */
/* The E8 HS PHY is 16-bit UTMI+ (UTMIW) -- the DFP hard-defaults hsphy_mode to
 * UTMIW (usbd_initialize.c:276): PHYIF=1 (16-bit), USBTRDTIM=5, SUSPHY set.
 * An 8-bit config (PHYIF=0, USBTRDTIM=9) mismatches the PHY data width so the
 * UTMI clock is wrong and the xHCI core stays frozen (HCRST never completes). */
#define DWC3_GUSB2PHYCFG_PHYIF_16BIT     (1u << 3)      /* UTMI_PHYIF_16_BIT */
#define DWC3_GUSB2PHYCFG_USBTRDTIM_16BIT (5u << 10)     /* USBTRDTIM_UTMI_16_BIT */

/* Global SoC bus config (0xC100), user control (0xC12C), frame-length adjust
 * (0xC630) -- DFP soc.h USB_Type offsets; bits from usbd.h. */
#define DWC3_GSBUSCFG0          0xC100u
#define DWC3_GSBUSCFG0_INCRBRSTENA   (1u << 0)
#define DWC3_GSBUSCFG0_INCR16BRSTENA (1u << 3)
#define DWC3_GUCTL              0xC12Cu
#define DWC3_GUCTL_HSTINAUTORETRY    (1u << 14)
#define DWC3_GFLADJ             0xC630u
#define DWC3_GFLADJ_30MHZ_MASK       0x3Fu
#define DWC3_GFLADJ_30MHZ_SDBND_SEL  (1u << 7)
#define DWC3_GFLADJ_30MHZ_DEFAULT    0x20u

/* USB clock + PHY power-on-reset live in CLKCTL_PER_MST (0x4903F000), NOT the
 * controller window -- DFP drivers/include/sys_ctrl_usb.h.  Transcribed here
 * so the driver is self-contained (no hal_alif USB module exists). */
#define CLKCTL_PER_MST_BASE          0x4903F000u
#define CLKCTL_PERIPH_CLK_ENA        (CLKCTL_PER_MST_BASE + 0x0Cu)
#define CLKCTL_PERIPH_CLK_ENA_USB    (1u << 20)   /* PERIPH_CLK_ENA_USB_CKEN */
#define CLKCTL_USB_CTRL2             (CLKCTL_PER_MST_BASE + 0xACu)
#define CLKCTL_USB_CTRL2_PHY_POR     (1u << 8)    /* USB_CTRL2 bit8 = POR_RST_MASK (SVD) */
#define CLKCTL_USB_CTRL2_FLADJ_MASK  (0x3Fu << 0) /* USB_CTRL2 FLADJ_30MHZ_REG [5:0] */
#define CLKCTL_USB_CTRL2_FLADJ_30MHZ (0x20u << 0) /* SoC-wrapper 30MHz frame-len adjust */

/* xHCI operational registers (xHCI spec §5.4, at controller base + CAPLENGTH). */
#define XHCI_OP_USBCMD          0x00u
#define XHCI_OP_USBSTS          0x04u
#define XHCI_USBCMD_HCRST       (1u << 1)   /* Host Controller Reset (§5.4.1) */
#define XHCI_USBSTS_CNR         (1u << 11)  /* Controller Not Ready (§5.4.2)  */
#define XHCI_USBSTS_HCH         (1u << 0)   /* HCHalted (§5.4.2)              */

/* ---------------------------------------------------------------------------
 * xHCI capability registers (at controller base; xHCI spec §5.3).
 * ---------------------------------------------------------------------------
 */
struct xhci_cap_regs {
	uint8_t  caplength;    /* Capability Registers Length (§5.3.1) */
	uint8_t  rsvd;
	uint16_t hciversion;   /* Interface Version Number (§5.3.2) */
	uint32_t hcsparams1;   /* Structural Parameters 1 (§5.3.3) */
	uint32_t hcsparams2;   /* Structural Parameters 2 (§5.3.4) */
	uint32_t hcsparams3;   /* Structural Parameters 3 (§5.3.5) */
	uint32_t hccparams1;   /* Capability Parameters 1 (§5.3.6) */
	uint32_t dboff;        /* Doorbell Array Offset (§5.3.7) */
	uint32_t rtsoff;       /* Runtime Register Space Offset (§5.3.8) */
	uint32_t hccparams2;   /* Capability Parameters 2 (§5.3.9) */
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
	/* Enumeration (device slot 1): 64-byte contexts (HCCPARAMS1.CSZ=1).  Input
	 * context = input-control + slot + EP0 (3 x 64B); device context = slot + EP0
	 * (2 x 64B, pointed at by DCBAA[slot]).  EP0 control transfer ring + a buffer
	 * for GET_DESCRIPTOR data.  All in SRAM0 (DMA-reachable). */
	uint32_t input_ctx[3 * 16] __aligned(64);
	uint32_t device_ctx[2 * 16] __aligned(64);
	struct xhci_trb ep0_ring[16] __aligned(64);
	struct xhci_ring ep0;
	uint8_t  descriptor_buf[64] __aligned(64);
	/**
	 * First-light snapshot: the xHCI capability registers read after the DWC3
	 * host-mode init + xHCI HCRST.  Populated by uhc_xhci_alif_first_light() so a
	 * bench read (SWD or LOG) confirms the controller is alive and reports the
	 * real HCSPARAMS the ring/slot TODOs must be sized from.  fl_ok = the reset
	 * settled (USBSTS.CNR cleared) and HCIVERSION looks sane (>= 0x0100).
	 */
	struct {
		uint32_t magic;      /* 0x58484349 ("XHCI") once populated */
		int32_t  fl_status;  /* 0 = ok, negative = reset/probe failure */
		uint8_t  caplength;
		uint16_t hciversion;
		uint32_t hcsparams1; /* [7:0]=MaxSlots [18:8]=MaxIntrs [31:24]=MaxPorts */
		uint32_t hcsparams2;
		uint32_t hccparams1;
		uint32_t dboff;
		uint32_t rtsoff;
		uint32_t usbsts;     /* post-reset status snapshot */
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
	const struct uhc_xhci_alif_config *cfg = dev->config;
	struct uhc_xhci_alif_data *data = dev->data;
	const uintptr_t base = cfg->base;
	uint32_t reg;
	int timeout;

	data->fl.magic = 0u;
	data->fl.fl_status = -EIO;

	/* 1. Ungate the USB peripheral clock (CLKCTL_PER_MST.PERIPH_CLK_ENA bit20).
	 *    Without this every controller-window read bus-faults (the PL330 lesson). */
	sys_set_bits(CLKCTL_PERIPH_CLK_ENA, CLKCTL_PERIPH_CLK_ENA_USB);

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
	reg &= ~(DWC3_GUSB2PHYCFG_ULPIAUTORES | DWC3_GUSB2PHYCFG_ULPI_UTMI |
		 DWC3_GUSB2PHYCFG_PHYIF_MASK | DWC3_GUSB2PHYCFG_USBTRDTIM_MASK |
		 DWC3_GUSB2PHYCFG_SUSPHY);
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
	uint8_t caplength = sys_read8(base);            /* §5.3.1 */
	uint16_t hciversion = sys_read16(base + 2u);    /* §5.3.2 */
	const uintptr_t op = base + caplength;

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
	data->fl.caplength   = caplength;
	data->fl.hciversion  = hciversion;
	data->fl.hcsparams1  = sys_read32(base + 0x04u);
	data->fl.hcsparams2  = sys_read32(base + 0x08u);
	data->fl.hccparams1  = sys_read32(base + 0x10u);
	data->fl.dboff       = sys_read32(base + 0x14u);
	data->fl.rtsoff      = sys_read32(base + 0x18u);
	data->fl.usbsts      = sys_read32(op + XHCI_OP_USBSTS);
	data->fl.magic       = 0x58484349u; /* "XHCI" */

	if (timeout == 0) {
		LOG_ERR("xhci: HCRST/CNR did not settle (USBSTS=0x%08x)", data->fl.usbsts);
		data->fl.fl_status = -ETIMEDOUT;
		return -ETIMEDOUT;
	}
	/* Sanity: a live xHCI reports HCIVERSION >= 0x0100 and a nonzero CAPLENGTH. */
	if (caplength == 0u || caplength == 0xFFu || hciversion < 0x0100u) {
		LOG_ERR("xhci: implausible caps (CAPLENGTH=0x%02x HCIVERSION=0x%04x)",
			caplength, hciversion);
		data->fl.fl_status = -ENODEV;
		return -ENODEV;
	}
	data->fl.fl_status = 0;
	LOG_INF("xhci first light: CAPLENGTH=0x%02x HCIVERSION=0x%04x "
		"HCSPARAMS1=0x%08x (MaxSlots=%u MaxPorts=%u) DBOFF=0x%x RTSOFF=0x%x",
		caplength, hciversion, data->fl.hcsparams1,
		data->fl.hcsparams1 & 0xFFu, (data->fl.hcsparams1 >> 24) & 0xFFu,
		data->fl.dboff, data->fl.rtsoff);
	return 0;
}

static int uhc_xhci_alif_init(const struct device *dev)
{
	const struct uhc_xhci_alif_config *cfg = dev->config;
	struct uhc_xhci_alif_data *data = dev->data;

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
		uint64_t g = xhci_l2g(data->cmd_ring_seg);
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
	data->dcbaa[0] = xhci_l2g(data->scratchpad_array);

	return 0;
}

/* Primary interrupter (interrupter 0) register offsets from the runtime base
 * (xHCI spec §5.5.2): the interrupter array starts at RT + 0x20. */
#define XHCI_IR0             0x20u
#define XHCI_IR_ERSTSZ       0x08u
#define XHCI_IR_ERSTBA_LO    0x10u
#define XHCI_IR_ERSTBA_HI    0x14u
#define XHCI_IR_ERDP_LO      0x18u
#define XHCI_IR_ERDP_HI      0x1Cu

/* Operational register offsets used at enable (spec §5.4). */
#define XHCI_OP_CRCR_LO      0x18u
#define XHCI_OP_CRCR_HI      0x1Cu
#define XHCI_OP_DCBAAP_LO    0x30u
#define XHCI_OP_DCBAAP_HI    0x34u
#define XHCI_OP_CONFIG       0x38u
#define XHCI_OP_PORTSC(p)    (0x400u + (p) * 0x10u) /* port 0 PORTSC (§5.4.8) */
#define XHCI_ERDP_EHB        (1u << 3)              /* Event Handler Busy (write-1-clear) */

/* Consume the next event of `want` type from the event ring, advancing the
 * dequeue pointer + ERDP (spec §4.9.4).  Any other event types seen first are
 * consumed and skipped (e.g. a Port Status Change before a Transfer Event).
 * Returns the completion code (0 on timeout); *slot_id gets the event's Slot ID. */
static uint8_t uhc_xhci_alif_wait_event(struct uhc_xhci_alif_data *data,
					uint8_t want, uint8_t *slot_id)
{
	const uintptr_t ir = data->rt_base + XHCI_IR0;
	int timeout = 500000;

	if (slot_id != NULL) {
		*slot_id = 0u;
	}
	while (timeout-- > 0) {
		volatile struct xhci_trb *evt = &data->event_ring_seg[data->event_deq];

		if ((evt->control & XHCI_TRB_CYCLE) != (uint32_t)data->event_cycle) {
			k_busy_wait(1);
			continue;
		}
		uint8_t etype = XHCI_TRB_GET_TYPE(evt->control);
		uint8_t cc = XHCI_TRB_GET_CC(evt->status);
		uint8_t slot = XHCI_TRB_GET_SLOT(evt->control);

		data->event_deq++;
		if (data->event_deq >= ARRAY_SIZE(data->event_ring_seg)) {
			data->event_deq = 0u;
			data->event_cycle ^= 1u;
		}
		uint64_t erdp = xhci_l2g(&data->event_ring_seg[data->event_deq]);

		sys_write32((uint32_t)erdp | XHCI_ERDP_EHB, ir + XHCI_IR_ERDP_LO);
		sys_write32((uint32_t)(erdp >> 32), ir + XHCI_IR_ERDP_HI);
		if (etype == want) {
			if (slot_id != NULL) {
				*slot_id = slot;
			}
			return cc;
		}
	}
	return 0u;
}

/* Enqueue a command TRB, ring DB[0], wait for its Command Completion Event. */
static uint8_t uhc_xhci_alif_submit_cmd(struct uhc_xhci_alif_data *data,
					const struct xhci_trb *cmd, uint8_t *slot_id)
{
	xhci_ring_enqueue(&data->cmd_ring, cmd);
	sys_write32(0u, data->db_base); /* DB[0] = command doorbell */
	return uhc_xhci_alif_wait_event(data, XHCI_TRB_TYPE_CMD_COMPLETION, slot_id);
}

/* Enumerate the device on root-hub port 0 (spec §4.3): reset the port, Address
 * Device (build the input slot+EP0 contexts, slot already Enabled), then a
 * control GET_DESCRIPTOR(device, 8) over EP0 -- the core of USB enumeration.
 * Records how far it got + the first descriptor bytes in fl.  No-op if no device
 * is attached (PORTSC.CCS=0). */
static void uhc_xhci_alif_enumerate(struct uhc_xhci_alif_data *data, uintptr_t op)
{
	uint32_t portsc = sys_read32(op + XHCI_OP_PORTSC(0));
	const uint8_t slot = (uint8_t)data->fl.slot_id;
	int t;

	if ((portsc & 1u) == 0u || slot == 0u) { /* CCS=0: nothing attached */
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
	uint32_t mps = (speed == 2u) ? 8u : 64u; /* speed 2 = Low Speed -> MPS 8 */

	/* 2. EP0 control transfer ring (fix its Link TRB to the global alias). */
	xhci_ring_init(&data->ep0, data->ep0_ring, ARRAY_SIZE(data->ep0_ring));
	{
		uint64_t g = xhci_l2g(data->ep0_ring);
		struct xhci_trb *link = &data->ep0_ring[ARRAY_SIZE(data->ep0_ring) - 1u];

		link->param_lo = (uint32_t)g;
		link->param_hi = (uint32_t)(g >> 32);
	}

	/* 3. Input context (64-byte contexts): input-control [0..15], slot [16..31],
	 *    EP0 [32..47].  Add-flags A0(slot)|A1(EP0); slot dword0 speed+1-entry,
	 *    dword1 root-hub port=1; EP0 dword1 type=Control CErr=3 MPS, dword2/3 TR
	 *    dequeue = EP0 ring | DCS. */
	memset(data->input_ctx, 0, sizeof(data->input_ctx));
	memset(data->device_ctx, 0, sizeof(data->device_ctx));
	data->input_ctx[1] = 0x3u;
	data->input_ctx[16] = (speed << 20) | (1u << 27);
	data->input_ctx[17] = (1u << 16);
	data->input_ctx[33] = (4u << 3) | (3u << 1) | (mps << 16);
	uint64_t ep0g = xhci_l2g(data->ep0_ring);

	data->input_ctx[34] = (uint32_t)(ep0g & ~0xFu) | 1u; /* DCS=1 */
	data->input_ctx[35] = (uint32_t)(ep0g >> 32);
	data->dcbaa[slot] = xhci_l2g(data->device_ctx);

	/* 4. Address Device command. */
	struct xhci_trb addr = {0};
	uint64_t ing = xhci_l2g(data->input_ctx);

	addr.param_lo = (uint32_t)ing;
	addr.param_hi = (uint32_t)(ing >> 32);
	addr.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_ADDRESS_DEVICE) | XHCI_SLOT_ID(slot);
	data->fl.addr_cc = uhc_xhci_alif_submit_cmd(data, &addr, NULL);
	if (data->fl.addr_cc != XHCI_CC_SUCCESS) {
		return;
	}
	data->fl.enum_stage = 2u;

	/* 5. GET_DESCRIPTOR(device, 8) over EP0: Setup (immediate data) + Data-IN +
	 *    Status-OUT stage TRBs, ring DB[slot] target EP0 (DCI 1), wait for the
	 *    Transfer Event.  Setup packet 80 06 00 01 00 00 08 00. */
	memset(data->descriptor_buf, 0, sizeof(data->descriptor_buf));
	struct xhci_trb setup = {0};

	setup.param_lo = 0x01000680u; /* bmReqType=80 bReq=06 wValue=0100 */
	setup.param_hi = 0x00080000u; /* wIndex=0000 wLength=0008 */
	setup.status = 8u;
	setup.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_SETUP) | XHCI_TRB_IDT | XHCI_TRB_TRT_IN;
	xhci_ring_enqueue(&data->ep0, &setup);

	struct xhci_trb dstage = {0};
	uint64_t bufg = xhci_l2g(data->descriptor_buf);

	dstage.param_lo = (uint32_t)bufg;
	dstage.param_hi = (uint32_t)(bufg >> 32);
	dstage.status = 8u;
	dstage.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_DATA) | XHCI_TRB_DIR_IN;
	xhci_ring_enqueue(&data->ep0, &dstage);

	struct xhci_trb sstage = {0};

	sstage.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_STATUS) | XHCI_TRB_IOC;
	xhci_ring_enqueue(&data->ep0, &sstage);

	sys_write32(1u, data->db_base + (uint32_t)slot * 4u); /* DB[slot] EP0 = DCI 1 */
	data->fl.xfer_cc = uhc_xhci_alif_wait_event(data, XHCI_TRB_TYPE_TRANSFER_EVENT, NULL);

	data->fl.desc0 = sys_read32((uintptr_t)&data->descriptor_buf[0]);
	data->fl.desc1 = sys_read32((uintptr_t)&data->descriptor_buf[4]);
	if (data->fl.xfer_cc == XHCI_CC_SUCCESS) {
		data->fl.enum_stage = 3u;
	}
}

static int uhc_xhci_alif_enable(const struct device *dev)
{
	struct uhc_xhci_alif_data *data = dev->data;
	const uintptr_t op = data->op_base;
	const uintptr_t rt = data->rt_base;
	const uintptr_t ir = rt + XHCI_IR0;
	int timeout;

	/* 1. Program the operational registers (spec §4.2): DCBAAP, CRCR (RCS=1),
	 *    CONFIG.MaxSlotsEn -- all with GLOBAL (DMA-reachable) addresses. */
	uint64_t dcbaa_g = xhci_l2g(data->dcbaa);
	uint64_t cmdr_g = xhci_l2g(data->cmd_ring_seg);

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
	data->erst[0].size = ARRAY_SIZE(data->event_ring_seg);
	data->erst[0].rsvd = 0u;
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
	data->event_deq = 0u;
	data->event_cycle = 1u;

	/* 4. No-Op command: command ring -> doorbell -> event ring round-trip (no
	 *    device needed). */
	struct xhci_trb noop = {0};

	noop.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_NOOP_CMD);
	data->fl.noop_cc = uhc_xhci_alif_submit_cmd(data, &noop, NULL);

	/* 5. Enable Slot command: a REAL command -- the xHC allocates a device slot
	 *    and returns its Slot ID in the completion event (still no device needed;
	 *    the slot is provisioned for the device we would then Address). */
	struct xhci_trb en_slot = {0};
	uint8_t slot_id = 0u;

	en_slot.control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_ENABLE_SLOT);
	data->fl.slot_cc = uhc_xhci_alif_submit_cmd(data, &en_slot, &slot_id);
	data->fl.slot_id = slot_id;

	/* 6. Root-hub port: apply power (PORTSC.PP, bit 9) and snapshot the status
	 *    (CCS bit0 = a device is attached; PLS bits8:5; speed bits13:10). */
	uint32_t portsc = sys_read32(op + XHCI_OP_PORTSC(0));

	sys_write32(portsc | (1u << 9), op + XHCI_OP_PORTSC(0));
	k_busy_wait(100000); /* let PP settle + connect debounce */
	data->fl.portsc = sys_read32(op + XHCI_OP_PORTSC(0));

	/* 7. If a device is attached (PORTSC.CCS), enumerate it: port reset ->
	 *    Address Device -> control GET_DESCRIPTOR over EP0. */
	uhc_xhci_alif_enumerate(data, op);

	data->fl.run_usbsts = sys_read32(op + XHCI_OP_USBSTS);

	LOG_INF("xhci: run HCH=%u No-Op=%u Slot cc=%u id=%u PORTSC=0x%08x", data->fl.run_hch,
		data->fl.noop_cc, data->fl.slot_cc, data->fl.slot_id, data->fl.portsc);
	LOG_INF("xhci enum: stage=%u speed=%u Addr cc=%u Xfer cc=%u desc=%08x %08x",
		data->fl.enum_stage, data->fl.port_speed, data->fl.addr_cc,
		data->fl.xfer_cc, data->fl.desc0, data->fl.desc1);

	/* Controller-level bring-up PASS = running + command/event ring proven (No-Op
	 * + Enable Slot).  Enumeration (enum_stage 3) only completes with a device on
	 * the port; report it separately in fl. */
	return (data->fl.run_hch == 0u && data->fl.noop_cc == XHCI_CC_SUCCESS &&
		data->fl.slot_cc == XHCI_CC_SUCCESS && data->fl.slot_id != 0u) ? 0 : -EIO;
}

static int uhc_xhci_alif_disable(const struct device *dev)
{
	/* TODO(aen401-bench): clear USBCMD.R/S; mask IMAN.IE; disable IRQ. */
	return 0;
}

static int uhc_xhci_alif_shutdown(const struct device *dev)
{
	/* TODO(aen401-bench): USBCMD.HCRST; poll HCH; gate PHY clock. */
	return 0;
}

static int uhc_xhci_alif_bus_reset(const struct device *dev)
{
	/*
	 * TODO(aen401-bench): USB bus (port) reset via PORTSC:
	 *   1. Set PORTSC.PR (bit 4) on the target port.
	 *   2. Poll until PORTSC.PRC (bit 21) is set (reset complete).
	 *   3. Read PORTSC.SPD (bits 13:10) for negotiated speed.
	 *   4. Emit uhc_submit_event(dev, UHC_EVT_RESETED, 0).
	 */
	return 0;
}

static int uhc_xhci_alif_sof_enable(const struct device *dev)
{
	/*
	 * TODO(aen401-bench): xHCI schedules SOF automatically when
	 * USBCMD.R/S is set and a port is enabled; no explicit SOF-enable
	 * register in xHCI (unlike OHCI/UHCI/EHCI).  This op may be a no-op
	 * or used to (re-)start the microframe counter after a suspend.
	 */
	return 0;
}

static int uhc_xhci_alif_bus_suspend(const struct device *dev)
{
	/*
	 * TODO(aen401-bench): set PORTSC.U3 (LPM state) or PORTSC.PLS=U3
	 * (bits 8:5 = 0b0011) to request suspend; poll PORTSC.PLC;
	 * clear USBCMD.R/S; emit UHC_EVT_SUSPENDED.
	 */
	return 0;
}

static int uhc_xhci_alif_bus_resume(const struct device *dev)
{
	/*
	 * TODO(aen401-bench): set PORTSC.PLS = Resume (0b1111) to drive
	 * K-state for >= 20 ms; then set PLS = U0 (0b0000); poll PLC;
	 * re-set USBCMD.R/S; emit UHC_EVT_RESUMED.
	 */
	return 0;
}

static int uhc_xhci_alif_ep_enqueue(const struct device *dev,
				     struct uhc_transfer *const xfer)
{
	/*
	 * TODO(aen401-bench): map the transfer to a slot/endpoint context and
	 * enqueue TRBs on the endpoint transfer ring:
	 *   Control: Setup TRB (type 2) + Data TRB (type 3) + Status TRB (4).
	 *   Bulk/Int: Normal TRBs (type 1) with IOC set on the last one.
	 *   Ring the doorbell: DB[slot] with endpoint target (ep_ctx index).
	 *   Completion arrives as Transfer Event TRB in the event ring ISR;
	 *   call uhc_xfer_return(dev, xfer, err) from the event handler.
	 *   Slot/device-context setup (Enable Slot + Address Device commands)
	 *   must precede the first transfer on a new device.
	 */
	ARG_UNUSED(xfer);
	return -ENOTSUP;
}

static int uhc_xhci_alif_ep_dequeue(const struct device *dev,
				     struct uhc_transfer *const xfer)
{
	/*
	 * TODO(aen401-bench): issue a Stop Endpoint command (via the command
	 * ring); wait for Command Completion Event TRB; then dequeue the TRB
	 * from the transfer ring and call uhc_xfer_return(dev, xfer,
	 * -ECONNABORTED).  Use the Set TR Dequeue Pointer command to reset
	 * the ring dequeue pointer.
	 */
	ARG_UNUSED(xfer);
	return -ENOTSUP;
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
	struct uhc_xhci_alif_data *data = dev->data;

	/* Initialize the UHC subsystem mutex (required before any API call).
	 * common is first in uhc_xhci_alif_data so the cast in
	 * uhc_lock_internal / uhc_unlock_internal is valid. */
	k_mutex_init(&data->common.mutex);

	/* TODO(aen401-bench): connect the SoC IRQ:
	 *   const struct uhc_xhci_alif_config *cfg = dev->config;
	 *   cfg->irq_config();
	 * The IRQ handler (not yet written) will drain the xHCI event ring
	 * and dispatch Transfer/Command-Completion/Port-SC-Change events. */
	return 0;
}

#define UHC_XHCI_ALIF_INIT(n)                                                 \
	static void uhc_xhci_alif_irq_config_##n(void)                        \
	{                                                                      \
		/*                                                             \
		 * TODO(aen401-bench): connect and enable the USB IRQ:        \
		 *   IRQ_CONNECT(DT_INST_IRQN(n),                             \
		 *               DT_INST_IRQ(n, priority),                    \
		 *               uhc_xhci_alif_isr,                           \
		 *               DEVICE_DT_INST_GET(n), 0);                   \
		 *   irq_enable(DT_INST_IRQN(n));                             \
		 * IRQ 101 confirmed from DFP soc.h USB_IRQ_IRQn = 101.       \
		 */                                                            \
	}                                                                      \
                                                                               \
	static const struct uhc_xhci_alif_config uhc_xhci_alif_cfg_##n = {    \
		.base       = DT_INST_REG_ADDR(n),                            \
		.irq_config = uhc_xhci_alif_irq_config_##n,                   \
	};                                                                     \
                                                                               \
	static struct uhc_xhci_alif_data uhc_xhci_alif_data_##n;              \
                                                                               \
	DEVICE_DT_INST_DEFINE(n,                                               \
			      uhc_xhci_alif_driver_init,                       \
			      NULL,                                            \
			      &uhc_xhci_alif_data_##n,                        \
			      &uhc_xhci_alif_cfg_##n,                         \
			      POST_KERNEL,                                     \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,              \
			      &uhc_xhci_alif_api);

DT_INST_FOREACH_STATUS_OKAY(UHC_XHCI_ALIF_INIT)
