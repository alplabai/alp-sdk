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
| 3 | Metadata values | **Audio + board_id captured**; `ti,tas2563` audio nodes + HW wiring pending | `metadata/boards/e1m-x-evk.yaml` |
| 4 | Errata | **Done** | `docs/errata-e1m-x-v2n.md` |
| 5 | Yocto build flow | **Base images bake**: `core-image-minimal` (2026-05-26) and `drpai`-OFF `alp-image-edge` (12118 tasks, 716 MB `.wic.gz`); `drpai`-enabled bake + on-bench boot pending | [`bring-up-drpai-v2n.md`](bring-up-drpai-v2n.md) status banner |

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
- **WSL-baked** (bitbake-layers, BSP v6.30): the carrier
  dtsi/dts + kernel patches apply cleanly to linux-renesas 6.1.141-cip43 (SHA 6717c06c —
  the exact kernel the BSP ships, so no regen), and `core-image-minimal`
  bakes a `.wic.gz` + the carrier dtb for `MACHINE=e1m-v2n101-a55`. A few
  overlay fixes the bake surfaced are staged separately pending bench
  confirmation. A later `alp-image-edge` bake also completed (12118 tasks,
  716 MB `.wic.gz`), but with `drpai` OFF. A `drpai`-enabled
  `alp-image-edge` bake and on-bench boot of an image from this branch remain
  unverified. [`bring-up-drpai-v2n.md`](bring-up-drpai-v2n.md)'s status
  banner is the authority for the current bake state. (The TF-A DDR-injection
  bbappend + its DDR overwrite ordering live in alp-sdk-internal.)

## Audio + board_id (gap 3) — captured

The carrier audio + board-rev data has landed in
`metadata/boards/e1m-x-evk.yaml` (`audio:` block + `board_id`): the two
TAS2563 amps on `ALP_E1M_X_I2C0`, I2S on `ALP_E1M_X_I2S0`, the TMUX1574 path
mux, the `\SD_N` / `IRQ_N` control lines on E1M IOs, and `board_id` on
`ALP_E1M_X_ADC7`.

## Audio: A55 today, CM33 gap (#1171)

**A55 / Linux** plays through `rcar_sound` (SSIU1 + SSIU2; PR #2536, #2331):
SSI2 carries the data line and SSI1 supplies SCK/WS
(`SSIU_SSI_MODE1.ssi2_pin`, R01UH1072EJ0120 8.5.2.3.16).  The card runs from
Audio_CLKB only (24.576 MHz from the clock generator); Audio_CLKA and
Audio_CLKC are unfed on the SoM and carrier, and listing them makes the CPG
fail with `-110`.  Playback was run on E1M-V2M103; nobody has listened to the
speaker yet, and capture and the SSIU3/SSIU4 group are untried.

**CM33 / Zephyr has no I2S path.** Neither Zephyr `drivers/i2s` (RA `ssie`,
NXP, STM32, ...; nothing for RZ/V) nor hal_renesas (`r_ssi` exists only for
RA; RZ carries just register headers for RZ/A and RZ/G) provides an SSIU
driver, so `src/backends/i2s/zephyr_drv.c` has nothing to bind on `m33_sm`
and the class resolves to `src/common/stub/stub_i2s.c` (`ALP_ERR_NOSUPPORT`).
A CM33 backend therefore means writing a new Zephyr `i2s_*` driver against
the SSIU registers.  Manual references (R01UH1071EJ0120 Rev.1.20, register
detail in R01UH1072EJ0120):

| Need | Where |
|---|---|
| SSIU overview, ten SSI modules, SCK 297.3 kHz to 12.5 MHz | 8.5.1 |
| Register access port and DMA access port | 8.5.2.1, 8.5.2.2 |
| `SSIU_SSIn_BUSIF_*`, `SSIU_SSIq_MODE/CONTROL/STATUS`, `SSIU_SSI_MODE1..3`, `SSIU_SSICRn/SSISRn/SSIWSRn/SSIFMRn`, `SSIU_SSIn_BUSIF` FIFO | 8.5.2.3.1 to 8.5.2.3.34 |
| Basic configuration, shared-SCK groups, full duplex, bus formats | 8.5.3.2, 8.5.3.3, 8.5.3.7, 8.5.3.8 |
| Tx/Rx sequence, bit-clock control, pin connections, interrupts | 8.5.3.10 to 8.5.3.14 |
| Audio clock generator (ADG) input dividers and audio master clocks | 8.1.3.6.1, 8.1.3.6.3, 8.1.3.6.4, 8.3 |
| Bus addresses: SSIU registers and SSIU (DMAC) FIFO port, secure and non-secure windows | 4.x memory map (SSIU 0x43C3_0000 / 0x53C3_0000, SSIU DMAC 0x43C4_0000 / 0x53C4_0000) |
| DMA request lines `ssipNM_dreq_rx/tx` per SSI in the ICU input-event list | Table 4.6-23 |
| Clock and reset: SSIF clock, SSI module resets, MSTOP | 4.4 (CPG) |

Facts a driver author must plan for:

- **FSP/HAL:** no FSP module exists for this IP on RZ/V2N, so the register
  layer, the CPG clock/reset/MSTOP calls, and the pinmux calls are new code.
  Reuse only the existing FSP DMAC-B glue (a DMAC-B channel parks after its
  transfer completes and must be re-armed explicitly; see
  `docs/rzv2n-m33-swd-debug.md` for the CM33 debug setup) and the `renesas,rz-*` DT/pinctrl idiom.
- **DMA path:** the SSIU FIFO is fed through the SSIU (DMAC) window with the
  per-SSI DREQ lines; the ICU must route those events to the DMAC unit the
  CM33 owns.  The CM33 reaches DDR only through the 128 MiB window described
  in `metadata/socs/renesas/rzv2n/n44.json`, so audio buffers must sit in
  CM33-reachable RAM.
- **Clocks:** the only usable audio master is Audio_CLKB (the SoM's clock
  generator SE3 output, 24.576 MHz).  Do not enable Audio_CLKA or Audio_CLKC.
  The SSIF module clock is a CPG-owned clock the A55 also gates (see the
  `0001-clk-renesas-r9a09g056-keep-CM33-owned-RSCI7-on.patch` in
  `meta-alp-sdk/recipes-kernel/linux/linux-renesas_%.bbappend` is the precedent).
- **Ownership:** SSIU1/SSIU2 (I2S0 pads P44, P45, P47) are A55-owned today.
  The SSIU register block and the audio clock generator are shared silicon;
  A55 and CM33 cannot both drive SSI1/SSI2 (SSI2 is slaved to SSI1's pins),
  so the CM33 may own an SSI group only if the Linux DT disables `rcar_sound`
  for it.  The natural split is per group: I2S0 (SSIU1+2) on one core and
  I2S1 (SSIU3+4, pads P12/P13/P04/P15) on the other.  Never share ADG state.

Until a maintainer picks the ownership split and a driver is written,
`i2s0`/`i2s1` on `m33_sm` stay `driver_status: none` and the class stays
NOSUPPORT there.

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
