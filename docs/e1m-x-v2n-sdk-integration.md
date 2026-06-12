# E1M-X-EVK + V2N-M1 — making alp-sdk the single source of truth

Status of landing the bench-validated RZ/V2N (r9a09g056n48; AI SDK
platform 7.1 / BSP v6.30, linux-renesas 6.1.141-cip43) carrier bring-up
into alp-sdk so a clean checkout reproduces a working board. Branch:
`feat/e1m-x-v2n-carrier-bringup`.

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
- **Yocto orchestration = bitbake-layers** per
  [`meta-alp-sdk/README.md`](../meta-alp-sdk/README.md) (kas retired):
  the carrier image bakes from the BSP v6.30 Source Code package + the
  meta-alp-sdk overlay.

## Gap status

| # | Gap | State | Where |
|---|-----|-------|-------|
| 1 | Carrier device tree | **Staged, HW-validated content** | `meta-alp-sdk/recipes-kernel/linux/` (8 patches 0006–0013 + `linux-renesas_%.bbappend`); machine confs updated |
| 2 | Bootloader (alp DDR in BL2) | **Recipe + binary + DDR.c → alp-sdk-internal** | not in public alp-sdk (licensing) |
| 3 | Metadata values | **Audio + board_id captured**; DT patch 0010 (audio-on) + HW wiring pending | `metadata/boards/e1m-x-evk.yaml` |
| 4 | Errata | **Done** | `docs/errata-e1m-x-v2n.md` |
| 5 | Yocto build flow | **WSL-baked 2026-05-26** (core-image-minimal, bitbake-layers); full alp-image-edge pending | `meta-alp-sdk/README.md` |

## What's validated vs not

- **HW-validated end-to-end** (booted on the board): the DT deltas
  (model, EVK-peripheral disables, RTL8211F-VD @ MDIO addr 2,
  RIIC3/6/7 off, audio off, USB-OVC suppression — since revised to
  PB.1-only with usb20 OVC suppressed at the controllers (spurious-oc), see
  [`errata-e1m-x-v2n.md`](errata-e1m-x-v2n.md) E3 revision 2026-06-12 —
  / USB2.0 kept), and the alp DDR
  in BL2 (DDR 7.9 GiB, boots). The 0006–0013 patches were also dtc-clean
  rebuilt from source.
- **WSL-baked 2026-05-26** (bitbake-layers, BSP v6.30): DT patches
  0006-0013 apply cleanly to linux-renesas 6.1.141-cip43 (SHA 6717c06c —
  the exact kernel the BSP ships, so no regen), and `core-image-minimal`
  bakes a `.wic.gz` + the carrier dtb for `MACHINE=e1m-v2n101-a55`. A few
  overlay fixes the bake surfaced are staged separately pending bench
  confirmation; a full `alp-image-edge` bake + on-bench boot are the
  remaining steps. (The TF-A DDR-injection bbappend + its DDR overwrite
  ordering live in alp-sdk-internal.)

## Audio + board_id (gap 3) — captured

The carrier audio + board-rev data has landed in
`metadata/boards/e1m-x-evk.yaml` (`audio:` block + `board_id`): the two
TAS2563 amps on `E1M_X_I2C0`, I2S on `E1M_X_I2S0`, the TMUX1574 path
mux, the `\SD_N` / `IRQ_N` control lines on E1M IOs, and `board_id` on
`E1M_X_ADC7`.

Still pending: convert DT patch 0010 from "audio off" to a `ti,tas2563`
codec + audio-graph-card (the `CONFIG_SND_SOC_TAS2562=y` fragment is
already in `meta-alp-sdk/recipes-kernel/linux/`), plus the on-board
control-line wiring on the current PCB rev.

## Follow-ups (not blockers)

- Standalone `renesas/e1m-x-evk.dts` (instead of patching the rzv2n-evk
  dtb) — clean, but requires the production bootloader's bootcmd to load
  the new dtb filename. Coordinate with the bootloader landing.
- V2M (DEEPX) SKUs reuse the same DT deltas against an `e1m-x-evk-v2m`
  target — wire when those boards are exercised.
- Errata E1 (MDI pair reversal) and E2 (PHY addr-latch) are **layout**
  items for the next board respin; the DT carries software workarounds
  meanwhile.
