# usb-host-storage

Opens the USB host role via `<alp/usb.h>` and enumerates an attached USB
mass-storage device on the E1M-AEN401 Cortex-M55-HP.

## Status

**Driver implemented; end-to-end enumeration bench-gated on the EVK D+/D- path.**

The `uhc_xhci_alif` driver implements the full `uhc_api`: DWC3 host-mode init +
xHCI reset (`first_light`), command-ring/event-ring processing, root-hub port
reset + single-device enumeration, `ep_enqueue`/`ep_dequeue` (control, bulk,
interrupt), bus reset/suspend/resume, disable/shutdown, and an event-ring IRQ
for hotplug (connect/disconnect) notification.  The USB host controller is
grounded at `0x48200000` / IRQ 101 from the Alif DFP `soc.h` (AE402FA0E5597).

What has actually run on silicon: the `first_light` + `enable()`/`enumerate()`
bring-up sequence, proven on an E8 EVK (2026-07-04 -- see the timing comments
in `uhc_xhci_alif_first_light()`).  `ep_enqueue`/`ep_dequeue`, bus
suspend/resume, disable/shutdown, and the event-ring ISR follow the same
register sequencing but have **not themselves been bench-run**.  Separately,
end-to-end enumeration of a real device on this EVK is blocked on the D+/D-
signal path, independent of the driver's software state -- see issue #388's
triage comments.

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

## Bench verification checklist (needs-silicon, issue #388)

Everything below is implemented in software and needs a bench pass to confirm
it behaves as designed on real silicon -- none of it is unwritten:

- Confirm the EVK's D+/D- signal path so a real device actually enumerates
  (the independent hardware blocker triage recorded on issue #388).
- Bench-run `ep_enqueue`/`ep_dequeue` against a real bulk-only mass-storage
  device (BOT protocol: control + one bulk OUT + one bulk IN).
- Bench-run bus suspend/resume and disable/shutdown.
- Confirm the event-ring IRQ (hotplug connect/disconnect while idle) actually
  fires and that it never races a synchronous `ep_enqueue`/command poll over
  the shared event-ring consumer state (`uhc_xhci_alif_wait_event()`'s
  `irq_disable`/`irq_enable` bracketing is a software-only argument for why
  it shouldn't -- not bench-proven).
- Confirm cache-maintenance calls around the DMA rings/buffers
  (`xhci_alif_flush`/`xhci_alif_invd`) are actually needed/sufficient for this
  target's D-cache configuration.

## Sibling example

- USB device mode (CDC-ACM) on the same silicon, via
  `<alp/usb.h>`'s `alp_usb_device_*` API, is not shipped as an
  example yet -- no `examples/peripheral-io/usb-device-cdc/` exists
  today.
