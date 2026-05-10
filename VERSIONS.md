# ALP SDK Versions

Living roadmap for the ALP SDK.  Each version below lists what ships,
what's stubbed, and the acceptance bar.  When a version is tagged the
"Status" cell flips to `released`; CHANGELOG.md captures the bump.

This roadmap consolidates the original quarterly plan (which carried
each phase as its own minor — *No-OS Peripherals*, *Multi-Processor
BSP Framework*, *Test Examples*, *Zephyr Integration*, *EdgeAI App*,
*IoT App*, *Multi-Processor Completion*, *Unified Repo and Docs*,
repeated per SoM) into compressed v0.x milestones.  Mapping from
the old plan to this one:

| Original phase                       | Now lives in            |
|--------------------------------------|-------------------------|
| AEN: No-OS Peripherals               | v0.2 (bare-metal real)  |
| AEN: Multi-Processor BSP Framework   | v0.1 (foundation), v0.3 (completion) |
| AEN: Test Examples                   | v0.1 (ztest), v0.3+ (examples/) |
| AEN: Zephyr Integration              | **v0.1** ✓              |
| AEN: EdgeAI Application Example      | v0.2                    |
| AEN: IoT Application Example         | v0.3                    |
| AEN: Multi-Processor Support Completion | v0.3                 |
| AEN: Unified Software Repo and Docs  | v1.0                    |
| V2N: same eight phases               | folded into v0.2 (intro) → v0.4 (Yocto first-class) |
| V2N+M1: same eight phases            | folded into v0.3 (DX-M1 intro) → v0.4 (full V2N+M1 Yocto) |

| Version | Status      | Target           |
|---------|-------------|------------------|
| v0.1.0  | in-progress | AEN bring-up (Zephyr peripherals + multi-proc BSP foundation) |
| v0.2.0  | planned     | bare-metal AEN real, V2N intro, EdgeAI reference app |
| v0.3.0  | planned     | IoT reference app, multi-proc completion, display polish, V2N+M1 intro |
| v0.4.0  | planned     | Yocto first-class (V2N + V2N+M1 full)  |
| v1.0.0  | planned     | unified repo, docs, ABI freeze, production-ready  |

---

## v0.1.0 — "AEN bring-up"

**Goal:** support alp-studio's v0 demo end-to-end on Zephyr-on-AEN with
the three starter blocks (button+LED, OLED, IMU).  Stub everything
else with API-stable headers so blocks can compile against future
versions cleanly.

### Libraries shipped

| Library     | Status    | Surface                                                                  |
|-------------|-----------|--------------------------------------------------------------------------|
| Peripherals | full      | `alp_i2c_*`, `alp_spi_*`, `alp_gpio_*`, `alp_uart_*` (Zephyr backend, AEN) |
| Chips       | full      | `lsm6dso_*`, `ssd1306_*`, `button_led_*` (no `alp_` prefix on chip drivers) |
| Display     | minimal   | `alp_display_init/clear/print` routes through Zephyr `display_*`         |
| Math        | re-export | `<alp/math.h>` re-exports CMSIS-DSP                                      |
| Camera      | header    | `<alp/camera.h>` API frozen; impl returns `ALP_ERR_NOSUPPORT`            |
| Signal      | header    | `<alp/signal.h>` re-exports CMSIS-DSP filters                            |
| GUI/LVGL    | header    | `<alp/gui.h>` includes upstream LVGL with ALP defaults; no widgets       |
| IoT         | header    | `<alp/iot.h>` API frozen; impl stubbed                                   |
| Audio       | —         | not in v0.1                                                              |
| BLE         | —         | not in v0.1                                                              |

### E1M EVK support

v0.1 ships first-class support for the **E1M Development Board**
(UG-E1M-001) populated with an E1M-AEN family SoM:

- `docs/boards/e1m-evk.md` — SDK-side cheat sheet for the EVK:
  power tree, I²C bus map, sensor addresses, IO-expander
  assignment, JTAG/SWD header, BOOT/reset/module-enable.
- `tests/zephyr/peripheral/boards/alp_e1m_evk_aen.overlay` — DT
  overlay that wires the SDK's `alp_i2c0` / `alp_spi0` / `alp_uart0`
  aliases and the `alp,pin-array` (rotary encoder + RGB LED + IO
  expander) to the EVK connectors.
- `testcase.yaml` defines an `alp_sdk.peripheral.evk_aen` build-only
  scenario tagged `alp-evk` so the nightly HW-in-loop CI on a real
  EVK selects it explicitly.

The Zephyr **board file** for `alp_e1m_evk_aen` itself does not live
in this repo — it lands in
[`alplabai/alp-zephyr-modules`](https://github.com/alplabai/alp-zephyr-modules)
per the architectural split.  The overlay above re-aliases what
that board file exposes.

EVK support for V2N-family SoMs (`alp_e1m_evk_v2n`) lands in v0.2;
V2N+M1 in v0.3.

### Multi-Processor BSP foundation (AEN)

E1M-AEN ships with at minimum two heterogeneous Cortex-M55 cores
(HP @ 400 MHz + HE @ 160 MHz), and on E5/E6/E7/E8 also one or two
Cortex-A32 cores at 800 MHz.  v0.1 lays the foundation:

- Zephyr boots on the M55-HP core ("application core" in v0.1).
- M55-HE remains in reset (released by v0.3 multi-proc completion).
- A32 cores remain in reset on E5+ variants (Linux/Yocto handoff in
  v0.4).
- Inter-core IPC primitives (`MHU`, `HWSEM`) declared in headers
  but not exposed as public API yet — the wrapping lands in v0.3
  when the M55-HE side becomes a first-class peer.

### Chip metadata shipped

Real datasheet extraction:
- `alif:ensemble:e3` — `Alif_E3_Datasheet_v2.8.pdf`
- `alif:ensemble:e7` — `Alif_E7_Datasheet_v2.9.pdf`
- `alif:ensemble:e8` — `Alif E8 Datasheet 0.51.pdf` (preliminary)

Preliminary stubs (`pending_alif_datasheet: true`):
- `alif:ensemble:e4`, `e5`, `e6` — derived from ALP Lab E1M-AEN
  module datasheet draft + Ensemble family pattern.  Each stub
  carries the canonical Alif part number that backs the
  corresponding E1M-AEN module SKU.

### Build matrix

| Target              | Status                                |
|---------------------|---------------------------------------|
| AEN + Zephyr        | real (HW-in-loop CI on E3 dev kit)    |
| AEN + Bare-metal    | stubs (compile-only)                  |
| V2N + Yocto         | not in v0.1                           |

### CI

- ztest on `native_sim` (QEMU host) for every PR.
- HW-in-loop on a real E3 dev kit nightly.
- ABI snapshot diffed against previous release.

### Acceptance

- alp-studio's three v0 blocks compile + flash + run against the
  built SDK.
- All public headers Doxygen-documented.
- `alif:ensemble:{e3,e4,e5,e6,e7,e8}.json` validate against
  `metadata/schemas/soc-spec-v1.schema.json`.

### Deliverables

- Tagged `v0.1.0` on `main`.
- `CHANGELOG.md` entry.
- This file flips v0.1 to `released`, v0.2 to `in-progress`.

---

## v0.2.0 — "richer blocks + V2N intro" (~6 weeks after v0.1)

**Goal:** unblock the studio's next wave of blocks (camera, audio,
more sensors) and add a second SoM family.

### New libraries / surface

- **Peripherals:** `alp_spi_*` lands on bare-metal too.
- **Chips:** `ov5640` (MIPI CSI camera), `bme280` (env), `lis2dw12`
  (3-axis accel), `ssd1331` (small color OLED), plus `camera_parallel`
  (CPI fallback) and `pdm_mic` (PDM microphone) helper drivers.
- **Audio:** `alp_audio_*` PDM input + I²S output, ALP-default DSP chain.
- **Signal:** real FIR/IIR helpers using CMSIS-DSP, FFT wrappers.
- **Math:** BLAS-style helpers for tensor pre/post processing.
- **Camera:** real impl over Zephyr's `video_*` API (was stub in v0.1).
- **IoT:** `alp_iot_wifi_station` + `alp_iot_mqtt_publish` on Zephyr.
  BLE arrives in v0.3.

### Chip metadata added

- `renesas:rzv2n:n44` (`metadata/socs/renesas/rzv2n/n44.json`) —
  shipped early in v0.1 alongside the V2N silicon datasheet
  ingest; covers all 8 RZ/V2N + RZ/V2NP orderable SKUs in one
  file.  ALP Lab's `R9A09G056N44GBG#AC0` default is recorded.
- `deepx:dx:m1` (`metadata/socs/deepx/dx/m1.json`) — the DEEPX
  DX-M1 companion accelerator behind PCIe on V2N-M1 SoMs.
  25 TOPS @ 1.0 GHz, FC-BGA 625-ball.
- `alif:ensemble:e4/e5/e6` upgraded from preliminary to released
  (if Alif publishes datasheets in time).
- `nxp:imx9:imx93` (`metadata/socs/nxp/imx9/imx93.json`) — first
  v0.1 stub shipped early; per-variant peripheral counts and the
  orderable SKU list fill in for v0.4 (Yocto first-class)
  alongside the i.MX 93 BSP work.  Vendor wrapper directory
  scaffolded at `vendors/nxp-imx93/`.

### Build matrix

| Target              | Status                                |
|---------------------|---------------------------------------|
| AEN + Zephyr        | real                                  |
| AEN + Bare-metal    | real (was stub in v0.1)               |
| V2N + Yocto         | real (HW-in-loop CI on V2N dev kit)   |
| V2N + Zephyr        | stubs                                 |

### EdgeAI reference application

`examples/edgeai-vision-aen/` — end-to-end reference:
camera capture → ISP / format-convert → Ethos-U inference (a
Vela-compiled MobileNetV2 or similar) → results overlay on the
on-board OLED.  This is the original roadmap's "EdgeAI Application
Example" deliverable for AEN, materialised as a shipped example.

### Acceptance

- alp-studio's v0.5 demo (smart counter + camera classifier)
  compiles on AEN + V2N from the same project document, swapping
  SoMs only.
- The EdgeAI reference app runs on a real E1M-AEN701 dev kit with
  ≥10 fps inference.
- ABI changes vs v0.1 limited to additive (no header removals).

---

## v0.3.0 — "IoT + display polish" (~6 weeks after v0.2)

**Goal:** make the SDK suitable for connected products and richer UIs.

- **IoT:** BLE peripheral + central (`alp_ble_*`), Wi-Fi provisioning
  helpers, `alp_iot_http` (HTTP/HTTPS over MbedTLS), provisioning UI
  templates.
- **Display:** e-paper drivers (`ssd1683`, `uc8175`), small TFTs
  (`st7789`, `ili9341`), LVGL widget pack with ALP visual defaults.
- **Security:** `alp_security_*` re-export of MbedTLS + Alif/Renesas
  hardware crypto.
- **GUI:** ALP widget set on top of LVGL (cards, status bars, tile
  layouts).

### Multi-Processor Support Completion (AEN)

The original roadmap's "Multi-Processor Support Completion" lands
here.  v0.3 brings up the M55-HE core as a first-class peer of the
M55-HP application core, with:

- `<alp/mproc.h>` — public API for inter-core IPC: shared memory
  regions, mailbox channels (Zephyr `MHU` wrapping), hardware
  semaphores (Zephyr `HWSEM`).
- An OpenAMP-based RPC primitive on top so blocks can offload
  compute to the M55-HE core without hand-rolling IPC.
- A second-firmware build target (`prj-rtss-he.conf`) that produces
  the M55-HE image; `west build` orchestrates both core images.
- Pre-built reference firmware for M55-HE that hosts CMSIS-DSP
  filters reachable from M55-HP via RPC (the studio's `signal`
  library uses this).

A32 cores remain in reset on E5/E6/E7/E8 in v0.3 — Linux/Yocto
handles that handoff in v0.4.

### IoT reference application

`examples/iot-connected-camera/` — connected-camera reference: V2N
captures → on-device classifier → MQTT/TLS publish to a broker,
local LVGL UI showing the live classification.  This is the original
roadmap's "IoT Application Example" deliverable.

### Build matrix delta

- + V2N + Zephyr: real
- + AEN + Yocto: stubs

### Acceptance

- A "connected camera" reference app builds on V2N (Yocto), pushes
  inferences over MQTT/TLS, with a local LVGL UI.

---

## v0.4.0 — "Yocto first-class" (~6 weeks after v0.3)

**Goal:** Linux variants (V2N family + i.MX 93 family) become
first-class with full Yocto support and Linux-native versions of
every library.

- **Yocto:** `meta-alp` layer, BSPs for V2N + V2N+M1, recipes per ALP
  module, image templates for vision/audio/IoT product classes.
- **IoT:** full TCP/UDP/HTTP/MQTT over Linux network stack, MQTT
  broker optional, time-series buffering helpers.
- **Camera:** `alp_camera_v4l2` wrapper, GStreamer pipeline helpers.
- **Audio:** ALSA-backed `alp_audio_*`.
- **Signal:** ARM Compute Library bindings.
- **IoT:** OTA helpers (Mender / SWUpdate integration).

### Build matrix

All three OS targets × both SoM families: real.

### Acceptance

- "Vision-on-the-edge" reference app: video capture → on-device
  classifier → MQTT to cloud, runs the same logic on AEN-Zephyr
  and V2N-Yocto with only OS-target switching.

---

## v1.0.0 — "production-ready"

**Goal:** ABI freeze, full doc, real customer-grade.

- ABI snapshot frozen; major bumps required for breaking changes.
- Doxygen 100 % coverage on public headers.
- Tutorials per library.
- Performance baselines per chip in metadata.
- Long-term support (LTS) cadence: 1.0 supported for 24 months.
- Vendor partnerships: agreements with Alif + Renesas for upstream
  driver merges where applicable.
- Fuzz testing on every parser surface (manifest readers, MQTT, BLE).
- Coverity / static-analysis clean.
- Ten reference applications shipped under `examples/`.

---

## Build / dependency matrix per version

| Version | Zephyr | Alif HAL | Renesas RZ | LVGL | MbedTLS | CMSIS-DSP |
|---------|--------|----------|------------|------|---------|-----------|
| v0.1    | 3.7.0  | latest   | —          | 9.x  | —       | 1.16+     |
| v0.2    | 3.7.0  | latest   | latest     | 9.x  | —       | 1.16+     |
| v0.3    | 3.8.0  | latest   | latest     | 9.x  | 3.6.x   | 1.16+     |
| v0.4    | 3.8.0  | latest   | latest     | 9.x  | 3.6.x   | 1.16+     |
| v1.0    | LTS    | LTS      | LTS        | LTS  | LTS     | LTS       |

(Note: at scaffold time the workspace was set up against Zephyr `main`
to unblock test-bed bring-up; the v0.1 release will pin to **3.7.0**
per this matrix before tagging.)

---

## Quality bar (every version)

- Public headers C99-compatible with Doxygen comments.
- Every public function: at least one Unity / ztest test.
- CI matrix builds + tests every supported (OS × SoM) combination.
- ABI snapshot diffed vs previous release; breaking change requires
  major bump after v1.0.
- No GPL deps.  Apache-2.0 / MIT / BSD only.
- Static analysis (clang-tidy, cppcheck) clean before tag.
- Doxygen build passes with zero warnings.

## Release cadence

- v0.x.0: roughly every 6 weeks during pre-1.0 development.
- v0.x.y patches: as needed for shipping bug fixes.
- v1.0: when v0.4 has been stable for 8+ weeks with real users.
- v1.x.y patches after 1.0: monthly cadence.
- v1.x.0 minor: quarterly cadence with API additions only.

## Deprecation policy (post-1.0)

- Mark a function `__attribute__((deprecated("message")))` for at
  least 2 minor versions before removal.
- A removal requires a major version bump.
- CHANGELOG.md entry per deprecation.

## Working agreement

- Each version starts with a VERSIONS.md update that fills in the
  acceptance criteria specifically (no hand-waving).
- Open one PR per library per version (e.g. *feat(peripherals): v0.1
  Zephyr-AEN backend*).  PR description references the matrix row it
  implements.
- Tag releases `v0.1.0`, `v0.2.0`, …; never tag a `0.x.y` patch from
  `main` without first cherry-picking to a release branch.
- HW-in-loop CI must be green for the SoM the version targets before
  tagging.
- Studio integration: when a version ships, post a one-line update on
  the alp-studio side noting which "specs from SDK pending"
  placeholders now resolve.
