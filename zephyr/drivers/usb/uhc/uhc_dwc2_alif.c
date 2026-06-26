/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble DWC2 USB host (UHC) driver -- SoC-IP-generic (E4/E6/E8).
 *
 * SKELETON: the uhc_api op table + register map are real; the actual host
 * bring-up (bus reset, channel programming, transfer completion, the ISR) is
 * bench work -- every such path is marked TODO(aen401-bench).
 * Do NOT treat this as a validated driver.
 *
 * Register map grounded from:
 *   zephyr/drivers/usb/common/usb_dwc2_hw.h  (Zephyr 4.4, shared DWC2 core)
 *
 * Grounded registers (USB_DWC2_* names from usb_dwc2_hw.h):
 *   Core global:
 *     GAHBCFG  0x0008 -- AHB/DMA config (DMAEN, HBSTLEN, GLBINTRMASK)
 *     GUSBCFG  0x000C -- USB config (FORCEHSTMODE bit 29, PHY selection)
 *     GRSTCTL  0x0010 -- soft reset (CSFTRST bit 0, AHBIDLE bit 31)
 *     GINTSTS  0x0014 -- global interrupts (PRTINT bit 24, HCHINT bit 25)
 *     GINTMSK  0x0018 -- global interrupt mask (same bit positions)
 *     HPTXFSIZ 0x0100 -- host periodic TX FIFO size
 *   Host global (offset 0x0400+):
 *     HCFG     0x0400 -- host config (FSLSPCLKSEL, FSLSSUPP)
 *     HFIR     0x0404 -- frame interval
 *     HAINT    0x0414 -- all-channel interrupt status
 *     HPRT     0x0440 -- host port (PRTPWR bit 12, PRTRST bit 8,
 *                         PRTENA bit 2, PRTCONNDET bit 1, PRTSPD bits 18:17,
 *                         PRTENCHNG bit 3, PRTSUSP bit 7, PRTRES bit 6)
 *   Host per-channel (HCCHAR(n) = 0x0500 + n*0x20):
 *     HCCHARn  -- CHENA/CHDIS/DEVADDR/EPTYPE/EPDIR/EPNUM/MPS
 *     HCINTn   0x0508+n*0x20 -- channel interrupt (XFERCOMPL, CHHLTD, ...)
 *     HCINTMSKn 0x050C+n*0x20 -- channel interrupt mask
 *     HCTSIZn  0x0510+n*0x20 -- transfer size (XFERSIZE, PKTCNT, PID)
 *     HCDMAn   0x0514+n*0x20 -- channel DMA address
 *
 * Host-only registers NOT in usb_dwc2_hw.h (TODO(aen401-bench) -- confirm
 * offsets from the Alif/Synopsys HWRM at bench):
 *   HFNUM     0x0408 -- current host frame number / time remaining
 *   HAINTMSK  0x0418 -- all-channel interrupt mask
 *   HCSPLTn   per-channel -- split-transaction control (FS/LS behind HS hub)
 */

#define DT_DRV_COMPAT alif_dwc2_uhc

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/logging/log.h>
#include "uhc_common.h"

LOG_MODULE_REGISTER(uhc_dwc2_alif, CONFIG_UHC_DRIVER_LOG_LEVEL);

/* ---------------------------------------------------------------------------
 * Driver config + data structs
 * ---------------------------------------------------------------------------
 */

struct uhc_dwc2_alif_config {
	/** DWC2 register block base address (from DT reg property). */
	uintptr_t base;
	/** Called during init to connect the SoC interrupt line. */
	void (*irq_config)(void);
};

struct uhc_dwc2_alif_data {
	/**
	 * MUST be first: the Zephyr UHC subsystem casts dev->data to
	 * struct uhc_data * and owns this layout (mutex, event_cb, status …).
	 * Driver-private state goes in fields after common, accessed via the
	 * full uhc_dwc2_alif_data * rather than uhc_get_private().
	 */
	struct uhc_data common;
};

/* ---------------------------------------------------------------------------
 * uhc_api lock / unlock
 *
 * uhc_lock_internal / uhc_unlock_internal are inline helpers in uhc_common.h
 * (Zephyr 4.4 confirmed).  They wrap k_mutex_lock/unlock on common.mutex.
 * ---------------------------------------------------------------------------
 */

static int uhc_dwc2_alif_lock(const struct device *dev)
{
	return uhc_lock_internal(dev, K_FOREVER);
}

static int uhc_dwc2_alif_unlock(const struct device *dev)
{
	return uhc_unlock_internal(dev);
}

/* ---------------------------------------------------------------------------
 * uhc_api operations -- all bench-gated
 * ---------------------------------------------------------------------------
 */

static int uhc_dwc2_alif_init(const struct device *dev)
{
	/*
	 * TODO(aen401-bench): core soft-reset sequence:
	 *   1. Poll GRSTCTL.AHBIDLE (bit 31) until 1 -- AHB is idle.
	 *   2. Set GRSTCTL.CSFTRST (bit 0) to initiate soft-reset.
	 *   3. Poll GRSTCTL.CSFTRST until cleared (HW clears when done).
	 *      On Alif E4 the register is USB_DWC2_GRSTCTL (offset 0x0010).
	 *   4. Force host mode: set GUSBCFG.ForceHstMode (bit 29).
	 *      USB_DWC2_GUSBCFG.FORCEHSTMODE -- offset 0x000C.
	 *   5. Program HCFG.FslsPclkSel (bits 1:0) for the PHY clock:
	 *      USB_DWC2_HCFG_FSLSPCLKSEL_CLK3060 for 30/60 MHz HS PHY.
	 *      USB_DWC2_HCFG -- offset 0x0400.
	 *   6. Set USB_DWC2_GAHBCFG.DMAEN (bit 5) if DMA mode is used.
	 */
	return 0;
}

static int uhc_dwc2_alif_enable(const struct device *dev)
{
	/*
	 * TODO(aen401-bench): power up and unmask:
	 *   1. Power the host port: set USB_DWC2_HPRT.PrtPwr (bit 12).
	 *      USB_DWC2_HPRT -- offset 0x0440.
	 *      CAUTION: HPRT is a mixed read/write register; some bits are
	 *      write-1-to-clear (PRTENCHNG, PRTCONNDET, PRTOVRCURRCHNG) and
	 *      must be written as 0 to avoid clearing them.
	 *   2. Unmask host-mode global interrupts in GINTMSK:
	 *      USB_DWC2_GINTSTS_PRTINT (bit 24) -- port interrupt
	 *      USB_DWC2_GINTSTS_HCHINT (bit 25) -- host channel interrupt
	 *      USB_DWC2_GINTSTS_DISCONNINT (bit 29) -- disconnect
	 *      USB_DWC2_GINTSTS_WKUPINT (bit 31) -- remote wakeup
	 *   3. Unmask all channel interrupts: HAINTMSK (0x0418) -- all ones.
	 *      (HAINTMSK is host-only, absent from usb_dwc2_hw.h:
	 *       TODO(aen401-bench): confirm offset from Alif TRM.)
	 *   4. Enable the global interrupt: set GAHBCFG.GlbIntrMsk (bit 0).
	 *      USB_DWC2_GAHBCFG.GLBINTRMASK -- offset 0x0008.
	 *   5. Enable the SoC IRQ line via cfg->irq_config(dev).
	 */
	return 0;
}

static int uhc_dwc2_alif_disable(const struct device *dev)
{
	/* TODO(aen401-bench): mask GINTMSK, disable GAHBCFG.GlbIntrMsk,
	 * power-off the host port (HPRT.PrtPwr = 0). */
	return 0;
}

static int uhc_dwc2_alif_shutdown(const struct device *dev)
{
	/* TODO(aen401-bench): full core soft-reset, gate the PHY clock. */
	return 0;
}

static int uhc_dwc2_alif_bus_reset(const struct device *dev)
{
	/*
	 * TODO(aen401-bench): USB bus reset on the host port:
	 *   1. Assert HPRT.PrtRst (bit 8) for >= 10 ms (USB 2.0 spec §7.1.7.5).
	 *      USB_DWC2_HPRT.PRTRST -- offset 0x0440.
	 *   2. Deassert HPRT.PrtRst (bit 8 = 0).
	 *   3. Wait for HPRT.PrtEnChng (bit 3) interrupt -- port enabled after
	 *      reset.  Speed can be read from HPRT.PrtSpd (bits 18:17).
	 *      USB_DWC2_HPRT_PRTSPD_{HIGH|FULL|LOW} constants available.
	 *   4. Re-program HCFG.FslsPclkSel if speed changed.
	 *   Emit uhc_submit_event(dev, UHC_EVT_RESETED, 0) when done.
	 */
	return 0;
}

static int uhc_dwc2_alif_sof_enable(const struct device *dev)
{
	/* TODO(aen401-bench): start SOF generation -- on DWC2 this happens
	 * automatically after bus-reset when the port is enabled; no explicit
	 * SOF-enable register.  May need to start the internal frame counter
	 * (HFNUM / HFIR at 0x0408 / 0x0404). */
	return 0;
}

static int uhc_dwc2_alif_bus_suspend(const struct device *dev)
{
	/* TODO(aen401-bench): set HPRT.PrtSusp (bit 7), stop SOF, emit
	 * UHC_EVT_SUSPENDED when GINTSTS.USBSUSP (bit 11) fires. */
	return 0;
}

static int uhc_dwc2_alif_bus_resume(const struct device *dev)
{
	/* TODO(aen401-bench): set HPRT.PrtRes (bit 6) for >= 20 ms, then
	 * clear it; SOF resumes within 3 ms; emit UHC_EVT_RESUMED. */
	return 0;
}

static int uhc_dwc2_alif_ep_enqueue(const struct device *dev,
				     struct uhc_transfer *const xfer)
{
	/*
	 * TODO(aen401-bench): allocate a free host channel and program:
	 *   HCCHARn: DEVADDR, EPTYPE, EPDIR, EPNUM, MPS, CHENA.
	 *     USB_DWC2_HCCHAR(n) = 0x0500 + n * 0x20.
	 *     Bit fields: CHENA (31), CHDIS (30), DEVADDR (29:22),
	 *                 EPTYPE (19:18), EPDIR (15), EPNUM (14:11),
	 *                 MPS (10:0).
	 *   HCTSIZn: XFERSIZE, PKTCNT, PID.
	 *     USB_DWC2_HCTSIZ0 = 0x0510 (channel 0 base; n-th = base+n*0x20).
	 *   HCDMAn: DMA buffer address (if DMA mode).
	 *     USB_DWC2_HCDMA0 = 0x0514.
	 *   HCINTMSKn: enable XFERCOMPL (bit 0) + CHHLTD (bit 1).
	 *     USB_DWC2_HCINTMSK0 = 0x050C.
	 *   Then set HCCHARn.CHENA to kick the transfer.
	 *   Completion arrives via HCINTn.XFERCOMPL in the ISR; call
	 *   uhc_xfer_return(dev, xfer, err) from the channel ISR handler.
	 *   For periodic (INT/ISO) transfers: check HPTXSTS (0x0100 range)
	 *   to avoid overflowing the periodic FIFO.
	 *   HCSPLTn (split control for FS/LS behind HS hub) is a host-only
	 *   register not in usb_dwc2_hw.h: TODO(aen401-bench) -- confirm
	 *   offset from Alif TRM.
	 */
	ARG_UNUSED(xfer);
	return -ENOTSUP;
}

static int uhc_dwc2_alif_ep_dequeue(const struct device *dev,
				     struct uhc_transfer *const xfer)
{
	/*
	 * TODO(aen401-bench): halt the channel that owns xfer:
	 *   Set HCCHARn.CHDIS (bit 30) + HCCHARn.CHENA (bit 31) atomically
	 *   to request a halt; wait for HCINTn.CHHLTD (bit 1).
	 *   Then call uhc_xfer_return(dev, xfer, -ECONNABORTED).
	 */
	ARG_UNUSED(xfer);
	return -ENOTSUP;
}

/* ---------------------------------------------------------------------------
 * uhc_api table
 * ---------------------------------------------------------------------------
 */

static const struct uhc_api uhc_dwc2_alif_api = {
	.lock        = uhc_dwc2_alif_lock,
	.unlock      = uhc_dwc2_alif_unlock,
	.init        = uhc_dwc2_alif_init,
	.enable      = uhc_dwc2_alif_enable,
	.disable     = uhc_dwc2_alif_disable,
	.shutdown    = uhc_dwc2_alif_shutdown,
	.bus_reset   = uhc_dwc2_alif_bus_reset,
	.sof_enable  = uhc_dwc2_alif_sof_enable,
	.bus_suspend = uhc_dwc2_alif_bus_suspend,
	.bus_resume  = uhc_dwc2_alif_bus_resume,
	.ep_enqueue  = uhc_dwc2_alif_ep_enqueue,
	.ep_dequeue  = uhc_dwc2_alif_ep_dequeue,
};

/* ---------------------------------------------------------------------------
 * Device initialization + instantiation macro
 * ---------------------------------------------------------------------------
 */

static int uhc_dwc2_alif_driver_init(const struct device *dev)
{
	struct uhc_dwc2_alif_data *data = dev->data;

	/* Initialize the UHC subsystem mutex (required before any API call). */
	k_mutex_init(&data->common.mutex);

	/*
	 * TODO(aen401-bench): map the register base address and connect the
	 * SoC IRQ:
	 *   const struct uhc_dwc2_alif_config *cfg = dev->config;
	 *   cfg->irq_config(dev);
	 * The IRQ handler (not yet written) will call the per-channel and
	 * port-status handling paths marked TODO(aen401-bench) above.
	 */
	return 0;
}

#define UHC_DWC2_ALIF_INIT(n)                                                  \
	static void uhc_dwc2_alif_irq_config_##n(void)                         \
	{                                                                       \
		/*                                                              \
		 * TODO(aen401-bench): connect the SoC IRQ line, e.g.:        \
		 *   IRQ_CONNECT(DT_INST_IRQN(n),                              \
		 *               DT_INST_IRQ(n, priority),                     \
		 *               uhc_dwc2_alif_isr,                            \
		 *               DEVICE_DT_INST_GET(n), 0);                    \
		 *   irq_enable(DT_INST_IRQN(n));                              \
		 */                                                             \
	}                                                                       \
                                                                                \
	static const struct uhc_dwc2_alif_config uhc_dwc2_alif_cfg_##n = {     \
		.base       = DT_INST_REG_ADDR(n),                             \
		.irq_config = uhc_dwc2_alif_irq_config_##n,                    \
	};                                                                      \
                                                                                \
	static struct uhc_dwc2_alif_data uhc_dwc2_alif_data_##n;               \
                                                                                \
	DEVICE_DT_INST_DEFINE(n,                                                \
			      uhc_dwc2_alif_driver_init,                        \
			      NULL,                                             \
			      &uhc_dwc2_alif_data_##n,                         \
			      &uhc_dwc2_alif_cfg_##n,                          \
			      POST_KERNEL,                                      \
			      CONFIG_KERNEL_INIT_PRIORITY_DEVICE,               \
			      &uhc_dwc2_alif_api);

DT_INST_FOREACH_STATUS_OKAY(UHC_DWC2_ALIF_INIT)
