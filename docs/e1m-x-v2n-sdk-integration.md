# E1M-X-EVK + V2N-M1 — making alp-sdk the single source of truth

Status of landing the bench-validated RZ/V2N (r9a09g056n48, AI SDK 6.30,
linux-renesas 6.1-cip43) carrier bring-up into alp-sdk so a clean
checkout reproduces a working board. Branch:
`feat/e1m-x-v2n-carrier-bringup`. Nothing here is committed/pushed yet.

## Provisioning model (decided)

- **Bootloader = production-flashed by ALP** onto the SoM xSPI. The BL2
  carries SoM-fixed LPDDR4X init; customers never rebuild it. The
  customer's normal flow is **kernel + rootfs only** (Yocto → eMMC/SD).
- **Public/internal split (licensing, not secrecy):** the public,
  Apache-licensed alp-sdk carries *recipes + sources ALP owns*. The
  Renesas-derived bits — the BSP itself, the alp DDR param `.c`, the
  TF-A DDR-injection bbappend, and the prebuilt BL2/FIP `.srec` — stay
  in **alp-sdk-internal** (same reason
  the SDK never bundled the Renesas BSP / NXP / DEEPX bits). Nothing in
  BL2/FIP is secret; this is purely redistribution-rights alignment.
- **Yocto orchestration = kas** (`kas/e1m-v2n.yml`).

## Gap status

| # | Gap | State | Where |
|---|-----|-------|-------|
| 1 | Carrier device tree | **Staged, HW-validated content** | `meta-alp-sdk/recipes-kernel/linux/` (8 patches 0006–0013 + `linux-renesas_%.bbappend`); machine confs updated |
| 2 | Bootloader (alp DDR in BL2) | **Recipe + binary + DDR.c → alp-sdk-internal** | not in public alp-sdk (licensing) |
| 3 | Metadata values | **BLOCKED — needs ALP data** | `metadata/boards/e1m-x-evk.yaml` + V2N SoM YAML |
| 4 | Errata | **Done** | `docs/errata-e1m-x-v2n.md` |
| 5 | Yocto build manifest | **Scaffold (needs BSP source + SHA pins)** | `kas/e1m-v2n.yml` |

## What's validated vs not

- **HW-validated end-to-end** (booted on the board): the DT deltas
  (model, EVK-peripheral disables, RTL8211F-VD @ MDIO addr 2,
  RIIC3/6/7 off, audio off, USB-OVC off / USB2.0 kept), and the alp DDR
  in BL2 (DDR 7.9 GiB, boots). The 0006–0013 patches were also dtc-clean
  rebuilt from source.
- **NOT yet run through bitbake** (authored from the recipe mechanics,
  needs an ALP build pass): the `linux-renesas` bbappend and the kas
  manifest. Check the SRC_URI override key (`e1m-v2n101`) against your
  MACHINEOVERRIDES and the patch apply against the pinned linux-renesas
  SRCREV (generated at kernel SHA 6717c06c). (The TF-A DDR-injection
  bbappend + its DDR overwrite ordering live in alp-sdk-internal.)

## Remaining data needed from ALP (gap 3)

To fill the carrier/SoM metadata (you mentioned this may already be in
alp-sdk-internal — if so, just point me at it):

- **TAS2563 ×2 (U27/U28):** I2C control bus + 7-bit address per amp;
  SSI/I2S instance + channel; shutdown/IRQ GPIO; MCLK source. (Then the
  audio can be re-enabled as a `ti,tas2563` codec + audio-graph-card,
  with a `CONFIG_SND_SOC_TAS2562=y` cfg fragment — replacing DT
  patch 0010's "audio off".)
- **board_id:** ADC channel + resistor-divider values + per-rev
  expected mV / bin radius.

## Follow-ups (not blockers)

- Standalone `renesas/e1m-x-evk.dts` (instead of patching the rzv2n-evk
  dtb) — clean, but requires the production bootloader's bootcmd to load
  the new dtb filename. Coordinate with the bootloader landing.
- V2M (DEEPX) SKUs reuse the same DT deltas against an `e1m-x-evk-v2m`
  target — wire when those boards are exercised.
- Errata E1 (MDI pair reversal) and E2 (PHY addr-latch) are **layout**
  items for the next board respin; the DT carries software workarounds
  meanwhile.
