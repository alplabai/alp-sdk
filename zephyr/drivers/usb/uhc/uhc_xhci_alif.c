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
#include "uhc_common.h"

LOG_MODULE_REGISTER(uhc_xhci_alif, CONFIG_UHC_DRIVER_LOG_LEVEL);

/* ---------------------------------------------------------------------------
 * DWC3 global registers (offsets from the USB controller base).
 * Source: Alif DFP USB_Type struct; HWRM §14.10.5.3.
 * ---------------------------------------------------------------------------
 */
#define DWC3_GCTL               0xC110u   /* Global Core Control Register */
#define DWC3_GCTL_PRTCAPDIR_SHIFT   12u
#define DWC3_GCTL_PRTCAPDIR_MASK    (3u << DWC3_GCTL_PRTCAPDIR_SHIFT)
#define DWC3_GCTL_PRTCAPDIR_HOST    (1u << DWC3_GCTL_PRTCAPDIR_SHIFT)

/* TODO(aen401-bench): add GUSB2PHYCFG0 (0xC200), DCTL (0xC704),
 * GTXFIFOSIZ0 (0xC300), GRXFIFOSIZ0 (0xC380) with exact field masks
 * from HWRM §14.10.5.3 Table 14-168 once bench values are confirmed. */

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

static int uhc_xhci_alif_init(const struct device *dev)
{
	const struct uhc_xhci_alif_config *cfg = dev->config;
	struct uhc_xhci_alif_data *data = dev->data;

	data->cap = (volatile struct xhci_cap_regs *)cfg->base;

	/*
	 * TODO(aen401-bench): DWC3 G*-register host-mode init sequence:
	 *   1. Assert DCTL.CoreSoftReset (0xC704 bit 30); poll until cleared.
	 *   2. Set GCTL.PrtCapDir (bits 13:12) = 0b01 (host mode).
	 *      DWC3_GCTL (0xC110): write ((read & ~DWC3_GCTL_PRTCAPDIR_MASK) | DWC3_GCTL_PRTCAPDIR_HOST).
	 *   3. Program GUSB2PHYCFG0 (0xC200) -- PHY type, turnaround, suspend.
	 *   4. Size TX/RX FIFOs: GTXFIFOSIZ0 (0xC300), GRXFIFOSIZ0 (0xC380).
	 *   5. Set GCTL.U2RSTECN (bit 16).
	 * Then xHCI init:
	 *   6. Read cap->caplength to find operational register offset.
	 *   7. Assert USBCMD.HCRST, poll until cleared.
	 *   8. Read HCSPARAMS1 for MaxSlots, MaxPorts; HCCPARAMS1 for AC64.
	 *   9. Allocate DCBAA (aligned 64-byte) + write DCBAAP.
	 *  10. Allocate command ring (TRBs) + write CRCR (with RCS=1).
	 *  11. Allocate event ring segment + ERST; write ERSTBA + ERSTSZ.
	 *  12. Set CONFIG.MaxSlotsEn.
	 */
	return 0;
}

static int uhc_xhci_alif_enable(const struct device *dev)
{
	/*
	 * TODO(aen401-bench): start the controller and unmask interrupts:
	 *   1. Set USBCMD.R/S (bit 0) to start the schedule.
	 *   2. Set primary interrupter IMAN.IE (bit 1) + IMAN.IP clear.
	 *   3. Set USBCMD.INTE (bit 2) to unmask the global interrupt.
	 *   4. Set PORTSC.PP (bit 9) on each root-hub port to apply power.
	 *   5. Enable the SoC IRQ via cfg->irq_config().
	 */
	return 0;
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
