# E1M-AEN401 M55/Zephyr USB host — xHCI (corrected IP)

**Date:** 2026-06-26
**Branch:** `feat/aen401-usb-host` (off `dev`) · **PR:** #268 (re-pointed)
**Supersedes:** `2026-06-26-aen401-usb-host-design.md` (the DWC2 design — wrong IP).

## Why this supersedes the DWC2 design

The earlier sub-project built a **DWC2 host-channel** driver. Grounding against the
Alif sources corrected the IP:

- The Alif **DFP** `USB_Type` register block has `GCTL`/`DCTL` (DWC3-family), and the
  **E4 datasheet** states "1× **USB 2.0 HS/FS Host/Device**", "the USB module consists
  of an **xHCI USB2.0 Dual-Role Device**".
- The **E4 HWRM** §14.10.5.3 "Programming USB controller in Host Mode" says: *"to
  initialize the controller as a host, the application must perform the steps in the
  **xHCI Specification**"*, plus Alif-specific DWC3 global-register (`G*`) overrides
  (Table 14-168). The xHCI CAP registers live at **`0x48200000`** (`CAPLENGTH`,
  `HCSPARAMS1/2/3`, `HCCPARAMS1/2`, `DBOFF`, `RTSOFF`); USB **IRQ = 101**.

So E4 **does** support USB host (in silicon), and the host interface is **standard
xHCI** — not the DWC2 host-channel model. The DWC2 driver is replaced by an xHCI host
driver. (The Linux side uses `dwc3` + `xhci-hcd`; E4 is M55-only so that path can't run
there — the M55 needs its own xHCI host driver, which no vendor/Zephyr source provides.)

## Goal

A Zephyr **xHCI USB-2.0 host controller driver** for the Alif Ensemble USB
(DWC3-family, xHCI host interface), wired to `alp_usb_host_*`, building no-board for
`alp_e1m_aen401_m55_hp`. The driver is **standard xHCI (per the spec) + thin Alif DWC3
glue**; the register map + glue are grounded from the E4 HWRM/DFP; the
ring/transfer/enumeration bring-up is bench-validated.

## Non-goals

- Live USB enumeration / transfers on silicon (board-gated).
- USB SuperSpeed (the controller is USB-2.0 HS/FS only).
- USB device role (already supported via the existing `udc`/device path).
- E6 (not in this DFP) — the DFP covers AE402/E4, AE722/E7, AE822/E8, AE1C1/E1C.

## Grounding (authoritative)

| Fact | Source |
|---|---|
| USB = xHCI USB-2.0 dual-role; host supported | E4 datasheet `Alif_E4_Datasheet_v1.0` ("USB 2.0 HS/FS Host/Device", "xHCI USB2.0 Dual-Role Device") |
| Host-mode programming = xHCI spec + DWC3 `G*` overrides (Table 14-168: `GUSB2PHYCFG0`, FIFO sizes, `GCTL` PrtCapDir=host) | E4 HWRM §14.10.5.3 |
| xHCI CAP regs at `0x48200000`: `CAPLENGTH`/`HCSPARAMS*`/`HCCPARAMS*`/`DBOFF`/`RTSOFF` (op regs at +CAPLENGTH, runtime at +RTSOFF, doorbells at +DBOFF) | E4 HWRM §14.10.6.2; DFP `USB_BASE 0x48200000` |
| USB IRQ = 101 | DFP `soc.h` `USB_IRQ_IRQn = 101` |
| E4 memory map: MRAM `0x80000000`/5.5 MB; SRAM0 `0x02000000`/4 MB; SRAM1 `0x08000000`/4 MB; HP ITCM `0x50000000`/256 KB, DTCM `0x50800000`/1 MB; HE ITCM `0x58000000`/256 KB, DTCM `0x58800000`/256 KB; `USB_EP_TOTAL=16` | DFP `Device/soc/AE402FA0E5597/include/soc_features.h` |
| `alp_usb_host_*` API + backend host ops + the `z_host_*`→`usbh` wiring | `include/alp/usb.h`; `src/backends/usb/zephyr_drv.c` (PR #268) |
| Zephyr `uhc` has NO xHCI driver (max3421e/mcux-{ehci,ohci,khci,ip3516hs}/virtual only) | `~/zephyrproject/zephyr/drivers/usb/uhc/` |

**Confidential-source rule:** the E4 HWRM is `Confidential-`. Extract only the
register *facts* (offsets, init steps) into our code/metadata — never commit the doc,
its OneDrive path, or verbatim HWRM prose. The xHCI register semantics themselves are
the public xHCI spec.

## Components

### C0 — DFP/HWRM peripheral + memory grounding

- Populate `metadata/socs/alif/ensemble/{e4,e7,e8}.json` `peripherals` from the DFP
  `soc_features.h` (replacing `{}`/the E3-sibling `usb_2` guess with the real set).
- Correct the AEN401 board memory map (MRAM/SRAM/TCM) + the **USB node** to the
  grounded values: `usb@48200000`, `interrupts = <101 0>`, the xHCI compatible.

### C1 — xHCI host `uhc` driver (the major piece)

`zephyr/drivers/usb/uhc/uhc_xhci_alif.{c,h}` (+ Kconfig + CMake + DT binding
`zephyr/dts/bindings/usb/alif,xhci-uhc.yaml`), **replacing** `uhc_dwc2_alif.*`. Two
layers:
- **Standard xHCI host core** (per the xHCI spec): CAP/operational/runtime/doorbell
  register access at `0x48200000`; the command ring, event ring (+ ERST), transfer
  rings, the DCBAA + device contexts, the root-hub port (reset/enumerate), and the
  event-ISR. **Apache-2.0, written from the xHCI spec** — the GPL Linux/u-boot xHCI
  drivers are reference-only, not copied.
- **Alif DWC3 glue**: the `G*` global-register init from HWRM §14.10.5.3 / Table
  14-168 (`GCTL` host mode, `GUSB2PHYCFG0`, FIFO sizes) before the xHCI core runs.
- Exposes the Zephyr `uhc_api` op table. Register defs + the DWC3 init are grounded;
  the ring-processing / transfer-completion / enumeration paths that need silicon are
  marked `TODO(aen401-bench)`. (An xHCI stack is large even as a grounded skeleton —
  this is the bulk of the effort.)

### C2 — alp backend (keep + re-point)

`src/backends/usb/zephyr_drv.c`: keep the PR #268 `z_host_*` → `usbh_init/enable/
disable/shutdown` wiring behind `CONFIG_USB_HOST_STACK`; re-point the
`USBH_CONTROLLER_DEFINE` / `DT_HAS_COMPAT_STATUS_OKAY` to the new `alif,xhci-uhc`
compatible.

### C3 — AEN401 board + example

- AEN401 M55-HP board: correct the USB node (xHCI, `0x48200000`, IRQ 101) + the
  grounded memory map; `chosen { zephyr,uhc = … }`.
- `examples/peripheral-io/usb-host-storage/` (MSC host demo over `alp_usb_host_*`).

### C4 — disposition

Remove `uhc_dwc2_alif.*` + its binding/Kconfig; land the xHCI work on
`feat/aen401-usb-host`; update PR #268's title/body; mark the DWC2 spec/plan superseded.

## Validation (no-board)

- `west build -b alp_e1m_aen401_m55_hp ...` with `CONFIG_USB_HOST_STACK` +
  `CONFIG_UHC_XHCI_ALIF` compiles + links the xHCI driver, the backend wiring, and the
  example — `uhc_api` complete, the xHCI register map + DWC3 glue grounded.
- Metadata schema-validates; clang-format-22 (`src/**`); doc-drift passes.

## Board-gated (`TODO(aen401-bench)`)

- The xHCI ring processing, transfer completion, the event ISR, root-hub enumeration —
  the parts that need the live controller.
- Confirm the DWC3 `G*` reset overrides + PHY timing on silicon.
- Live `alp_usb_host_*` enumeration + mounting a USB stick on the AEN401 M55.

## Risks

- **xHCI is a large stack.** Even a grounded, compiling skeleton (register map + ring
  scaffolding + DWC3 glue + `uhc_api`) is substantial; the full transfer/enumeration
  logic is real engineering, validated only on the board. Size honestly; don't claim
  runtime correctness.
- **Zephyr `uhc` fit.** The `uhc` API is transfer/endpoint-oriented; mapping it onto
  xHCI's ring/context model needs care (the root-hub + endpoint→ring mapping). If the
  impedance is severe, a thin xHCI core under a `uhc` adapter is the fallback shape.
- **Confidential HWRM.** Facts only into code; never the doc/path/prose (classification).

## Constraints

- Never invent register offsets — transcribe from the E4 HWRM §14.10 / DFP `soc.h`
  (facts only), else `TODO(aen401-bench)`. xHCI semantics from the public xHCI spec.
- `zephyr/**` follows Zephyr driver style (excluded from clang-format-22); `src/**`
  obeys clang-format-22. "Alp Lab"; no `Co-Authored-By: Claude`. No binaries in git.
  No OneDrive paths / confidential prose in the repo.
