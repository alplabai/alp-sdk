# Design: teaching example per missing 3rd-party library

Date: 2026-07-10
Branch: `feat/3p-library-examples` (off `origin/dev`)
Worktree: `/tmp/alp-3p-lib-examples`

## Problem

The SDK integrates 25 third-party libraries (one profile dir each under
`metadata/library-profiles/`). Customers use these libraries through their
native APIs — the SDK does **not** wrap them; it ships per-library
compile-time profile headers + `hw-backends.yaml` + a west pin so they build
correctly under the SDK's invariants (no heap on hot path, no exceptions on
M-class, no `<iostream>`).

10 libraries are already demonstrated in some example (usually as a supporting
library inside a larger app). **15 have no dedicated teaching example.** A
customer picking up, say, `nanopb` or `u8g2` has nothing showing how to enable
and use it in the SDK. Some of the 15 are also not fully wired — a build would
fail before the customer got anywhere.

## Goal

One focused, self-teaching example per undemonstrated library, each of which
**builds and runs on `native_sim/native/64`** so it needs no hardware to try.
Where a library is not actually build-ready in the SDK, fix the compatibility
gap so the example genuinely compiles.

## Scope

**In scope** — the 15 undemonstrated libraries:
`jsmn`, `nanopb`, `coremqtt_sn`, `libcoap`, `libwebsockets`, `tinygsm`,
`minimp3`, `libhelix`, `u8g2`, `gfx_compat`, `littlefs`, `etl`, `fmt`,
`catch2`, `doctest`.

**Out of scope**
- Bench / real-silicon verification. Every example is `native_sim` build-only,
  marked `[UNTESTED]` in its README (matches the repo norm for v0.5–v0.6).
- Wrapping any library in an `<alp/*>` API. Examples use native library APIs.
- The 10 already-demonstrated libraries (tflite_micro, lvgl, mbedtls,
  cmsis_dsp, nlohmann_json, modbus, madgwick_ahrs, pid, opus, bearssl).

## Design principle: demonstrate core value, native_sim-runnable

Many of these libraries normally drive hardware (u8g2 → OLED, tinygsm → modem
UART, minimp3 → I2S out). On `native_sim` there is no such hardware. Each
example therefore exercises the **library's own logic** in a CPU/RAM-backed
way that runs on the host, and documents the HW-backed path as a `board.yaml`
swap:

- Decoders (`minimp3`, `libhelix`) decode an embedded compressed blob to PCM
  in RAM and print a checksum / RMS — pure CPU.
- Parsers / codecs (`jsmn`, `nanopb`, `libcoap`, `libwebsockets`,
  `coremqtt_sn`) build a message/PDU into a buffer and parse it back
  (round-trip or against a canned frame) — no live network.
- `tinygsm` runs its AT state machine against a mock UART transcript.
- `u8g2` renders into its RAM framebuffer device and dumps the buffer as ASCII;
  the SSD1306/SPI HW device is the board.yaml swap.
- `gfx_compat` blits/fills via the shim using the pure-C SW fallback.
- `littlefs` mounts a RAM-backed block device, writes/reads/lists a KV file.
- `etl` / `fmt` are pure and run anywhere.
- `catch2` / `doctest` are host unit-test frameworks — the example is a small
  passing test binary that builds and runs on the host.

Each `src/main.c` (or `.cpp` for the C++ libs) carries ~50% teaching comments
per the `writing-self-teaching-comments` skill.

## Per-library example plan

| Lib | Dir | Lang | Demonstrates (native_sim-runnable) |
|---|---|---|---|
| `jsmn` | `examples/connectivity/jsmn-json-parse` | C | Tokenize embedded JSON config → typed struct |
| `nanopb` | `examples/connectivity/nanopb-encode-decode` | C | `.proto` msg encode→buffer→decode round-trip |
| `coremqtt_sn` | `examples/connectivity/mqtt-sn-publish` | C | Serialize MQTT-SN PUBLISH; parse it back |
| `libcoap` | `examples/connectivity/coap-client-get` | C | Build CoAP GET PDU; parse a canned response PDU |
| `libwebsockets` | `examples/connectivity/websocket-frame` | C | WS frame encode/decode round-trip (offline) |
| `tinygsm` | `examples/connectivity/tinygsm-modem-at` | C++ | AT state machine over a mock UART stream |
| `minimp3` | `examples/audio/minimp3-decode` | C | Decode embedded MP3 blob → PCM, print RMS |
| `libhelix` | `examples/audio/libhelix-decode` | C | Decode embedded MP3 frame → PCM via Helix |
| `u8g2` | `examples/display/u8g2-oled-draw` | C | Render to RAM framebuffer; ASCII dump (HW = SSD1306) |
| `gfx_compat` | `examples/display/gfx-compat-blit` | C | Fill/blit via shim SW fallback |
| `littlefs` | `examples/power-timing/littlefs-keyvalue` | C | Mount RAM-backed LFS; write/read/list a KV file |
| `etl` | `examples/peripheral-io/etl-fixed-containers` | C++ | `etl::vector`/`etl::map`, no heap, no STL |
| `fmt` | `examples/peripheral-io/fmt-formatting` | C++ | `fmt::format_to` into fixed buffer, no iostream |
| `catch2` | `examples/testing/catch2-selftest` | C++ | Host unit-test demo (builds + runs on host) |
| `doctest` | `examples/testing/doctest-selftest` | C++ | Header-only host unit-test demo |

## Per-example deliverable

Single-OS example anatomy (per `examples/README.md`):

```
examples/<category>/<name>/
├── CMakeLists.txt   # invokes scripts/alp_project.py + delegates to west build
├── prj.conf         # feature selection lives in board.yaml; keep minimal
├── board.yaml       # native_sim target + cores.<id>.libraries: [<lib>]
├── src/main.c(pp)   # heavily commented (~50%) application code
├── README.md        # what it shows + [UNTESTED] status + HW-path note
└── testcase.yaml    # native_sim/native/64 twister scenario
```

`examples/testing/` is a new category subdir; add it to `examples/README.md`'s
index with a short "Testing / host" section.

## Compatibility fixes (only where a build blocks)

Fixes are made **only** where an example cannot otherwise build. Each is
verified by the example compiling on native_sim.

- **etl / fmt / doctest** — profile headers exist (`etl_profile.h`,
  `fmt_config.h`, `doctest_config.h`) but there is no west pin. Add a west
  project entry (behind `extras-tier1` or a new group) so `#include` resolves,
  and wire the profile-header dir onto the include path via the loader / a
  Kconfig knob. If a header-only vendored drop is cleaner than a west pin for
  these three, vendor them under `vendors/<lib>/` with a README (classify
  public/internal per the skill).
- **gfx_compat** — `west.yml` states it ships in-tree at `src/lib/gfx_compat/`,
  but `src/lib/` is empty. Create the minimal in-tree shim (pure-C SW-blit
  fallback, matching `CONFIG_ALP_GFX_COMPAT_SW`) the example links against.
- **littlefs** — confirm the Zephyr `fs`/littlefs module resolves on a
  native_sim build; add the missing pin / Kconfig select if it does not.
- **profile headers** — add a profile header for any of the remaining libs
  whose upstream default breaks the SDK build (no heap / no exceptions /
  no iostream). Do not add a header where the upstream default already builds
  clean.

Shared-file edits (`west.yml`, `zephyr/Kconfig.alp-libraries`,
`examples/README.md`, `metadata/catalog.json` if regenerated) are made
**serially by one agent** — never by parallel writers.

## Execution (orchestrator pattern)

1. **Worktree** (done) — `feat/3p-library-examples` off `origin/dev` at
   `/tmp/alp-3p-lib-examples`.
2. **Compatibility fixes first** — one agent does the serial shared-file work
   (west pins, gfx_compat shim, Kconfig knobs, littlefs) so the example authors
   build against a wired tree.
3. **Parallel example authoring** — fan out file-disjoint example units across
   `alp-implementor` agents, **batched by category** (connectivity, audio,
   display, cpp/peripheral-io, testing) to dodge server rate-limits
   (one-agent-per-file at scale rate-limits). Each agent loads
   `writing-self-teaching-comments` + the example anatomy.
4. **Index + regen** — one agent updates `examples/README.md` and regenerates
   `metadata/catalog.json` if the gate requires it.
5. **Local CI** — `twister native_sim/native/64` + `clang-format` +
   `check_*.py` gates, then an `alp-reviewer` pass. Fix findings.
6. **Integrate** — single batched commit set on the branch; open PR against
   `dev` (not `main`).

## Testing

- Every example ships a `testcase.yaml` that builds (and, where it produces
  deterministic output, runs) on `native_sim/native/64` under twister.
- `catch2` / `doctest` examples run their test binary on the host and must exit
  0.
- The full local CI gate set (twister + clang-format + `check_*.py`) must pass
  before the PR.

## Risks / open items

- **etl/fmt/doctest fetch mechanism** — if adding a west pin is heavier than
  vendoring the header-only source, vendor instead; decide per-lib at
  implementation time. Either way the example must build offline on native_sim.
- **libhelix / minimp3 / tinygsm / bearssl west pins are `main`/TBD** — pins
  may be unstable. If a pin does not resolve, note it in the example README and
  the compatibility-fix agent pins a working SHA.
- **catalog.json drift** — adding examples + re-docstringing may drift
  `metadata/catalog.json`; regenerate as part of step 4.
