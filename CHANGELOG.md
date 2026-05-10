# Changelog

All notable changes to the ALP SDK are documented here.  Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
See [`VERSIONS.md`](VERSIONS.md) for the forward roadmap.

## [Unreleased] — v0.1.0 candidate

### Added

- DEEPX DX-M1 inference backend on the Yocto / Linux user-space path.
  New `src/yocto/inference_yocto.c` is the first real Yocto-side
  surface in the SDK -- it owns the `alp_inference_*` public symbols
  (overriding `src/common/stub_backend.c` via the
  `ALP_VENDOR_OVERRIDES_INFERENCE` guard) and routes
  `ALP_INFERENCE_BACKEND_DEEPX_DX` through `inference_deepx.cpp`.
  `vendors/deepx-dxm1/` ships a stub `<dxnn/dxnn.h>` so the dispatcher
  compiles on hosts that don't have the proprietary DEEPX runtime
  installed -- the real `dxnn_*` link arrives v0.4 alongside the
  `deepx-dxm1-host-sdk` Yocto recipe.  Gated via the CMake option
  `ALP_SDK_USE_DEEPX_DXM1`; meta-alp's `e1m-x-v2n-m1.conf` MACHINE
  drives that ON.  `resolve_auto()` for Yocto prefers DEEPX_DX first
  on V2N-M1, falling back to NOSUPPORT when no backend is compiled
  in.  PLAN.md §2.3 + docs/recommended-libraries.md updated.
- `yocto/meta-alp/` is now a v0.3-scaffolded BSP layer (vs the
  v0.2 placeholder).  Three machine configs land:
  `e1m-x-v2n.conf` (RZ/V2N), `e1m-x-v2n-m1.conf` (V2N + DEEPX),
  `e1m-n93.conf` (i.MX 93 + Ethos-U65).  Three new recipes land:
  `alp-sdk-runtime` (libalp_sdk.so + headers, per-machine
  inference-backend cmake defines), `alp-chips` (libalp_chips.a +
  per-chip PACKAGECONFIG knobs for all 19 drivers), and
  `alp-studio-codegen` (CLI helper; opt-in via DISTRO_FEATURES,
  guarded with bb.parse.SkipPackage until alplabai/alp-studio is
  public).  layer.conf advertises `LAYERRECOMMENDS_alp` for
  meta-renesas-rz + meta-imx.  Recipes parse but don't build --
  real `do_compile` against the Yocto cross-toolchain lands in
  v0.4 with `src/yocto/` going from stub to real.
- [ADR 0006](docs/adr/0006-secure-boot-secure-ota.md) lands the
  v0.4 secure boot + secure OTA design.  Per-SoM vendor-native
  secure boot (Alif Secure Enclave + MCUboot on AEN; NXP AHAB on
  N93; Renesas SBM on V2N), unified `alp_ota_*` surface in
  `<alp/iot.h>` routed through MCUboot (Zephyr) or RAUC (Linux),
  trust-anchor pinning + min-version anti-rollback baked into the
  config.  No code yet -- v0.4 cycle implements it.
- `firmware/cc3501e/` scaffolding -- the alp-sdk side of the
  two-repo boundary with the future `alplabai/cc3501e-firmware`:
  bootstrap README that mirrors the contract from
  `docs/cc3501e-bridge.md`, a `flash.py` stub (dry-run + SHA-256
  + signature presence checks today; real bootloader sequence
  lands with the first signed binary), `protocol-version.txt`
  pinned to wire-protocol v1, and an empty `prebuilt/` with its
  own CHANGELOG ready to receive `cc3501e-vX.Y.Z.bin` releases.

### Removed (pre-1.0)

- `<alp/math.h>` and `<alp/signal.h>` deleted.  They were thin
  re-exports of CMSIS-DSP that added zero value over a direct
  `#include "arm_math.h"`.  Application code now includes
  CMSIS-DSP directly; ALP SDK internals (e.g. inside
  `<alp/audio.h>`) may still pull CMSIS-DSP in via the
  build-time `ALP_HAS_CMSIS_DSP` option.  Documented stance
  in `docs/architecture.md` + `VERSIONS.md` +
  `docs/os-support-matrix.md`.

### Added

- Public headers under `include/alp/` for the v0.1 surface:
  `peripheral.h`, `display.h`, `camera.h`, `gui.h`, `iot.h`.
  C99-compatible, Doxygen-commented.
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
  `alplabai/e1m-spec` v1.0.
- **Global pin/instance map** at `include/alp/e1m_pinout.h` — the
  E1M-standard peripheral instance IDs (`ALP_E1M_I2C0`,
  `ALP_E1M_SPI0`, `ALP_E1M_UART0`, …) and pad-level GPIO indices
  (`ALP_E1M_GPIO_IO0`–`IO25`, `PWM0`–`PWM7`, `ENC0_X`–`ENC3_Y`)
  baked as C macros.  Carrier `alp,pin-array` overlays MUST follow
  the canonical ordering so the macros stay portable across every
  E1M-conformant carrier.  42 GPIO indices total.
- Carrier-feature names at
  `include/alp/boards/alp_e1m_evk.h` — readable EVK-side aliases
  (`EVK_PWM_LED_RED`, `EVK_PIN_ENCODER_SW`,
  `EVK_I2C_BUS_SENSORS`, on-board sensor I2C addresses) layered on
  top of the global E1M map.  The header is SoM-agnostic: the E1M
  EVK accepts 35x35 mm SoMs (currently AEN, soon N93).  Per-SoM
  dispatch differences (e.g. AEN-side CC3501E proxying for IO11 /
  IO13 / IO15..IO21) are called out per-pad in the doc-comments.
  The 45x65 E1M-X carrier gets its own future header
  `<alp/boards/alp_e1m_x_evk.h>`.
- M.2 E-key wake + radio-disable wiring on the EVK header:
  `EVK_PIN_M2E_UART_WAKE` (IO19), `EVK_PIN_M2E_SDIO_WAKE` (IO18),
  `EVK_PIN_W_DISABLE1` (IO17, Wi-Fi disable, open-drain),
  `EVK_PIN_W_DISABLE2` (IO16, BT disable, open-drain).  All four
  proxy through the on-module CC3501E.
- BMI323 INT1 macro: `EVK_PIN_BMI323_INT1` (IO15, CC3501E-side).
  The IMU's data-ready / motion / FIFO interrupt does not pass
  through the main TCAL9538 expander -- those bits hold the
  ICM-42670 + BMP581 interrupt lines.
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
  until `alplabai/alp-zephyr-modules` publishes the V2N board file.
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

- **v0.2 peripheral HAL expansion** — eight new peripheral classes
  wrapped, doubling the SDK's coverage of what an MCU actually
  exposes (was 4 classes, now 12).  Each new class follows the v0.1
  pattern: a public header with full Doxygen, a Zephyr backend that
  resolves studio-supplied IDs via the `alp-<class>N` DT alias and
  forwards to the matching Zephyr driver class, a Kconfig opt-in
  gated on the underlying subsystem, a static handle pool with a
  per-class quota, and runtime validation against the active SoC's
  documented caps:
  - `<alp/pwm.h>` + `src/zephyr/peripheral_pwm.c` — Zephyr `pwm_*`.
  - `<alp/adc.h>` + `src/zephyr/peripheral_adc.c` — Zephyr `adc_*`.
  - `<alp/counter.h>` + `src/zephyr/peripheral_counter.c` /
    `peripheral_qenc.c` — free-running counter via `counter_*`,
    quadrature-decoder via `sensor_*`.
  - `<alp/i2s.h>` + `src/zephyr/peripheral_i2s.c` — Zephyr `i2s_*`
    with a 2-block ping-pong memory slab.
  - `<alp/can.h>` + `src/zephyr/peripheral_can.c` — Zephyr `can_*`,
    classic + CAN-FD.
  - `<alp/rtc.h>` + `src/zephyr/peripheral_rtc.c` — Zephyr `rtc_*`.
  - `<alp/wdt.h>` + `src/zephyr/peripheral_wdt.c` — Zephyr `wdt_*`.
- **Last-error mechanism for `*_open` failure diagnosis.**  New
  status code `ALP_ERR_OUT_OF_RANGE` (= -8); new public accessor
  `alp_last_error()` (thread-local) lets callers learn *why* a
  failed `*_open` returned NULL — distinguishing
  config-out-of-range, NULL-arg, pool-exhausted, device-not-ready,
  and underlying driver error.  Internal helpers
  `alp_z_set_last_error` / `alp_z_clear_last_error` live in
  `src/zephyr/last_error.c`.
- **SoC capability validation** — `scripts/gen_soc_caps.py` reads
  `metadata/socs/**.json` and emits `include/alp/soc_caps.h` with
  per-SoC `ALP_SOC_*_COUNT` / `ALP_SOC_*_MAX_*` macros gated by
  `CONFIG_ALP_SOC_<TOKEN>`.  v0.2 wrappers consult the matching
  macros to reject configs that exceed the active SoC's documented
  caps before any I/O.  Canonical case: a 16-bit ADC request on
  Alif E3 (12-bit max) fails at `alp_adc_open` with
  `alp_last_error()` = `ALP_ERR_OUT_OF_RANGE`.
- **Kconfig SoC choice** at `zephyr/Kconfig` for the active
  capability profile.  Default `ALP_SOC_NONE` keeps validation
  permissive; alp-studio's generated build picks the matching
  `CONFIG_ALP_SOC_<VENDOR>_<FAMILY>_<PART>=y` automatically.
- **Architecture documentation** — `docs/architecture.md` gains a
  "Why this wrapper exists (despite Zephyr already abstracting
  vendors)" section and a "Capability validation" section.  New
  `docs/adr/` directory with the ADR template plus three accepted
  records:
  - [ADR 0001](docs/adr/0001-wrapper-on-top-of-zephyr.md) — why
    ALP SDK wraps Zephyr (and why the wrapper stays thin).
  - [ADR 0002](docs/adr/0002-error-mechanism.md) —
    `alp_last_error()` + compile-time SoC capability validation.
  - [ADR 0003](docs/adr/0003-peripheral-coverage.md) — wrap 12
    peripheral classes at v0.2, not just I2C/SPI/GPIO/UART.
- **ABI snapshot bumped** to reflect the new headers — 29 headers,
  ~165 functions, ~110 typedefs, ~135 macros (was 21/123/80/114).
- **E1M portability-bound macros** at `<alp/e1m_pinout.h>` —
  `ALP_E1M_<CLASS>_COUNT` constants for each peripheral class
  (I2C=2, SPI=2, UART=2, I2S=2, PDM=2, I3C=1, CAN=1, ETH=1,
  CSI=1, DSI=1, ADC=8, DAC=2, PWM=8, ENC=4, GPIO_IO=26).  These
  are the **portability contract** — apps that use only
  `ALP_E1M_<CLASS><N>` for `N < ALP_E1M_<CLASS>_COUNT` are
  guaranteed cross-SoM compatibility.  Higher indices fall into
  the SoC-specific gap (e.g. RZ/V2N's six CAN channels) which the
  SDK still accepts but apps lose the "swap the SoM" property.
- **[ADR 0004](docs/adr/0004-e1m-portability-bound.md)** — E1M-spec
  instance counts as the portability bound.  Documents the three
  concentric capability tiers: E1M reservation < studio block
  declaration < SoC count < driver array.
- `docs/architecture.md` gains an "E1M as the portability bound"
  section explaining the three-tier validation model.
- **GitHub org migration** — repo references updated from
  `alpCaner/*` to `alplabai/*` (alp-sdk, e1m-spec,
  alp-zephyr-modules, alp-studio).  27 files / 56 substitutions,
  pure rename.  Older history rewritten to drop spurious
  co-author footers per the repo's solo-attribution preference.
- **VERSIONS.md v0.2 row** updated to call out the peripheral
  expansion explicitly (12 wrapped classes), capability validation,
  and the E1M portability bound.
- **Peripheral ztest suite extended** with v0.2 wrapper coverage:
  - 12 generic NULL-arg / out-of-range cases for PWM, ADC, Counter,
    QEnc, I2S, CAN, RTC, WDT, each verifying `alp_last_error()`
    returns the precise reason.
  - New `alp_sdk.peripheral.caps_e3` twister scenario pinning the
    SoC choice to `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E3=y` and
    asserting that a 25-bit ADC request exceeds the documented
    24-bit cap and fails at `alp_adc_open` with
    `ALP_ERR_OUT_OF_RANGE` — the canonical case the user
    flagged ("16-bit ADC on a 12-bit SoC needs to fail
    cleanly").
  - `prj.conf` enables `CONFIG_PWM=y` / `CONFIG_ADC=y` /
    `CONFIG_COUNTER=y` / `CONFIG_SENSOR=y` / `CONFIG_I2S=y` /
    `CONFIG_CAN=y` / `CONFIG_RTC=y` / `CONFIG_WATCHDOG=y` /
    `CONFIG_THREAD_LOCAL_STORAGE=y` so the v0.2 wrapper sources
    compile and link in the test app.
- **v0.1 peripherals retrofitted** with `alp_last_error()` +
  SoC-capability validation so the diagnostic story is uniform
  across all 12 wrapped classes:
  `peripheral_i2c.c` / `peripheral_spi.c` / `peripheral_uart.c` /
  `peripheral_gpio.c` now stamp a precise reason
  (ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE / ALP_ERR_NOT_READY /
  ALP_ERR_NOMEM / ALP_ERR_IO) before returning NULL.  ADR 0002's
  v0.3 "retrofit" follow-up landed early.
- **CI: pr-generated-files.yml** verifies generated artefacts stay
  in sync with their inputs — re-runs `gen_soc_caps.py` and
  `abi_snapshot.py` then fails the PR if `include/alp/soc_caps.h`
  or `docs/abi/v0.1-snapshot.json` differ from the committed
  copies (ignoring the snapshot's `generated` date stamp).
  Catches "metadata edited but soc_caps.h not regenerated" and
  "header added/removed but ABI snapshot not bumped" PR mistakes.
- **PLAN.md §6 Open work** updated — gaps the v0.2 expansion
  closed are kept on the list with strikethrough plus a "closed"
  reference to the matching ADR or implementation, so the audit
  trail of what was promised vs. what shipped stays explicit.
- **Per-peripheral example apps** — nine standalone hand-written
  reference apps under `examples/<peripheral>-<demo>/`, one per
  wrapped peripheral class.  Each is a minimal Zephyr app that
  shows the canonical open / call / close pattern + reads
  `alp_last_error()` on failure.  Twister gates on
  `[<class>] done` console output.  Examples shipped:
  `gpio-button-led`, `i2c-scanner`, `spi-loopback`, `uart-echo`,
  `pwm-led-fade`, `adc-voltmeter` (incl. capability-validation
  rejection demo), `counter-alarm`, `rtc-clock`, `wdt-feed`,
  `can-loopback`.  ADR 0001's "standalone usage is first-class"
  principle made tangible.
- **Standalone usage callouts** — README's "Two consumer paths"
  section explicitly lists hand-written firmware as first-class
  alongside studio-codegen apps.  ADR 0001 expanded with the
  reasoning.  Saved as a persistent project memory so future
  sessions don't accidentally treat hand-written usage as a
  workaround.
- **VS Code support** — README "Using with VS Code" section,
  fresh `.vscode/{extensions,settings,tasks,c_cpp_properties}.json`
  config aligned with the current Zephyr-module + plain-CMake
  layout.  Tasks for `validate metadata`, `regen soc_caps`,
  `regen ABI snapshot`, twister, and `west build` for each
  example.  Extension recommendations are vendor-neutral —
  no Nordic-branded tooling (the SDK targets Alif / Renesas /
  NXP, not Nordic).
- **GitHub org migration** — repo references updated from
  `alpCaner/*` to `alplabai/*` (alp-sdk, e1m-spec,
  alp-zephyr-modules, alp-studio).  27 files / 56 substitutions.
- **CI infrastructure fixes** — bumped
  `ZEPHYR_SDK_INSTALL_DIR` to 0.17.0 to match the
  `ci:v0.27.4` Docker image; switched `--testsuite-root` paths
  from `${{ github.workspace }}` (host path) to
  `$GITHUB_WORKSPACE` (container path); fixed a `/**` sequence
  in the generated `soc_caps.h` comment that triggered
  `-Werror=comment`.  Twister CI was failing on the same three
  seams since the workflow was authored.

- **3 additional chip drivers (E1M EVK on-board sensors)** —
  `chips/icm42670/` (TDK 6-axis IMU), `chips/bmi323/` (Bosch 6-axis
  IMU), `chips/bmp581/` (Bosch barometer with already-compensated
  24-bit P + T outputs).  Each follows the existing chip-driver
  pattern with WHO_AM_I / CHIP_ID verify, ODR + FS / OSR config,
  burst register reads.  Brings the SDK's chip count to **11**.
- **Shared stub backend** at `src/common/stub_backend.c` — every
  public `alp_*` function defined as a NOSUPPORT stub, wired into
  both `src/baremetal/` and `src/yocto/` via `target_sources`.
  Plain-CMake builds with `-DALP_OS=baremetal` or `-DALP_OS=yocto`
  now produce link-complete `libalp_sdk.a`.
- **CI: pr-plain-cmake.yml** — runs `cmake -DALP_OS=baremetal` and
  `cmake -DALP_OS=yocto` against host gcc on every PR; catches
  "new public function added without a stub entry"
  undefined-reference errors that the Zephyr CI doesn't see.
- **`<alp/inference.h>` declared early (was held to v0.2)** —
  unified ML inference surface with backend selector
  (AUTO/CPU/Ethos-U/DRP-AI/DEEPX), model-format selector
  (TFLite/Vela/DRP-AI/DXNN/ExecuTorch), tensor descriptor with
  shape + dtype + quant params.  Stub returns NOSUPPORT; real
  impls land per backend (Ethos-U + CPU v0.2 on AEN-Zephyr,
  DRP-AI v0.3, DEEPX v0.4).  PLAN.md §6 entry strikethrough'd.
- **`<alp/storage.h>` declared early (v0.4 surface)** — block-
  oriented persistent storage: internal flash, QSPI / OSPI NOR,
  SD / eMMC.  open / get_info / read / write / erase / sync /
  close.  Real impls v0.4 (Yocto first-class).
- **`<alp/usb.h>` declared early (v0.3 surface)** — device + host
  roles in one header.  Three stock device classes wrapped
  (CDC-ACM, MSC, HID); other classes via vendor escape hatches.
- **`yocto/meta-alp/` skeleton** — Yocto BSP layer with
  `conf/layer.conf` + recipe shells (`alp-sdk_git.bb`,
  `alp-edgeai_git.bb`).  External Yocto integrators can
  `bitbake-layers add-layer` today; do_compile / do_install /
  FILES wiring lands v0.4.
- **`docs/getting-started.md`** — standalone-first walkthrough
  from `git clone` through `west build -b native_sim` and onto
  real silicon.  Reinforces ADR 0001's
  "standalone is first-class" stance with a concrete recipe
  every hand-written firmware author can follow.
- **EVK on-module audio + mux + current-sense + wake batch** --
  more user-supplied EVK schematic detail:
  - **TAS2563RPP smart-amp pair** -- two amps on I2C0 + I2S0
    (addresses 0x4D and 0x4E, plus broadcast 0x48).  New
    `chips/tas2563/` driver: init / probe / mode_ctrl /
    hw_enable.  Tuning-blob loader + I2S binding land in v0.3.x.
    AMP_ENABLE drives the chip's SD_N (active-high enable);
    AMP_FAULT reads IRQ_N (open-drain, internal pull-up).
    `CONFIG_ALP_SDK_CHIP_TAS2563` opt-in.
  - **Four MP34DT05TR-A PDM mics** -- two pairs on PDM0 and PDM1
    respectively, each pair sharing a PDM data line via the LR
    convention (LR=high samples on rising edge, LR=low on
    falling).  Documented inline; chips/pdm_mic helper covers
    the part.
  - **I2S0 74LVC157 mux** swaps the I2S0 bus between TAS2563 and
    M.2 E-key.  Control pins split across chips:
    `EVK_PIN_I2S_MUX_EN = IO8` (Alif P7.1) +
    `EVK_PIN_I2S_MUX_SEL = IO13` (CC3501E GPIO13).
  - **USB2 TMUXHS221 mux** swaps USB2 between the external USB-A
    jack and M.2 E-key USB.  Single control:
    `EVK_PIN_USB2_MUX_SEL = IO11` (CC3501E GPIO2).  /OEN
    tied to GND so the mux is always live.
  - **M.2 E-key UART wake** -- `EVK_PIN_M2E_UART_WAKE = IO19`
    (CC3501E GPIO19).
  - **Six INA236 current shunt monitors** -- one per power rail
    on I2C0 (3V3, 1V8, VIO, +V_CAM0, +V_CAM1, 5V).  INA236A
    occupies 0x40..0x43 and INA236B occupies 0x44..0x47, so all
    six fit on one bus.  Macros key on rail name; the user's
    notes had a ref-des typo (three "U30"s) -- cross-check
    pending.
  - **ENCODER_SW correction** -- moves from IO3 to IO4 per the
    schematic.  PEC12R-4222F-S0024 encoder + push switch.
  ABI: ADDED public header `alp/chips/tas2563.h` (snapshot now
  44 public headers).  SDK chip count climbs to **19**.

- **EVK Arduino + mikroBUS headers + wiring corrections** --
  user-supplied EVK wiring continues.  Arduino UNO header full
  pin map: `EVK_ARD_PWM1..PWM4` (= E1M PWM1/PWM4/PWM5/PWM2),
  `EVK_ARD_DIO1..DIO4` (= I2S1_SDO / I2S1_WS / SPI0_SCLK /
  SPI0_MOSI), `EVK_ARD_RST` (= I2S1_SCLK), and
  `EVK_ARD_A0..A5` (= ALP_E1M_ADC0..5).  Bus aliases:
  `EVK_SPI_BUS_ARDUINO` (= SPI1, CC3501E-mediated),
  `EVK_I2C_BUS_ARDUINO` (= I3C0), `EVK_UART_PORT_ARDUINO`
  (= UART1).  mikroBUS click header reuses the Arduino macros for
  the shared lines (SPI / I2C / UART / RST) and adds
  `EVK_MB_PWM` (= PWM6), `EVK_PIN_MB_INT` (= I2S1_SDI),
  `EVK_MB_ANA` (TBD-confirm).  In-flight wiring corrections
  bundled into the same commit:
  - **LED_GREEN -> PWM0** (was PWM2 inferred-and-wrong).
  - **CTP_INT -> SPI1_CS1** (was I2S1_SDI; the I2S1_SDI pad now
    correctly serves only as mikroBUS INT).  CTP_INT routes
    through the on-module CC3501E like the other CC3501E-side
    pins.
  - **CTP_RST is the TCAL9538 P3 only.**  An earlier draft also
    listed SPI1_CS0 as CTP_RST; the user clarified that's a
    mis-label -- SPI1_CS0 is the Arduino CK_CS (chip select).
  - **BMI323 address -> 0x68** (with SDO=low; user confirmed the
    strap is low, not high).  Resolves the apparent 0x69
    collision with ICM-42670-P.
  - **`EVK_SPI_BUS_M2_KEYM` removed** -- M.2 on this EVK
    uses PCIe + SDIO, not SPI; the previous macro was a guess.
  - **SPI0 fully repurposed** -- all five SPI0 pads
    (MISO/CS0/CS1/MOSI/SCLK) are GPIOs on this carrier
    (AMP_FAULT/AMP_ENABLE/IO_EXP_RST/CK_DIO4/CK_DIO3).  No
    peripheral SPI0 bus available on the EVK; documented in the
    header.
  - **I2S1 fully repurposed** -- all four I2S1 pads
    (SDO/WS/SDI/SCLK) are GPIOs (CK_DIO1/CK_DIO2/MB_INT/CK_RST).
    No peripheral I2S1 bus on the EVK; the I2S0 path remains
    available for audio.
  ABI snapshot still 43 public headers (no shape change).
- **EVK wiring batch: I2C1 = DSI/CSI control, IO5 = CAM_RST,
  PWM-driven RGB LED, repurposed SPI0 + I2S1 + AUDIO_CLK pads,
  TCAL9538 I/O expander, SDIO mux, touch-screen control** --
  large user-supplied EVK schematic update lands in
  `<alp/boards/alp_e1m_evk.h>` + `chips/tcal9538/`:
  - I2C0 stays the sensor / IO-expander / current-monitor bus;
    new `EVK_I2C_BUS_DSI_CSI = ALP_E1M_I2C1` is the
    display-and-camera-control bus per the EVK's `DSI_CSI_I2C` net.
  - `IO5` is `CAM_RST` (the camera reset line), correcting an
    earlier placeholder that had `IO5 = IO_EXP_RST`.
  - RGB LED now drives via PWM rather than GPIO:
    `EVK_PWM_LED_RED = ALP_E1M_PWM3`,
    `_GREEN = ALP_E1M_PWM2` (TBD-confirm; user stated PWM3=R + PWM1=B
    but didn't name the green PWM, PWM2 is inferred), and
    `_BLUE = ALP_E1M_PWM1`.
  - Off-GPIO_IO repurposed pads (`AUDIO_CLK`, `SPI0_MISO`,
    `SPI0_CS0`, `SPI0_CS1`, `I2S1_SDI`, `SPI1_CS0`) are exposed
    via `EVK_PIN_OVERLAY_BASE + N` indices that the carrier
    `alp,pin-array` overlay extends past the standard 42-entry
    range.  Names: `EVK_PIN_IO_EXP_INT` (AUDIO_CLK),
    `_IO_EXP_RST` (SPI0_CS1), `_AMP_FAULT` (SPI0_MISO),
    `_AMP_ENABLE` (SPI0_CS0), `_CTP_INT` (I2S1_SDI),
    `_CTP_RST` (SPI1_CS0, routed through CC3501E).
  - SDIO mux for SD-card vs M.2 E-key: 74LVC157 controlled by
    `EVK_PIN_SDIO_MUX_EN = ALP_E1M_GPIO_IO20` (active-low
    enable) and `EVK_PIN_SDIO_MUX_SEL = ALP_E1M_GPIO_IO21`
    (0 = M.2 E SDIO, 1 = SD card).  Both pins route through the
    on-module CC3501E (per from-cc3501e.tsv), so firmware drives
    the mux via `ALP_CC3501E_CMD_GPIO_WRITE` on the inter-chip
    SPI1, NOT through Alif's GPIO peripheral.  Noted in the
    header.
  - On-board sensor I2C addresses corrected per user-supplied
    strap values: ICM-42670-P AD0=high -> 0x69 (was 0x2C
    placeholder), BMI323 SDO=high -> 0x69 (was 0x68 placeholder),
    BMP581 SDO=high -> 0x47, TCAL9538 A1=1/A0=0 -> 0x72 (was 0x70).
    **Address collision warning**: ICM-42670-P and BMI323 both
    compute to 0x69 with the documented straps -- header carries
    a TODO comment asking the user to confirm whether only one
    IMU is populated at a time or whether the schematic strap is
    actually different.
  - **TCAL9538 I/O expander pin map** (8 channels of LCD / camera /
    capacitive-touch control + sensor interrupts) materialised as
    a typed enum `evk_ioexp_pin_t` so apps don't pass raw
    indices.  Note: P3 was earlier listed as `CTP_RST` but the
    user has since separately said `CTP_RST = SPI1_CS0`; both
    routes are preserved in the header with a TBD-confirm note
    flagging the conflict.
  - New `chips/tcal9538/` driver -- TI TCA9538 / TCAL9538
    8-channel I/O expander.  Per-pin and bulk
    direction / write / read APIs with cached register state
    (avoids read-modify-write round-trips on every set call).
    `CONFIG_ALP_SDK_CHIP_TCAL9538` opt-in.
  ABI snapshot now 43 public headers; SDK chip count climbs to
  **18**.
- **EVK MIPI CSI camera-mux helper + IO2 wiring correction** --
  the EVK routes a single MIPI CSI lane pair from the SoM through
  a **PI3WVR626XEBEX** 2:1 mux (camera A vs camera B); its
  `SEL` pin lands on E1M `IO2` (`W2` / Alif `P12.5`).  New thin
  driver at `chips/cam_mux_pi3wvr626/` + matching public header
  wraps SEL as a GPIO with a typed `INPUT_A` / `INPUT_B` enum.
  `<alp/boards/alp_e1m_evk.h>` gains `EVK_PIN_CAM_MUX_SEL`
  and **drops the previous placeholder `EVK_PIN_LED_BLUE = IO2`
  mapping** -- the schematic confirms `IO2` is the mux line, not
  an LED.  `LED_BLUE` is now TBD; `LED_RED` / `LED_GREEN` carry
  an explicit "TBD-confirm" caveat.  `docs/boards/e1m-evk.md`
  picks up the camera-mux details + truth table.  SDK chip count
  climbs to **17**; ABI snapshot to 42 headers.
- **On-module I2C device drivers (E1M-AEN)** -- four new chip
  drivers covering the SoM-internal I2C devices the user
  documented:
  - `chips/tmp112/` + `<alp/chips/tmp112.h>` -- TI TMP112 digital
    temperature sensor (LPI2C, +/-0.5 C, 12/13-bit, 0.0625 C/LSB).
    Init / set_rate / set_extended_mode /
    read_temp_milli_c (avoids float on M-class).
  - `chips/rv3028c7/` + `<alp/chips/rv3028c7.h>` -- Micro Crystal
    RV-3028-C7 32.768 kHz 1 PPM RTC (LPI2C, addr 0x52 fixed).
    BCD time get/set, alarm with match-mask, INT-pin enable,
    alarm flag check+clear.  Routes the alarm into Alif
    `P15_0_FLEX` per the inter-chip wiring TSV.
  - `chips/optiga_trust_m/` + `<alp/chips/optiga_trust_m.h>` --
    Infineon OPTIGA Trust M secure element
    (SLS32AIA010MLUSON10XTMA2; LPI2C, addr 0x30).  v0.3 ships
    init + I2C connectivity-probe; full APDU command set lands
    via Infineon's host library + MbedTLS PSA registration in
    v0.3.x so `<alp/security.h>` picks up HW acceleration
    transparently.
  - `chips/eeprom_24c128/` + `<alp/chips/eeprom_24c128.h>` --
    Generic 24Cxx-class 128-Kbit (16 KB) EEPROM driver covering
    Onsemi N24S128C4DYT3G (E1M-AEN default) and the
    footprint-compatible STMicro M24128-BFMH6TG (DNP alt).
    Page-aware writes (64-byte page, ACK-poll wait), arbitrary-
    range reads.  On I2C2.
  Brings the SDK's chip count to **16** (was 12).  ABI snapshot
  now 41 public headers.
- **Secure boot + secure OTA roadmap entries (v0.4)** -- production
  E1M deployments need chain-of-trust from immutable ROM up.
  Plan: MCUboot already pulled in via west.yml verifies
  application image signatures at every boot; signing keys live
  in OPTIGA Trust M's secure NVM (provisioned during factory
  programming); the verification path goes through MbedTLS PSA,
  routing to the OPTIGA hardware accelerator transparently once
  the v0.3.x PSA driver lands.  Secure OTA delivers signed
  payloads over `<alp/iot.h>` MQTT/HTTP and swaps via MCUboot's
  `swap-using-scratch` mode.  Yocto-on-V2N / i.MX 93 uses
  `meta-mender` for the equivalent.  Documented in `PLAN.md` §2.4.1
  and `VERSIONS.md` v0.4 section.
- **CC3501E Alif-side host driver** -- new chip driver at
  `chips/cc3501e/cc3501e.c` + public header
  `include/alp/chips/cc3501e.h`.  Alif-side of the inter-chip
  bridge: `cc3501e_init` / `_reset` / `_get_version` /
  `_request` / `_set_event_callback` / `_deinit`.  Synchronous
  `cc3501e_request` frames a header + payload, drives SPI1 via
  `alp_spi_transceive`, copies the response back out.  Reset
  pulses `WIFI.EN` (P15_5) + `E_WIFI.NRST` (P15_1_FLEX) via
  `alp_gpio_*` when those pins are populated.  New
  `CONFIG_ALP_SDK_CHIP_CC3501E` Kconfig (depends on `SPI && GPIO`).
  SDK's chip count climbs to **12**.  ABI snapshot now 37
  headers.  Followed in a later commit by the
  `<alp/iot.h>` / `<alp/ble.h>` dispatcher branches that route
  through this driver on E1M-AEN.
- **CC3501E wire protocol + bridge architecture** -- the E1M-AEN
  family carries a separate TI CC3501E Wi-Fi 6 + BLE 5.4 combo
  MCU, and certain E1M pads (SPI1 + IO11 / IO13 / IO15..21 + the
  camera-enable LDOs) terminate on it rather than on the Alif
  silicon.  Communication runs over an inter-chip SPI1 bus
  (Alif master, CC3501E slave) carrying a custom binary wire
  protocol.  New `include/alp/protocol/cc3501e.h` freezes that
  protocol at v1: 4-byte header + <=512 B payload, opcodes grouped
  by subsystem (meta / Wi-Fi / sockets / BLE / GPIO proxy /
  camera enable / diagnostics), per-subsystem payload structs
  (Wi-Fi connect, BLE adv start, GPIO configure, ...).
  `docs/cc3501e-bridge.md` documents the architecture, the
  two-repo split per ADR 0005 (alp-sdk owns the protocol +
  Alif-side client; the firmware that runs on the CC3501E lives
  in `alplabai/cc3501e-firmware`), and the firmware bootstrap
  recipe.  Alif-side `chips/cc3501e/` driver +
  `<alp/iot.h>` / `<alp/ble.h>` route-through-CC3501E follow.
- **Authoritative E1M-AEN pinout data** (item: pending HW configs) --
  five user-supplied TSVs land at `metadata/e1m_modules/aen/`:
  `from-alif.tsv` (E1M pad -> Alif silicon, 91 routed entries),
  `from-cc3501e.tsv` (E1M pad -> CC3501E, 14 entries),
  `inter-chip.tsv` (Alif <-> CC3501E SPI1 + SDIO + control
  signals + camera-enable wiring), `alif-ospi.tsv` (Alif's OSPI
  controller pad reservation for optional on-module memories),
  `alif-ethernet-phy.tsv` (DP83825IRMQR PHY <-> Alif RMII MAC
  pad map).  Resolves the AEN-side TBDs in
  `project_pending_hw_configs` memory note; downstream artefacts
  (`include/alp/boards/alp_e1m_evk.h`, the per-SoC metadata
  in `metadata/socs/alif/ensemble/`, alp-studio's pin allocator)
  can now regenerate against this single source of truth.  V2N +
  i.MX 93 family pinouts remain pending until the user supplies them.
- **DRP-AI inference dispatcher hook (item 11 / v0.3 milestone)** --
  `<alp/inference.h>` dispatcher in `src/zephyr/inference_zephyr.c`
  now routes `ALP_INFERENCE_BACKEND_DRPAI` into a per-backend
  `inference_drpai.c` source compiled when
  `CONFIG_ALP_SDK_INFERENCE_DRPAI=y`.  The AUTO resolver prefers
  DRPAI before falling through to TFLM-CPU.  v0.3 ships the
  dispatch routing + a 16-byte placeholder open() so apps verify
  routing today; v0.4 fills the body via Renesas's DRP-AI
  translator runtime once the vendor pack is in CI.
- **V2N vendor scaffolding (item 10 / v0.3 milestone)** --
  `vendors/renesas-rzv2n/` gains `i2c.c` / `spi.c` / `gpio.c` /
  `uart.c` source skeletons mirroring the Alif vendor layout.
  i2c.c carries the full FSP `r_riic_master` binding shape; the
  other three are starter shells with the FSP includes wired in
  for v0.4 to fill bodies.  `vendors/renesas-rzv2n/CMakeLists.txt`
  follows the same `ALP_HAS_RENESAS_FSP` gate as Alif, so the
  files compile to empty translation units when the FSP pack
  isn't present.  `src/baremetal/CMakeLists.txt` extends the
  ALP_SOM dispatch with a v2n branch; `pr-plain-cmake.yml` gains
  a `baremetal-v2n` scenario verifying the dispatch + scaffolding
  build green without the proprietary FSP pack.  Real FSP link
  follows in v0.4 alongside the V2N HIL bring-up.
- **`<alp/usb.h>` real device-stack lifecycle wrapper (v0.3 milestone)** --
  `src/zephyr/usb_zephyr.c` replaces the v0.1 NOSUPPORT stub.  Wraps
  Zephyr's `usb_enable` / `usb_disable` for the device-role
  lifecycle.  Per-class endpoint read/write (CDC-ACM, MSC, HID)
  and host-role bring-up land in v0.3.x.  New
  `CONFIG_ALP_SDK_USB` (depends on `USB_DEVICE_STACK`, default y),
  `CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES` (default 1).
  `tests/zephyr/usb/` smoke suite (5 cases).
- **`<alp/mproc.h>` real Zephyr MBOX wrapper (v0.3 milestone)** --
  `src/zephyr/mproc_zephyr.c` replaces the v0.1 NOSUPPORT stub.
  Wraps Zephyr's MBOX driver class (Alif's MHU is registered as
  a Zephyr mbox device on AEN) for the mailbox path:
  `mbox_init_channel` + `mbox_send` + `mbox_register_callback`.
  DT-anchored shared-memory regions (`alp-shmemN` aliases) and
  per-SoC HWSEM register access return NOSUPPORT until v0.3.x
  finishes the EVK overlay and the Alif HWSEM driver.  New
  Kconfig: `CONFIG_ALP_SDK_MPROC` (depends on MBOX, default y),
  pool-size knobs (`MAX_SHMEM_HANDLES`, `MAX_MBOX_HANDLES`,
  `MAX_HWSEM_HANDLES`).  `tests/zephyr/mproc/` smoke suite
  (7 cases).
- **`<alp/security.h>` real MbedTLS PSA Crypto wrapper (v0.3 milestone)** --
  `src/zephyr/security_zephyr.c` replaces the v0.1 NOSUPPORT stub.
  Wraps PSA Crypto (`psa_crypto_init`, `psa_hash_*`, `psa_aead_*`,
  `psa_generate_random`) -- when MbedTLS is built with the
  vendor's HW-accelerator driver registered, PSA routes to the
  Alif Ensemble crypto subsystem (E7/E8) or the Renesas RZ/V2N
  RSIP automatically without code changes here.  Hash covers
  SHA-256/384/512; AEAD covers AES-128/256-GCM and
  ChaCha20-Poly1305; TRNG via `psa_generate_random`.  New
  Kconfig: `CONFIG_ALP_SDK_SECURITY` (depends on
  `MBEDTLS_PSA_CRYPTO_C`, default y),
  `CONFIG_ALP_SDK_MAX_HASH_HANDLES` / `_AEAD_HANDLES` (default 4
  each).  AEAD encrypt/decrypt path uses a 4 KiB stack scratch
  buffer (heap fall-back lands in v0.3.x for larger blobs).
  `tests/zephyr/security/` smoke suite (7 cases).
- **`<alp/ble.h>` real Zephyr `bt`-host wrapper (v0.3 milestone)** --
  `src/zephyr/ble_zephyr.c` replaces the v0.1 NOSUPPORT stub.
  Wraps Zephyr's `bt` host stack: `bt_enable` for the singleton
  controller, `bt_le_adv_start` / `bt_le_adv_stop` for the
  peripheral role, `bt_le_scan_start` / `bt_le_scan_stop` with a
  per-packet `recv` callback for the central role, and
  `bt_conn_le_create` / `bt_conn_disconnect` for connections.
  Gated on new `CONFIG_ALP_SDK_BLE` (depends on `BT`, default y).
  GATT runtime registration + sync read/write helpers stay
  NOSUPPORT until v0.3.x lands a callback-with-semaphore shim
  (Zephyr's GATT API is async-only; the public surface is
  synchronous).  New `CONFIG_ALP_SDK_BLE_MAX_CONNS` (default 2)
  sizes the connection-handle pool.  `tests/zephyr/ble/` smoke
  suite (6 cases) verifies the NOSUPPORT-fall-back contract +
  every NULL-arg branch under native_sim.
- **Bare-metal AEN backend scaffolding (v0.2 milestone, item 4)** --
  v0.2's "stub-to-real" baseline for the bare-metal Alif Ensemble
  path.  New `vendors/alif/{i2c,spi,gpio,uart}.c` source files
  wrap the four core peripherals on Alif's CMSIS-Driver layer
  (`Driver_I2C0..3`, `Driver_SPI0..3`, `Driver_GPIO0..7`,
  `Driver_USART0..3`).  Each wrapper's body is gated on
  `ALP_HAS_ALIF_HAL` so the files compile cleanly even without
  the proprietary Ensemble CMSIS pack present.  When the option
  flips ON the wrapper sets `ALP_VENDOR_OVERRIDES_PERIPHERAL`,
  which excludes the matching stubs from `src/common/stub_backend.c`
  -- no duplicate symbols.  CMake wiring: new top-level
  `ALP_SOM` variable (`none|aen|v2n|imx93`) layered on top of
  `ALP_OS`; `src/baremetal/CMakeLists.txt` pulls
  `vendors/alif/CMakeLists.txt` only when `ALP_OS=baremetal`
  AND `ALP_SOM=aen`, leaving non-AEN bare-metal builds on the
  pure stub path.  `pr-plain-cmake.yml` gains a `baremetal-aen`
  job that exercises the new dispatch.  Real HAL link follows in
  v0.2.x via `cpackget add AlifSemiconductor::Ensemble` -- the
  scaffolding here lets that change be a body-of-function-only
  follow-up.
- **Cross-platform development support made explicit** -- README
  gains a "Cross-platform development" table calling out
  first-class macOS / Windows / Linux support; `docs/getting-started.md`
  prerequisite section adds per-OS install one-liners (Homebrew,
  apt, winget, WSL).  No code changes -- the toolchain (Zephyr +
  west + CMake + Python + GCC) was already cross-platform; the
  documentation just admits it now.  New `.gitattributes` pins
  LF endings on every source file so a Windows checkout, a macOS
  clone, and a Linux pull see and commit identical bytes --
  avoids clang-format-diff CI failures when a developer crosses
  hosts.
- **[ADR 0005](docs/adr/0005-alp-sdk-vs-alp-studio-boundary.md)** —
  alp-sdk vs alp-studio repo boundary.  Codifies the dual-use
  acid test ("would a hand-written-firmware author ever directly
  use this?") and the canonical table of which artefacts belong
  in which repo.  `docs/architecture.md` gains a "Repository
  boundary" subsection so reviewers can resolve where a new
  addition belongs without opening the ADR.
- **`<alp/inference.h>` real wrapper + TFLM/Ethos-U C++ executor**
  — v0.2's third "stub-to-real" milestone.  `src/zephyr/inference_zephyr.c`
  replaces the v0.1 NOSUPPORT stub with the public-API glue: handle
  pool (default 2), capability validation, backend selector
  (AUTO / CPU / ETHOS_U / DRPAI / DEEPX_DX), and dispatch into a
  per-backend executor.  `src/zephyr/inference_tflm.cpp` -- new C++
  source compiled only when `CONFIG_ALP_SDK_INFERENCE_TFLM=y` (depends
  on `TENSORFLOW_LITE_MICRO`) -- wraps `tflite::MicroInterpreter`
  and `tflite::MicroMutableOpResolver<32>` with the canonical
  MobileNetV2 op set + the Arm `AddEthosU()` registration so
  Vela-compiled `.tflite` files dispatch their ETHOS-U custom op
  to the NPU.  Default 128 KiB tensor arena (caller-supplied via
  `cfg.arena` overrides).  The CPU backend uses TFLM's reference
  kernels for any layer Vela leaves on-host plus pure CPU models;
  ETHOS_U routes through the same MicroInterpreter with the op
  resolver picking the NPU op when available.  `west.yml`
  name-allowlist gains `tflite-micro` so `west update` pulls the
  module on every refresh; CI's native_sim builds keep
  `CONFIG_ALP_SDK_INFERENCE_TFLM=n` and the wrapper falls back to
  ALP_ERR_NOSUPPORT cleanly.  New Kconfig options:
  `CONFIG_ALP_SDK_INFERENCE_TFLM` (default n; enables the C++
  executor), `CONFIG_ALP_SDK_INFERENCE_ETHOS_U` (default y if
  TFLM and SOC_SERIES_ENSEMBLE_E7), `CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES`
  (default 2).  The `examples/edgeai-vision-aen/` example now
  wires `alp_inference_open` into stage 3 (model load) using
  `ALP_INFERENCE_BACKEND_AUTO` -- skips on native_sim with the
  precise NOSUPPORT diagnostic, runs the real Vela model on AEN HW
  once the toolchain output lands under `models/`.  New
  `tests/zephyr/inference/` ztest suite (7 cases) verifies the
  cfg-validation + NOSUPPORT-fall-back contract under native_sim.
- **`<alp/audio.h>` real PDM input + I²S output on AEN-Zephyr** —
  v0.2's second "stub-to-real" milestone.  `src/zephyr/audio_zephyr.c`
  replaces the v0.1 `audio_stub.c` and splits in two halves: a
  `audio_in` path wrapping Zephyr's `audio_dmic` API
  (`dmic_configure` / `dmic_trigger` / `dmic_read`), and a
  `audio_out` path that delegates straight to the existing
  `alp_i2s_*` wrapper (TX direction, frame translation in the
  config).  Two Kconfig toggles -- `CONFIG_ALP_SDK_AUDIO_IN`
  (depends on `AUDIO_DMIC`, default y) and `CONFIG_ALP_SDK_AUDIO_OUT`
  (depends on `I2S`, default y) -- gate the real glue behind the
  v0.1 NULL-with-NOSUPPORT contract for native_sim and any other
  no-audio target.  The PDM path runs the ALP DSP chain on every
  block: a 1st-order DC-block (alpha = 0.995 in Q15, ~10 Hz
  cut-off at 16 kHz) silences the DC bias every PDM mic carries.
  AGC and resample land in v0.3 alongside `<alp/security.h>`; the
  hook stays in `dmic_read`'s post-pass so v0.3 only adds passes.
  The output path adds a Q8.8 software volume scale (default
  unity, `alp_audio_out_set_volume(out, vol)` from 0..255) that
  applies before `alp_i2s_write` -- usable on codecs without a
  separate gain pin.  Pool sizes
  (`CONFIG_ALP_SDK_MAX_AUDIO_IN_HANDLES` / `_OUT_HANDLES`, both
  default 1) and the per-handle slab (4 KiB *
  `CONFIG_ALP_SDK_AUDIO_IN_SLAB_BLOCKS`, default 4 blocks) are
  statically allocated -- no `k_malloc` in the audio path.  New
  `examples/audio-loopback/` hand-written reference: opens
  `ALP_E1M_PDM0` + `ALP_E1M_I2S0`, streams 50 blocks of 256
  S16-mono frames from mic to speaker at 16 kHz, prints
  `[audio] done`.  Skips gracefully on native_sim (no audio
  devices) so the existing twister scenario continues to pass.
  New `tests/zephyr/audio/` ztest suite (9 cases) verifies the
  NOSUPPORT-fall-back contract under native_sim plus every
  NULL-arg / invalid-config branch.
- **`<alp/iot.h>` real Wi-Fi station + MQTT on AEN-Zephyr** —
  v0.2's first "stub-to-real" milestone.  `src/zephyr/iot_zephyr.c`
  replaces the v0.1 `iot_stub.c` and wraps Zephyr's `wifi_mgmt`
  (`net_mgmt(NET_REQUEST_WIFI_CONNECT, …)`) plus `mqtt_client`
  (`mqtt_connect`, `mqtt_publish`, `mqtt_subscribe`, `mqtt_input`,
  `mqtt_live`).  Two new Kconfig toggles --
  `CONFIG_ALP_SDK_IOT_WIFI` (depends on `WIFI && NET_MGMT`,
  default y) and `CONFIG_ALP_SDK_IOT_MQTT` (depends on
  `MQTT_LIB && NET_TCP`, default y) -- gate the real glue.  When
  neither is on (host build, native_sim, any target without a
  Wi-Fi/TCP stack) the wrapper honours the v0.1 contract:
  `*_open()` returns NULL with `alp_last_error()` =
  `ALP_ERR_NOSUPPORT`.  Static handle pools sized via
  `CONFIG_ALP_SDK_MAX_WIFI_HANDLES` (default 1, single-radio per
  E1M SoM) and `CONFIG_ALP_SDK_MAX_MQTT_HANDLES` (default 2),
  with `CONFIG_ALP_SDK_MQTT_BUF_SIZE` (default 256 B) controlling
  the per-client rx / tx / topic scratch.  `examples/iot-connected-camera/`
  ships a new `overlay-aen.conf` that pulls in `CONFIG_NET_*`,
  `CONFIG_WIFI`, `CONFIG_WIFI_NM_WPA_SUPPLICANT`, `CONFIG_MQTT_LIB`
  for AEN builds; the base `prj.conf` remains native_sim-friendly
  and the example continues to print `[iotcam] done` end-to-end.
  New `tests/zephyr/iot/` ztest suite (12 cases) verifies the
  NOSUPPORT-fall-back contract + every NULL-arg branch, bringing
  twister's `native_sim/native/64` total to **64 cases** (52 + 12).

### Notes

- Repo migrated from a CMSIS-Toolbox csolution / VFT mock-driver
  layout to the OS-pivoted layout described in `docs/architecture.md`.
  The previous tree is preserved in git history.
- Apache-2.0 license replaces the original MIT badge.
- Twister build hit a Kconfig-discovery issue (the alp-sdk module
  surface isn't being merged) — fix tracked, will be green before tag.
