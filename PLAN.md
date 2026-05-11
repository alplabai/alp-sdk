# ALP SDK — Product + Engineering Plan

Last revised: 2026-05-11 (afternoon refresh).

This plan is the bridge between the canonical product slides
(`ALP Lab — E1M™ Software Stack`, `Product Overview: Software`)
and the per-version engineering deliverables in
[`VERSIONS.md`](VERSIONS.md).  It re-states the architecture in
the slides' own terms, maps each marketing pillar to concrete
shipped artefacts, and identifies the gaps still to close.

> **Scope.**  This document defines *what* the SDK is and *which
> deliverables prove it*.  Day-to-day execution (sprint
> ordering, who-does-what) lives in `VERSIONS.md` and the GitHub
> issue tracker.  Hardware-specific values (pin tables,
> per-board sensor addresses) wait on the user-supplied exact HW
> configurations — see the project memory note "pending exact
> hardware configurations."

---

## 1. The canonical stack

Lifted from `ALP Lab — E1M™ Software Stack` slide:

```
┌─────────────────────────────────────────────────────────────────────┐
│                         AI Framework                                │
│                      TensorFlow · PyTorch                           │
├─────────────────────────────────────────────────────────────────────┤
│                              OS                                     │
│              Bare Metal · Yocto Linux · Zephyr RTOS                 │
├─────────────────────────────────────────────────────────────────────┤
│                            ALP SDK                                  │
│   Libraries:  GUI/LVGL · Display · Camera · Math · IoT ·            │
│              Peripherals · Signal Processing                        │
│   Vendor wrappers + Chip drivers + Chip metadata                    │
│                                                       ARM CMSIS     │
├─────────────────────────────────────────────────────────────────────┤
│                          Vendor SDK                                 │
│            (Alif HAL · Renesas FSP · NXP MCUXpresso · …)            │
├─────────────────────────────────────────────────────────────────────┤
│                            HW & HAL                                 │
│              SoM Hardware (E1M-AEN, E1M-X-V2N, E1M-i.MX93, …)       │
└─────────────────────────────────────────────────────────────────────┘
```

Key shape points the slide makes explicit:

1. **AI framework above the SDK.**  TensorFlow and PyTorch are
   first-class consumers — the SDK ships compiled-model loaders +
   inference dispatch, not just peripherals.
2. **CMSIS sibling, not parent.**  CMSIS sits *next to* the ALP
   SDK at the same architectural layer; both consume the vendor
   HAL below.  Apps can call into CMSIS directly when needed —
   the SDK doesn't gatekeep.
3. **Three application classes.**  Generic Application, Edge-AI
   Application, IoT Application — the example tree mirrors this
   split: `examples/edgeai-vision-aen/` (edge AI),
   `examples/iot-connected-camera/` (IoT), and the per-peripheral
   reference apps under `examples/<peripheral>-<demo>/`
   (generic).

### 1.1 Two consumer paths (codified during v0.2)

The SDK is **dual-consumer** by design — both paths are
first-class:

- **alp-studio codegen.**  The visual programmer reads block
  manifests, runs the pin allocator, and emits C that calls
  `<alp/...>` headers.  Pin-allocation correctness comes for
  free.
- **Standalone / hand-written firmware.**  A developer writes a
  Zephyr/Yocto/baremetal app directly against the headers,
  picking instance IDs by hand from `<alp/e1m_pinout.h>`
  (`ALP_E1M_I2C0`, `ALP_E1M_PWM3`, …).  No studio in the loop.

The standalone path is **not** a studio escape hatch.  Anything
the studio can emit, a developer should be able to write by
hand.  See [`docs/adr/0001-wrapper-on-top-of-zephyr.md`](docs/adr/0001-wrapper-on-top-of-zephyr.md)
for the rationale and [`examples/<peripheral>-<demo>/`](examples/)
for hand-written reference apps covering every wrapped
peripheral class.

---

## 2. The four pillars (and their deliverables)

The product slide names four pillars.  Each maps to a concrete
shipped artefact in this repo.

### 2.1 Unified Software Stack

> *Facilitates smoother transitions and significantly minimises
> compatibility issues.*

The same `<alp/...>` headers compile and run unchanged across
every E1M-conformant SoM.  An app written for E1M-AEN701 builds
for E1M-V2N101 by switching the OS backend; for E1M-i.MX93 by
switching to the Yocto path.

| Deliverable                                       | Status      |
|---------------------------------------------------|-------------|
| Public surface frozen for v0.1                    | ✅ shipped — `include/alp/{peripheral,display,camera,gui,math,signal,iot}.h` |
| **v0.2 peripheral expansion (12 wrapped classes)** | ✅ shipped — `pwm.h`, `adc.h`, `counter.h`, `i2s.h`, `can.h`, `rtc.h`, `wdt.h` added on top of v0.1.  See [ADR 0003](docs/adr/0003-peripheral-coverage.md). |
| **v0.2 capability validation**                    | ✅ shipped — `alp_last_error()` thread-local + generated `<alp/soc_caps.h>` reject configs that exceed the active SoC's documented hardware caps.  See [ADR 0002](docs/adr/0002-error-mechanism.md). |
| **E1M portability bound**                         | ✅ shipped — `ALP_E1M_<CLASS>_COUNT` macros document the cross-SoM portable instance count per class.  See [ADR 0004](docs/adr/0004-e1m-portability-bound.md). |
| **v0.2/v0.3 surface declared early**              | ✅ shipped — `audio.h`, `ble.h`, `security.h`, `mproc.h` ship as compile-clean stubs returning `ALP_ERR_NOSUPPORT`.  Apps can compile against the full v1.0-shape surface today. |
| **Per-peripheral hand-written examples**          | ✅ shipped — 11 reference apps under `examples/<peripheral>-<demo>/`, one per wrapped peripheral. |
| OS pivot: `src/{zephyr,baremetal,yocto}/`         | 🟡 Zephyr code complete (HIL pending); baremetal stubs; **yocto core-4 peripherals code complete, untested** (I²C / SPI / UART / GPIO via i2c-dev / spidev / termios / gpiochip v2 — see [§6 entries 18-21](#6-open-work--explicit-gaps)).  Failure-path ctest in `pr-plain-cmake`; real-hardware roundtrip parked behind the `hil-yocto` runner per [`docs/test-plan.md`](docs/test-plan.md).  Audio / camera / display still stubs (v0.4.x). |
| Cross-SoM portability proof                       | 🟡 single-OS proof per SoM; cross-OS proof v0.2+ |
| ABI snapshot tooling                              | ✅ shipped — `scripts/abi_snapshot.py` + `docs/abi/v0.1-snapshot.json`.  CI gate via `pr-generated-files.yml` post-1.0. |
| ABI freeze + deprecation policy                   | 🔮 v1.0 |

### 2.2 CMSIS Integration

> *Facilitates smooth software integration among various vendors.*

ARM CMSIS is the lowest-common abstraction across ARM-based
silicon.  The SDK never re-implements CMSIS — it re-exports
CMSIS-DSP, links against CMSIS-Driver where vendor HALs expose
it, and trusts CMSIS-Core for atomics / barriers / cache ops.

| Deliverable                                       | Status      |
|---------------------------------------------------|-------------|
| Direct CMSIS-DSP usage from app code (no ALP wrapper) | ✅ doc-stance — `<alp/math.h>` / `<alp/signal.h>` removed; users `#include "arm_math.h"` directly. |
| ALP-internal CMSIS-DSP option (`ALP_HAS_CMSIS_DSP`) | ✅ shipped — SDK internals (audio etc.) opt in. |
| Per-SoC validated CMSIS-DSP feature groups         | ✅ shipped — see `docs/os-support-matrix.md` |
| CMSIS-Driver alignment for plain-CMake bare-metal | 🔮 v0.2 |

### 2.3 Edge AI Frameworks

> *Enables efficient Edge AI deployment, letting developers
> create solutions with ease.*

Bring a TensorFlow Lite or PyTorch model, run it on whichever
NPU the active SoM exposes — Ethos-U55 (E1M-AEN) / Ethos-U65
(E1M-i.MX93) / DRP-AI3 (E1M-V2N) / DEEPX DX-M1 (E1M-V2N-M1).

| Deliverable                                       | Status      |
|---------------------------------------------------|-------------|
| Chip metadata for every supported NPU              | ✅ shipped (Ethos-U55 in e3/e7/e8, Ethos-U85 in e8, Ethos-U65 in imx93, DRP-AI in n44, DEEPX in m1) |
| `examples/edgeai-vision-aen/` skeleton             | ✅ shipped |
| `<alp/inference.h>` unified surface — model load + invoke + bind | 🔮 v0.3 (planned) |
| Vela (Ethos-U) toolchain integration               | 🔮 v0.2 |
| DRP-AI translator integration (Renesas)            | 🔮 v0.2 |
| ExecuTorch / TFLite-Micro back-ends                | 🔮 v0.3 |
| DEEPX SDK integration (DX-M1)                      | 🟡 Yocto-side dispatcher + vendor stub shipped — real `dxnn_*` link lands v0.4 |
| Ethos-U65 on i.MX 93                               | 🟡 Kconfig + per-variant config file shipped; same TFLM path as Ethos-U55.  Real attach + Vela toolchain land v0.4 |

The `<alp/inference.h>` API has not been declared yet because
the four NPU vendor toolchains have meaningfully different
abstractions — locking the API before exercising all four is
how we'd ship a header that needs to break in v0.4.  v0.2 ships
*one* working path (Ethos-U55 on AEN) and the API is reverse-
engineered from that experience.

### 2.4 IoT-Ready Software

> *Ensures secure connectivity and real-time monitoring for IoT
> applications.*

Wi-Fi 6 + BLE 5.4 are *on every E1M-conformant SoM* per the
platform invariant in `e1m-spec`.  The SDK exposes them through
`<alp/iot.h>` so app code is portable across vendor radio
implementations (Murata + Infineon on V2N; TI CC3501E on AEN).

| Deliverable                                       | Status      |
|---------------------------------------------------|-------------|
| `<alp/iot.h>` Wi-Fi-station + MQTT API frozen     | ✅ shipped (header + stub) |
| `examples/iot-connected-camera/` skeleton          | ✅ shipped |
| Real Wi-Fi-station + MQTT on AEN-Zephyr            | 🔮 v0.2 |
| TLS via MbedTLS                                    | 🔮 v0.3 (`<alp/security.h>`) |
| BLE peripheral + central                           | 🔮 v0.3 (`<alp/ble.h>`, Zephyr `bt` host stack) |
| Provisioning helpers (`alp_iot_wifi_provision`)    | 🔮 v0.3.x |
| Yocto IoT stack (Mosquitto, Paho, OTA)             | 🟡 **MQTT via libmosquitto code complete, untested** (`src/yocto/iot_yocto.c`, gated on `pkg_check_modules(libmosquitto)`); broker roundtrip parked behind the `hil-yocto` runner -- see [`docs/test-plan.md`](docs/test-plan.md).  Paho not pursued -- libmosquitto covers the same surface.  OTA (Mender) still v0.4.x. |
| **Secure boot** (MCUboot + signed images on AEN)   | 🔮 v0.4 |
| **Secure OTA** (signed update channel + rollback)  | 🔮 v0.4 |
| OPTIGA Trust M-rooted device identity              | 🔮 v0.4 (paired with secure boot) |

### 2.4.1 Secure boot + secure OTA

Production E1M deployments require chain-of-trust from immutable
ROM through to the application.  The SDK lands this in v0.4 on
two prongs:

1. **Secure boot.**  MCUboot (already pulled in via `west.yml`)
   verifies the application image's signature at every boot.
   Signing keys live in OPTIGA Trust M's secure NVM (per the
   chip metadata in `metadata/e1m_modules/aen/`); MCUboot's
   verification path goes through MbedTLS PSA, which routes to
   the OPTIGA HW accelerator transparently once the v0.3.x PSA
   driver lands.  Failed verification triggers a rollback to
   the prior known-good image.

2. **Secure OTA.**  The same key pair signs OTA payloads.  The
   delivery channel is platform-specific:
   - **Zephyr-on-AEN:** Mender / SWUpdate via the v0.3-shipped
     `<alp/iot.h>` MQTT/HTTP client; image swap at next reboot
     via MCUboot's `swap-using-scratch` mode.
   - **Yocto-on-V2N / i.MX 93 (v0.4 first-class):** Mender as
     the reference OTA agent; the meta-alp recipes integrate
     `meta-mender`.

The OPTIGA Trust M's TRNG and ECC primitives are surfaced via
`<alp/security.h>` -- apps that opt in to secure boot get the
same crypto surface they use for TLS, no separate API.  See
[`docs/cc3501e-bridge.md`](cc3501e-bridge.md) for the Wi-Fi
bridge's role in OTA delivery on AEN.

---

## 3. SoM coverage matrix

This is what's *currently in the SDK*, not what the product
roadmap aspires to.  Living matrix — refreshed each release.

| SoM family          | Form factor | Default Alif/Renesas/NXP part | Metadata `ref`             | Vendor wrapper status      | Reference EVK       |
|---------------------|-------------|-------------------------------|----------------------------|----------------------------|---------------------|
| E1M-AEN             | E1M 35×35   | Alif `AE722F80F55D5LS` (E7)   | `alif:ensemble:e7` (+e3..e8)| Active — v0.1 Zephyr full | E1M EVK             |
| E1M-X V2N           | E1M-X 45×65 | Renesas `R9A09G056N44GBG#AC0` | `renesas:rzv2n:n44`        | Stub — v0.2 target         | E1M-X EVK           |
| E1M-X V2N-M1        | E1M-X 45×65 | RZ/V2N + DEEPX `DX-M1`        | `renesas:rzv2n:n44` + `deepx:dx:m1` | Stub — v0.3 target | E1M-X EVK           |
| E1M-i.MX93          | E1M 35×35   | NXP `i.MX 93` (variant TBD)   | `nxp:imx9:imx93`           | Stub — v0.4 target         | E1M EVK             |

Adding a new SoM family is a sequence of concrete artefacts —
see [`docs/porting-new-som.md`](docs/porting-new-som.md).

---

## 4. AI Framework integration plan

The product slide puts TensorFlow + PyTorch above the SDK.
Here's the path to making that real.

### 4.1 TensorFlow Lite Micro (v0.2)

Target: any model that fits in the SoM's memory budget compiles
through Vela + lands in `examples/edgeai-vision-aen/models/`.

- Bring TensorFlow Lite Micro into the Zephyr workspace as a
  module (already supported upstream).
- Vela binary cached in CI; per-PR rebuild only on model files
  changing.
- `alp_inference_tflm_*` C wrappers around TFLM's `MicroInterpreter`,
  with the Ethos-U operator resolver wired in for AEN and i.MX93.
- The wrappers feed `<alp/inference.h>` (declared v0.3 once the
  shape is real).

### 4.2 PyTorch (v0.3)

PyTorch on edge silicon means **ExecuTorch** (formerly TorchEdge).
ExecuTorch programs compile to flatbuffers that run on a target
backend — Ethos-U via the Arm ExecuTorch backend, Renesas DRP-AI
via the Renesas backend (in development upstream), DEEPX via the
DEEPX SDK (proprietary, requires a separate adapter).

- v0.3 ships `alp_inference_executorch_*` for the AEN family
  first (Arm ExecuTorch + Ethos-U backend is the most mature).
- V2N + DX-M1 follow as their backends stabilise.

### 4.3 Vendor-specific ML SDKs

The unification is best-effort, not absolute.  When a vendor SDK
is genuinely better than TFLM/ExecuTorch for a target NPU, the
SDK exposes it under a vendor-named header in addition to the
unified one:

- `<alp/vendors/alif/ethosu.h>` — direct Ethos-U driver (already
  shippable in v0.2 alongside the Zephyr ARM Ethos-U module).
- `<alp/vendors/renesas/drpai.h>` — DRP-AI direct dispatch.
- `<alp/vendors/deepx/dxm1.h>` — DEEPX SDK adapter.

These are escape hatches, not the recommended path.

---

## 5. Versioned roadmap → pillar mapping

A quick cross-reference so each pillar's progress is visible per
release.  Bold = landed during this revision pass; the v0.2
column captures the larger-than-originally-planned surface that
shipped early in 2026-Q2.

| Version | Unified Stack | CMSIS | Edge AI | IoT |
|---------|---------------|-------|---------|-----|
| **v0.1** | Public surface frozen, AEN-Zephyr full | DSP re-export | Skeleton + chip metadata for all NPUs | Skeleton + header surface |
| **v0.2** | **12 peripheral classes wrapped (was 4) + capability validation + E1M portability bound + per-peripheral hand-written examples + ADRs**.  AEN-baremetal + V2N intro. | CMSIS-Driver alignment | Vela + TFLM on AEN, EdgeAI app real | Real Wi-Fi-station + MQTT on AEN-Zephyr |
| **v0.3** | V2N-Zephyr + AEN-Yocto stubs, M1 intro.  Real impl behind v0.2-declared `<alp/audio.h>` / `<alp/ble.h>` / `<alp/security.h>` / `<alp/mproc.h>` surfaces.  **+ `board.yaml` project config** (one YAML per project; SoM SKU + carrier + libraries + features), with loader emitting Kconfig / CMake / Yocto natives. | (held) | `<alp/inference.h>` unified, ExecuTorch on AEN.  DEEPX DX-M1 + Ethos-U65/i.MX 93 dispatchers wired (real link v0.4). | TLS + BLE + provisioning |
| **v0.4** | **Yocto core-4 peripheral wrappers + GPIO IRQ + ALSA audio + OpenSSL security all code-complete; failure-path CI green, real-target HIL pending**.  AEN-Zephyr LwRB UART RX + nanopb mproc framing both compile-verified.  Full V2N + i.MX 93 Yocto BSP bring-up still ahead.  HIL verification gates the v0.4 tag — see [`docs/test-plan.md`](docs/test-plan.md). | (held) | DRP-AI + Ethos-U65 backends | **MQTT (cleartext + TLS) via libmosquitto, OpenSSL crypto, ALSA audio all code-complete**; MCUboot AEN scaffolding + meta-alp Mender opt-in landed; OTA-server side is a separate-repo product owned outside alp-sdk. |
| **v1.0** | ABI freeze across the matrix (snapshot tooling shipped v0.1) | LTS-aligned | Full multi-vendor inference unification | Production IoT stack |

---

## 6. Open work / explicit gaps

Items the slides imply.  Items the v0.2 SDK *did* close are kept in
the list with strikethrough so the audit trail stays clear; items
that remain open take priority for the upcoming releases.

1. ~~**`<alp/inference.h>`** — unified ML inference API.~~
   **Surface closed:** declared with backend selector
   (AUTO/CPU/Ethos-U/DRP-AI/DEEPX), model-format selector
   (TFLite/Vela/DRP-AI/DXNN/ExecuTorch), tensor descriptor with
   shape + dtype + quant params, and arena-handle config.  Real
   impls arrive per the per-backend roadmap (Ethos-U + CPU
   v0.2 on AEN-Zephyr; DRP-AI v0.3; DEEPX v0.4; i.MX 93 Ethos-U
   v0.4).  The "wait for v0.2 to exercise the API" reasoning
   was overtaken by enough cross-backend reading to confirm the
   shape converges -- vendor-specific escape hatches at
   `<alp/vendors/.../>` remain available where it doesn't.
2. ~~**`<alp/audio.h>`** — declared in `VERSIONS.md` for v0.2 but no
   header file yet.~~  **Closed v0.2:** header surface lands with
   PDM input + I²S output + per-block read/write API; impl arrives
   alongside the `<alp/i2s.h>` real path.
3. ~~**`<alp/ble.h>`** — declared for v0.3.~~  **Surface closed:**
   peripheral + central + GATT shape declared; Zephyr `bt` host
   impl pending v0.3.
4. ~~**`<alp/security.h>`** — declared for v0.3.~~  **Surface closed:**
   hash + AEAD + TRNG surface declared; MbedTLS impl + per-SoC HW
   accelerator routing pending v0.3.
5. ~~**`<alp/mproc.h>`** — declared in `VERSIONS.md` for v0.3.~~
   **Surface closed:** shared-memory + mailbox + hwsem primitives
   declared; OpenAMP RPC + dual-core orchestration pending v0.3.
6. **i.MX 93 datasheet ingest** — peripheral counts and orderable
   variants are TBD.  Lands when the user supplies the exact HW
   config (per the project memory note).
7. **Alif E4 / E5 / E6 datasheets** — the three preliminary stubs
   in `metadata/socs/alif/ensemble/` get upgraded to released when
   Alif publishes those datasheets.
8. **Authoritative Zephyr board files** — `alp_e1m_evk_aen` and
   `alp_e1m_evk_v2n` board files live in
   [`alplabai/alp-zephyr-modules`](https://github.com/alplabai/alp-zephyr-modules)
   (TBD).  Three twister entries are parked behind comments until
   those land.
9. **HW-in-loop CI** — `.github/workflows/nightly-aen-hil.yml` is a
   skeleton; needs a self-hosted runner with the `hil-aen` label.
   Setup contract documented at `ci/HW-IN-LOOP.md`.

### Closed in v0.2 (additional gaps the slides didn't anticipate)

10. ~~**Peripheral coverage at 4 of 12.**~~  v0.2 wrappers PWM,
    ADC, Counter, QEnc, I²S, CAN-FD, RTC, Watchdog brought the
    SDK to 12 wrapped peripheral classes.  See
    [ADR 0003](docs/adr/0003-peripheral-coverage.md).
11. ~~**No error mechanism for SoC-capability mismatches.**~~
    `alp_last_error()` thread-local + `<alp/soc_caps.h>` generated
    from `metadata/socs/**.json` reject configs that exceed the
    active SoC's documented hardware caps.  See
    [ADR 0002](docs/adr/0002-error-mechanism.md).
12. ~~**No documented portability bound.**~~ `ALP_E1M_<CLASS>_COUNT`
    macros in `<alp/e1m_pinout.h>` enumerate the e1m-spec's
    minimum routed instance counts.  See
    [ADR 0004](docs/adr/0004-e1m-portability-bound.md).

### Closed in v0.3 (additional gaps the slides didn't anticipate)

13. ~~**Configuration sprawl across three formats.**~~  Pre-v0.3
    a consumer had to track `prj.conf` (Zephyr Kconfig), cmake
    `-D` flags (plain CMake), and `local.conf` (Yocto MACHINE +
    image install) separately to declare a single firmware
    target.  v0.3 introduces **`board.yaml`** -- one declarative
    per-project file that picks the SoM SKU (MPN), per-component
    carrier population, OS backend, inference backend, optional
    libraries, and connectivity features.  The
    `scripts/alp_project.py` loader compiles it down to the
    per-backend native config (Kconfig fragment / -D args /
    local.conf snippet).  Schema:
    [`metadata/schemas/board-config-v1.schema.json`](metadata/schemas/board-config-v1.schema.json).
    Design + reference: [`docs/board-config.md`](docs/board-config.md).
14. ~~**SoM vs carrier conflation.**~~ Initial scaffolding mixed
    on-module components (Alif silicon + CC3501E + OPTIGA +
    RV-3028 + TMP112 + 24C128) with carrier-board components
    (LSM6DSO + BMI323 + BMP581 + OLEDs + cameras + speaker amps).
    The `board.yaml` schema's `som` block now covers SoM-fab-time
    fixed parts; the new `carrier` block covers per-board
    population.  Two stock carrier presets ship at
    `metadata/carriers/E1M-EVK/board.yaml` and
    `metadata/carriers/E1M-X-EVK/board.yaml`, explicitly
    positioned as **reference designs** that customer carriers
    fork.  Worked customer-fork example at
    `metadata/carriers/custom-example/board.yaml`.
15. ~~**No CI gate on the project-config schema.**~~ The
    `pr-metadata-validate` workflow now smoke-tests
    `scripts/alp_project.py` against `metadata/templates/board.yaml.example`
    on every PR -- catches schema / loader regressions before
    they ship.
16. ~~**Only two SKU presets shipped.**~~ The SDK now ships a
    `som.yaml` preset for **every released MPN**:
    `metadata/e1m_modules/<MPN>/som.yaml` exists for
    AEN301/401/501/601/701/801, V2N101/V2N102, V2M101/V2M102,
    and the NXP NX9101 placeholder.  Customer `board.yaml` is
    typically three lines: `som.sku: <MPN>`, `carrier.name:
    E1M-EVK`, `os: zephyr` -- the rest flows from the preset.
    Per the project memory note "pending exact hardware
    configurations", memory capacities + datasheet-specific
    fields stay TBD in each preset until the user supplies
    authoritative HW config writeups.
17. ~~**Library wrappers were a chaos risk.**~~ Settled the
    design: SDK does NOT ship `<alp/...>` wrappers around
    upstream libraries (LVGL, ETLCPP, fmt, nlohmann/json,
    MbedTLS, CMSIS-DSP, LittleFS, doctest).  Apps use the
    upstream native API; the SDK ships compile-time profile
    headers under `metadata/library-profiles/<lib>/` (named to
    match each library's expected config filename:
    `lv_conf.h`, `etl_profile.h`, `mbedtls_config.h`, ...)
    so the library works under the SDK's invariants
    (no exceptions, no `<iostream>`, no STL on M-class).
    SDK-internal libraries (LwRB, nanopb) don't appear in the
    user-facing `libraries:` enum -- they're pulled in
    unconditionally when their consumer SDK code is built.

### Code-complete in v0.4-prep (untested — full v0.4 tag still ahead)

> These items are merged on `main` and pass `pr-plain-cmake`'s
> failure-path ctest, but no row in [`docs/test-plan.md`](docs/test-plan.md)
> has flipped to ✅ verified yet -- they all gate on the
> `hil-yocto` self-hosted runner (parked per
> [`ci/HW-IN-LOOP.md`](../ci/HW-IN-LOOP.md)).

18. ~~**Yocto peripheral surface was 100 % stubs.**~~  The four
    core peripheral classes are now coded against the Linux UAPI
    (verification still pending):
    - **I²C** (`src/yocto/peripheral_i2c.c`) binds `alp_i2c_*` to
      i2c-dev (`/dev/i2c-N`), using `I2C_RDWR` ioctl for
      repeated-start write-then-read.
    - **SPI** (`src/yocto/peripheral_spi.c`) binds `alp_spi_*` to
      spidev (`/dev/spidev<bus>.<cs>`), with `SPI_IOC_MESSAGE`
      for full-duplex transceive.
    - **UART** (`src/yocto/peripheral_uart.c`) binds `alp_uart_*`
      to termios (`/dev/ttyS|AMA|USB`), with per-call `VTIME`
      timeouts.
    - **GPIO** (`src/yocto/peripheral_gpio.c`) binds `alp_gpio_*`
      to the GPIO character-device v2 ABI at `/dev/gpiochipN`
      (no libgpiod dependency), with pin-id packed as
      `(chip << 16) | line`.
    Per-class `ALP_VENDOR_OVERRIDES_<CLASS>` macros in
    `src/common/stub_backend.c` let each class roll out
    independently.  Failure-path ctest coverage gated in
    `pr-plain-cmake.yml`; real-adapter HIL coverage parked
    behind `ci/HW-IN-LOOP.md`.
19. ~~**No GPIO interrupt support on Linux.**~~  A shared pthread
    dispatcher runs a `poll()` loop across every pin with IRQ
    enabled; an eventfd wakes it on mutator events
    (irq_enable / irq_disable / close).  Reconfigures the line
    via `GPIO_V2_LINE_SET_CONFIG_IOCTL` with rising / falling /
    both edge flags.  Dispatcher starts lazily on the first
    `alp_gpio_irq_enable`.  Callback contract documented at the
    function header (no nested SDK calls, deadlock otherwise).
20. ~~**No real MQTT on Linux.**~~  `src/yocto/iot_yocto.c` binds
    `alp_mqtt_*` against libmosquitto.  Caller-driven
    `alp_mqtt_loop`; subscription dispatch through `on_message`
    + `mosquitto_topic_matches_sub` so MQTT `+` / `#` wildcards
    work without us re-implementing the matcher.  URI parser
    supports `mqtt://host[:port]`; `mqtts://` returns
    `ALP_ERR_NOSUPPORT` until the secure-stack work lands.
    Build gated on `pkg_check_modules(libmosquitto)`;
    workspaces without libmosquitto-dev fall back cleanly to
    the NOSUPPORT stubs.  Per-class `ALP_VENDOR_OVERRIDES_MQTT`
    gate.  `alp_wifi_*` stays stubbed on Linux (Wi-Fi bring-up
    is a wpa_supplicant / NetworkManager concern, not SDK-side).
21. ~~**Per-peripheral override granularity.**~~  The earlier
    `ALP_VENDOR_OVERRIDES_PERIPHERAL` umbrella forced a backend
    to provide all four peripheral classes (I²C, SPI, GPIO,
    UART) at once or none.  Split into per-class
    `ALP_VENDOR_OVERRIDES_{I2C,SPI,GPIO,UART,MQTT}` macros with
    the umbrella preserved -- existing `vendors/alif/` builds
    unchanged, but Yocto-style incremental rollout now works.
22. ~~**west.yml carried no pins for v0.4 SDK-internal libs.**~~
    `MaJerle/lwrb@v3.2.0` + `nanopb/nanopb@nanopb-0.4.9` now
    pinned behind a `extras-v04` group disabled by default in
    the manifest's `group-filter`, so v0.3 workspaces stay
    untouched.  Flipping the group on (`west update
    --group-filter +extras-v04`) makes the upstream sources
    win the include search ahead of the stubs at
    `vendors/{lwrb,nanopb}/include/`.
23. ~~**Coverity workflow was a stub.**~~ `.github/workflows/coverity.yml`
    now switches to POST-with-form-encoded body (the GET
    variant Coverity rejected with exit-6 auth-failure),
    sources the project name from a `COVERITY_PROJECT` Actions
    variable, checks the downloaded tarball's gzip magic
    bytes, and echoes the response body on non-2xx.  Secrets
    (`COVERITY_TOKEN`, `COVERITY_EMAIL`) provisioned; first
    submission to <https://scan.coverity.com/projects/alplabai-alp-sdk>
    completed successfully on 2026-05-11.
24. ~~**LwRB pinned but never consumed.**~~  First in-tree LwRB
    consumer landed: opt-in `alp_uart_rx_ringbuf_*` helper in
    `src/zephyr/peripheral_uart.c` gated on
    `CONFIG_ALP_SDK_UART_RX_RINGBUF`.  Interrupt-driven RX path
    drains the UART FIFO into a caller-supplied ring; consumer
    pops batched bytes without polling.  Backed by an in-tree
    LwRB stub impl at `vendors/lwrb/src/lwrb_stub_impl.c` (~140
    LoC, canonical empty/full disambiguation) until the
    `extras-v04` west group flips upstream `MaJerle/lwrb` in.
    Reference app at `examples/uart-rx-ringbuf/`.
25. ~~**nanopb pinned but never consumed.**~~  First in-tree
    nanopb consumer landed: optional 12-byte placeholder IPC
    envelope wrapping `alp_mbox_send` payloads under
    `CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING`.  Replaced wholesale
    by the nanopb-generated codec against
    `metadata/protos/alp_mproc.proto` once the `extras-v04`
    group lands upstream nanopb.  Encode/decode helpers at
    `src/common/proto/alp_mproc_frame.{h,c}` cover the same
    call sites the generator will own in v0.4-final.
26. ~~**MCUboot for AEN was a `west.yml` import with no
    sysbuild integration.**~~  `sysbuild/aen/sysbuild.conf`
    configures MCUboot + ECDSA-P256 + `swap-using-scratch`;
    `keys/generate_dev_key.sh` wraps `imgtool` for local dev-key
    generation; `keys/.gitignore` keeps private keys out of git;
    `docs/secure-boot.md` documents the full chain of trust
    (Alif Secure Enclave ROM -> first-stage -> MCUboot ->
    application), the production OPTIGA Trust M lifecycle, the
    `swap-using-scratch` failure-mode matrix, and the multi-key
    rotation playbook.  Live compile-verification gates on the
    authoritative `alp_e1m_evk_aen` board file (gap #8).
27. ~~**Mender OTA was undeclared on the Yocto side.**~~
    Opt-in distro include at
    `yocto/meta-alp/conf/distro/include/mender.inc` configures
    `mender-full` with A/B rootfs + 256 MiB-per-slot default
    layout + storage / server / tenant placeholders.  V2N /
    V2N-M1 / i.MX 93 machine configs gain commented opt-in hook
    blocks (consumers uncomment `require` to enable).
    `meta-mender-core` added to `LAYERRECOMMENDS_alp`.
    Cross-cutting `docs/ota.md` covers the trust model + Yocto
    Mender flow + the AEN-Zephyr Mender vs Hawkbit decision
    pending v0.4-final.  OTA-server side is owned by a separate
    repo, per the alp-ota-server product split.
28. ~~**Yocto MQTT was cleartext-only.**~~  `apply_tls` hook
    routes `mqtts://` URIs through `mosquitto_tls_set` (OpenSSL
    underneath).  New `alp_mqtt_tls_config_t` in `<alp/iot.h>`
    carries optional CA / cert / key paths + insecure flag;
    default port 8883; default CA path `/etc/ssl/certs`.  TLS
    config errors surface at `alp_mqtt_open()` time rather than
    later at connect.
29. ~~**`<alp/audio.h>` had no Yocto backend.**~~
    `src/yocto/audio_yocto.c` binds the full
    `alp_audio_in_*` + `alp_audio_out_*` surface against
    ALSA's `snd_pcm_*`.  Device naming maps `peripheral_id` to
    `"default"` / `"hw:N-1,0"`; software linear volume scale on
    S16_LE output; pkg_check_modules-gated.  Eleven failure-path
    tests at `tests/yocto/audio_alsa.c`.
30. ~~**`<alp/security.h>` had no Yocto backend.**~~
    `src/yocto/security_yocto.c` implements `alp_hash_*`
    (SHA-256/384/512), `alp_aead_*` (AES-128/256-GCM + ChaCha20-
    Poly1305), `alp_random_bytes` on OpenSSL's `EVP_*`.
    Tag-mismatch on decrypt maps to `ALP_ERR_IO` per the header
    contract; key material wiped on close via `OPENSSL_cleanse`.
    Sixteen tests at `tests/yocto/security_openssl.c` including
    a SHA-256 NIST KAT against the `"abc"` vector and full
    AEAD round-trip with tag-mismatch detection.

---

## 7. What this plan is not

- **Not a marketing document.**  No promises about ship dates or
  pricing.  Those live with the product team's collateral, not in
  the engineering repo.
- **Not a substitute for `VERSIONS.md`.**  This plan describes the
  product shape; `VERSIONS.md` describes which acceptance bar each
  release has to clear.  When they conflict, `VERSIONS.md` wins —
  it's the contract CI gates against.
- **Not a guarantee.**  Pre-1.0 the API can change between minors.
  The `<alp/...>` surface stabilises at v1.0 and the deprecation
  policy in `docs/contribution.md` kicks in then.

---

## 8. How to update this plan

- Bump the "Last revised" date at the top.
- Edit Section 5 (the version × pillar table) when a release
  ships.
- Add to Section 6 when a real gap surfaces during development.
- Don't restate `VERSIONS.md` here — link to it.
