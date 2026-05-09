# vendors/nxp-imx93

Vendor wrapper for the **NXP i.MX 93** family — backs the
**E1M-i.MX93** SoM family (a future Linux-class E1M variant).

## Status

**v0.4 deliverable.**  v0.1 ships the metadata stub and this
README; the actual peripheral wrappers under `src/zephyr/`,
`src/yocto/`, and the chip-driver glue land alongside the
v0.4 "Yocto first-class" milestone in
[`VERSIONS.md`](../../VERSIONS.md).

## SKUs covered

| Family             | SKUs                          | NXP part         | Notes                                        |
|--------------------|-------------------------------|------------------|----------------------------------------------|
| **E1M-i.MX93**     | TBD (E1M-NX9xxx)              | TBD              | Dual Cortex-A55 + Cortex-M33 + Ethos-U65 NPU. Linux-class SoM in the E1M 35 × 35 form factor. |

The exact i.MX 93 variant (`i.MX 9301` / `9302` / `9311` / `9312`
/ `9352`) the SoM ships with is **pending the user-supplied exact
hardware configuration** — see
[`metadata/socs/nxp/imx9/imx93.json`](../../metadata/socs/nxp/imx9/imx93.json)
and the project memory note "pending exact hardware configurations."

## On-module support silicon (per `OneDrive/E1M Project/E1M - i.MX93/...`)

Confirmed from the OneDrive datasheets folder; specific assembly
variant per SKU is pending the user's HW-config writeup:

| Role                    | Part                                              | Datasheet on file                                    |
|-------------------------|---------------------------------------------------|------------------------------------------------------|
| PMIC                    | NXP `PCA9451A`                                    | `AN14413.pdf` (application note)                     |
| Wi-Fi 6 + BLE           | One of: Murata Type `2DL` / `2EL` / `2KL` / `2LL` | `TYPE2DL.pdf`, `TYPE2KL.pdf`, `TYPE2LL.pdf`, `type2el-2dl-2ll-2kl-unified-design-guide.pdf` |
| Wi-Fi 6 (alt path)      | NXP `IW610`                                       | `IW610.pdf`                                          |

The combo module that ships on the E1M-i.MX93 production SoM is
TBD until the user writes it.

## Routing caveats (anticipated)

The E1M form factor exposes a strict subset of the i.MX 93 SoC
pinout per the standard.  Anticipated caveats (verify when the
HW config lands):

- **No GPU** — the i.MX 93 line deliberately omits a 3D GPU (cf.
  i.MX 95).  `<alp/gui.h>` LVGL paths run software-rendered;
  display composition uses the LCDIF block + MIPI DSI controller
  directly.
- **Single MAC, single CSI** — i.MX 93 has 2 × ENET on-die but the
  AEN-style E1M routing pattern probably exposes only `ETH0_*`.
  Confirm.
- **No PCIe** — i.MX 93 does not implement PCIe; M.2-Key-M-style
  expansion isn't possible on this SoM.

## HAL pinning

When the v0.4 implementation lands, the wrapper links against
NXP's MCUXpresso SDK (`mcux-sdk`) for bare-metal / Cortex-M33
work and against the i.MX 93 BSP layer of `meta-imx` for Yocto
builds.

## Ethos-U65 toolchain

The on-die Ethos-U65 NPU is fed by the same Vela compiler used on
Alif Ensemble's Ethos-U55-HP — see
[`examples/edgeai-vision-aen/models/README.md`](../../examples/edgeai-vision-aen/models/README.md)
for the workflow.  The `<alp/inference.h>` unification layer
(v0.3 deliverable) dispatches to Ethos-U65 on i.MX 93 using the
same calling convention as on AEN, so EdgeAI applications port
unchanged.
