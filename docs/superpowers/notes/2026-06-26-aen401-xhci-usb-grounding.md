# AEN401 xHCI USB host — grounding note

**Date:** 2026-06-26
**Branch:** `feat/aen401-usb-host`

## Context

The original USB-host plan (`docs/superpowers/plans/2026-06-26-aen401-usb-host.md`)
targeted the DWC2 IP. Inspection of the Alif DFP `soc.h` for AE402FA0E5597 showed
the on-SoC USB controller is a **DWC3-family dual-role controller (xHCI host path)**,
not a stand-alone DWC2. The DWC2 plan was superseded; a corrected xHCI plan was
produced and executed across four tasks (2026-06-26).

## No-board build evidence

```
Board:   alp_e1m_aen401_m55_hp/ae402fa0e5597le0/rtss_hp
Example: examples/peripheral-io/usb-host-storage
Build:   PASS (west, Zephyr 4.4.0, arm-zephyr-eabi)

FLASH: 62 612 B / 5 632 KB (1.09 %)
RAM:   19 044 B / 1 MB     (1.82 %)

/tmp/xhci_host/zephyr/zephyr.elf:
    ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV),
    statically linked, with debug_info, not stripped
```

Architecture confirmed: Cortex-M55 (ARM, 32-bit, EABI5).

All components compiled and linked:

- `uhc_xhci_alif.c` — xHCI/DWC3 UHC driver skeleton (`CONFIG_UHC_XHCI_ALIF=y`).
- `src/backends/usb/zephyr_drv.c` — `z_host_*` ops compiled in
  (`DT_HAS_COMPAT_STATUS_OKAY(alif_xhci_uhc)` evaluated true; the
  `USBH_CONTROLLER_DEFINE` block + `usbh_init/enable/disable/shutdown` resolved).
- `examples/peripheral-io/usb-host-storage/src/main.c` — `alp_usb_host_*` call
  chain linked at the correct MRAM addresses.

## DFP grounding (facts only)

| Item | Value | Source |
|------|-------|--------|
| USB controller base | `0x48200000` | Alif DFP `soc.h` (AE402FA0E5597) |
| USB IRQ | 101 | Alif DFP `soc.h` (AE402FA0E5597) |
| DWC3 GCTL offset | `0xC110` | Alif DFP `USB_Type` struct |

## TODO(aen401-bench) — hardware bring-up list

The following paths are guarded as skeletons and require live AEN401 silicon:

1. **DWC3 soft-reset + host-mode init** (`uhc_xhci_alif_init`):
   - Assert `DCTL.CoreSoftReset` (0xC704 bit 30); poll until clear.
   - Set `GCTL.PrtCapDir` (0xC110 bits 13:12) = `0b01` (host mode).
   - Program `GUSB2PHYCFG0` (0xC200) for the embedded HS PHY.
   - Size TX/RX FIFOs: `GTXFIFOSIZ0`/`GRXFIFOSIZ0` (0xC300/0xC380+).
   - Set `GCTL.U2RSTECN` (bit 16) for USB-2.0 reset control.

2. **xHCI startup** (`uhc_xhci_alif_enable`):
   - Read CAP registers (CAPLENGTH, HCIVERSION, HCSPARAMS1/2, HCCPARAMS1).
   - Allocate and program DCBAA, command ring, primary event ring (ERST).
   - Write USBCMD.RS to start the controller.

3. **Port reset** (`uhc_xhci_alif_bus_reset`):
   - Assert `PORTSC.PR`; wait `PORTSC.PRC`; confirm `PORTSC.PED`.

4. **IRQ wiring** (`uhc_xhci_alif_irq_config_0`):
   - `IRQ_CONNECT(101, …)` — priority and flags TBD from bench.

5. **Transfer scheduling** (`uhc_xhci_alif_ep_enqueue`):
   - Allocate a transfer ring slot; write TRBs (Setup/Data/Status for control;
     Normal TRB for bulk); ring the doorbell.

6. **Event ISR + completion**:
   - Process Transfer Event TRBs from the event ring; call `uhc_xfer_buf_free`.

## Artifacts

- Driver: `zephyr/drivers/usb/uhc/uhc_xhci_alif.c`
- Binding: `zephyr/dts/bindings/usb/alif,xhci-uhc.yaml`
- Kconfig: `zephyr/drivers/usb/uhc/Kconfig.xhci_alif`
- Board DTS: `zephyr/boards/alp/e1m_aen401_m55_hp/alp_e1m_aen401_m55_hp_ae402fa0e5597le0_rtss_hp.dts`
- Backend: `src/backends/usb/zephyr_drv.c` (host ops behind `CONFIG_USB_HOST_STACK`)
- Example: `examples/peripheral-io/usb-host-storage/`
