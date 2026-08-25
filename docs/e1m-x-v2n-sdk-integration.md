# E1M-X-EVK + V2N-M1 — making alp-sdk the single source of truth

Status of landing the bench-validated RZ/V2N (r9a09g056n48; AI SDK
platform 7.1 / BSP v6.30, linux-renesas 6.1.141-cip43) carrier bring-up
into alp-sdk so a clean checkout reproduces a working board. Branch:
`feat/e1m-x-v2n-carrier-bringup`.

## Provisioning model (decided)

- **Bootloader = production-flashed by Alp** onto the SoM xSPI. The BL2
  carries SoM-fixed LPDDR4X init; customers never rebuild it. The
  customer's normal flow is **kernel + rootfs only** (Yocto → eMMC/SD).
- **Public/internal split (licensing, not secrecy):** the public,
  Apache-licensed alp-sdk carries *recipes + sources Alp owns*. The
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
| 1 | Carrier device tree | **Staged, HW-validated content** | `meta-alp-sdk/recipes-kernel/linux/` (layered `e1m-v2n-som.dtsi` → `e1m-x-evk.dtsi` → per-board `e1m-v2n101-x-evk.dts`/`e1m-v2m101-x-evk.dts`, plus 3 kernel-source patches 0001–0003, via `linux-renesas_%.bbappend`); machine confs updated |
| 2 | Bootloader (alp DDR in BL2) | **Recipe + binary + DDR.c → alp-sdk-internal** | not in public alp-sdk (licensing) |
| 3 | Metadata values | **Audio + board_id captured**; `ti,tas2563` DT nodes landed (playback only, MCLK pin claim + control-line wiring pending -- see "Audio + board_id" below) | `metadata/boards/e1m-x-evk.yaml` |
| 4 | Errata | **Done** | `docs/errata-e1m-x-v2n.md` |
| 5 | Yocto build flow | **WSL-baked 2026-05-26** (core-image-minimal, bitbake-layers); full alp-image-edge pending | `meta-alp-sdk/README.md` |

## What's validated vs not

- **HW-validated end-to-end** (booted on the board): the DT deltas
  (model, EVK-peripheral disables, RTL8211F-VD @ MDIO addr 2,
  RIIC3/6/7 off, audio off, USB-OVC suppression — since revised to
  PB.1-only with usb20 OVC suppressed at the controllers (spurious-oc,
  cold-boot-verified 2026-06-12), see
  [`errata-e1m-x-v2n.md`](errata-e1m-x-v2n.md) E3 revision 2026-06-12 —
  USB2.0 host kept enabled), and the alp DDR
  in BL2 (DDR 7.9 GiB, boots). The carrier dtsi/dts were also dtc-clean
  rebuilt from source.
- **WSL-baked 2026-05-26** (bitbake-layers, BSP v6.30): the carrier
  dtsi/dts + kernel patches apply cleanly to linux-renesas 6.1.141-cip43 (SHA 6717c06c —
  the exact kernel the BSP ships, so no regen), and `core-image-minimal`
  bakes a `.wic.gz` + the carrier dtb for `MACHINE=e1m-v2n101-a55`. A few
  overlay fixes the bake surfaced are staged separately pending bench
  confirmation; a full `alp-image-edge` bake + on-bench boot are the
  remaining steps. (The TF-A DDR-injection bbappend + its DDR overwrite
  ordering live in alp-sdk-internal.)

## Audio + board_id (gap 3) — captured

The carrier audio + board-rev data has landed in
`metadata/boards/e1m-x-evk.yaml` (`audio:` block + `board_id`): the two
TAS2563 amps on `ALP_E1M_X_I2C0`, I2S on `ALP_E1M_X_I2S0`, the TMUX1574 path
mux, the `\SD_N` / `IRQ_N` control lines on E1M IOs, and `board_id` on
`ALP_E1M_X_ADC7`.

The `ti,tas2563` codec nodes + audio-graph playback path (SSI1 -> left amp,
SSI2 -> right amp) landed in the carrier `e1m-x-evk.dtsi` (issue #1171); the
`CONFIG_SND_SOC_TAS2562=y` fragment (`linux-renesas/tas2563-audio.cfg`) now
has a DT consumer. dtc-clean against the vendored linux-renesas source for
both `e1m-v2n101-x-evk` and `e1m-v2m101-x-evk`; not bench-verified.

The `ssi1_pins`/`ssi2_pins` vs `i2c6_pins`/`i2c7_pins` pad conflict (both
groups claimed port 4, pins 4-7) is resolved: `&i2c6`/`&i2c7` are now
`status = "disabled"` -- neither RIIC6 nor RIIC7 names a board net in
`renesas-peripheral-map.tsv` or a bus in `e1m-x-evk.yaml`'s
`carrier: buses:` list, while SSIU1/SSIU2 on those exact pins do; this also
matches the RIIC3/6/7-off state already recorded above as HW-validated.

Still pending:
- **MCLK pin claim** -- the TAS pair's MCLK comes from an off-chip
  Renesas/IDT 5L35023B clock generator (`metadata/chips/clk_5l35023b.yaml`)
  driving board net Audio_CLKB into SoC pad P10, gated by an
  Audio_CLKB_OE GPIO on P67 -- not the SoC's own AUDIO_CLKOUT function
  (which is P97, CM33-owned `GD32_SPI.CS0`, and must never be muxed from
  Linux). `e1m-x-evk.dtsi` no longer claims P97, but does not yet claim P10
  either: `metadata/pinmux/v2n.yaml` has no verified alternate-function
  number for any Renesas pad on this SoM (`e1m_function: "TBD"`
  everywhere), so the pinctrl group is a TODO pending the R9A09G056
  pin-function table.
- The `\SD_N` / `IRQ_N` amp control lines (on-board wiring, shared by both
  U27/U28) are not wired to `shutdown-gpios` / `interrupts`: board.yaml
  pins them to the I2S1 SDI/SDO pads but doesn't resolve which physical pad
  is SDI vs SDO.
- IV-sense capture (chips/tas2563.h's SDOUT -> host I2S RX) is not wired;
  only the playback DAI path landed.

## Follow-ups (not blockers)

- The per-board `renesas/e1m-v2n101-x-evk.dts` / `e1m-v2m101-x-evk.dts`
  now compose up from the SoC + SoM + carrier dtsi (no longer patching the
  rzv2n-evk dtb), but still require the production bootloader's bootcmd to
  load the new per-product dtb filename. Coordinate with the bootloader landing.
- V2M (DEEPX) SKUs reuse the same DT deltas via `e1m-v2m-deepx.dtsi` +
  the `e1m-v2m101-x-evk.dts` board target — to be exercised when those
  boards are on the bench.
- Errata E1 (MDI pair reversal) and E2 (PHY addr-latch) are **layout**
  items for the next board respin; the DT carries software workarounds
  meanwhile.
