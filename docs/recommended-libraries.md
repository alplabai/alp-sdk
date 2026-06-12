# Recommended third-party libraries

Alp SDK is intentionally small — it wraps peripherals, ships chip
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

By policy, the SDK does NOT ship `<alp/foo.h>` wrappers around upstream
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

## Using enabled libraries (no wrapper, just use them)

When an app lists a Tier-1 library in `board.yaml`'s `libraries:`
array (or the lib is Tier-3 Zephyr-native, like LVGL), the SDK
adds the library to the build with its native API on the include
path.  There's **no `<alp/...>` wrapper**.  You include the
upstream header and call the upstream functions.  The SDK ships
the right compile-time profile under
[`metadata/library-profiles/<lib>/`](../metadata/library-profiles/)
so the library is set up correctly for the SDK's invariants
(no exceptions, no `<iostream>`, no STL on M-class).

Below: a short "what does using this look like in app code"
snippet per library.  Apps are expected to read the upstream
documentation for the full surface -- these snippets just show
the shape so you know what to expect.

### CMSIS-DSP

```c
#include <arm_math.h>

float32_t in[256];
float32_t out[256];
arm_fir_instance_f32 fir;
float32_t state[256 + 32 - 1];
float32_t coeffs[32] = { /* taps */ };

arm_fir_init_f32(&fir, 32, coeffs, state, 256);
arm_fir_f32(&fir, in, out, 256);
```

Enable: `CONFIG_CMSIS_DSP=y` in board.yaml-generated alp.conf
(triggered automatically when the SoM's `capabilities:` block
declares a backend that needs CMSIS-DSP, or when you explicitly
add `cmsis_dsp` to a core's `libraries:` list).

### ETLCPP

```cpp
#include <etl/vector.h>
#include <etl/map.h>

etl::vector<int, 64> samples;   // capacity 64, no heap
samples.push_back(42);

etl::map<int, const char*, 8> table;
table.insert({1, "one"});
```

Enable: `libraries: [etl]` in board.yaml.  The SDK's profile turns
on `ETL_NO_STL` + `ETL_NO_EXCEPTIONS` automatically.

### fmt

```cpp
#include <fmt/format.h>

char buf[64];
auto result = fmt::format_to_n(buf, sizeof(buf), "x={} y={:.2f}", x, y);
*result.out = '\0';
puts(buf);
```

Enable: `libraries: [fmt]`.  The SDK's profile turns on
`FMT_HEADER_ONLY=1`, `FMT_USE_IOSTREAM=0`, `FMT_EXCEPTIONS=0`.

### nlohmann/json

```cpp
#include <nlohmann/json.hpp>
using json = nlohmann::json;

json doc = json::parse(payload, /*cb=*/nullptr, /*allow_exceptions=*/false);
if (!doc.is_discarded()) {
    int temp_c = doc["temperature_c"];
    /* ... */
}
```

Enable: `libraries: [nlohmann_json]`.  Profile sets
`JSON_NOEXCEPTION=1` so `parse(...)` returns a discarded sentinel
on malformed input instead of throwing.

### LVGL (Zephyr-native)

```c
#include <lvgl.h>

lv_obj_t *screen = lv_scr_act();
lv_obj_t *label = lv_label_create(screen);
lv_label_set_text(label, "Hello E1M");
lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
```

Enable: set `iot:` and `display:` features in your board.yaml; the
loader translates them to `CONFIG_LVGL=y` + `CONFIG_DISPLAY=y` in
the generated alp.conf.  Or for finer control, drop a Zephyr
`prj.conf` override (see "Today's gaps" in `docs/board-config.md`).

Driver-side wiring (which display, which framebuffer) comes from
the board preset -- e.g. `metadata/boards/e1m-evk.yaml`
populates the SSD1306 OLED, and `<alp/boards/alp_e1m_evk.h>` maps
the I²C bus + reset pin.  Your app code just calls `lv_*` against
the resolved display.

### LittleFS

```c
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs);
static struct fs_mount_t mp = {
    .type = FS_LITTLEFS, .fs_data = &lfs,
    .storage_dev = (void *)FLASH_AREA_ID(storage),
    .mnt_point = "/lfs",
};

fs_mount(&mp);
```

Enable: `CONFIG_FILE_SYSTEM_LITTLEFS=y` (Zephyr-native; the SDK
doesn't gate this).  Set partition / mount point per Zephyr's
fs subsystem documentation.

### doctest (test-only)

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "chips/lsm6dso/lsm6dso.h"

TEST_CASE("lsm6dso_init validates whoami") {
    fake_i2c_t bus;
    fake_i2c_set_response(&bus, 0x0F, 0x6C);
    CHECK(lsm6dso_init(&bus) == 0);
}
```

Enable: `libraries: [doctest]`.  Test-only -- doctest doesn't
ship in production builds.

---

If a library you want isn't in the Tier-1 list above, see the
deferred / considered tiers below or open an issue.  Adding a
library is a matter of writing a profile header at
`metadata/library-profiles/<lib>/` + a `libraries:` enum entry
in `metadata/schemas/board.schema.json` -- low friction
once the case is made.

## HW-backend profiles (per-library accelerator binding)

Alongside the compile-time profile header (`etl_profile.h`,
`fmt_config.h`, ...) each enabled library also ships a
[`hw-backends.yaml`](../metadata/library-profiles/) table.  This
table is the source of truth that
[`scripts/alp_project.py`](../scripts/alp_project.py) reads against
each SoM preset's `capabilities:` matrix to emit the right
accelerator-binding `CONFIG_*` lines into the slice's `alp.conf`.

The shape mirrors `metadata/library-profiles/mbedtls/hw-backends.yaml`:
a list of `accelerators:` (one block per accelerator class --
`crypto`, `gpu_2d`, `dma`, `simd`, `cordic`, `fft`, `ml_npu_primary`,
...) where each entry pairs a `requires_cap:` (capability flag
declared in the SoM preset) with the Kconfig that lights up that
backend.  A `sw_fallback:` floor (always `required: true`) backs
every library so a slice with no matching capability still builds
and runs.

**Coverage status (v0.6).**  All 25 libraries in the schema enum
ship a per-library `hw-backends.yaml`:

| Class                 | Libraries                                                            |
|-----------------------|----------------------------------------------------------------------|
| Crypto / TLS          | `mbedtls`, `bearssl`                                                 |
| ML inference          | `tflite_micro`                                                       |
| DSP / math            | `cmsis_dsp`                                                          |
| Filesystem            | `littlefs`                                                           |
| Graphics              | `lvgl`, `u8g2`, `gfx_compat`                                         |
| Sensor fusion / control | `madgwick_ahrs`, `pid`                                             |
| Industrial bus        | `modbus`                                                             |
| IoT / networking      | `coremqtt_sn`, `libcoap`, `tinygsm`, `libwebsockets`, `nanopb`, `jsmn` |
| Audio codecs          | `minimp3`, `opus`, `libhelix`                                        |
| Header-only utility   | `etl`, `fmt`, `nlohmann_json`, `doctest`                             |
| Test framework        | `catch2`                                                             |

Seven libraries declare an empty `accelerators:` list -- the four
header-only utility libraries (`etl`, `fmt`, `nlohmann_json`,
`doctest`) plus `catch2` (test framework, host-side), `jsmn`
(parser, pure-SW only), and `nanopb` (serialisation, pure-SW only)
-- their value lives in the pure-SW path with no accelerator class
to bind.  The other 18 libraries each carry at least one
`requires_cap:`-gated backend entry.

Regression-tested by
[`tests/scripts/test_library_profiles.py`](../tests/scripts/test_library_profiles.py):
the test fails if a library is added to the schema enum without a
matching `hw-backends.yaml`, if a profile's `library:` slug drifts
from the directory name, or if any emitted `kconfig:` line is not a
real-looking `CONFIG_<NAME>=<value>` token.  The test does NOT
validate that each emitted symbol exists in the pinned Zephyr's
Kconfig tree -- that's a build-time concern.

## Tier 2 — deferred to v0.5+

The libraries below cleared the evaluation but didn't land in v0.3's
library-integration pass.  Each carries a one-liner explaining why
it's parked.  v0.5 cycle revisits.

| Library      | Scope                                                | Why deferred                                                                                |
|--------------|------------------------------------------------------|---------------------------------------------------------------------------------------------|
| [TinyFrame](https://github.com/MightyPork/TinyFrame) | UART framing protocol | Overlaps with the cc3501e protocol once that ships as a public framing helper -- revisit when there's a second framed UART use case in the SDK. |
| [heatshrink](https://github.com/atomicobject/heatshrink) | LZSS-style compression for embedded/RT | OTA delta updates are post-v1.x per ADR 0006; until OTA actually ships there's no caller. |
| [trice](https://github.com/rokath/trice) | Fast trace-ID logging with PC-side decoding | Zephyr's LOG subsystem covers the v0.3 / v0.4 needs; trice's win is post-v1.0 production tracing. |
| [nanoMODBUS](https://github.com/debevv/nanoMODBUS) | Modbus RTU/TCP for embedded | Industrial-customer-driven; no committed v0.4 caller yet.  Adding it now means maintaining unused code. |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | O(1) deterministic heap allocator | The SDK's handle pools are statically sized (no heap on the hot path); o1heap's value lands when caller code starts allocating, which is post-1.0. |

## Tier 3 — already integrated / Zephyr-native

| Library     | Status                                                                                       |
|-------------|---------------------------------------------------------------------------------------------|
| MCUboot     | Bootloader for Zephyr targets (AEN, N93-RTcore).  Locked in by [ADR 0006](adr/0006-secure-boot-secure-ota.md). |
| MbedTLS     | PSA Crypto backend for `<alp/security.h>`.                                                  |
| TinyUSB     | Reference for `<alp/usb.h>` Zephyr backend.                                                  |
| LVGL        | `<alp/gui.h>` opt-in re-export with Alp defaults via `lv_conf.h`.                            |
| lwIP / Mongoose | Zephyr's net stack + `<alp/iot.h>` MQTT path.  Mongoose available for users wanting an embedded HTTP server. |
| u8g2        | Monochrome OLED graphics — pairs with the `chips/ssd1306` driver.                            |
| SSD1306 / SSD1331 | Already in `chips/` library.                                                          |
| [LwRB](https://github.com/MaJerle/lwrb) | **v0.4-prep, first consumer landed.**  In-tree LwRB stub impl at `vendors/lwrb/src/lwrb_stub_impl.c` (~140 LoC, single-producer / single-consumer with canonical empty/full disambiguation) backs the new opt-in `alp_uart_rx_ringbuf_*` helper on AEN-Zephyr (`CONFIG_ALP_SDK_UART_RX_RINGBUF`).  Upstream `MaJerle/lwrb@v3.2.0` west.yml pin stays behind the `extras-v04` group (default-disabled) until v0.4-final flips the group on and the upstream sources win the include search. |
| [nanoPB](https://github.com/nanopb/nanopb) | **v0.4-prep, first consumer landed.**  In-tree placeholder framing helper at `src/common/proto/alp_mproc_frame.{h,c}` backs the optional 12-byte envelope wrapping `alp_mbox_send` payloads under `CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING`.  Schema at `metadata/protos/alp_mproc.proto` is the source of truth; the nanopb-generated `.pb.{c,h}` codec replaces the placeholder when the `extras-v04` west group lands upstream nanopb at v0.4-final.  Stub `<pb.h>` + `<pb_encode.h>` + `<pb_decode.h>` headers remain in place so `#include <pb.h>` doesn't break builds pre-swap. |

## Tier 4 — alternative inference backends (considered, deferred)

Already wired: TFLM (Cortex-M, Zephyr), Ethos-U (AEN), DRP-AI (V2N, Zephyr
dispatch stub), and DEEPX DX-M1 (V2N-M1, Yocto dispatch stub).  The
libraries below are smaller / different in scope:

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
   `<alp/math.h>` and `<alp/signal.h>` were removed under this
   rule (May 2026).

## See also

- [`VERSIONS.md`](../VERSIONS.md) — versioned surface table.
- [Awesome Embedded Software](https://github.com/iDoka/awesome-embedded-software)
  and [Awesome C++](https://github.com/fffaraz/awesome-cpp) — the source
  lists this document was curated from on 2026-05-10.
