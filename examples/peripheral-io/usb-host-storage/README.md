# usb-host-storage

Opens the USB host role via `<alp/usb.h>` and enumerates an attached USB
mass-storage device on the E1M-AEN401 Cortex-M55-HP.

## Status

**Compile-verified, bring-up bench-gated.**

The `uhc_dwc2_alif` driver is a skeleton: the `uhc_api` op table is complete
and every function is reachable, but the register-level bring-up sequences
(bus reset, channel programming, transfer completion, IRQ wiring) are marked
`TODO(aen401-bench)` pending hardware access to the AEN401 SoC.  The USB
host base address and IRQ in the board DTS are placeholder values from the
Ensemble address map family pattern; they must be confirmed from the E4 HWRM
before any hardware bring-up attempt.

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

1. Calls `alp_usb_host_open()` which drives `usbh_init()` on the DWC2
   controller context (`alp_usbh`, bound to the `zephyr_uhc0` DT node).
2. Calls `alp_usb_host_enable()` which drives `usbh_enable()`.
3. Waits 2 seconds (placeholder for the enumeration wait).
4. Calls `alp_usb_host_disable()` then `alp_usb_host_close()`.

## Bench bring-up checklist (TODO(aen401-bench))

- Confirm DWC2 USB2 base address from the E4 HWRM (`Confidential-Alif_E4_HWRM_v0.3.pdf`).
- Confirm the USB interrupt number from the E4 HWRM.
- Update `alp_e1m_aen401_m55_hp_ae402fa0e5597le0_rtss_hp.dts` with the
  confirmed `reg` and `interrupts` values.
- Wire `IRQ_CONNECT` in `uhc_dwc2_alif_irq_config_0()`.
- Implement the register bring-up sequence in `uhc_dwc2_alif_init()` /
  `uhc_dwc2_alif_enable()` / `uhc_dwc2_alif_bus_reset()`.
- Implement per-channel transfer scheduling in `uhc_dwc2_alif_ep_enqueue()`.

## Sibling example

- `examples/peripheral-io/usb-device-cdc/` — USB device (CDC-ACM) on the
  same silicon (uses the USB2 controller in device mode via `alp_usb_dev_*`).
