# Chip + library ecosystem expansion for the ALP SDK

**Status**: design approved 2026-05-14.  Ready for implementation planning.
**Cross-refs**: docs/branching-and-merge-policy.md (PR + override flow),
docs/vendor-partnerships.md (vendor SDK status), docs/v1.0-readiness.md.

## Problem

Embedded SDKs that win their developer audience are the ones
that reach thousands of community-contributed chip drivers +
libraries through a curated index.  A customer who wants to
talk to chip X or use library Y should find it in minutes,
drop it in, and write their app.

The ALP SDK today has **30 chip drivers** (under `chips/`) and
**8 libraries** behind the `board.yaml` `libraries:` enum
(`etl`, `fmt`, `nlohmann_json`, `doctest`, `lvgl`, `mbedtls`,
`cmsis_dsp`, `littlefs`).  That's enough to demonstrate the
pattern but not enough for "hand-write firmware against alp-sdk
and find what you need".  The portable peripheral surfaces
(`<alp/i2c.h>` etc.) and the `chips/<name>/` driver pattern are
the right primitives -- they need an order-of-magnitude more
content and a contribution path so customers can extend the
ecosystem without depending on the maintainer's bandwidth.

## Goals

1. **By v1.0**: 75 chips + 25 libraries in the curated alp-sdk
   repo.  All maintainer-reviewed, Apache-2.0, portability-
   tested, doxygen-clean.
2. **By v1.0**: a parallel `alplabai/alp-sdk-community` repo
   that accepts community-contributed chips + libraries
   under a clear template + lower quality bar (still
   Apache/MIT/BSD; CI-builds-clean).
3. **By v1.0**: customer-facing docs that explain the two-tier
   model + how to add your own (private) chip driver in a
   third tier.

## Non-goals

- **Not** a public registry / search infrastructure on the
  scale of a thousands-of-package package manager.  v1.0 is
  direct GitHub-browsing + `west update --group-filter`.
- **Not** a binary-package distribution (prebuilt
  `.zip`-via-package-manager).  Everything is source-tree
  consumed via `west update`.
- **Not** automated chip-driver generation from datasheet
  PDFs.  Each driver is hand-written + reviewer-approved
  before landing.

## Three-tier ecosystem

```
┌─────────────────────────────────────────────────────────────┐
│ Tier 1: alp-sdk (this repo)                                 │
│ ────────────────────────                                    │
│ 75 chips + 25 libraries.  Maintainer-curated.               │
│ Apache-2.0, portability-tested, doxygen-clean,              │
│ ABI-tracked, CHANGELOG'd.                                   │
│ In-tree under chips/ and the library Kconfig knobs.         │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ Tier 2: alplabai/alp-sdk-community (NEW repo)                │
│ ────────────────────────────────────                         │
│ Community-contributed chips + libraries.                     │
│ Apache/MIT/BSD only.  Standard template + metadata.yaml.     │
│ CI-builds-clean for native_sim.  Per-contribution            │
│ CODEOWNERS = author.                                         │
│ Customer opts-in via `west update --group-filter`.           │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ Tier 3: customer / private repos                            │
│ ─────────────────────────────                               │
│ Customer-owned drivers that don't make sense upstream        │
│ (proprietary silicon, NDA-encumbered, one-off carrier).      │
│ Same `chips/<name>/` shape; consumed via                     │
│ EXTRA_ZEPHYR_MODULES or their own west.yml.                  │
│ Pattern-supported but zero infra on our side.                │
└─────────────────────────────────────────────────────────────┘
```

## Tier 1 chip curation (target 75 chips total: 30 existing + 45 new)

### Existing (30, unchanged)

`act8760`, `bme280`, `bmi323`, `bmp581`, `button_led`,
`cam_mux_pi3wvr626`, `cc3501e`, `clk_5l35023b`, `da9292`,
`deepx_dxm1`, `eeprom_24c128`, `gd32_swd`, `gd32g553`,
`icm42670`, `ina236`, `lis2dw12`, `lsm6dso`,
`murata_lbee5hy2fy`, `optiga_trust_m`, `ov5640`, `pdm_mic`,
`pi3dbs12212`, `rtl8211fdi`, `rv3028c7`, `ssd1306`, `ssd1331`,
`tas2563`, `tcal9538`, `tmp112`, `tps628640`.

### Edge AI + vision (18 new)

| Chip | Class | Bus | Rationale |
|---|---|---|---|
| `ov2640` | image sensor | DVP | ESP32-CAM ecosystem default |
| `ov5645` | image sensor | MIPI CSI-2 | 5 MP, RPi-grade |
| `ov7670` | image sensor | DVP | VGA reference classic |
| `ov9281` | image sensor | MIPI CSI-2 | global-shutter mono, AR/VR/ALPR |
| `ar0234` | image sensor | MIPI CSI-2 | 1080p global shutter, industrial |
| `imx219` | image sensor | MIPI CSI-2 | RPi Cam v2 standard |
| `imx477` | image sensor | MIPI CSI-2 | RPi HQ Camera |
| `gc2145` | image sensor | DVP | 2 MP cost-sensitive China-domestic |
| `ti_ds90ub953_954` | camera serdes | I²C + FPD-Link III | FPD-Link III long-cable cameras |
| `maxim_max9295_9296` | camera serdes | I²C + GMSL2 | GMSL2 automotive ecosystem |
| `st7789` | display | SPI | 240×240 round + 240×320 rect IPS |
| `ili9341` | display | SPI | 240×320 — most common embedded TFT |
| `ili9488` | display | SPI | 480×320 |
| `ra8875` | display | SPI | 5–7″ with resistive touch + LCD ctrl |
| `sh1106` | display | I²C | OLED 128×64 alt to SSD1306 |
| `il3820` | e-paper | SPI | 4.2″ tri-color |
| `gdew0154t8` | e-paper | SPI | 1.54″ |
| `hailo_8l` | ML accelerator | M.2 PCIe | 13 TOPS NPU |

### Industrial sensing + control (16 new)

| Chip | Class | Bus | Rationale |
|---|---|---|---|
| `bmp390` | pressure | I²C/SPI | high-precision altimeter |
| `ms5611` | pressure | I²C/SPI | drone barometer classic |
| `lps22hb` | pressure | I²C/SPI | ST pressure ecosystem |
| `vl53l1x` | ToF distance | I²C | up-to-4m single-zone |
| `vl53l5cx` | ToF distance | I²C | 8×8 multi-zone |
| `a02yyuw` | ultrasonic | UART | waterproof distance |
| `drv8833` | motor driver | GPIO+PWM | dual brushed DC |
| `drv8825` | motor driver | GPIO+PWM | bipolar stepper |
| `tmc2209` | motor driver | UART | silent stepper (3D printer ecosystem) |
| `a4988` | motor driver | GPIO+PWM | low-cost stepper driver classic |
| `as5048a_b` | encoder | SPI/I²C | 14-bit magnetic |
| `mt6701` | encoder | I²C | 14-bit magnetic |
| `hx711` | strain | bit-banged SPI | 24-bit load-cell ADC |
| `max31855` | thermocouple | SPI | K-type |
| `max31865` | RTD | SPI | PT100/PT1000 |
| `tsl2591` | light | I²C | wide-dynamic-range lux |
| `qmc5883l` | magnetometer | I²C | 3-axis compass |
| `veml7700` | light | I²C | high-precision ambient |

### IoT + connectivity (9 new)

| Chip | Class | Bus | Rationale |
|---|---|---|---|
| `quectel_bg95` | LTE-M / NB-IoT | UART AT | popular cellular module |
| `quectel_bg77` | LTE-M / NB-IoT | UART AT | newer Quectel |
| `ublox_sara_r5` | LTE-M | UART AT | carrier-certified |
| `semtech_sx1262` | LoRa | SPI | LoRa (newer SX) |
| `semtech_sx1276` | LoRa | SPI | LoRa (legacy, ubiquitous) |
| `ublox_neo_m9n` | GNSS | UART | high-precision GPS |
| `ublox_max_m10s` | GNSS | UART | small-footprint GNSS |
| `atgm336h` | GNSS | UART | cost-optimised GPS |
| `atecc608b` | secure element | I²C | Microchip Crypto |

### Audio + speech (6 new)

| Chip | Class | Bus | Rationale |
|---|---|---|---|
| `ics_43434` | MEMS mic | I²S | popular MEMS mic with breakout-board support |
| `inmp441` | MEMS mic | I²S | low-cost embedded MEMS mic |
| `wm8960` | codec | I²C + I²S | Wolfson stereo (RPi HAT standard) |
| `tlv320aic3204` | codec | I²C + I²S | TI premium codec |
| `max98357a` | class-D amp | I²S | very common audio breakout |
| `es8388` | codec | I²C + I²S | China-domestic cost-sensitive |

**Total Tier 1 chip count: 30 existing + 49 new = 79.**

## Tier 1 library curation (target 25 libraries total: 8 existing + 17 new)

### Existing (8, unchanged)

`etl`, `fmt`, `nlohmann_json`, `doctest`, `lvgl`, `mbedtls`,
`cmsis_dsp`, `littlefs`.

### New (17)

| Library | Class | Source | Rationale |
|---|---|---|---|
| `tflite_micro` | ML inference | TensorFlow upstream | already in west.yml; add `libraries:` knob |
| `u8g2` | graphics | github.com/olikraus/u8g2 | monochrome OLED default |
| `gfx_compat` | graphics | maintainer-written thin shim | drop-in graphics API for ports of community drawing code |
| `madgwick_ahrs` | sensor fusion | xioTechnologies | quaternion IMU fusion |
| `pid` | control | maintainer-written thin C lib | generic PID |
| `modbus` | industrial bus | libmodbus | RTU + TCP |
| `coremqtt_sn` | IoT | FreeRTOS coreMQTT-SN | sensor-network MQTT |
| `libcoap` | IoT | obgm/libcoap | CoAP + OSCORE |
| `tinygsm` | cellular | vshymanskyy/TinyGSM | modem AT lib |
| `nanopb` | serialization | nanopb-0.4.9 | already in west.yml; add knob |
| `libwebsockets` | networking | warmcat/libwebsockets | WS client/server |
| `jsmn` | parsing | zserge/jsmn | streaming JSON |
| `bearssl` | crypto/TLS | bearssl.org | smaller TLS alt to mbedtls |
| `minimp3` | audio | lieff/minimp3 | MP3 decode |
| `opus` | audio | xiph.org | speech codec |
| `libhelix` | audio | upstream | MP3/AAC decode (smaller than opus) |
| `catch2` | testing | catchorg/Catch2 | alt to doctest |

## Hardware-acceleration backend wiring (per-library HW table)

A library shipping as pure source is the floor.  The ALP SDK's
distinguishing value is that the **same `libraries:` knob also
wires the matching hardware accelerators** — GPU, NPU, DMA, FPU
SIMD, hardware crypto, hardware DSP blocks, timers — when the
selected SoM target has them.  Customers get full compatibility
on every backend and best performance on each.

### Mechanism

1. Each library declares its accelerator hooks in
   `metadata/library-profiles/<name>/hw-backends.yaml`.
2. `scripts/alp_project.py` cross-references the library's
   declared accelerators against the active SoM's
   `metadata/e1m_modules/<SKU>.yaml` `capabilities:` block and emits
   the matching `CONFIG_*` knobs in addition to the library's
   own `CONFIG_*`.
3. Every library MUST have a working pure-software fallback
   (libm / CMSIS-DSP scalar / pure-C reference kernels).
   The HW backend is an opt-in performance bump, never a
   hard dependency.
4. The loader picks **at most one** HW backend per
   (library, accelerator-class) tuple in priority order
   declared in `hw-backends.yaml`.

### Per-library HW backend table (Tier 1)

Cross-platform pattern: rows below name the accelerator family
the library binds to; the actual `CONFIG_*` emitted depends
on the SoM target (Alif AEN / Renesas V2N / GD32 bridge).
"—" means the library is pure-SW with no meaningful HW
binding.

| Library | NPU / ML | GPU / 2D | DSP / SIMD | Crypto | DMA / timing | SW fallback |
|---|---|---|---|---|---|---|
| `tflite_micro` | Ethos-U (AEN), DRP-AI (V2N), CMSIS-NN (Helium MVE), Neon (A55) | — | Helium MVE / Neon | — | DMA for input tensor copy | ref kernels |
| `cmsis_dsp` | — | — | Helium MVE (AEN HE), Neon (A55), TMU CORDIC + FFT (GD32 bridge) | — | DMA for ADC stream chaining | scalar |
| `lvgl` | — | GPU2D (AEN), DAVE2D (Alif), TMU rotate/scale (GD32 bridge) | — | — | DMA2D + display tearing-effect | pure-C blit |
| `u8g2` | — | DMA2D (AEN), TMU (GD32 bridge) | — | — | I²C/SPI DMA frame push | pure-C |
| `gfx_compat` | — | GPU2D (AEN), DMA2D | — | — | SPI DMA | pure-C |
| `madgwick_ahrs` | — | — | FPU, TMU CORDIC (GD32 bridge) | — | — | libm trig |
| `pid` | — | — | FPU | — | timer-tick for loop period | int math |
| `modbus` | — | — | — | — | UART/Ethernet DMA | — |
| `coremqtt_sn` | — | — | — | mbedtls / bearssl via library link | — | — |
| `libcoap` | — | — | — | mbedtls / OSCORE | — | — |
| `tinygsm` | — | — | — | — | UART DMA | — |
| `nanopb` | — | — | — | — | — | pure-SW |
| `libwebsockets` | — | — | — | mbedtls / bearssl | TCP socket + DMA | — |
| `jsmn` | — | — | — | — | — | pure-SW |
| `bearssl` | — | — | — | CAU (GD32 bridge), CryptoCell (AEN), Inline-AES (Alif), OPTIGA-Trust-M handshake | — | pure-C |
| `mbedtls` | — | — | — | CAU (GD32 bridge), CryptoCell (AEN), Inline-AES (Alif), OPTIGA-Trust-M | — | pure-C |
| `minimp3` | — | — | Helium MVE, Neon, FPU | — | I²S DMA out | pure-C |
| `opus` | — | — | Helium MVE, Neon, FPU | — | I²S DMA out | pure-C |
| `libhelix` | — | — | FPU | — | I²S DMA out | pure-C |
| `catch2` | — | — | — | — | — | pure-SW (host only) |
| `etl` | — | — | — | — | — | pure-SW |
| `fmt` | — | — | — | — | — | pure-SW |
| `nlohmann_json` | — | — | — | — | — | pure-SW |
| `doctest` | — | — | — | — | — | pure-SW (host only) |
| `littlefs` | — | — | — | — | xSPI DMA (AEN), QuadSPI DMA (Alif), eMMC DMA (V2N) | sync I/O |

### `hw-backends.yaml` example (shipped per library)

```yaml
# metadata/library-profiles/tflite_micro/hw-backends.yaml
schema_version: 1
library: tflite_micro
accelerators:
  - class: ml_npu
    priority:
      - { soc_family: alif_ensemble,    backend: ethos_u,  kconfig: CONFIG_ALP_TFLM_ETHOS_U=y }
      - { soc_family: renesas_rzv2n,    backend: drp_ai,   kconfig: CONFIG_ALP_TFLM_DRP_AI=y }
  - class: simd
    priority:
      - { soc_family: alif_ensemble,    cpu: cortex_m55,  backend: helium_mve, kconfig: CONFIG_ALP_TFLM_HELIUM=y }
      - { soc_family: renesas_rzv2n,    cpu: cortex_a55,  backend: neon,       kconfig: CONFIG_ALP_TFLM_NEON=y }
  - class: dma
    priority:
      - { backend: tensor_dma_copy,  kconfig: CONFIG_ALP_TFLM_DMA_COPY=y }
sw_fallback:
  required: true
  kconfig: CONFIG_ALP_TFLM_REF_KERNELS=y
```

### Peripheral-class HW wiring (cross-library)

Beyond the per-library accelerators above, the SDK's existing
`<alp/*>` peripheral surfaces stay the bedrock — every library
that needs ADC / PWM / timer / GPIO / I²C / SPI / UART / I²S /
CAN / RTC / WDT / USB / DAC reaches it through the portable
surface, and the **SoM-specific backend picks the most-
accelerated path** (DMA channel, hardware timer chain, IRQ
priority slot) for that SoM.  This is the §C.15+ work already
in flight (ADC stream-DMA, timer-sync, DSP chain pool).  The
library layer doesn't see backend choice — `<alp/adc.h>` reads
ADC samples the same way whether the backend is GD32's stream
DMA or AEN's xSPI-DMA-fed analog front end.

This means a Tier 2 contribution that uses, say, `<alp/pwm.h>`
inside a `tmc2209` stepper driver automatically gets the GD32
bridge's hardware-timer-PWM on V2N + the AEN's TIM/LPTIM-DMA
chain on AEN.  The contributor writes one driver; the SDK
wires the eight underlying backends.

### Implementation cost

- Library-profile loader change: ~80 LoC in
  `scripts/alp_project.py` to consume `hw-backends.yaml` and
  cross-reference with the active SoM's capabilities.
- One `hw-backends.yaml` per Tier 1 library: 25 files,
  hand-written, ~30–60 LoC each.
- SoM capability blocks already exist in
  `metadata/e1m_modules/*.yaml`; one additive pass to fill
  `capabilities:` for each existing SoM (AEN-DK, AEN-CARRIER,
  V2N-DEV, V2N-M101, V2N-M102, GD32-Discovery, etc.).
- Backend Kconfig knobs (`CONFIG_ALP_<LIB>_<BACKEND>`) added
  per library in `Kconfig.alp-libraries`.
- ABI surface: the cross-reference is build-time only.  No
  new C symbols escape to the public ABI.

## Tier 2: alplabai/alp-sdk-community repo bootstrap

### Repo skeleton

```
alp-sdk-community/
├── README.md                        # how-to-contribute + tier-2 quality bar
├── LICENSE                          # Apache-2.0 (umbrella)
├── registry.yaml                    # INDEX of all contributions
├── west.yml                         # consumable as a project from alp-sdk's workspace
├── chips/
│   └── <name>/
│       ├── README.md                # required: usage + author + datasheet
│       ├── CMakeLists.txt           # required: alp-sdk-style
│       ├── include/<name>.h         # required: public API
│       ├── src/<name>.c             # implementation
│       ├── metadata.yaml            # required: families, deps, ABI version
│       └── samples/<sample>/        # ≥ 1 sample app per contribution
├── libraries/<name>/                # same shape as chips/
├── templates/
│   ├── chip-skeleton/               # `cp -r templates/chip-skeleton chips/foo`
│   └── library-skeleton/
└── .github/
    ├── workflows/
    │   ├── pr-build-contribution.yml  # builds each touched contribution
    │   ├── pr-metadata-validate.yml   # registry.yaml + metadata.yaml schemas
    │   └── pr-lint.yml                # clang-format + license header check
    └── CODEOWNERS                     # alplabai/* owns registry + infra
```

### registry.yaml schema

One entry per contribution.  Customers consume this file as the
single source of truth for "what's available".

```yaml
schema_version: 1
contributions:
  - kind: chip          # or "library"
    name: bme680        # filesystem dir under chips/ or libraries/
    family: sensor      # sensor / display / motor / etc
    interfaces: [i2c]   # buses used
    description: "Bosch BME680 air-quality + temp + pressure + humidity"
    author: "@yourgh"
    license: Apache-2.0
    abi_version: "0.1"
    upstream_status: "first-class community"  # or "ported-from-upstream", etc.
```

### Per-contribution `metadata.yaml`

```yaml
schema_version: 1
name: bme680
kind: chip
family: sensor
interfaces: [i2c]
dependencies:
  - alp-sdk: ">=0.5.0"
license: Apache-2.0
author: "@yourgh"
abi_version: "0.1"
```

### Quality gate (CI)

`pr-build-contribution.yml` runs per touched contribution:

1. `metadata.yaml` validates against
   `metadata/schemas/contribution-v1.schema.json` (alp-sdk-side).
2. `registry.yaml` entry exists for the touched contribution
   and matches `metadata.yaml`'s `name`/`kind`/`family`.
3. `west build -b native_sim/native/64 samples/<first-sample>`
   compiles clean (warnings tolerated; errors fail).
4. `clang-format-diff` runs on changed lines (style warnings,
   not errors -- lower bar than alp-sdk Tier 1).
5. Apache-2.0 / MIT / BSD license-header check on every `.c`/
   `.h` (rejects GPL).

### Customer integration patterns

**Pattern A: pull-everything** (broadest reach):

```bash
# In customer's workspace, add to west.yml:
manifest:
  projects:
    - name: alp-sdk-community
      url: https://github.com/alplabai/alp-sdk-community
      revision: main
      groups: [community]

west update --group-filter +community
```

**Pattern B: per-contribution selection** (cleaner; what most
customers will want):

```bash
# Add the specific contributions to your west.yml's import:
manifest:
  ...
  imports:
    - file: west.yml
      name-allowlist:
        - chip-bme680
        - lib-modbus
```

**Pattern C: search-then-clone** (offline workflows):

```bash
gh repo view alplabai/alp-sdk-community --json file:registry.yaml
# Browse, clone the whole repo, copy contributions in manually.
```

### "Verified" promotion path

A Tier 2 contribution becomes a candidate for Tier 1
promotion when:

- Used in at least one alp-sdk customer's shipping product
  (verifiable via the verification ledger in
  `docs/test-plan.md`).
- HiL evidence captured under the contribution's `samples/`.
- 6+ months of stable use without ABI-breaking changes.
- Author agrees to transfer maintenance to alplabai/*.

Promotion lands as an alp-sdk PR that:

- Copies the contribution from `alp-sdk-community/chips/<name>/`
  to `alp-sdk/chips/<name>/`.
- Adds the chip to `metadata/chip-registry.yaml`.
- Adds a CHANGELOG entry.
- The original alp-sdk-community entry stays but its
  registry.yaml row gains `promoted_to: alp-sdk` to redirect
  customers.

## Implementation strategy

### Phase 1: Tier 1 chip + library scaffolding (existing maintainer; ~4 weeks)

- Land 49 new chip drivers in alp-sdk under `chips/<name>/`.
  Each commit covers 1 chip with: header, src/, README, sample,
  CHANGELOG entry, ABI snapshot bump.
- Land 17 new library knobs in alp-sdk under
  `scripts/alp_project.py`'s `_LIBRARY_KCONFIG` table +
  `metadata/schemas/board-config-v1.schema.json` enum
  expansion.  Each commit covers 1 library.

Commit cadence: one chip / library per commit per the §C.<N>
convention, batched in domain groups
(`§D.AI`, `§D.industrial`, `§D.iot`, `§D.audio` for chips;
`§D.libN` for libraries).

### Phase 2: Tier 2 repo bootstrap (~2 weeks)

- Create `alplabai/alp-sdk-community` repo.
- Land the skeleton (README, registry.yaml, west.yml, templates/,
  .github/workflows/) per the §"Repo skeleton" section above.
- Land `metadata/schemas/contribution-v1.schema.json` in
  alp-sdk (the schema file Tier 2 contributions validate
  against).
- Land `docs/contributing-tier-2.md` in alp-sdk with the
  per-contribution checklist + customer integration patterns.

### Phase 3: Seed Tier 2 (~2 weeks)

- Port 10 popular embedded-community chips/libraries that
  don't quite belong in Tier 1 but customers will ask for:
  `bme680`, `mpu6050`, `pca9685` (PWM driver), `sx126x` (LoRa
  alt), `nrf24l01`, `dhtxx` (temp/hum), `pcf8574` (I/O
  expander), `ssd1351` (color OLED), `rfm95w`, `ds18b20`.
- These prove the contribution template + CI flow before
  inviting external contributors.

### Phase 4: Public announcement + contribution call (after v1.0 tag)

- Blog post / docs page announcing the two-tier model.
- "Adding your first chip to alp-sdk-community" tutorial.
- Issue labels in alp-sdk-community for `good-first-chip`,
  `wanted`, etc.

## Validation gates

Tier 1 contributions must pass on PR:

- `pr-static-analysis` clang-format + cppcheck.
- `pr-doxygen` zero warnings on public header.
- `pr-plain-cmake` builds clean across baremetal AEN / V2N
  / Yocto / stub backends.
- `pr-twister` builds clean on native_sim.
- `pr-metadata-validate` schema check.
- `pr-generated-files` soc_caps.h + v0.5 snapshot in sync.
- ABI snapshot diff approved (or flagged as new symbol).
- CODEOWNERS approval.

Tier 2 contributions must pass on PR (lower bar):

- `pr-build-contribution` builds clean on native_sim.
- `pr-metadata-validate` per the contribution schema.
- `pr-lint` clang-format-diff + license-header.
- Reviewer = contribution's listed `author` per
  contribution-level CODEOWNERS.

## Risks + mitigations

| Risk | Mitigation |
|---|---|
| Maintainer review bandwidth for 49 new Tier 1 chips | Batch by domain (4 commits = AI, industrial, IoT, audio chips), one PR per batch.  Allow per-chip merges if a single chip fails review. |
| Tier 2 quality drift over time | Quarterly maintenance sweep on alp-sdk-community; archive contributions where build-clean CI has been red for >90 days. |
| Customers confused by two repos | Single docs page + the `registry.yaml` index file as the canonical "what's available" reference. |
| License contamination from community PRs | `pr-lint` rejects GPL on commit time.  CODEOWNERS auto-tags @alpCaner for any LICENSE-file change in `alp-sdk-community`. |
| Branching-policy override fatigue on alp-sdk | Open a single PR with all 49 chip drivers + 17 libraries (one chip per commit), use the existing override-push-rearm pattern for the merge. |

## Out of scope (defer past v1.0)

- A first-party `west alp-search <pattern>` CLI tool.
  Customers use `gh repo view` + grep `registry.yaml` for v1.0.
- Cross-promotion: marking which alp-sdk-community
  contributions are "verified to work on each E1M SoM".  The
  contribution's own `samples/` carries that today.
- A web UI for browsing contributions.  registry.yaml +
  GitHub's file viewer is enough.
- Automated migration of community-library metadata files
  (`.properties` / `library.json` / `package.yml` style) into
  Tier 2 contributions.

## Open questions (none blocking)

- Should the existing chips/<name>/ pattern get a `metadata.yaml`
  added in alp-sdk too, mirroring Tier 2?  Probably yes for
  consistency; one mechanical pass.
- Does `alp_project.py` need a per-contribution KConfig
  emission for Tier 2 chips?  Yes -- add a hook that scans
  Tier 2 chips in scope and emits `CONFIG_ALP_SDK_CHIP_<NAME>=y`
  per the same convention.
