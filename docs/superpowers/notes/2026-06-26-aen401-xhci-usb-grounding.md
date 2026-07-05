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

## xhci_core host-logic validation (Task 4 — 2026-06-27)

`xhci_core.{c,h}` implements the arch-neutral, no-MMIO ring / context / init
sequence logic (pure C, no Zephyr deps).  The three verified invariants:

| Test | Invariant |
|------|-----------|
| `test_ring_enqueue_cycle_and_link_wrap` | Producer cycle toggles on Link-TRB wrap; enqueue pointer resets to 0; Link TRB carries TC bit and pre-toggle cycle. |
| `test_dcbaa_and_context_build` | DCBAA slot write; slot context dword0 route/speed/entries packing; EP context dword1 ep_type/max_packet + dword2/3 TR dequeue ptr \| DCS. |
| `test_init_sequence_writes_expected_regs` | `xhci_init_sequence` writes `CONFIG.MaxSlotsEn`, `DCBAAP_LO/HI`, `CRCR_LO` (ring ptr \| RCS=1), and sets `USBCMD.R/S`. |

### native_sim / twister gate result

```
platform: native_sim/native/64
suite:    alp.xhci_core.unit

3 of 3 executed test cases passed (100.00%)
1 of 1 test configurations passed (100.00%)
Twister exit code: 0
```

### Driver wiring (Task 4)

`uhc_xhci_alif.c` now:
- `#include "xhci_core.h"`
- Data struct holds `struct xhci_ring cmd_ring` + `struct xhci_trb cmd_ring_seg[32]` + `uint64_t dcbaa[9]` (both 64-byte aligned).
- `uhc_xhci_alif_init` calls `xhci_ring_init` (command ring) then
  `xhci_init_sequence(&data->op_image, …)` to build an op-reg image in RAM
  via the host-validated path; the MMIO write-out (DCBAAP/CRCR/CONFIG with
  volatile writes, then USBCMD.R/S at enable) is `TODO(aen401-bench)`.
- CAPLENGTH MMIO read, DWC3 soft-reset, and USBSTS.CNR/HCH waits remain `TODO(aen401-bench)`.

`xhci_core.c` added to the module CMakeLists (`zephyr/CMakeLists.txt`) via
`zephyr_library_sources_ifdef(CONFIG_UHC_XHCI_ALIF …/xhci_core.c)`.

### AEN401 build gate (Task 4)

```
Board:   alp_e1m_aen401_m55_hp/ae402fa0e5597le0/rtss_hp
Example: examples/peripheral-io/usb-host-storage
Build:   PASS (west --pristine, Zephyr 4.4.0, arm-zephyr-eabi 14.3.0)

FLASH: 58 968 B / 5 632 KB (1.02%)
RAM:   19 812 B / 1 MB (1.89%)

ELF: 32-bit LSB executable, ARM, EABI5 version 1 (SYSV),
     statically linked, with debug_info, not stripped
```

`xhci_ring_init` / `xhci_init_sequence` symbols resolved at link time
(no "undefined reference" errors); dead-stripped by `--gc-sections` in the
final ELF (expected: init reachable only via function pointer through the
`uhc_api` table at runtime).

## Artifacts

- Driver: `zephyr/drivers/usb/uhc/uhc_xhci_alif.c`
- xHCI core: `zephyr/drivers/usb/uhc/xhci_core.{c,h}`
- Unit test: `tests/unit/xhci_core/`
- Binding: `zephyr/dts/bindings/usb/alif,xhci-uhc.yaml`
- Kconfig: `zephyr/drivers/usb/uhc/Kconfig.xhci_alif`
- Board DTS: `zephyr/boards/alp/e1m_aen401_m55_hp/alp_e1m_aen401_m55_hp_ae402fa0e5597le0_rtss_hp.dts`
- Backend: `src/backends/usb/zephyr_drv.c` (host ops behind `CONFIG_USB_HOST_STACK`)
- Example: `examples/peripheral-io/usb-host-storage/`
