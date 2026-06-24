<!-- cross-platform-lint:ignore -->
# AEN801 (Alif Ensemble E8) bench flash + RAM-run helpers

Runnable companions to [`docs/aen-bench-bringup.md`](../../../docs/aen-bench-bringup.md).
These wrap the J-Link CommanderScript and Alif SETOOLS flows used to flash,
RAM-run, and read back AEN801 (E8, M55-HE) bench apps over SWD. They are
**Linux-side bench tooling**: J-Link Commander (`JLinkExe`) plus the Alif
Security Toolkit (`app-gen-toc` / `app-write-mram`), both Linux binaries on
this bench. Run them under WSL2 on Windows; macOS has J-Link but not the
Alif SETOOLS.

Maintained by **Alp Lab AB**.

## SETOOLS is license-gated — alp-sdk does not redistribute it

The **Alif Security Toolkit (SETOOLS)** — the `app-release-exec-linux`
directory with `app-gen-toc` and `app-write-mram` — is **license-gated** and
is **NOT redistributed by alp-sdk**. Obtain it from Alif under their license,
then point the helpers at it:

```sh
export SETOOLS_DIR=<path-to>/app-release-exec-linux
```

Flow A (`flash-run.sh`) and Flow D (`flash-jlink.sh`) hard-require it and
error out if `SETOOLS_DIR` is unset. Flow C (`ram-run.sh`), `reread.sh`, and
`build.sh` do **not** need SETOOLS.

## Quick start

```sh
# 1. Resolve env (workspace, toolchain, J-Link, board). Source the shared
#    layer; override any host-specific value by exporting it first.
export ZEPHYR_SDK_INSTALL_DIR=<your-zephyr-sdk>     # for the arm-zephyr-eabi tools
export SETOOLS_DIR=<...>/app-release-exec-linux     # Flow A/D only (license-gated)
export SE_UART=<your-serial-device>                 # Flow A only

# 2. Build an app for the AEN801 M55-HE target.
scripts/bench/aen/build.sh examples/aen/aen-gpio-bench

# 3. Flash + boot + read back the RAM console (pick a flow).
scripts/bench/aen/flash-jlink.sh "$BENCH_ROOT/build/aen-gpio-bench"   # Flow D
scripts/bench/aen/flash-run.sh   "$BENCH_ROOT/build/aen-gpio-bench"   # Flow A
scripts/bench/aen/ram-run.sh     "$BENCH_ROOT/build/aen-gpio-bench"   # Flow C
```

## Scripts

| Script | Flow | What it does |
| ------ | ---- | ------------ |
| `bench-env.sh` | — | Shared, sourced env layer. Resolves `BENCH_ROOT`, the arm-zephyr-eabi toolchain prefix, the JLink binary, board, J-Link device profiles. **Source it, don't execute it.** |
| `build.sh <app> [-D...]` | — | Pristine `west build` for the M55-HE target. Auto-detects the app overlay, passes `EXTRA_ZEPHYR_MODULES` (alp-sdk + hal_alif), prints errors + the memory-region summary + a `BIN OK` line. |
| `flash-jlink.sh <build-dir> [read-bytes]` | **D** | J-Link **direct MRAM flash** (no SE-UART). `app-gen-toc` builds the signed ATOC, the part-number device profile unlocks the built-in Alif MRAM loader, `loadbin`/`verifybin` write the package at its per-build start address (parsed from `app-package-map.txt`), `RSetType 2`/`r`/`g` pin-resets so the SE reloads it, then a generic-device RAM-console read-back. |
| `flash-jlink-mramxip.sh <build-dir> [read-bytes]` | **D** | J-Link **MRAM-XIP / slot0 two-blob** flash for an app linked into MRAM slot0 (a real NPU model that overflows ITCM). Writes the app → `0x80010000` + the signed ATOC → its parsed address; needs `CONFIG_USE_DT_CODE_PARTITION=y` in the app build. See the script header for the gotcha on returning to ITCM apps afterwards. |
| `flash-run.sh <build-dir> [read-bytes]` | **A** | **Production MRAM flash** over the SE-UART. Stages the signed-ATOC JSON, `app-gen-toc` + `app-write-mram` burn over `$SE_UART` (SES auto-enters maintenance, resets + boots), then a J-Link read-back of the RAM console. |
| `ram-run.sh <build-dir> [sleep_ms] [size] [preload]` | **C** | **RAM-run** an ITCM image (no MRAM write): `loadbin` to `0x0`, `setpc <entry>` (thumb-bit cleared), `go`, sleep, halt, dump + ASCII-decode the RAM console. Optional `preload` JLink file runs after halt / before loadbin (e.g. clear a SoC integration reg). |
| `reread.sh <build-dir> [size]` | (B) | Re-read `ram_console_buf` over SWD with no reflash — attach generic device, halt, `mem8`, ASCII-decode. |
| `flash-all-flowd.sh [app ...]` | **D** | Batch Flow D over a list of apps (argv, else `apps.txt`). Strictly serial (one board / one probe), resilient (a failed app is logged, the batch continues), scrapes each app's `RESULT` line into a summary. |
| `apps.txt` | — | Default app list for the batch runner (one build-dir name per line). |
| `aen-bench-shared.conf` | — | Portable Kconfig fragment: RAM-console observability (the app UART is not on USB on this bench) + `CONFIG_DCACHE=n`. Add via `-DEXTRA_CONF_FILE=...`. |

## Environment variables

All host-specific values are resolved in `bench-env.sh`. Override any of them
by exporting before you invoke a helper.

| Variable | Default | Purpose |
| -------- | ------- | ------- |
| `BENCH_ROOT` | `git rev-parse --show-toplevel` | Where build outputs live (`$BENCH_ROOT/build/<app>`). |
| `ALP_SDK_DIR` | `$BENCH_ROOT` | The alp-sdk checkout (build source + `EXTRA_ZEPHYR_MODULES`). |
| `ZEPHYR_BASE` | `west topdir`/zephyr | Pinned Zephyr 4.4.0 checkout. |
| `ZEPHYR_SDK_INSTALL_DIR` | *(none)* | Zephyr SDK root; the `arm-zephyr-eabi-*` tools are resolved from here, else off `PATH`. |
| `HAL_ALIF_DIR` | `west list hal_alif` | hal_alif module path (passed as an extra Zephyr module). **TBD fallback:** export it if `west list` can't resolve it — we do not invent a path. |
| `AEN_BOARD` | `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` | Qualified board target. |
| `SE_UART` | *(none)* | SE-UART serial device for Flow A (`<your-serial-device>`; host-specific). |
| `SETOOLS_DIR` | *(none, error-if-unset)* | Alif SETOOLS `app-release-exec-linux` dir. **License-gated, not shipped.** |
| `JLINK_DEVICE_FLASH` | `AE822FA0E5597LS0_M55_HE` | Part-number device profile — unlocks the built-in Alif MRAM loader (Flow D). |
| `JLINK_DEVICE_READ` | `Cortex-M55` | Generic device for all reads/attach/RAM-run (attaches to the live core). |
| `JLINK_SPEED` | `4000` | SWD clock (kHz). |
| `JLINK_EXE` | `JLinkExe` | JLink Commander binary (override for a non-PATH install). |

## Which flow? (A / B / C / D)

The four bench flows are defined and compared in
[`docs/aen-bench-bringup.md`](../../../docs/aen-bench-bringup.md):

- **Flow A — Production MRAM flash (SETOOLS / ISP)** over the SE-UART →
  `flash-run.sh`. Shipping image, QA, re-keying.
- **Flow B — Seeing the console** (the app UART is not on USB; read
  `ram_console_buf` over SWD) → `reread.sh`.
- **Flow C — J-Link RAM-run** (dev/debug iteration, no MRAM burn) →
  `ram-run.sh`.
- **Flow D — J-Link MRAM flash** (built-in Alif loader, no SE-UART; the
  fast day-to-day default) → `flash-jlink.sh` / `flash-all-flowd.sh`.

## `west flash` (the `alif_flash` runner) = Flow A, productised

These helpers are the bench harness. For the *customer* SES → MCUboot → slot0
chain, alp-sdk also wires Flow A into **standard `west flash`** via the
**`alif_flash`** west runner
([`scripts/west_commands/runners/alif_flash.py`](../../west_commands/runners/alif_flash.py)).
The AEN801 M55-HE/HP board files
(`zephyr/boards/alp/e1m_aen801_m55_*/board.cmake`) wire it as the default
flasher, so `west flash` runs the **same** `app-gen-toc` + `app-write-mram`
recipe as `flash-run.sh` (it stages the identical signed-ATOC config,
`loadAddress 0x58000000` for the M55-HE), and `jlink` stays the
debug/attach runner.

```sh
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <your-app> --sysbuild
export SETOOLS_DIR=<...>/app-release-exec-linux   # license-gated; not shipped
export SE_UART=<your-serial-device>               # the SE-UART (host-specific)
west flash                                        # -> alif_flash -> SETOOLS
```

The runner reads `SETOOLS_DIR` / `SE_UART` (the same env vars these helpers
use), or takes `--setools-dir` / `--se-uart`; pass `--mram-xip` for a
slot0-linked app that overflows ITCM (mramAddress `0x80010000`).

**One-off setup.** `alif_flash` is **not** in upstream Zephyr's `runners`
package — alp-sdk ships it and surfaces it through `zephyr/module.yml`'s
`runners:` list (no edit to the pinned Zephyr tree). Also `pip install fdt`
once: `app-gen-toc` needs the `fdt` Python package, which is not a Zephyr
requirement (the runner warns if it is missing).
<!-- cross-platform-lint:resume -->
