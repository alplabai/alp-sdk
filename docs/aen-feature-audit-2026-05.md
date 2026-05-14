# Alif Ensemble Feature Audit

**Date:** 2026-05-12
**Owner:** alpCaner
**Status:** research-only, no implementation
**Trigger:** maintainer flagged AEN-family migration risk before promoting
any E1M-AEN SoM to production-supported. Specifically called out GPU2D as
a known gap and asked which other AEN-family accelerators would be
silently lost when an application moves from RZ/V2N to AEN through the
`<alp/*.h>` portable surface.

This is a **block-level** audit. It deliberately does not record
ALP-specific bond-out choices (which AEN pads land on which E1M-AEN SKU,
or which optional blocks are populated on which silicon variant). For
SKU-level detail see `metadata/socs/alif/ensemble/*.json` and
`metadata/e1m_modules/E1M-AEN*.yaml`; the audit cites those files by
path but does not reproduce SKU pin mappings.

---

## 1. Scope

### 1.1 SoMs in audit scope

The E1M-AEN family covers six SKUs sharing one module PCB and one
vendor HAL. The SKUs differ only in which Ensemble silicon tier is
populated:

| SKU            | Silicon | Application CPU                | Real-time CPU         | NPU(s) (datasheet)            |
|----------------|---------|--------------------------------|-----------------------|-------------------------------|
| `E1M-AEN301`   | E3      | -- (no A32)                    | 2x Cortex-M55         | 1x U55-46 + 1x U55-204        |
| `E1M-AEN401`   | E4      | -- (no A32)                    | 2x Cortex-M55         | 1x U55-46 + 1x U55-204 + 1x U85-204 |
| `E1M-AEN501`   | E5      | 1x Cortex-A32 (preliminary)    | 2x Cortex-M55         | 1x U55-46 + 1x U55-204        |
| `E1M-AEN601`   | E6      | 1x Cortex-A32                  | 2x Cortex-M55         | 1x U55-46 + 1x U55-204 + 1x U85-204 |
| `E1M-AEN701`   | E7      | 2x Cortex-A32                  | 2x Cortex-M55         | 1x U55-46 + 1x U55-204        |
| `E1M-AEN801`   | E8      | 2x Cortex-A32                  | 2x Cortex-M55         | 1x U55-46 + 1x U55-204 + 1x U85-204 |

NPU populations are taken from
`metadata/socs/alif/ensemble/e[3-8].json::npus[]`. The E5 entry is
flagged `preliminary: true` because Alif has not released its silicon
datasheet as of 2026-05-12; the topology in the audit follows the SKU
metadata stub (`Topology assumed identical to E7 with one of the two
Cortex-A32 cores depopulated`) and is therefore TBD until confirmed.

### 1.2 AEN SoCs covered

E1 and E1C are listed by Alif on the family page but neither carries a
2D GPU per the product index, and neither is referenced from any
`metadata/socs/alif/ensemble/*.json` file in alp-sdk. They are not
populated on any E1M-AEN SKU and are therefore out of scope for this
audit.

### 1.3 alp-sdk public-surface reference

The currently-shipped portable surfaces this audit compares against are
the public headers in `include/alp/`:

- `peripheral.h` -- GPIO, I2C, SPI, UART
- `pwm.h`, `adc.h`, `counter.h`
- `i2s.h`, `audio.h`
- `can.h`
- `rtc.h`, `wdt.h`
- `tmu.h` (math acceleration)
- `usb.h`
- `display.h`, `camera.h`
- `inference.h`
- `gui.h` (LVGL re-export with `alp_display_t` binding)
- `security.h` (hash / AEAD / TRNG)
- `iot.h`, `ble.h`
- `mproc.h`
- `storage.h`
- `hw_info.h`
- `tmu.h`

Capability counts are emitted into `include/alp/soc_caps.h` per SoC.
See section 2 for what those counts cover today.

---

## 2. Audit method

### 2.1 Sources

Primary public sources (URLs only, no local copies):

- Alif Semiconductor product pages, individual SoC briefs:
  - <https://alifsemi.com/ensemble-e3-series/>
  - <https://alifsemi.com/ensemble-e4-series/>
  - <https://alifsemi.com/ensemble-e5-series/>
  - <https://alifsemi.com/ensemble-e7-series/>
  - <https://alifsemi.com/ensemble-e8-series/>
- Alif open-source repositories:
  - <https://github.com/alifsemi/hal_alif> -- HAL integration layer
  - <https://github.com/alifsemi/zephyr_alif> -- downstream Zephyr fork
  - <https://github.com/alifsemi/sdk-alif> -- official Zephyr SDK
  - <https://github.com/alifsemi/alif_dave2d-driver> -- D/AVE 2D driver
  - <https://github.com/alifsemi/alif_lvgl-dave2d> -- LVGL integration
  - <https://github.com/alifsemi/alif_ml-embedded-evaluation-kit> -- NPU evaluation
  - <https://github.com/alifsemi/alif_ensemble-cmsis-dfp> -- CMSIS DFP
- Zephyr project documentation, all `main` branch as of audit date:
  - <https://docs.zephyrproject.org/latest/hardware/peripherals/index.html>
  - <https://github.com/zephyrproject-rtos/zephyr/tree/main/drivers>
  - <https://github.com/zephyrproject-rtos/zephyr/tree/main/include/zephyr/drivers>
  - <https://github.com/zephyrproject-rtos/zephyr/tree/main/drivers/video>
  - <https://github.com/zephyrproject-rtos/zephyr/tree/main/modules>

Secondary cross-checks from the alp-sdk repo itself:

- `metadata/socs/alif/ensemble/e[3-8].json` -- per-SoC peripheral counts
- `metadata/socs/renesas/rzv2n/n44.json` -- V2N baseline for migration risk
- `metadata/e1m_modules/E1M-AEN*.yaml` -- per-SKU manifests
- `metadata/e1m_modules/aen/from-alif.tsv` -- pad-level routing
- `include/alp/soc_caps.h` -- current portable capability bounds
- `include/alp/peripheral.h`, `adc.h`, `display.h`, `camera.h`,
  `inference.h`, `security.h`, `audio.h`, `i2s.h`, `can.h`, `gui.h`,
  `mproc.h`, `usb.h`, `storage.h`
- `vendors/alif/README.md` -- ALP-side wrapper status
- `docs/soms/aen.md` -- current SoM documentation
- `docs/os-support-matrix.md` -- current per-class status grid

### 2.2 Zephyr revision referenced

The audit is grounded against Zephyr `main` at audit time. The
alp-sdk `west.yml` pins `zephyr v3.7.0` as the verified LTS revision
(`docs/zephyr-version-policy.md`); driver-class availability on that
revision is a strict subset of `main`. The peripherals doc page on
`docs.zephyrproject.org` reflects `main` and may surface classes that
are not yet in v3.7.0. Where this matters the audit flags the
discrepancy.

### 2.3 Caveats

- Alif's per-SoC product brief pages were used as the primary source
  for the "which blocks exist on the silicon" decision. The
  `metadata/socs/alif/ensemble/e[3-8].json` peripheral counts are
  cross-checks; where the JSON `peripherals: {}` is empty (E4/E5/E6)
  the audit uses the Alif brief instead.
- The E5 datasheet is not yet public. Where E5 is called out the audit
  marks the block "speculative" or "per SoM yaml stub".
- Alif HWRM files (Hardware Reference Manuals) for E4/E6/E8 are
  Alif-confidential per `metadata/socs/alif/ensemble/e[468].json` and
  cannot be cited here. Anything that required HWRM-only detail is
  marked TBD.
- Whether the upstream Zephyr `hal_alif` module wires a given block to
  a Zephyr driver class is a separate question from whether the block
  exists. The audit answers both, but block presence is more confident
  than wiring presence.

---

## 3. Per-block matrix

The matrix sorts by bucket-priority -- gaps that surface as silent
feature-loss on a V2N to AEN swap come first, then the LP_PERIPHERAL
set, then routine RT-MCU peripherals already covered today.

Bucket key:

- **C**   COVERED -- Zephyr has a stable driver class AND Alif's HAL pack wires it AND alp-sdk has a portable surface today.
- **CP**  COVERED-AFTER-DRIVER-PACK -- Zephyr class exists; the alp-sdk surface exists; only the AEN-side backend wiring is pending.
- **N**   NEEDS-PORTABLE-SURFACE -- no Zephyr driver class. alp-sdk should add an `<alp/*.h>` surface with vendor-HAL backend on AEN and NOSUPPORT / SW fallback elsewhere.
- **D**   DEFERRED -- specialty / low demand; documented but not on the roadmap.

Score columns are integer 1..5 unless stated; "Migration risk" is
low / med / high meaning what a V2N owner would experience on a
silent silicon swap.

| # | Block | Capability summary | SoCs that ship it | Zephyr driver class | Bucket | Cust. demand | Datasheet importance | Migration risk |
|--:|-------|--------------------|-------------------|---------------------|:------:|:------------:|:--------------------:|:--------------:|
| 1 | GPU2D (D/AVE 2D) | Vector / blit / alpha-blend 2D acceleration for LVGL & UI | E3, E5, E7 (per brief). E4/E6/E8: TBD; product pages mention 2D GPU only on E1/E5/E7. | **None.** No `gpu` / `2d` / `dave2d` class in Zephyr drivers tree or include/zephyr/drivers/. | **N** | 5 | 5 | high |
| 2 | Ethos-U55 NPU | 256 MAC/cycle int8 inference engine, ~204 GOPS @ 400 MHz | E3, E4, E5, E6, E7, E8 (all six) | **None.** `modules/hal_ethos_u` exists; no driver-class abstraction. Inference dispatch lives at app layer via TFLM + Vela. | **N (via `<alp/inference.h>`)** | 5 | 5 | high |
| 3 | Ethos-U85 NPU | Generative-AI tier, Transformer-supporting | E4, E6, E8 only | None (same as U55 -- no class abstraction). | **N (via `<alp/inference.h>`)** | 4 | 5 | high |
| 4 | ISP Acceleration Unit | Demosaic, white-balance, exposure, gamma; 2MP @ up to 400 MHz | E4, E6, E8. NOT present on E3, E5, E7. | **None.** Zephyr `drivers/video/` covers sensor + bridge + framebuffer but no standalone ISP class. | **N** | 4 | 5 | high |
| 5 | JPEG encoder | 2MP @ 200 FPS, MJPEG, YUV/RGB | E4, E6, E8. NOT on E3, E5, E7. | **None** at class level. `drivers/video/video_stm32_jpeg.c` exists as a per-vendor driver, but there is no `jpeg` driver class header. | **N** | 3 | 4 | med |
| 6 | MIPI CSI-2 receiver (2-lane) | 2.5 Gbps/lane camera ingress | E3, E4, E5, E6, E7, E8 (all six, 1 instance each) | **Partial.** Zephyr `drivers/video/` ships MIPI-CSI2 RX implementations for NXP & ST. No Alif backend upstreamed. `alifsemi/zephyr_alif` fork adds a separate `mipi_dphy` driver class. | **CP** | 4 | 5 | med |
| 7 | MIPI DSI transmitter (2-lane) | 2.5 Gbps/lane display output | E3, E4, E5, E6, E7, E8 (all six, 1 instance each) | **Yes.** Zephyr ships `drivers/mipi_dsi/` and `include/zephyr/drivers/mipi_dsi.h`. No Alif backend in upstream Zephyr today. | **CP** | 4 | 5 | med |
| 8 | Parallel CPI (camera) | up to 16-bit camera-parallel ingress | All six SoCs (1 instance each) | None for CPI specifically; folds into `drivers/video/`. | **CP / N** | 2 | 3 | low |
| 9 | LP CPI | up to 8-bit low-power CPI | All six SoCs | None. | **D** | 1 | 2 | low |
| 10 | Parallel DPI (display) | up to 24-bit RGB display-parallel | All six SoCs | Folds into `drivers/display/`; no standalone DPI class. | **CP** | 2 | 3 | low |
| 11 | LCD controller | Graphics LCD controller (display engine) | All six SoCs (per product briefs) | Same `drivers/display/` class as DPI. | **CP** | 3 | 4 | low |
| 12 | Inline AES on OSPI / HexSPI | XIP-time decryption, AES-256, no extra round-trip | E3 / E5 / E7 (OSPI 2x), E4 / E6 / E8 (HexSPI 2x with 400 MB/s SDR / 800 MB/s DDR) | **None.** No Zephyr `inline_aes` or "encrypted-xip" driver class. | **N** (vendor-only feature; alp-sdk should expose a small surface or document it as opt-out) | 3 | 5 | med |
| 13 | TRNG (NIST-compliant) | Entropy for AEAD/TLS/BLE | All six SoCs | **Yes.** Zephyr `drivers/entropy/` is stable. No upstream Alif backend. | **CP** -- already declared via `<alp/security.h>::alp_random_bytes` | 5 | 5 | low |
| 14 | Crypto AES-256 + ECC-384 + SHA-256 + RSA-3072 | Symmetric + asymmetric crypto + hash | All six SoCs | Zephyr `drivers/crypto/` exists (Experimental); PSA Crypto via MbedTLS is the stable path. No upstream Alif backend wires AES into PSA. | **CP** -- already declared via `<alp/security.h>::alp_hash_*` + `alp_aead_*`. Hardware acceleration on AEN is opportunistic. | 5 | 5 | med |
| 15 | Hardware Root-of-Trust + Unique Device ID + Secure Enclave | Factory-provisioned identity, signed certificates | All six SoCs | None as a portable driver class; `drivers/hwinfo/` covers Device ID but not RoT cert chain. | **N** | 3 | 5 | med |
| 16 | USB 2.0 HS/FS Host/Device | HS USB peripheral with DMA | All six SoCs (1 instance) | **Yes.** `drivers/usb/`. No upstream Alif backend. | **CP** -- already declared via `<alp/usb.h>` | 4 | 4 | low |
| 17 | USB-C Port Controller (TCPC) | USB-C role detection | **Not present** on Ensemble. E1M-AEN family docs (`vendors/alif/README.md`) note "Only USB 2.0 is exposed (`USB2_*`)". | `drivers/usb_c/` exists. Class is upstream but Ensemble silicon lacks the block. | **D** for AEN. Document the gap. | 3 | 2 | low |
| 18 | Ethernet MAC 10/100 + DMA | 1x RMII | All six SoCs (1 instance) | **Yes.** `drivers/ethernet/`. No upstream Alif backend. | **CP** -- network stack via Zephyr `net_*`; no `<alp/eth.h>` exists. | 4 | 4 | low |
| 19 | CAN-FD (10 Mbps) | 1x CAN-FD | All six SoCs (1 instance) | **Yes.** `drivers/can/`. No upstream Alif backend; AEN routing has `CAN_STBY` per the alif TSV. | **CP** -- already declared via `<alp/can.h>` | 4 | 4 | low |
| 20 | SDIO v4.1 + eMMC v5.1 (1 channel + DMA) | Mass storage / Wi-Fi SDIO | All six SoCs | **Yes.** `drivers/sdhc/` + `drivers/disk/`. No upstream Alif backend. | **CP** -- declared via `<alp/storage.h>`. The AEN family also uses SDIO toward the CC3501E co-MCU for Wi-Fi data. | 5 | 5 | low |
| 21 | I2C (4 instances) | up to 3.4 Mbps | All six SoCs | **Yes** | **C** -- `<alp/peripheral.h>::alp_i2c_*` | 5 | 5 | low |
| 22 | LP I2C (1-2 instances) | low-power variant | E3 / E5 / E7: 1. E8: 2. (E4/E6: TBD per JSON) | Same Zephyr `i2c` class -- LP wakeup semantics are not class-level. | **CP** -- same surface, backend must mark instance LP-capable for the studio. | 3 | 3 | med (silent wake-from-stop loss) |
| 23 | LP UART | 1 instance | All six SoCs | Same Zephyr `serial` class | **CP** -- same surface; LP wakeup semantics drop without backend wiring. | 3 | 3 | med |
| 24 | LP SPI | 1 instance | All six SoCs | Same `spi` class | **CP** -- same surface. | 2 | 3 | med |
| 25 | LP I2S | 1 instance | All six SoCs | Same `i2s` class | **CP** -- same surface. | 2 | 3 | low |
| 26 | LP PDM (4 instances, 2-channel each) | LP DMIC ingress for always-on voice wake | All six SoCs | Zephyr `drivers/audio/dmic/` covers PDM via the `audio_dmic` class. | **CP** -- declared via `<alp/audio.h>::alp_audio_in`. LP-wake semantics are NOT in-class today. | 4 | 4 | med (silent loss of always-on voice wake on a V2N migration) |
| 27 | UART (8 instances) | up to 2.5 Mbps | All six SoCs | **Yes** | **C** -- `<alp/peripheral.h>::alp_uart_*` | 5 | 5 | low |
| 28 | SPI (4 instances) | up to 50 Mbps | All six SoCs | **Yes** | **C** -- `<alp/peripheral.h>::alp_spi_*` | 5 | 5 | low |
| 29 | I3C (1 instance) | MIPI I3C | All six SoCs | **Yes** -- `drivers/i3c/` | **CP** -- not yet declared via `<alp/*.h>` | 2 | 3 | low |
| 30 | I2S (4 sync + 1 LP) | stereo audio I/O | All six SoCs | **Yes** -- `drivers/i2s/` | **C** -- `<alp/i2s.h>` ships | 4 | 4 | low |
| 31 | PDM (4 instances, 2-channel each, full-power) | DMIC ingress | All six SoCs | Same DMIC class | **C** -- `<alp/audio.h>` | 4 | 4 | low |
| 32 | ADC 12-bit (3 instances, 18 SE inputs) | general-purpose analog ingest | All six SoCs | **Yes** -- `drivers/adc/` | **C** -- `<alp/adc.h>` | 5 | 5 | low |
| 33 | ADC 24-bit (1 instance, 4 diff inputs, PGA 1-128x) | precision differential ADC | All six SoCs | Same class; 24-bit resolution is in-class.  | **CP** -- `<alp/adc.h>::resolution_bits` already supports 24 per `soc_caps.h`. Backend wiring TBD. | 4 | 5 | med (a V2N migration silently loses 24-bit if backend stays 12) |
| 34 | DAC 12-bit (2 instances) | analog output | All six SoCs | **Yes** -- `drivers/dac/` | **C** -- `<alp/adc.h>::alp_dac_*` | 4 | 4 | low |
| 35 | High-speed comparator (4 + 16 inputs) | 2.5 ns response | All six SoCs | Zephyr `drivers/comparator/` exists. | **CP** -- not yet declared via `<alp/*.h>` | 3 | 4 | med |
| 36 | LP comparator (1, 4 inputs) | always-on threshold detection | All six SoCs | Same class | **CP** | 3 | 3 | med (always-on wake gone if not wired) |
| 37 | 32-bit timers (12 instances, motor/LED capable) | high-resolution timers | All six SoCs | Zephyr `drivers/counter/` + `pwm/` + `timer/` | **C** -- `<alp/counter.h>` + `<alp/pwm.h>` ship | 4 | 4 | low |
| 38 | LP 32-bit timers (4, plus 3 LP-32 on E8) | low-power timing | All six SoCs | Same class | **CP** | 2 | 3 | low |
| 39 | Watchdogs (2 on E3, 4 on E7/E8) | independent + window watchdog | All six SoCs | Zephyr `drivers/watchdog/` | **C** -- `<alp/wdt.h>` ships | 4 | 4 | low |
| 40 | RTC | 1 instance with backup register | All six SoCs | Zephyr `drivers/rtc/` | **C** -- `<alp/rtc.h>` ships | 4 | 4 | low |
| 41 | Quadrature encoder (4 instances) | motor encoder | All six SoCs | Zephyr `drivers/sensor/` `qdec` | **CP** -- declared via `<alp/counter.h>`; backend TBD | 3 | 3 | low |
| 42 | DMA (3 controllers, 32 channels each, general) | block transfer engine | All six SoCs (E7/E8 same as E3) | Zephyr `drivers/dma/` | **CP** -- never user-facing; backend transparent | 2 | 3 | low |
| 43 | OSPI (2 instances, 100 MHz / 200 MB/s DDR) | external XIP NOR | E3, E5, E7 | Zephyr `drivers/flash/` + `drivers/mspi/` | **CP** -- declared via `<alp/storage.h>` | 4 | 4 | low |
| 44 | HexSPI (2 instances, 400 MB/s SDR / 800 MB/s DDR) | external XIP NOR with 16-bit data bus | E4, E6, E8 ONLY -- replaces OSPI on the U85 tier | No upstream Zephyr `hexspi` class. Vendor HAL only (`hal_alif/drivers/ospi/`). | **N (or CP via `<alp/storage.h>`)** | 3 | 5 | high (the new SKU's primary external-flash interface is silent if the surface doesn't expose it) |
| 45 | MRAM (on-die, 5.5 MB) | non-volatile retentive RAM | All six SoCs | Zephyr `drivers/flash/` if registered as flash; no MRAM-specific class. | **CP** -- declared via `<alp/storage.h>::ALP_STORAGE_KIND_INTERNAL_FLASH` | 5 | 5 | low |
| 46 | SRAM banks (3.75 - 13.75 MB depending on variant) | on-die SRAM | All six SoCs | Not a driver class; lives in linker map. | n/a | 5 | 5 | low |
| 47 | Always-on utility SRAM (4 KB) | retained over STOP | All six SoCs | Zephyr `drivers/retained_mem/` | **CP** | 3 | 4 | med (always-on retention gone if not wired) |
| 48 | OTP (one-time programmable) | factory + customer fuses | All six SoCs (per product briefs; not enumerated in `metadata/socs/alif/ensemble/*.json`) | Zephyr `drivers/otp/` (subsystem) | **CP / N** | 3 | 5 | med |
| 49 | Brown-out detector | reset on supply dip | E3 (called out on product brief); E7/E8 likely (not explicitly listed) | No class; folds into `drivers/hwinfo/`. | **D** | 2 | 3 | low |
| 50 | CRC accelerator | hardware CRC | All six SoCs | Zephyr `drivers/crc/` | **CP** | 2 | 3 | low |
| 51 | Global event mapping | inter-peripheral trigger fabric | All six SoCs | None | **N (low priority)** | 2 | 3 | low |
| 52 | Clock generation (LFRC/HFRC/LFXO/HFXO/PLL) | system clocks | All six SoCs | `drivers/clock_control/` | **CP** -- transparent | 2 | 4 | low |
| 53 | "aiPM" power management | autonomous power gating | All six SoCs (E3 brief calls out "Autonomous Intelligent Power Management") | None directly; folds into `drivers/pm_cpu_ops/` + Zephyr PM subsystem | **N / D** | 4 | 5 | high (always-on duty-cycle differs vs V2N, silent migration risk) |
| 54 | HW IPC / MHU mailbox between M55-HP / M55-HE / A32 | inter-core wake-up signaling | All six SoCs (M55 pair) and E5/E6/E7/E8 also A32 | Zephyr `drivers/mbox/`, `drivers/ipm/`, `drivers/hwspinlock/` | **CP** -- declared via `<alp/mproc.h>` | 4 | 5 | med |
| 55 | OPAMP (operational amplifier) | analog signal conditioning | TBD -- not explicitly called out in the E3/E5/E7 brief. Some Ensemble docs reference it; needs HWRM confirmation. | `drivers/opamp/` (exp.) | **D / TBD** | 2 | 2 | low |
| 56 | Temperature sensor (internal die) | thermal monitoring | All six SoCs | `drivers/sensor/` | **CP** | 3 | 3 | low |
| 57 | Precision voltage reference | reference for ADC | All six SoCs | Folds into `drivers/adc/` reference selection | **C** -- `<alp/adc.h>::reference` already exposes this | 3 | 4 | low |
| 58 | Wi-Fi (on-die) | -- | **Not present on Ensemble**. E1M-AEN routes Wi-Fi via the off-module TI CC3501E co-MCU (see `vendors/alif/README.md`, `docs/cc3501e-bridge.md`). | `drivers/wifi/` | n/a -- handled at module level | 5 | n/a | n/a |
| 59 | BLE (on-die) | -- | Not present on Ensemble (CC3501E owns BLE on E1M-AEN, per `docs/soms/aen.md`). | `drivers/bluetooth/` | n/a | 5 | n/a | n/a |

Notes on the matrix:

- "All six SoCs" is shorthand for the E3..E8 series populated on the
  six E1M-AEN SKUs. Where a feature is conditional on a SKU variant
  (`optional_features` in the SoC JSON), the row says so.
- "TBD per JSON" means the `peripherals: {}` block in the relevant
  SoC JSON is intentionally empty pending v1.0 re-ingest (see the
  notes in `metadata/socs/alif/ensemble/e[4-6].json`).
- "speculative" entries are based on the E7-equivalent topology
  documented in the SoM yaml stub and the Alif family-overview page,
  not on an E5 datasheet.
- "Migration risk" considers the V2N to AEN swap specifically:
  - **high** = the feature is present on AEN but invisible through
    today's `<alp/*.h>` surface, so an application that doesn't
    explicitly opt into a vendor escape-hatch will silently lose it.
  - **med** = the surface is class-correct on Zephyr but the
    semantic wins (e.g. LP wakeup, 24-bit ADC) are silently lost
    until backend wiring lands.
  - **low** = behaviour parity is reachable without an SDK surface
    change, only backend wiring.

---

## 4. Bucket breakdown

### 4.1 COVERED (C) -- 10 blocks

These ship today via existing `<alp/*.h>` surfaces and have either
Zephyr backend wiring or a defined plan in `docs/os-support-matrix.md`
v0.2:

- I2C (`<alp/peripheral.h>`)
- SPI (`<alp/peripheral.h>`)
- UART (`<alp/peripheral.h>`)
- GPIO (`<alp/peripheral.h>`)
- I2S (`<alp/i2s.h>`)
- PDM (full-power, `<alp/audio.h>`)
- ADC 12-bit (`<alp/adc.h>`)
- DAC 12-bit (`<alp/adc.h>`)
- Timers, PWM, watchdog, RTC, counter, QEnc (`<alp/pwm.h>`,
  `<alp/wdt.h>`, `<alp/rtc.h>`, `<alp/counter.h>`)
- Reference selection on ADC (folds into `<alp/adc.h>::reference`)

Migration impact: zero -- a V2N application that only touches the C
bucket runs portably on E1M-AEN.

### 4.2 COVERED-AFTER-DRIVER-PACK (CP) -- 27 blocks

These have a Zephyr driver class but no upstream Alif backend wiring,
or have a class + a backend but the `<alp/*.h>` surface hasn't grown
an explicit knob:

- MIPI CSI-2 RX (1) -- class exists; backend TBD; `<alp/camera.h>` is
  already a stub.
- MIPI DSI (1) -- class exists; backend TBD; `<alp/display.h>` is
  shipped but no DSI mode is configurable today.
- Parallel CPI / DPI / LCD controller (3) -- fold into video / display.
- TRNG (1) -- declared via `<alp/security.h>`; backend TBD.
- Crypto AES/ECC/SHA/RSA (1) -- declared via `<alp/security.h>`;
  hardware-accelerated path on AEN is opportunistic.
- USB 2.0 (1) -- `<alp/usb.h>` declared; backend TBD.
- Ethernet 10/100 (1) -- exposed via Zephyr `net_*`; no `<alp/eth.h>`.
- CAN-FD (1) -- `<alp/can.h>` ships.
- SDIO/eMMC + flash (3 -- SDIO, OSPI, MRAM) -- declared via
  `<alp/storage.h>`; backend TBD.
- LP variants (5 -- LP I2C, LP UART, LP SPI, LP I2S, LP PDM) -- same
  Zephyr classes; LP wakeup semantics need backend wiring.
- I3C (1) -- class exists; surface TBD.
- ADC 24-bit (1) -- `<alp/adc.h>` already supports the resolution;
  backend wiring TBD.
- High-speed + LP comparator (2) -- class exists; surface TBD.
- LP 32-bit timers (1) -- same `counter` class.
- DMA (1) -- transparent to consumer; backend only.
- Retained mem (1 -- always-on SRAM) -- class exists; surface TBD.
- OTP (1) -- subsystem exists; surface TBD.
- CRC accelerator (1) -- class exists; surface TBD.
- Mailbox / IPM / HW-spinlock (3 -- combined) -- `<alp/mproc.h>` shipped.
- Clock control (1) -- transparent.
- Internal temperature sensor (1) -- class exists; surface TBD.

Migration impact: medium for LP and ADC-24 categories (the wins
disappear without backend wiring), low for everything else.

### 4.3 NEEDS-PORTABLE-SURFACE (N) -- 10 blocks

Blocks that have NO Zephyr driver class. alp-sdk should add a small
`<alp/*.h>` surface so applications stay portable across V2N and AEN,
even if the V2N backend lands `ALP_ERR_NOSUPPORT`:

1. **GPU2D (D/AVE 2D)** -- 2D blit / blend acceleration for LVGL & UI.
2. **NPU inference** -- the Ethos-U55 / U85 dispatch path. Already
   reserved as `<alp/inference.h>` (v0.1 stub). Promote to first-class
   v0.x deliverable.
3. **ISP** -- demosaic + WB + exposure pipeline. Present on E4/E6/E8 only.
4. **JPEG encoder** -- 2MP @ 200 FPS hardware MJPEG. Present on E4/E6/E8.
5. **Inline AES on OSPI/HexSPI** -- XIP-time decryption. Class-level
   gap across the embedded ecosystem; alp-sdk should expose it as an
   option on `<alp/storage.h>` rather than a separate header.
6. **Hardware Root-of-Trust + secure-enclave services** -- factory
   key provisioning, signed cert storage. Beyond `hwinfo` Device-ID.
7. **HexSPI (E4/E6/E8 only)** -- the U85-tier SoCs swap OSPI for
   HexSPI. Whether this is one surface with two backends or two
   surfaces is an open question (probably one with a `kind` enum).
8. **aiPM autonomous power management** -- Ensemble-specific power
   policy fabric. Goes well beyond Zephyr's `pm_cpu_ops`.
9. **Global event mapping / inter-peripheral trigger fabric** -- low
   priority but no class-level equivalent.
10. **MIPI D-PHY explicit configuration** -- the Alif `zephyr_alif`
    fork carries a `mipi_dphy` driver class that upstream Zephyr does
    not have; CSI-2 + DSI both ride it.

Per-class signature sketches live in section 5.

### 4.4 DEFERRED (D) -- 5 blocks

Documented for completeness; not on the roadmap:

- LP CPI -- niche; folds into `<alp/camera.h>` configuration eventually.
- USB-C TCPC -- AEN silicon does not implement it.
- Brown-out detector -- handled by reset-controller / power-domain
  Kconfig; no public API.
- OPAMP -- TBD presence on Ensemble; needs HWRM.
- Wi-Fi / on-die BLE -- not present on Ensemble. The CC3501E
  co-processor owns these on E1M-AEN.

---

## 5. Roadmap recommendations

### 5.1 Top-priority `<alp/*.h>` surfaces to add (sorted by demand x importance)

Score = demand x importance.

| Rank | Block | Demand x Importance | Surface |
|-----:|-------|--------------------:|---------|
| 1 | NPU inference (Ethos-U55 + U85) | 5 x 5 = 25 | `<alp/inference.h>` already declared in v0.1 as a stub. Promote to first-class implementation. |
| 2 | GPU2D (D/AVE 2D) | 5 x 5 = 25 | New `<alp/gfx2d.h>`. Optional; LVGL gets the backend automatically via `<alp/gui.h>`. |
| 3 | ISP pipeline | 4 x 5 = 20 | New `<alp/isp.h>` OR extend `<alp/camera.h>` with an `isp_config` block. Recommended: extend camera. |
| 4 | Inline AES on OSPI/HexSPI | 3 x 5 = 15 | Extend `<alp/storage.h>` config struct (one new field). Avoid a dedicated header. |
| 5 | Hardware Root-of-Trust + Secure Enclave | 3 x 5 = 15 | New `<alp/rot.h>` or extend `<alp/security.h>`. Recommended: extend security. |
| 6 | JPEG encoder | 3 x 4 = 12 | New `<alp/jpeg.h>` OR extend `<alp/camera.h>` with a "delivery format" enum. Recommended: extend camera. |
| 7 | aiPM autonomous PM | 4 x 5 = 20 (but low feasibility) | Out-of-scope for a portable surface; document, don't unify. Mark deferred. |

The "extend rather than create" recommendation aligns with the
SDK-wide "best-effort, not absolute" stance documented in
`include/alp/inference.h::35-39` ("Vendor-specific accelerator paths
remain available as escape hatches when the unified API can't express
what the vendor SDK offers").

### 5.2 Signature sketches for the top 3 priorities

Sketches only; no implementation, no header to be written from this
audit. The portable-surface rule applies: the public API hides the
`alif_*` vendor prefix and uses the SDK's `alp_status_t` /
`alp_*_open` idiom.

#### 5.2.1 NPU inference (already declared, ranks #1)

The existing surface in `include/alp/inference.h::40-167` already
covers the v1.0 shape:

- `alp_inference_open(cfg)` with `backend = ALP_INFERENCE_BACKEND_ETHOS_U`
- `alp_inference_num_inputs` / `_num_outputs`
- `alp_inference_get_input` / `_get_output`
- `alp_inference_invoke`
- `alp_inference_close`

What's needed to promote the surface from stub to first-class:

- Wire the Ethos-U backend in `src/zephyr/inference_ethos_u.c`.
- Provide Vela-compiled model loaders for `.tflite` + Vela meta.
- Pick which MAC tier the runtime targets when both U55-46 + U55-204
  are present (E3/E5/E7) -- the v0.1 stub picks "best available";
  applications wanting deterministic placement need an explicit knob.
- E8 introduces U85 which supports Transformer ops the U55 doesn't;
  the backend choice has to consider model graph compatibility too.

#### 5.2.2 GPU2D / D/AVE 2D (rank #2)

```c
/* <alp/gfx2d.h> -- hardware 2D blit / blend acceleration. */

typedef struct alp_gfx2d alp_gfx2d_t;

typedef struct {
    uint32_t engine_id;         /* studio-resolved -- 0 on AEN */
} alp_gfx2d_config_t;

alp_gfx2d_t *alp_gfx2d_open(const alp_gfx2d_config_t *cfg);

/* Blit src rect into dst, optionally with an alpha mask. */
alp_status_t alp_gfx2d_blit(alp_gfx2d_t *g,
                            const alp_gfx2d_surface_t *dst,
                            uint16_t dx, uint16_t dy,
                            const alp_gfx2d_surface_t *src,
                            uint16_t sx, uint16_t sy,
                            uint16_t w, uint16_t h,
                            uint8_t alpha);

/* Fill a rect with a constant colour (post-blend). */
alp_status_t alp_gfx2d_fill(alp_gfx2d_t *g,
                            const alp_gfx2d_surface_t *dst,
                            uint16_t x, uint16_t y,
                            uint16_t w, uint16_t h,
                            uint32_t argb8888);

/* Block until the engine is idle (callers usually don't need this). */
alp_status_t alp_gfx2d_wait(alp_gfx2d_t *g, uint32_t timeout_ms);

void alp_gfx2d_close(alp_gfx2d_t *g);
```

Backends:

- AEN / Zephyr: wraps `alif_dave2d-driver`.
- AEN / bare-metal: same vendor driver.
- V2N / Yocto: software fallback via Cairo or Skia, marked
  `ALP_ERR_NOSUPPORT` for the alpha-blend path until Mali-G31 wiring
  lands; everything else hits a software path.
- Other: `ALP_ERR_NOSUPPORT`.

`<alp/gui.h>::alp_gui_lvgl_attach` should auto-bind to the GFX2D
backend if `ALP_HAS_GFX2D` is set, so LVGL apps get the speedup
without app-code changes.

#### 5.2.3 ISP pipeline (rank #3) -- proposed as a camera-config extension

```c
/* Inside <alp/camera.h>, alp_camera_config_t gains an opt-in ISP block.
 * Existing fields (camera_id, width, height, fps, format) stay
 * unchanged for backward compatibility. */

typedef struct {
    bool      enable_demosaic;        /* sensor delivers Bayer; ISP debayers */
    bool      enable_white_balance;   /* auto WB on / off */
    bool      enable_auto_exposure;   /* AE on / off */
    uint8_t   gamma_curve_id;         /* 0 = default; vendor-specific id otherwise */
    bool      enable_rotation;        /* H + V flip */
    uint8_t   crop_x_pct;             /* 0..50 -- studio-quantised */
    uint8_t   crop_y_pct;             /* 0..50 */
} alp_camera_isp_config_t;

typedef struct alp_camera_config alp_camera_config_t;  /* existing */
/* alp_camera_config_t gains a .isp = NULL-or-pointer field. */
```

Backends:

- AEN E4/E6/E8: wraps the ISP Acceleration Unit via `hal_alif/drivers/isp/`.
- AEN E3/E5/E7: `ALP_ERR_NOSUPPORT` when `cfg.isp != NULL` -- the SoC
  has no ISP. Apps must fall back to software demosaic if they want the
  same pipeline.
- V2N: Mali-C55 ISP backend when the `optional_features.isp_mali_c55 ==
  true` SKU is populated; otherwise software fallback.
- Other: software fallback (CPU debayer at low FPS).

### 5.3 Per-AEN-SKU migration risk summary

How much silent-feature-loss a V2N owner takes on a per-SKU swap:

| Target SKU       | High-risk losses                                 | Net assessment |
|------------------|---------------------------------------------------|----------------|
| `E1M-AEN301`     | NPU not abstracted; GPU2D not abstracted          | Watchable -- two N-bucket items |
| `E1M-AEN401`     | + ISP not abstracted; + JPEG not abstracted; HexSPI is the boot interface | High -- four N-bucket items |
| `E1M-AEN501`     | NPU not abstracted; GPU2D not abstracted (Cortex-A32 adds Linux side too) | Watchable + A32 concerns |
| `E1M-AEN601`     | + ISP; + JPEG; + U85 NPU                          | High -- five N-bucket items |
| `E1M-AEN701`     | NPU not abstracted; GPU2D not abstracted          | Watchable |
| `E1M-AEN801`     | + ISP; + JPEG; + U85 NPU; HexSPI boot             | Highest -- six N-bucket items + boot-interface change |

Recommendation:

- Promote E1M-AEN301 and E1M-AEN701 to production-supported FIRST,
  after `<alp/inference.h>` and `<alp/gfx2d.h>` land. The N-bucket
  surface delta for these two SKUs is the smallest.
- Hold E1M-AEN401 / 601 / 801 in "preliminary" until ISP + JPEG +
  HexSPI surfaces are also in place; promoting them earlier means the
  alp-sdk-portable application loses the headline U85 / generative-AI
  / vision-pipeline value props that are why a customer would pick
  those SKUs in the first place.

---

## 6. Open questions for maintainer

These could not be resolved from public docs and need maintainer input
or HWRM access:

1. **Which `hal_alif` revision is the target?** The alp-sdk
   `west.yml::15-17` defers to Zephyr's `modules/hal/alif` pin and
   does not pick one explicitly. Some peripheral coverage (notably
   ISP / JPEG / NPU) lives in `hal_alif/drivers/{isp,jpeg,ethos_u}`
   but doesn't appear to be wired to Zephyr driver classes -- the
   audit can't tell whether this is by design or pending upstream.
2. **Is Ethos-U inference exposed via a Zephyr ML subsystem we
   missed, or does alp-sdk own the dispatch?** Upstream Zephyr has
   no `ml` / `npu` class; `modules/hal_ethos_u` provides only the
   raw HAL. `<alp/inference.h>` already declares the surface as v0.1
   stub -- confirm that promoting it to first-class implementation is
   the intended path and not "wait for an upstream Zephyr ML class".
3. **Does AEN E5 share the E7 peripheral list, or is the assumption
   in `metadata/socs/alif/ensemble/e5.json` going to be revised when
   the E5 datasheet ships?** Several rows in this audit lean on the
   "Topology assumed identical to E7" stub note in that JSON.
4. **HexSPI vs OSPI as a config-time decision** -- should
   `<alp/storage.h>` distinguish the two via separate
   `ALP_STORAGE_KIND_*` values, or fold both into
   `ALP_STORAGE_KIND_OSPI_FLASH` with a backend-internal mode bit?
   The audit suggested folding, but the bandwidth difference
   (200 MB/s vs 800 MB/s DDR) is large enough that customers might
   want to express which they have.
5. **Inline-AES exposure** -- the AEN OSPI / HexSPI block does inline
   decryption transparent to the consumer (set up at boot, then XIP
   reads decrypt invisibly). Should alp-sdk expose this at all, or
   leave it as a board-config decision invisible to the application
   surface? Hiding it is the SDK's usual stance but encrypted-XIP
   has correctness consequences (key wear-out, key rotation) that
   sometimes belong to the application.
6. **D/AVE 2D licensing** -- the `alif_dave2d-driver` repo on
   alifsemi/GitHub doesn't show its license on the index page in the
   audit fetch. Confirm it's compatible with Apache-2.0 distribution
   before alp-sdk binds it under `<alp/gfx2d.h>`. (Likely TES or BSD
   per vendor-driver norms but not yet verified.)
7. **`alifsemi/zephyr_alif` `mipi_dphy` driver class** -- this is a
   separate class from upstream's `mipi_dsi`. Does the upstreaming
   roadmap merge them, or is `mipi_dphy` going to live downstream
   permanently? If permanent, alp-sdk's MIPI surface needs to abstract
   over two classes, not one.
8. **Always-on voice wake** -- the LP_PDM block plus LP comparator
   plus retained_mem combine to drive always-on keyword-spot. None
   of the three classes today preserves the "wake-from-stop" semantic
   in an SDK-portable way -- is there an `<alp/*.h>` design pattern
   for "always-on monitor with wake callback"?
9. **aiPM exposure** -- treat as deferred (vendor-specific, no class
   equivalent), or treat as a config knob inside `<alp/pwr.h>` (which
   doesn't exist yet)?
10. **Whether E1 / E1C silicon is in scope long-term** -- the Alif
    family page references them but no `metadata/socs/alif/ensemble/`
    JSON exists. If a future E1M-AEN101 / 201 SKU lands, the audit's
    assumption "all six SoCs ship X" will need revising. They're out
    of scope today.
