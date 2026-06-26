# usb-host-storage

Opens the USB host role via `<alp/usb.h>` and enumerates an attached USB
mass-storage device on the E1M-AEN401 Cortex-M55-HP.

## Status

**Compile-only skeleton; enumeration bench-gated.**

The `uhc_xhci_alif` driver is a compile-only skeleton: the `uhc_api` op table
is complete and every function is reachable, but the ring processing, transfer
scheduling, IRQ wiring, and root-hub enumeration are marked `TODO(aen401-bench)`
pending hardware access to the AEN401 SoC.  The USB host controller is grounded
at `0x48200000` / IRQ 101 from the Alif DFP `soc.h` (AE402FA0E5597).

## Build

```bash
# From the alp-sdk root:
west build \
    -b alp_e1m_aen401_m55_hp/ae402fa0e5597le0/rtss_hp \
    examples/peripheral-io/usb-host-storage \
    -d /tmp/usb_host \
    -- -DEXTRA_ZEPHYR_MODULES=$PWD

# Flash (SETOOLS -- alif_flash runner):
west flash -d /tmp/usb_host
```

## What it does

1. Calls `alp_usb_host_open()` which drives `usbh_init()` on the xHCI
   controller context (`alp_usbh`, bound to the `zephyr_uhc0` DT node at
   `0x48200000`).
2. Calls `alp_usb_host_enable()` which drives `usbh_enable()`.
3. Waits 2 seconds (placeholder for the enumeration wait).
4. Calls `alp_usb_host_disable()` then `alp_usb_host_close()`.

## Bench bring-up checklist (TODO(aen401-bench))

- Wire `IRQ_CONNECT` in `uhc_xhci_alif_irq_config_0()` (IRQ 101).
- Implement DWC3 soft-reset + host-mode init in `uhc_xhci_alif_init()`:
  DCTL.CoreSoftReset (0xC704 bit 30), GCTL.PrtCapDir (0xC110 bits 13:12 = 01),
  GUSB2PHYCFG0 (0xC200) HS-PHY parameters, GTXFIFOSIZ0/GRXFIFOSIZ0 FIFO sizing.
- Implement xHCI cap-register read + DCBAA/command-ring/event-ring allocation
  in `uhc_xhci_alif_enable()`.
- Implement port-reset sequence in `uhc_xhci_alif_bus_reset()`.
- Implement per-slot transfer TRB scheduling in `uhc_xhci_alif_ep_enqueue()`.
- Implement the event-ring ISR and complete-transfer path.

## Sibling example

- `examples/peripheral-io/usb-device-cdc/` — USB device (CDC-ACM) on the
  same silicon (uses the USB2 controller in device mode via `alp_usb_dev_*`).
