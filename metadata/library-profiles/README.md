@page metadata_library_profiles_index Library compile-time profile headers

# metadata/library-profiles

Per-library **compile-time profile headers** the SDK ships so that
optional libraries enabled in `board.yaml`'s `libraries:` array
work correctly in the SDK's environment -- without wrapping them.

## The distinction: compatible vs wrapped

The SDK deliberately does **not** ship `<alp/...>` wrappers around
the libraries in `docs/recommended-libraries.md`'s Tier 1.  Apps use
those libraries through their native APIs (`#include "etl/vector.h"`,
`fmt::format(...)`, `nlohmann::json::parse(...)`).

But "no wrapper" doesn't mean "no integration."  Each library has
compile-time configuration headers / defines that govern its
behaviour: ETL's `etl_profile.h`, fmt's `FMT_*` macros, nlohmann's
`JSON_*` switches.  Default values aren't always right for embedded
firmware on Cortex-M:

- ETL's defaults assume the STL is available; embedded targets
  often need `ETL_NO_STL` and a custom profile.
- fmt's defaults include `<iostream>` integration we don't want;
  embedded builds set `FMT_HEADER_ONLY` + `FMT_USE_IOSTREAM=0`.
- nlohmann/json's defaults assume exceptions; the SDK runs with
  `JSON_NOEXCEPTION` to match the no-exceptions invariant.

This directory ships **one profile header per enabled library**,
pre-tuned for the SDK's invariants (no heap on hot path, no
exceptions on M-class targets, no `<iostream>`).  When the loader
detects a library in `libraries:`, it adds the matching profile
header's directory to the include path BEFORE the upstream
library's defaults, so the profile wins.

The model:

```
app code
   |
   v
#include "etl/vector.h"          <-- upstream library (vendored / fetched)
   |
   v
internally pulls #include "etl_profile.h"
   |
   v
metadata/library-profiles/etl/alp-embedded.h    <-- our profile
                                                    sets ETL_NO_STL,
                                                    pool sizes, etc.
```

## When customers override

Customers can supply their own profile by placing a header at
`<their-app>/include/etl_profile.h` (or `fmt_config.h`, etc.) -- the
loader prefers the app's profile over the SDK's when both exist.
That covers the case where a project needs ETL's exceptions even
though our default-embedded profile turns them off.

## Layout

Each profile header is named to match the **upstream library's
expected config filename** so it drops in as the file the
library actually looks for (e.g. ETL's `#include "etl_profile.h"`,
LVGL's `#include "lv_conf.h"`, MbedTLS's `MBEDTLS_CONFIG_FILE`).
The directory disambiguates which library each file is for.

```
metadata/library-profiles/
├── README.md                          (this file)
├── etl/
│   └── etl_profile.h                  (ETL.  Upstream looks for this name.)
├── fmt/
│   └── fmt_config.h                   (fmt config defines)
├── nlohmann_json/
│   └── json_config.h                  (nlohmann/json config defines)
├── lvgl/
│   └── lv_conf.h                      (LVGL.  Upstream looks for this name.)
├── doctest/
│   └── doctest_config.h               (doctest config defines)
├── mbedtls/
│   └── mbedtls_config.h               (MbedTLS.  Set MBEDTLS_CONFIG_FILE
│                                       to this when including.)
└── cmsis_dsp/
    ├── hw-backends.yaml               (SIMD/CORDIC/FFT/DMA backend bindings)
    └── README.md
```

Profile headers stay **small and conservative** -- they encode
the SDK's hard invariants (no exceptions on M-class, no heap on
hot path, no MD5/SHA-1/CBC etc.) and leave everything else at the
upstream default.  The point is compatibility-by-default;
opinionated tuning is the consumer's job.

Adding a new library: drop a directory + a config header named
to match the upstream's expected filename (or a `README.md` if
no compile-time config is needed), then extend the `libraries:`
enum (under `cores.<id>.libraries:`) in
`metadata/schemas/board.schema.json` and the
`_LIBRARY_KCONFIG` map in `scripts/alp_project.py` so consumers
can enable it via `board.yaml`.

## v0.3 scaffolding vs v0.4 wire-up

v0.3 ships the profile headers + this README.  v0.4 lands:

1. Loader emission -- when `libraries:` includes a library with a
   profile, the loader adds the profile directory to the build's
   include path (e.g. `zephyr_include_directories(... library-profiles/etl)`).
2. west.yml pins for the libraries -- the upstream sources land in
   `modules/lib/<lib>/` and the profiles take effect.
3. Per-library examples under `examples/` showing the
   enable + use pattern end-to-end.

Until then, the profiles are documentation: consumers can copy
them into their projects directly if they're wiring the libraries
manually in v0.3.

Accelerator priority entries may carry `status: planned` or
`status: stub`.  Those entries document the intended accelerator
binding, but the loaders skip them so generated configs do not claim
hardware acceleration before a real library consumer exists.  Omit
`status:` only for an implemented entry that is safe to emit as an
active `CONFIG_*=y` line.

## See also

- [`docs/recommended-libraries.md`](../../docs/recommended-libraries.md)
  -- the Tier 1 library list these profiles cover.
- [`docs/board-config-schema.md`](../../docs/board-config-schema.md)
  -- the `board.yaml` `libraries:` section.
- [`metadata/templates/board.yaml`](../templates/board.yaml) -- where
  consumers list which libraries to enable.
