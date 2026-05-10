# ALP SDK — Product + Engineering Plan

Last revised: 2026-05-10.

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
   `examples/iot-connected-camera/` (IoT), and the v0.2/v0.3
   simpler app templates (generic).

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
| OS pivot: `src/{zephyr,baremetal,yocto}/`         | 🟡 Zephyr full; baremetal stubs; yocto stubs (v0.4) |
| Cross-SoM portability proof                       | 🟡 single-OS proof per SoM; cross-OS proof v0.2+ |
| ABI freeze + deprecation policy                   | 🔮 v1.0 |

### 2.2 CMSIS Integration

> *Facilitates smooth software integration among various vendors.*

ARM CMSIS is the lowest-common abstraction across ARM-based
silicon.  The SDK never re-implements CMSIS — it re-exports
CMSIS-DSP, links against CMSIS-Driver where vendor HALs expose
it, and trusts CMSIS-Core for atomics / barriers / cache ops.

| Deliverable                                       | Status      |
|---------------------------------------------------|-------------|
| `<alp/math.h>` re-exports CMSIS-DSP               | ✅ shipped (re-export header + Kconfig toggle) |
| `<alp/signal.h>` forward marker for filter wrappers | ✅ shipped (header) |
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
| DEEPX SDK integration (DX-M1)                      | 🔮 v0.3 |

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
| Yocto IoT stack (Mosquitto, Paho, OTA)             | 🔮 v0.4 |

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
release:

| Version | Unified Stack | CMSIS | Edge AI | IoT |
|---------|---------------|-------|---------|-----|
| **v0.1** | Public surface frozen, AEN-Zephyr full | DSP re-export | Skeleton + chip metadata for all NPUs | Skeleton + header surface |
| **v0.2** | AEN-baremetal + V2N intro | CMSIS-Driver alignment | Vela + TFLM on AEN, EdgeAI app real | Real Wi-Fi-station + MQTT on AEN-Zephyr |
| **v0.3** | V2N-Zephyr + AEN-Yocto stubs, M1 intro | (held) | `<alp/inference.h>` unified, ExecuTorch on AEN | TLS + BLE + provisioning |
| **v0.4** | Yocto first-class on V2N + i.MX93 | (held) | DRP-AI + Ethos-U65 backends | Mosquitto + Paho + OTA |
| **v1.0** | ABI freeze across the matrix | LTS-aligned | Full multi-vendor inference unification | Production IoT stack |

---

## 6. Open work / explicit gaps

Items the slides imply.  Items the v0.2 SDK *did* close are kept in
the list with strikethrough so the audit trail stays clear; items
that remain open take priority for the upcoming releases.

1. **`<alp/inference.h>`** — unified ML inference API.  Held until
   v0.2 exercises Vela + TFLM end-to-end so the API isn't designed
   in a vacuum.
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
