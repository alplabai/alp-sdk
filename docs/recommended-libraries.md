# Recommended third-party libraries

ALP SDK is intentionally small — it wraps peripherals, ships chip
drivers, and routes inference / IoT through vendor backends.
Anything beyond that scope (ring buffers, RTOS-agnostic data
structures, embedded protobuf, embedded JSON, modern formatting,
flash KV stores, …) is **better delegated to existing,
battle-tested libraries** than reinvented inside `<alp/...>`.

This document curates the shortlist of libraries we've evaluated
against the awesome-embedded-software and awesome-cpp lists.
Each entry is either:

- **integrated** — wired via Kconfig + CMakeLists, opt-in.
- **recommended** — not pulled in by the SDK; consumers add it
  to their own build when they want it.  We just point at it.
- **considered, deferred** — kept here so future contributors
  don't re-evaluate from scratch.

Per [feedback memory "No zero-value re-export headers"](../memory),
the SDK does NOT ship `<alp/foo.h>` wrappers around upstream
libraries unless the wrapper genuinely adds portability /
defaults / type-shim value.

## Tier 1 — strongly recommended (use directly from app code)

| Library      | Scope                                       | Status        | Why                                                                                |
|--------------|---------------------------------------------|---------------|------------------------------------------------------------------------------------|
| [CMSIS-DSP](https://github.com/ARM-software/CMSIS-DSP) | DSP, FFT, filters, matrix math (Cortex-M / Cortex-A) | recommended (build opt-in via `ALP_HAS_CMSIS_DSP`) | The reason `<alp/math.h>` was deleted.  App code includes `arm_math.h` directly.   |
| [ETLCPP / etl](https://github.com/ETLCPP/etl) | C++ STL alternative, no heap, fixed sizes | recommended | Best-in-class for C++ firmware on M-class targets.  Gives you `etl::vector`, `etl::map`, `etl::queue` with compile-time capacities. |
| [fmt](https://github.com/fmtlib/fmt) / `std::format` | Modern C++ formatting | recommended | Type-safe replacement for printf.  Pairs naturally with logging in C++ user apps. |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing/serialization (C++) | recommended | For `<alp/iot.h>` MQTT JSON payloads + studio codegen output.  Header-only.       |
| [doctest](https://github.com/doctest/doctest) | Single-header C++ test framework | recommended | Lighter than gtest / Catch2 for chip-driver unit tests run on host (not on target). |
| [LittleFS](https://github.com/littlefs-project/littlefs) | Fail-safe filesystem for microcontrollers | recommended (Zephyr ships it) | For the microSD slot + on-flash storage.  Already in the Zephyr tree.             |

## Tier 2 — under evaluation for SDK integration

| Library      | Scope                                                | Proposed integration                                                                         |
|--------------|------------------------------------------------------|----------------------------------------------------------------------------------------------|
| [LwRB](https://github.com/MaJerle/lwrb) | Lock-free ring buffer | Internal use in `<alp/audio.h>` for DMA-staged capture + `<alp/peripheral.h>` UART RX.  Lock-free, MIT, ~300 LoC. |
| [nanoPB](https://github.com/nanopb/nanopb) | Protobuf for embedded (C) | Drive `<alp/mproc.h>` IPC framing + studio's wire format for over-the-wire config blobs.   |
| [TinyFrame](https://github.com/MightyPork/TinyFrame) | UART framing protocol | Available behind `CONFIG_ALP_SDK_TINYFRAME` for users building custom UART protocols.       |
| [heatshrink](https://github.com/atomicobject/heatshrink) | LZSS-style data compression for embedded/RT | For OTA delta updates (v1.x per ADR 0006's "deferred to v1.x" list).                        |
| [trice](https://github.com/rokath/trice) | Fast trace-ID logging with PC-side decoding | Diagnostic surface adjacent to `alp_last_error()`.  Real-time, ISR-safe.                    |
| [nanoMODBUS](https://github.com/debevv/nanoMODBUS) | Modbus RTU/TCP for embedded | Industrial users on V2N/N93 Yocto + AEN Zephyr.  Drop-in for ICS gateways.                  |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | O(1) deterministic heap allocator | Optional drop-in for `<alp/peripheral.h>` handle pools when callers want hard-RT bounds.    |

## Tier 3 — already integrated / Zephyr-native

| Library     | Status                                                                                       |
|-------------|---------------------------------------------------------------------------------------------|
| MCUboot     | Bootloader for Zephyr targets (AEN, N93-RTcore).  Locked in by [ADR 0006](adr/0006-secure-boot-secure-ota.md). |
| MbedTLS     | PSA Crypto backend for `<alp/security.h>`.                                                  |
| TinyUSB     | Reference for `<alp/usb.h>` Zephyr backend.                                                  |
| LVGL        | `<alp/gui.h>` opt-in re-export with ALP defaults via `lv_conf.h`.                            |
| lwIP / Mongoose | Zephyr's net stack + `<alp/iot.h>` MQTT path.  Mongoose available for users wanting an embedded HTTP server. |
| u8g2        | Monochrome OLED graphics — pairs with the `chips/ssd1306` driver.                            |
| SSD1306 / SSD1331 | Already in `chips/` library.                                                          |

## Tier 4 — alternative inference backends (considered, deferred)

Already shipped: TFLM (Cortex-M), Ethos-U (AEN, future N93), DRP-AI (V2N),
and DEEPX DX-M1 backend lands in #13.  The libraries below are smaller / different in scope:

| Library     | Niche                                                                  | Why we haven't integrated                                          |
|-------------|------------------------------------------------------------------------|--------------------------------------------------------------------|
| [TinyMaix](https://github.com/sipeed/TinyMaix) | Sub-100 KB inference for tiny MCUs (Cortex-M0, AVR)             | Below our target tier — alp-sdk's smallest target is the AEN M55.   |
| [nnom](https://github.com/majianjia/nnom) | Pure-C neural net on MCUs, Keras export                        | Overlap with TFLM; no clear win on AEN/N93.                          |
| [libonnx](https://github.com/xboot/libonnx) | C99 ONNX inference for embedded                                | ONNX path is reachable via TFLM (TFLite converter).  Revisit if model authors want pure ONNX. |

## Tier 5 — considered, NOT pursued

| Library      | Reason                                                                                       |
|--------------|---------------------------------------------------------------------------------------------|
| FreeRTOS direct | Zephyr is our RTOS choice (see [ADR 0001](adr/0001-wrapper-on-top-of-zephyr.md)).  FreeRTOS-on-Yocto is a Yocto-side concern, not SDK-side. |
| Mbed OS      | Superseded — Arm has wound down Mbed OS.                                                    |
| TouchGFX     | Proprietary STM32 tool.  Not portable to AEN/V2N/N93.                                       |
| eGUI         | NXP-specific; LVGL covers the cross-vendor case.                                            |
| EmbeddedProto | nanoPB is more widely adopted + actively maintained.                                       |
| picoTCP      | Effectively unmaintained; Zephyr's net stack + Yocto's native stack cover both worlds.      |

## Evaluation principles

When a new library candidate shows up:

1. **Scope test** — does it sit cleanly above the SDK's existing
   abstractions, or does it overlap them?  Overlap = no.
2. **License test** — Apache-2.0, MIT, BSD, Zlib, ISC, Boost.
   No GPL/LGPL in headers; LGPL is OK if linked dynamically on
   Yocto-only targets.
3. **Maintenance test** — commit activity in the last 12 months,
   no single-bus-factor maintainers.
4. **Footprint test** — sub-50 KB ROM / sub-4 KB RAM at typical
   config for anything touching the M-class build.
5. **No empty re-export rule** — if the wrapper would just be
   `#include "upstream.h"`, point users at the upstream directly.
   See [feedback memory](https://github.com/anthropics/claude-code)
   on this principle.

## See also

- [`PLAN.md`](../PLAN.md) — current roadmap.
- [`VERSIONS.md`](../VERSIONS.md) — versioned surface table.
- [Awesome Embedded Software](https://github.com/iDoka/awesome-embedded-software)
  and [Awesome C++](https://github.com/fffaraz/awesome-cpp) — the source
  lists this document was curated from on 2026-05-10.
