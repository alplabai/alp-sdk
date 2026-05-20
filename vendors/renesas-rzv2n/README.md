# vendors/renesas-rzv2n

Vendor wrapper for the **Renesas RZ/V2N** family — backs the
**E1M-X V2N** and **E1M-X V2N-M1** SoM families (and the Industrial
Camera variant).

## SKUs covered

| Family             | SKUs                          | Renesas part            | LPDDR4X         | eMMC                       | Companion accelerator   |
|--------------------|-------------------------------|-------------------------|-----------------|----------------------------|--------------------------|
| **E1M-X V2N**      | `E1M-V2N101`, `E1M-V2N102`    | `R9A09G056N44GBG#AC0`   | 32 / 64 Gbit    | eMMC 5.1, 32 / 128 Gbit    | —                        |
| **E1M-X V2N-M1**   | `E1M-V2M101`, `E1M-V2M102`    | `R9A09G056N44GBG#AC0`   | 32 / 64 Gbit    | eMMC 5.1, 32 / 128 Gbit    | DeepX DX-M1 (25 TOPS)    |

Authoritative per-SKU detail and the silicon stack live in
[`e1m-spec` Annex A.2 / A.3](https://github.com/alplabai/e1m-spec/blob/main/STANDARD.md#a2-e1m-x-v2n-family-renesas-rzv2n).

## On-module supervisory MCU (important for porting)

The V2N family carries a **separate, on-module GigaDevice `GD32G553`
Cortex-M33 IC at 216 MHz** — distinct from the Cortex-M33 core
embedded inside the Renesas RZ/V2N (200 MHz).  These are two different
CPUs:

| CPU                                       | Where it lives                   | Role                                                                           |
|-------------------------------------------|----------------------------------|--------------------------------------------------------------------------------|
| Renesas internal **Cortex-M33** @ 200 MHz | inside `R9A09G056N44GBG#AC0`     | Application-level real-time work (paired with the four A55 cores via the IPC). |
| GigaDevice **GD32G553** @ 216 MHz         | separate IC on the SoM           | Supervisory: PMIC sequencing alongside `DA9292`, on-module supervisory I/O, low-power-state orchestration. |

The ALP SDK's V2N backend talks to the **Renesas internal M33**
through the standard Zephyr / Yocto path; the supervisor `GD32G553`
runs SoM-internal firmware and is not directly addressable from
application code.  Porting docs that mention "the M33" need to be
specific about which one — these docs assume the Renesas internal
M33 unless they explicitly say "supervisor".

## Routing caveats

E1M-X V2N exposes most of the E1M-X pinout.  The relevant deltas
versus the standard's max are:

- **PCIe** — V2N silicon supports PCIe 3.0 ×2 lanes.  E1M-X exposes
  4 lanes per controller; lanes 2 and 3 of `PCIE0_*` (and all of
  `PCIE1_*`) are NC on this family.
- **PCIe sharing on V2N-M1** — the on-module DeepX DX-M1 sits behind
  `PCIE0` internally, so external `PCIE0_*` is functionally a bridge
  share, not an independent ×4.
- **Ethernet** — both `ETH0_*` and `ETH1_*` are routed and each goes
  through an on-module Realtek `RTL8211FDI-VD-CG` PHY; board-side
  pads are post-PHY differential MDI.
- **Wi-Fi 6 + BLE 5.4** — driven by an on-module Murata `LBEE5HY2FY`
  (Type2FY, Infineon `CYW55513`); `ANT_2.4GHz`, `ANT_5GHz`, and
  `ANT_6GHz` are all populated.
- **CAN** — both `CAN0` and `CAN1` are routed, each through an
  on-module CAN transceiver (board-side pads are bus-level
  `CANxH` / `CANxL`).
- **Boot strap** — E1M-X `BOOT3` (pad Y1) maps to Renesas
  `MD_BOOT4`, *not* `MD_BOOT3` (the chip has `MD_BOOT0/1/2/4`,
  no `MD_BOOT3`).  See
  [`examples/alp-x-v2n-m1.som-manifest.json`](https://github.com/alplabai/e1m-spec/blob/main/examples/alp-x-v2n-m1.som-manifest.json)
  in `e1m-spec`.

## On-module support silicon (informative)

Common to both V2N and V2N-M1:

| Role | Part |
|---|---|
| PMIC | Renesas `DA9292` (multi-channel) |
| Auxiliary I/O / supervisor MCU | GigaDevice `GD32G553` (Cortex-M33 @ 216 MHz) |
| Ethernet PHY (×2) | Realtek `RTL8211FDI-VD-CG` |
| Wi-Fi 6 + BLE 5.4 module | Murata `LBEE5HY2FY` (Type2FY, Infineon `CYW55513`) |

V2N-M1 adds:

| Role | Part |
|---|---|
| Companion AI accelerator | DeepX `DX-M1` (PCIe-attached, 25 TOPS) |
| DX-M1 memory | 2 × LPDDR5X |
| DX-M1 storage | SPI NAND flash |

## Status

v0.1: **stub only**.  Surface lands in v0.2 along with the MIPI CSI-2
camera implementation.  The directory exists now so the porting guide
(`docs/porting-new-som.md`) has a concrete worked example, and so the
SoM column in `docs/os-support-matrix.md` has somewhere to point.

## Yocto BSP integration

The V2N + V2N-M1 Yocto build is rooted on the **Renesas RZ/V2N AI
SDK 7.10** -- a free download from My Renesas, no NDA for the
standard build path.  Setup recipe (extract tarball, add the four
`meta-rz-features/*` sublayers, build `core-image-weston` against
`MACHINE=rzv2n-evk`) lives in
[`yocto/meta-alp/README.md`](../../yocto/meta-alp/README.md).
`meta-alp`'s `e1m-x-v2n.conf` inherits from Renesas's stock
`rzv2n-evk` MACHINE; board deltas are TBD per the user-supplied
HW config writeup.

## DRP-AI inference toolchain

The DRP-AI3 accelerator on V2N has a host-side compiler and a
target-side runtime; they're distributed separately under
different licenses.

- **Compiler (RUHMI / DRP-AI TVM)** -- runs on the build host,
  lowers `.tflite` / `.onnx` / `.pt` models into DRP-AI-executable
  binaries.  Apache-2.0.  Anchor at
  [`vendors/renesas-rzv2n/rzv_drp-ai_tvm/`](rzv_drp-ai_tvm/);
  upstream <https://github.com/renesas-rz/rzv_drp-ai_tvm>.
- **Runtime (`libdrpai`)** -- runs on the V2N, shipped by the
  `meta-rz-drpai` sublayer in the Renesas AI SDK 7.10 BSP.  The
  alp-sdk's `<alp/inference.h>` Yocto backend (planned for v0.4)
  links against this via the target sysroot; no separate
  per-app pkg-config plumbing.
