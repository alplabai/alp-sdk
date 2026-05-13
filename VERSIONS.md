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
| v0.2.0  | **surface complete; impl in progress** | 12 wrapped peripheral classes + capability validation + E1M portability bound + per-peripheral examples + v0.2/v0.3 stub headers + ADRs all shipped early.  Bare-metal AEN real, V2N intro, EdgeAI app real are the remaining v0.2 deliverables. |
| v0.3.0  | **surface complete; impl in progress** | Real impl behind v0.2-declared surfaces (`<alp/audio.h>`, `<alp/ble.h>`, `<alp/security.h>`, `<alp/mproc.h>`).  IoT reference app, multi-proc completion, display polish, V2N+M1 intro.  **Plus: `board.yaml` project config + loader (`scripts/alp_project.py`), DEEPX DX-M1 + Ethos-U65/i.MX 93 inference-backend dispatchers, bench + fuzz scaffolding, Coverity workflow stub, Renesas V2N AI SDK 7.10 BSP wire-up in meta-alp.** |
| v0.4.0  | **in-progress (prep code merged, untested)** | Yocto first-class (V2N + V2N+M1 full); secure boot + secure OTA on AEN-Zephyr.  **Prep merged on main:** Yocto core-4 peripheral wrappers (I²C / SPI / UART / GPIO + IRQ dispatcher), MQTT via libmosquitto, per-class override gates, `lwrb` + `nanopb` pinned behind `extras-v04` group.  Failure-path ctest green; **HW roundtrip still pending** — every row in [`docs/test-plan.md`](docs/test-plan.md)'s v0.4 section gates the tag. |
| v0.5.0  | **wave-2 surface complete; HAL bodies pending** | Wave-2 GD32-bridge DSP + advanced-timer + power-saving + AEN-audit top-five gap surfaces.  **Shipped on main** (per `memory/project_v05_autonomous_burst_2026_05_12.md`): PROTOCOL_VERSION_MINOR 4 -> 5 + seven new reserved opcodes (`0x23..0x28` + `0x36`).  `<alp/dsp.h>` standalone DSP-chain API (FIR / IIR / WINDOW / FFT) with CMSIS-DSP + portable-C fallback.  `alp_adc_filter_t` + `alp_adc_spectrum_t` in `<alp/adc.h>` composing stream + chain.  Advanced timer extras in `<alp/pwm.h>` (`alp_pwm_capture_t` + `alp_pwm_single_pulse`).  `<alp/power.h>` system-power-mode surface.  `<alp/gpu2d.h>` 2D-accelerator surface (AEN audit headline gap).  `<alp/camera.h>::alp_camera_configure_isp` for Mali-C55 ISP toggles.  `<alp/storage.h>::alp_storage_configure_inline_aes` for AEN SecAES on OSPI / HexSPI.  `alp_delay_us` + `alp_delay_ms` portable primitives.  CC3501E §2A.2-plan items §5.1..§5.5 + §5.7 (protocol docs hygiene, named GPIO enums, IRQ event structs, diag info, reset-timing fix, power policy).  v2n_supervisor `alp_z_v2n_supervisor_invalidate()` post-wake re-init hook.  Six `gd32g553_*` host helpers mirroring the new opcodes.  Tests for every new surface.  **HAL bodies pending** in the GD32 firmware tree (`gd32-bridge/hal/`) -- every wave-2 reserved opcode returns STATUS_NOSUPPORT until the firmware ships them.  CAU (DES / TDES / AES) deferred to v0.6 with PSA driver registration. |
| v1.0.0  | planned     | unified repo, docs, ABI freeze, production-ready  |

> **Note on v0.2 sequencing.**  The original v0.2 plan in this file
> assumed only ~5 chip drivers + a couple of new libraries.  In
> practice the v0.2 milestone landed a much larger architectural
> shift early — full peripheral coverage (12 classes), the
> diagnostic / capability-validation infrastructure, the
> standalone-usage hardening with VS Code support and per-peripheral
> reference apps, and the four new ADRs at `docs/adr/`.  Each of
> those is documented in `CHANGELOG.md` under [Unreleased] and in
> the per-section deliverables below.

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
| Math / DSP  | _(removed)_ | App code includes `arm_math.h` directly; ALP does not re-export CMSIS-DSP. SDK internals may use it via `ALP_HAS_CMSIS_DSP`. |
| Camera      | header    | `<alp/camera.h>` API frozen; impl returns `ALP_ERR_NOSUPPORT`            |
| GUI/LVGL    | header    | `<alp/gui.h>` includes upstream LVGL with ALP defaults; no widgets       |
| IoT         | header    | `<alp/iot.h>` API frozen; impl stubbed                                   |
| Audio       | —         | not in v0.1                                                              |
| BLE         | —         | not in v0.1                                                              |

### Pulled forward from v0.2 / v0.3 (shipped during the v0.1 cycle)

The architectural shifts below were originally scheduled later but
landed during the v0.1 cycle so apps can compile against the full
v1.0-shape surface from day one:

| Item                                                  | Originally planned for | Notes |
|-------------------------------------------------------|------------------------|-------|
| Peripheral coverage at 12 wrapped classes             | gradual through v0.4   | PWM, ADC, Counter+QEnc, I²S, CAN-FD, RTC, Watchdog all wrapped on Zephyr-AEN.  See [ADR 0003](docs/adr/0003-peripheral-coverage.md). |
| `alp_last_error()` thread-local diagnostic            | v0.3 retrofit          | Stamped by every `*_open` failure; lets standalone firmware diagnose `NULL` returns precisely.  See [ADR 0002](docs/adr/0002-error-mechanism.md). |
| `<alp/soc_caps.h>` capability tables                  | v0.3                   | Generated from `metadata/socs/**.json`; rejects 16-bit-ADC-on-12-bit-SoC at `*_open` time. |
| `ALP_E1M_<CLASS>_COUNT` portability bounds            | undocumented           | Made the cross-SoM-portable instance count per class explicit.  See [ADR 0004](docs/adr/0004-e1m-portability-bound.md). |
| `<alp/audio.h>` / `<alp/ble.h>` / `<alp/security.h>` / `<alp/mproc.h>` surface declarations | v0.2 / v0.3       | Compile-clean stubs that return `ALP_ERR_NOSUPPORT`; apps can `#include` them today, real impl in their target version. |
| Per-peripheral hand-written reference apps            | v0.4                   | 11 examples under `examples/<peripheral>-<demo>/` covering every wrapped peripheral.  Hand-written firmware authors can crib these as starting templates. |
| ABI snapshot tooling                                  | v1.0                   | `scripts/abi_snapshot.py` + `docs/abi/v0.1-snapshot.json` shipped.  CI gate via `pr-generated-files.yml` post-1.0. |
| Architecture Decision Records                         | undocumented           | Four ADRs under `docs/adr/` covering the wrapper-vs-Zephyr boundary, error mechanism, peripheral coverage, and E1M portability bound. |
| VS Code first-class support                           | undocumented           | `.vscode/` config (extensions, settings, tasks, c_cpp_properties) aligned with the Zephyr-module + plain-CMake layout.  Vendor-neutral extension list. |

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

> **Status note (2026-05-10).**  The bulk of v0.2's *surface* (the
> 12 wrapped peripheral classes, the diagnostic / capability
> infrastructure, the v0.2/v0.3 stub headers, the per-peripheral
> example apps, the four ADRs at `docs/adr/`) shipped early during
> the v0.1 cycle.  v0.2's remaining deliverables are the
> *implementations* behind that surface: real Wi-Fi station + MQTT
> on AEN-Zephyr, the EdgeAI reference app's full pipeline, the
> bare-metal AEN backend going from stub to real, and the V2N
> intro.  The surface that landed early is documented above
> under "Pulled forward from v0.2 / v0.3".

### New libraries / surface

- **Peripherals — coverage doubled to 12 classes** (was 4 in v0.1):
  PWM (`<alp/pwm.h>`), ADC (`<alp/adc.h>`), Counter + quadrature
  decoder (`<alp/counter.h>`), I²S/SAI (`<alp/i2s.h>`), CAN/CAN-FD
  (`<alp/can.h>`), RTC (`<alp/rtc.h>`), Watchdog (`<alp/wdt.h>`).
  Each follows the v0.1 wrapper pattern: thin Zephyr forward,
  per-class Kconfig opt-in, static handle pool.  See
  [ADR 0003](docs/adr/0003-peripheral-coverage.md).
- **Capability validation** — `alp_last_error()` thread-local +
  `<alp/soc_caps.h>` generated from `metadata/socs/**.json` reject
  configs that exceed the active SoC's documented hardware caps.
  Canonical case: 16-bit ADC on a 12-bit SoC fails at
  `alp_adc_open` with `ALP_ERR_OUT_OF_RANGE`.  See
  [ADR 0002](docs/adr/0002-error-mechanism.md).
- **E1M portability bound** — `ALP_E1M_<CLASS>_COUNT` macros in
  `<alp/e1m_pinout.h>` document the cross-SoM-portable instance
  count per peripheral class.  See
  [ADR 0004](docs/adr/0004-e1m-portability-bound.md).
- **Peripherals (v0.1 retrofit):** `alp_spi_*` lands on bare-metal
  too; v0.1 wrappers (I2C/SPI/UART/GPIO) gain `alp_last_error()`
  diagnostics.
- **Chips:** `ov5640` (MIPI CSI camera) + `bme280` (env) +
  `lis2dw12` (3-axis accel) + `ssd1331` (small colour OLED) +
  `pdm_mic` (PDM microphone) helper landed v0.1-tail.  Real impl
  for `ov5640` resolution presets + `pdm_mic` underlying I²S
  finishes in v0.2.  Plus `camera_parallel` (CPI fallback).
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

`examples/aen/edgeai-vision-aen/` — end-to-end reference:
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

### v0.4 prep merged on `main` (through 2026-05-11) — **mostly untested**

The structural pieces below landed ahead of the v0.4 tag during the
v0.3 cycle.  Each is gated so a workspace that doesn't need the
v0.4 deliverable falls back cleanly to the v0.3 stubs.  Each also
has a corresponding row in [`docs/test-plan.md`](docs/test-plan.md);
most rows are 🟡 partial -- failure paths covered by CI ctest, real
target verification still parked behind the `hil-yocto` runner.

**Peripherals + IoT (Yocto):**

- **Yocto core-4 peripherals — real on Linux.**
  `alp_i2c_*` via i2c-dev, `alp_spi_*` via spidev, `alp_uart_*` via
  termios, `alp_gpio_*` via the GPIO chardev v2 ABI -- direct
  ioctls against the kernel UAPI, no libgpiod dependency.
- **GPIO IRQ dispatcher.**  Shared pthread `poll()` loop with an
  eventfd wake; supports rising / falling / both edges.
- **MQTT via libmosquitto.**  Caller-driven `alp_mqtt_loop`;
  subscription dispatch through libmosquitto's wildcard matcher.
  Gated on `pkg_check_modules(libmosquitto)`.
- **MQTT TLS (`mqtts://`).**  `apply_tls` hook routes through
  `mosquitto_tls_set` (OpenSSL underneath).  Optional
  `alp_mqtt_tls_config_t` for CA / cert / key paths + insecure
  flag; default port 8883.  Default CA path `/etc/ssl/certs`.

**Audio (Yocto):**

- **ALSA-backed `<alp/audio.h>`.**  `src/yocto/audio_yocto.c`
  binds `alp_audio_in_*` + `alp_audio_out_*` against ALSA's
  `snd_pcm_*`.  Device naming: `peripheral_id == 0` -> ALSA
  `"default"`; `peripheral_id == N` -> `"hw:N-1,0"`.  Software
  linear volume scale on S16_LE output.  Gated on
  `pkg_check_modules(alsa)`.

**Security (Yocto):**

- **OpenSSL-backed `<alp/security.h>`.**  `src/yocto/security_yocto.c`
  implements `alp_hash_*` (SHA-256 / 384 / 512), `alp_aead_*`
  (AES-128-GCM / AES-256-GCM / ChaCha20-Poly1305), and
  `alp_random_bytes`.  Tag-mismatch on decrypt mapped to
  `ALP_ERR_IO` per the header contract.  Key material wiped on
  close via `OPENSSL_cleanse`.  Gated on
  `pkg_check_modules(libssl libcrypto)`.

**Peripherals + IPC (Zephyr):**

- **LwRB UART RX ring buffer.**  `alp_uart_rx_ringbuf_*` API:
  interrupt-driven RX path stages bytes into a caller-supplied
  ring; consumer drains via `_pop()` without polling.  Gated on
  `CONFIG_ALP_SDK_UART_RX_RINGBUF`.  First in-tree LwRB consumer;
  backed by an in-tree stub impl until upstream
  `MaJerle/lwrb` flips on via the `extras-v04` group.
- **nanopb mproc IPC framing.**  Placeholder 12-byte envelope
  (magic / sequence / length) wrapping `alp_mbox_send` payloads
  under `CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING`.  Replaced by the
  nanopb-generated codec against `metadata/protos/alp_mproc.proto`
  when the `extras-v04` group lands upstream nanopb.

**Build-system + override scaffolding:**

- **Per-class `ALP_VENDOR_OVERRIDES_<CLASS>` macros** in
  `src/common/stub_backend.c` (I2C / SPI / GPIO / UART / MQTT /
  AUDIO_IN / AUDIO_OUT / SECURITY / UART_RX_RINGBUF), so each
  backend rolls out one class at a time.  Default stubs stamp
  `z_last_error = ALP_ERR_NOSUPPORT` for `alp_last_error()`
  diagnostics.
- **west.yml pins for v0.4 SDK-internal libs** (`MaJerle/lwrb@v3.2.0`
  + `nanopb/nanopb@nanopb-0.4.9`) behind a default-disabled
  `extras-v04` group, recorded for audit ahead of the real
  consumers.

**Secure boot + OTA scaffolding:**

- **MCUboot sysbuild profile for AEN-Zephyr.**
  `sysbuild/aen/sysbuild.conf` configures MCUboot + ECDSA-P256 +
  `swap-using-scratch`.  Dev-key generator at
  `keys/generate_dev_key.sh` (idempotent; `chmod 600`).  Full
  chain-of-trust + key lifecycle in `docs/secure-boot.md`.
  Live compile-verification gates on the authoritative
  `alp_e1m_evk_aen` board file landing at
  `alplabai/alp-zephyr-modules`.
- **Mender OTA opt-in on meta-alp.**
  `yocto/meta-alp/conf/distro/include/mender.inc` configures
  Mender's `mender-full` class with A/B rootfs + storage layout +
  server/tenant placeholders.  V2N / V2N-M1 / i.MX 93 machine
  configs gain `require` opt-in hook blocks.  Mender server
  ownership: see project memory + `docs/ota.md` -- the server
  itself is a separate-repo product owned outside alp-sdk.
- **Cross-cutting `docs/ota.md`** covering trust model + Yocto
  Mender flow + the AEN-Zephyr Mender vs Hawkbit client
  decision (pending v0.4-final).

### Still ahead for v0.4

- **Yocto:** `meta-alp` recipes actually building (currently
  parse-clean shells); image templates for vision/audio/IoT
  product classes; full V2N / V2N-M1 / i.MX 93 BSP fleshout.
- **IoT:** Wi-Fi station on Yocto (NetworkManager / wpa_supplicant
  glue); HTTP/HTTPS client; time-series buffering helpers.
- **Camera:** `alp_camera_v4l2` wrapper, GStreamer pipeline helpers.
- **Signal:** ARM Compute Library bindings.
- **Secure boot:** authoritative `alp_e1m_evk_aen` board file
  (separate repo) + production OPTIGA Trust M provisioning flow.
- **Secure OTA on AEN-Zephyr:** Mender Zephyr client vs Hawkbit
  decision; image-write hook onto MCUboot secondary slot.
- **Device identity:** OPTIGA Trust M pre-provisioned ECC key pair
  surfaced via `<alp/security.h>` for TLS client certs.
- **DEEPX DX-M1 real link:** `dxnn_*` link in
  `src/yocto/inference_deepx.cpp` (pending DEEPX SDK provenance
  decision).
- **Ethos-U65 real attach on i.MX 93:** Vela toolchain integration.

See [`docs/secure-boot.md`](docs/secure-boot.md) and
[`docs/ota.md`](docs/ota.md) for the full secure boot / OTA
chain-of-trust design.

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
