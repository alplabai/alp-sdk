# Changelog

All notable changes to the ALP SDK are documented here.  Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
See [`VERSIONS.md`](VERSIONS.md) for the forward roadmap.

## [Unreleased] — v0.1.0 candidate

### Added

- Public headers under `include/alp/` for the v0.1 surface:
  `peripheral.h`, `display.h`, `camera.h`, `gui.h`, `math.h`,
  `signal.h`, `iot.h`.  C99-compatible, Doxygen-commented.
- Zephyr backend implementations for `alp_i2c_*`, `alp_spi_*`,
  `alp_gpio_*`, `alp_uart_*` under `src/zephyr/`, with a custom
  `alp,pin-array` devicetree binding for studio-resolved pin lookup.
- Static handle pools with Kconfig-tunable size limits
  (`CONFIG_ALP_SDK_MAX_*_HANDLES`).
- Top-level CMake support for both Zephyr-module consumption and
  plain `add_subdirectory()` consumption.
- Chip metadata schema `metadata/schemas/soc-spec-v1.schema.json`
  and per-SoC metadata files for the full Alif Ensemble line:
  `e3.json` and `e7.json` (released, real datasheet),
  `e8.json` (preliminary, datasheet v0.51), and
  `e4.json` / `e5.json` / `e6.json` (preliminary stubs derived from
  the ALP Lab E1M-AEN module datasheet draft + family pattern).
- Documentation: `README.md`, `docs/architecture.md`,
  `docs/os-support-matrix.md`, `docs/porting-new-som.md`,
  `vendors/alif/README.md`, `vendors/renesas-rzv2n/README.md`.
- ztest suite at `tests/zephyr/peripheral/` runs **12 of 12 cases
  green under twister on `native_sim/native/64`** with the alp-sdk
  module loaded via `EXTRA_ZEPHYR_MODULES`.  Coverage: open/close
  lifecycle, NULL-arg validation, status-code propagation, GPIO
  configure/write/read forwarding, pool exhaustion at the
  Kconfig-defined limits, and SPI / UART / I²C round-trips
  against Zephyr's emulated controllers.
- Devicetree binding `dts/bindings/alp,pin-array.yaml` is now
  picked up via the module's `dts_root: .`, with `alp` registered
  as a vendor prefix in `dts/bindings/vendor-prefixes.txt`.
- `docs/e1m-pinout.md` documents the pinout chain (e1m-spec →
  per-SoM manifest → studio pin allocator → SDK opaque integer).
  Confirms the SDK does not duplicate pad data; pinned to
  `alpCaner/e1m-spec` v1.0.
- **Global pin/instance map** at `include/alp/e1m_pinout.h` — the
  E1M-standard peripheral instance IDs (`ALP_E1M_I2C0`,
  `ALP_E1M_SPI0`, `ALP_E1M_UART0`, …) and pad-level GPIO indices
  (`ALP_E1M_GPIO_IO0`–`IO25`, `PWM0`–`PWM7`, `ENC0_X`–`ENC3_Y`)
  baked as C macros.  Carrier `alp,pin-array` overlays MUST follow
  the canonical ordering so the macros stay portable across every
  E1M-conformant carrier.  42 GPIO indices total.
- Carrier-specific feature names at
  `include/alp/boards/alp_e1m_evk_aen.h` — readable EVK-side
  aliases (`EVK_AEN_PIN_LED_RED`, `EVK_AEN_PIN_ENCODER_SW`,
  `EVK_AEN_I2C_BUS_SENSORS`, on-board sensor I2C addresses)
  layered on top of the global E1M map.
- **Chips library v0.1**:
  - `chips/lsm6dso/` + `<alp/chips/lsm6dso.h>` — STMicro
    LSM6DSO 6-axis IMU driver.  WHO_AM_I check, ODR/full-scale
    config, raw accel/gyro/temp reads.  Symbols use the chip's
    natural prefix (`lsm6dso_*`) per the no-`alp_`-on-chip-drivers
    rule.
  - `chips/ssd1306/` + `<alp/chips/ssd1306.h>` — Solomon
    Systech SSD1306 monochrome OLED driver.  I²C, 128×64/128×32
    geometries, init sequence, `clear` / `draw_pixel` /
    `set_contrast` / `set_inverted` / `display`.  Vertical-byte
    framebuffer matching the SSD1306 GDDRAM layout.
  - `chips/button_led/` + `<alp/chips/button_led.h>` — generic
    button + LED helper.  Carries the `alp_` prefix as the
    documented exception (block utility, not single-IC driver).
    Wraps `alp_gpio_*` for portable lifecycle.
  - Each chip is opt-in via `CONFIG_ALP_SDK_CHIP_*` Kconfig.
- New ztest suite at `tests/zephyr/chips/` covering all three
  chip drivers' lifecycle, NULL handling, and status-code
  propagation.  **Now 20 of 20 ztest cases pass on
  `native_sim/native/64`** (12 peripheral + 8 chips).
- **Renesas RZ/V2N metadata** at
  `metadata/socs/renesas/rzv2n/n44.json` — extracted from
  R01DS0466EJ0120 v1.20 (Sep 2025).  Captures the quad
  Cortex-A55 + Cortex-M33 + DRP-AI3 (4 dense / 15 sparse TOPS)
  topology, 1.5 MB on-chip SRAM (ECC), full peripheral inventory
  (2× GbE, USB 3.2 Gen2, PCIe Gen3, 6× CAN-FD, 9× I²C, 24× ADC,
  …), 840-pin FCBGA, and all 8 orderable RZ/V2N + RZ/V2NP SKUs
  (N41–N48) including the ALP Lab default `R9A09G056N44GBG#AC0`.
- **DEEPX DX-M1 companion accelerator metadata** at
  `metadata/socs/deepx/dx/m1.json` — extracted from DEEPX
  Commercial Datasheet v1.0 (June 2025).  25 TOPS @ 1.0 GHz INT8
  CNN accelerator behind PCIe Gen3 ×4, with up to 8 GB
  LPDDR4X/LPDDR5 + internal Cortex-M55 firmware controller.
  FC-BGA 625-ball.  Linked to the E1M-V2M101 / E1M-V2M102 SoM
  SKUs.
- OneDrive metadata mirror refreshed with the new files.
- **EdgeAI vision-AEN reference application skeleton** at
  `examples/edgeai-vision-aen/` — compiles under
  `native_sim/native/64` and prints the v0.1 init flow + v0.2
  pipeline TODOs.  Ships the full Zephyr-app layout (`README.md`,
  `CMakeLists.txt`, `prj.conf`, `boards/{native_sim_native_64,alp_e1m_evk_aen}.overlay`,
  `src/main.c`, `models/README.md`, `docs/pipeline.md`,
  `testcase.yaml`) so the v0.2 implementation slots into a stable
  scaffold.  The console twister harness asserts the skeleton's
  `[edgeai] done` line — passing now.
- `alp_pixfmt_t` typedef hoisted from `<alp/display.h>` to
  `<alp/peripheral.h>` so `<alp/camera.h>` can reference it
  without forcing a forward dependency on display.
- v0.1 stub for the camera surface at `src/zephyr/camera_stub.c`:
  every `alp_camera_*` entry returns `ALP_ERR_NOSUPPORT`,
  `alp_camera_open` returns `NULL` — matches the v0.1 contract
  in `<alp/camera.h>` and lets applications link cleanly until
  the v0.2 Zephyr-video integration arrives.
- **Twister now covers tests + examples**: 21 of 21 cases pass on
  `native_sim/native/64` across the peripheral suite, the chips
  suite, and the EdgeAI skeleton sample.
- **IoT connected-camera reference application skeleton** at
  `examples/iot-connected-camera/` — the v0.3 deliverable's
  v0.1 scaffold.  Targets the V2N family (Renesas RZ/V2N) for
  the connected-product path.  Six-stage skeleton:
  peripherals → camera → classifier → Wi-Fi station → MQTT/TLS
  → main loop, with TODO blocks marking which version each
  stage's real implementation lands in (v0.2 camera/classifier,
  v0.3 IoT).  Compiles + runs cleanly under
  `native_sim/native/64` (twister console-harness asserts the
  `[iotcam] done` line); EVK-V2N target parked behind a comment
  until `alpCaner/alp-zephyr-modules` publishes the V2N board file.
  Ships the full scaffold (`README.md`, `CMakeLists.txt`,
  `prj.conf`, `boards/{native_sim_native_64,alp_e1m_evk_v2n}.overlay`,
  `src/main.c`, `certs/README.md`, `docs/pipeline.md`,
  `testcase.yaml`) so the v0.3 implementation slots in cleanly.
- v0.1 stub for the IoT surface at `src/zephyr/iot_stub.c`:
  every `alp_wifi_*` / `alp_mqtt_*` entry returns
  `ALP_ERR_NOSUPPORT`, `*_open` returns `NULL` — matches the
  v0.1 "header-only" contract for `<alp/iot.h>` and lets
  applications link cleanly until v0.2 (Wi-Fi-station + MQTT
  on AEN-Zephyr) and v0.3 (V2N + TLS + BLE).
- **Twister now covers 4 scenarios on `native_sim/native/64`**:
  peripheral suite (12 cases), chips suite (8 cases), EdgeAI
  skeleton, IoT skeleton.  **22 of 22 cases pass.**
- **GitHub Actions CI shipped**:
  - `.github/workflows/pr-twister.yml` — runs the full twister
    matrix on every PR using the `zephyrprojectrtos/ci` Docker
    image (Zephyr v3.7.0 pinned per the v0.1 dependency matrix).
  - `.github/workflows/pr-metadata-validate.yml` — validates
    every `metadata/socs/**/*.json` against the v1 schema via
    `scripts/validate_metadata.py`.  Caught a real schema bug:
    `gops` was required on NPUs but V2N (DRP-AI) and DX-M1 quote
    `tops`.  Schema relaxed to `anyOf: [gops, tops]`; all 8
    SoC files now validate green.
  - `.github/workflows/pr-doxygen.yml` — best-effort Doxygen
    HTML build on every PR; v0.1 reports warnings as
    informational, v1.0 will gate on zero warnings.
  - `.github/workflows/pr-static-analysis.yml` — clang-format
    diff-only check (changed lines must satisfy
    `.clang-format`) plus `cppcheck` on `src/` + `chips/`.
  - `.github/workflows/nightly-aen-hil.yml` — skeleton for
    nightly HW-in-loop on a real E1M-AEN dev kit.  Gated on a
    `hil-aen` self-hosted runner (none yet).  Runner setup
    contract documented in [`ci/HW-IN-LOOP.md`](ci/HW-IN-LOOP.md).
  - `.clang-format` at the repo root (LLVM-base, 100-col,
    4-space indent).
  - `ci/README.md` is now a CI policy / index doc rather than
    claiming to hold the authoritative workflow copies.
- **PR template** (`.github/PULL_REQUEST_TEMPLATE.md`) gates on:
  roadmap-row attribution, ABI impact classification, twister +
  metadata-validate test plan, CHANGELOG/VERSIONS update.
- Issue templates updated to capture SoC `ref` from
  `metadata/socs/`, SoM SKU, carrier board, OS backend, Zephyr
  version, and Twister/HIL artefact paths.
- **Deeper chip ztests** (no emul fixture required):
  `ssd1306_draw_pixel` covers the page/column math (set bit,
  clear bit, OOB silent-ignore on 4 corners), `ssd1306_clear`
  verifies it wipes only the framebuffer (`width`/`height`/`addr`
  preserved), and `test_public_headers_co_compile` includes
  every `<alp/...>` header in one TU to catch
  typedef/macro-collision regressions.  Test count climbed
  **22 → 27** with no new failures.
- E1M EVK (UG-E1M-001) support for E1M-AEN family: SDK cheat
  sheet at `docs/boards/e1m-evk.md`, board overlay at
  `tests/zephyr/peripheral/boards/alp_e1m_evk_aen.overlay`, and
  an `alp_sdk.peripheral.evk_aen` build-only scenario tagged
  `alp-evk` for nightly HW-in-loop CI.
- `VERSIONS.md` tracking the v0.1 → v1.0 roadmap.
- **v0.2 chip-driver scaffolding** — public surface, opt-in build
  wiring, and lifecycle / NULL-arg ztests for five new drivers:
  - `chips/bme280/` + `<alp/chips/bme280.h>` — Bosch BME280
    combined T/H/P sensor.  I²C-only in v0.2.  Loads the
    per-die calibration coefficients on init and exposes
    `bme280_compensate` with the canonical Bosch integer-form
    arithmetic transcribed from BST-BME280-DS002 v1.6 §4.2.3.
  - `chips/lis2dw12/` + `<alp/chips/lis2dw12.h>` — STMicro
    LIS2DW12 3-axis ultra-low-power accelerometer.  WHO_AM_I
    check, ODR + full-scale + power-mode config in one call,
    raw 14-bit accel + on-die temp reads.
  - `chips/ssd1331/` + `<alp/chips/ssd1331.h>` — Solomon Systech
    SSD1331 96×64 colour OLED.  SPI 4-wire (D/C# pin),
    caller-supplied 12 KiB framebuffer (RGB565), pixel/clear/
    display API, full datasheet init sequence on `ssd1331_init`.
  - `chips/ov5640/` + `<alp/chips/ov5640.h>` — OmniVision OV5640
    5 MP image sensor SCCB-side configuration driver.  Chip-ID
    verify, soft-reset, resolution / format / test-pattern
    presets.  Capture-side (MIPI CSI-2) lives in v0.2's
    `<alp/camera.h>`; per-resolution register tables ship in
    v0.3 alongside the V2N alp_camera integration.
  - `chips/pdm_mic/` + `<alp/chips/pdm_mic.h>` — generic PDM
    microphone block helper.  `alp_`-prefixed (block utility,
    documented exception per the chip-driver naming rule).
    Surface declared; impl returns `ALP_ERR_NOSUPPORT` until
    v0.2's `<alp/audio.h>` lands the underlying I²S
    abstraction.
- **v0.2 / v0.3 public-header surface declared** so application
  code can compile against it now and the implementations slot
  in without ABI churn (same pattern `<alp/iot.h>` followed in
  v0.1):
  - `<alp/audio.h>` (v0.2) — PDM input + I²S output API,
    `alp_audio_in_*` / `alp_audio_out_*`, three sample formats
    (S16/S24/S32), per-block read/write with timeout.
  - `<alp/ble.h>` (v0.3) — peripheral + central, GATT server +
    client.  Zephyr `bt` host stack per the locked decision.
    Mesh / audio / DF explicitly out of scope for v1.0.
  - `<alp/security.h>` (v0.3) — MbedTLS re-export shape: hash
    (SHA-256/384/512), AEAD (AES-128/256-GCM, ChaCha20-Poly1305),
    TRNG.  Per-SoC hardware-accelerator routing happens at the
    backend layer.
  - `<alp/mproc.h>` (v0.3) — multi-processor IPC primitives:
    shared memory regions (`alp_shmem_*`), mailbox channels
    (`alp_mbox_*`, MHU on Alif), hardware semaphores
    (`alp_hwsem_*`).  Wraps the M55-HE core bring-up the
    "Multi-Processor Support Completion" milestone delivers.
  - Stub implementations at `src/zephyr/{audio,ble,security,
    mproc}_stub.c` returning `ALP_ERR_NOSUPPORT` and `*_open()
    → NULL`, matching the v0.1 stub contract.
- **Chip ztest suite extended** to the eight chip drivers.  Each
  new chip gets lifecycle + NULL-arg cases plus a "post-init
  calls reject uninitialised" pattern for the I²C drivers (the
  emul controller has no real device behind it, so `*_init` fails
  WHO_AM_I and downstream calls must report `NOT_READY`).  Also
  includes a pure-math test for `ssd1331_rgb565` and a runtime
  check that the v0.2 / v0.3 stubbed surfaces honour the
  documented `NULL` / `NOSUPPORT` contract.  Total tests across
  twister `native_sim/native/64` climb **27 → 43 cases** (chips
  suite alone: 13 → 29).
- `test_public_headers_co_compile` extended to include
  `<alp/audio.h>`, `<alp/ble.h>`, `<alp/security.h>`,
  `<alp/mproc.h>`, and the five new chip headers.
- **ABI snapshot tooling**: `scripts/abi_snapshot.py` walks
  `include/alp/**.h`, extracts function decls, typedefs, and
  `#define`s, and emits a stable JSON fingerprint per public
  symbol.  Supports `--diff <prior.json>` for per-symbol change
  reporting.  Pre-1.0 the diff is informational; v1.0 promotes
  it to a CI gate (`pr-abi-snapshot.yml`) where `REMOVED` /
  `CHANGED` entries require a major-version bump.
- `docs/abi/v0.1-snapshot.json` ships the v0.1 ABI fingerprint —
  21 headers, 123 functions, 80 typedefs, 114 macros.  See
  `docs/abi/README.md` for the workflow.
- `docs/architecture.md` gains a "Consumers of this SDK"
  section: alp-studio integration contract, Zephyr-application
  consumption recipe, and the v0.4 Yocto / `meta-alp` shape.
- `examples/README.md` per-example status descriptions tightened
  with the concrete pipeline stages each skeleton scaffolds and
  which version fills in each stage.
- **Register-protocol ztests** via test-only i2c-emul fixtures.
  Three fake targets (`tests/zephyr/chips/src/fake_{lsm6dso,
  ssd1306,bme280}.c`) attach to the test's `i2c0_emul`
  controller at the chip drivers' default I2C addresses,
  pre-populate `WHO_AM_I` / `CHIP_ID` plus calibration data,
  and either echo register writes back or capture the byte
  stream for the test to inspect via `fakes.h` helpers.
  - **fake LSM6DSO**: register-store with WHO_AM_I=0x6C seed.
    Tests verify `lsm6dso_init` succeeds, `set_accel`/`set_gyro`
    encode the documented byte into CTRL1_XL / CTRL2_G, and
    `read_accel` decodes the seeded LE register pairs.
  - **fake SSD1306**: command/data byte logger split by the
    SSD1306 control byte (0x00 vs 0x40).  Tests verify
    `ssd1306_init` streams DISPLAY_OFF first, charge-pump
    enable (0x8D 0x14), DISPLAY_ON last; `ssd1306_display`
    sets the full address window then pushes 1024 framebuffer
    bytes with the seeded pixel landing at offset 0.
  - **fake BME280**: register store seeded with the canonical
    BST-BME280-DS002 §4.2.2 example calibration coefficients
    and §4.2.3 example raw conversion (T_raw=519888,
    P_raw=415148).  Tests verify init loads every coefficient
    correctly, `read_raw` decodes the 20/20/16-bit values, and
    `compensate` reproduces the documented worked example
    (T = 25.08 °C, P = 100653 Pa) within ±2 LSB and ±50 Pa.
  - DT bindings live at `tests/zephyr/chips/dts/bindings/
    alp,fake-*.yaml`; the chip-test overlay attaches each
    fake at the canonical I2C address.
  - Chips suite tests grow **29 → 38 cases**; total twister
    cases on `native_sim/native/64` climb **43 → 52**.

### Notes

- Repo migrated from a CMSIS-Toolbox csolution / VFT mock-driver
  layout to the OS-pivoted layout described in `docs/architecture.md`.
  The previous tree is preserved in git history.
- Apache-2.0 license replaces the original MIT badge.
- Twister build hit a Kconfig-discovery issue (the alp-sdk module
  surface isn't being merged) — fix tracked, will be green before tag.
