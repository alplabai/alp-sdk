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
| `flash-jlink.sh <build-dir> [read-bytes]` | **D** | J-Link **direct MRAM flash** (no SE-UART). `app-gen-toc` builds the signed ATOC, the part-number device profile unlocks the built-in Alif MRAM loader, `loadbin`/`verifybin` write the package at its per-build start address (parsed from `app-package-map.txt`), `RSetType 2`/`r`/`g` pin-resets so the SE reloads it, then a generic-device RAM-console read-back. **Gates on the `verifybin` outcome (#1488): exit 3** when the transcript reports `verify failed`/`verification failed`/`mismatch`, and **exit 3** when it carries no `verify successful` line at all (the verify never ran) — either way the read-back is skipped and the board must not be treated as flashed. Also **exit 4** (DPIDR preflight says this is not the AEN E8), **exit 2** (the part-number device profile could not connect), **exit 7** (flash verified, but the post-boot console read never opened the probe). |
| `flash-jlink-mramxip.sh <build-dir> [read-bytes]` | **D** | J-Link **MRAM-XIP / slot0 two-blob** flash for an app linked into MRAM slot0 (a real NPU model that overflows ITCM). Writes the app → `0x80010000` + the signed ATOC → its parsed address; the slot0 link comes from the board `_defconfig` (`CONFIG_USE_DT_CODE_PARTITION=y`), so a plain build already qualifies — a `0x8000xxxx` vector means a Flow C fragment/overlay is still layered. See the script header for the gotcha on returning to ITCM apps afterwards. |
| `flash-run.sh <build-dir> [read-bytes]` | **A** | **Production MRAM flash** over the SE-UART. Stages the signed-ATOC JSON, `app-gen-toc` + `app-write-mram` burn over `$SE_UART` (SES auto-enters maintenance, resets + boots), then a J-Link read-back of the RAM console. |
| `flash-run-dualcore.sh <hp-build-dir> <he-build-dir>` | **A** | **Dual-core deferred-TOC** two-entry ATOC over the SE-UART: `ALP-HP` boots normally (`["load","boot"]`), `ALP-HE` is flagged `["load","boot","deferred"]` so the SES skips its boot-time release and the HP image un-defers + releases it at runtime with `se_service_process_toc_entry()` (service 500) via `CONFIG_ALP_SDK_MPROC_BOOT_ALIF_SE_DEFERRED_TOC`. Fixes the `["load"]`-only pattern, which reports "Loaded, Verified" while the ITCM destination holds no image and locks up the peer on release — see the script header and `docs/aen-bench-bringup.md`. |
| `flash-update-log-firewall-probe.sh [--package-only] <he-build-dir>` | **D** | Firmware-update-log HE direct-write MRAM firewall probe. Builds an app-only ATOC by default so any already-provisioned DEVICE/firewall policy is preserved; set `ALP_AEN_INCLUDE_DEVICE_CONFIG=yes` only for deliberate DEVICE replacement, and `ALP_AEN_DEVICE_CONFIG_JSON=<file>` to name a board-specific config already staged under SETOOLS `build/config`. **Gates on the `verifybin` outcome (#1488): exit 3** on a failed verify, **exit 3** when no `verify successful` line is present. **DATA LOSS at either exit 3:** `loadbin`, `verifybin`, `RSetType 2`, `r`, `g` are one CommanderScript JLinkExe has already finished, and the `HE-PROBE` ATOC entry is `"flags": ["load", "boot"]` — the board has already been pin-reset, booted and released, so `alp_ulog_partition` may ALREADY have been overwritten by the HE probe. The gate suppresses the false "flash complete" report and the beacon read-back; it cannot prevent the write or the boot. Re-read the partition from a known-good state before trusting the `BASELINE_WORDS` captured earlier in the same run. |
| `flash-update-log-dual.sh [--package-only] <hp-build-dir> <he-build-dir>` | **D** | Firmware-update-log dual-M55 package: HP owner boots first and releases HE client. Builds an app-only ATOC by default for the same firewall-policy reason as the probe helper. **Gates on the `verifybin` outcome (#1488): exit 3** on a failed verify, **exit 3** when no `verify successful` line is present. **DATA LOSS at either exit 3:** same one-CommanderScript shape as the firewall probe, and the `HP-OWNER` ATOC entry is `"flags": ["load", "boot"]` — the board has already been pin-reset and the HP owner booted and released the HE client, so `alp_ulog_partition` may ALREADY have been appended to by this unverified package. Re-read the partition from a known-good state before trusting the update log the run produced. |
| `read-update-log-proof.sh [--expect-hw\|--expect-firewall-probe]` | (B) | Re-read the firmware-update-log SRAM0 proof beacons without reflashing. Use this after the probe or dual-M55 run to prove what the silicon actually did; the firewall mode decodes PASS/FAIL and exits non-zero if HE changed the MRAM log partition. |
| `ram-run.sh <build-dir> [sleep_ms] [size] [preload]` | **C** | **RAM-run** an ITCM image (no MRAM write): the load base is DERIVED from the LOAD segment with the lowest `p_paddr` among segments with nonzero `p_filesz` (`readelf -l`) — NOT just the first LOAD segment (an ITCM-retargeted link's first LOAD segment is often a zero-FileSiz `.bss` in DTCM) and not hard-coded `0x0` — an app that hard-codes a slot0 `CONFIG_FLASH_LOAD_OFFSET` still links at a non-zero ITCM address and is loaded there. `loadbin` to that base, `setpc <entry>` (thumb-bit cleared), `go`, sleep, halt, dump + ASCII-decode the RAM console. **Refuses (exit 5)** if the derived base is `>= 0x80000000` (slot0/MRAM-linked). **Refuses (exit 6)** if the derived base isn't `0x0`, the ITCM global alias (`0x50000000`/`0x58000000`), or SRAM (`0x02xxxxxx`) — catches picking the wrong LOAD segment before it splats RAM. Optional `preload` JLink file runs after halt / before loadbin (e.g. clear a SoC integration reg). |
| `erase-storage.sh [--dry-run]` | **D** | **PROVISIONING (#1430) — erase the customer storage window before a SoM ships**, so the module does not leave manufacturing carrying a previous app's image where the customer's first NVS write lands (#1334 measured ~110 KiB of stale app image there). **[BENCH-VERIFIED 2026-08-30]** — run once against a real module (off-labgrid E1M-AEN801, `AE822FA0E5597LS0`), `verify successful`, cold power-cycle confirmed `u VB` on the `ALP-HE` row (ATOC band untouched). Still confirm the DPIDR gate and re-read the transcript on each subsequent unit — one bench run is not a standing guarantee against a different module. The window is DERIVED from `metadata/e1m_modules/E1M-AEN801.yaml`'s `memory_map:` (today `0x80560000`, 96 KiB) and refused (**exit 5**) unless it ends exactly where the SE-owned `atoc` band begins — an overshoot lands in the live ATOC and drops the board to `No ATOC`. **The erased value on this MRAM is `0x00`, NOT `0xFF`** (#1430: `write_block_size=16 erase_value=0x00`), so the pattern written is `/dev/zero`, which doubles as the `verifybin` reference; a J-Link `erase` does **not** clear MRAM on this part, hence `loadbin` through the part-number device profile. Same gates as `flash-jlink.sh`: **exit 4** (DPIDR says this is not the AEN E8), **exit 2** (part profile could not connect), **exit 3** (verify failed, or no verify result at all). Does **not** reset or boot the board — cold power-cycle by hand afterwards and confirm the SE still finds its ATOC. `--dry-run` prints the derived window and CommanderScript without opening a probe. |
| `reread.sh <build-dir> [size]` | (B) | Re-read `ram_console_buf` over SWD with no reflash — attach generic device, halt, `mem8`, ASCII-decode. |
| `flash-all-flowd.sh [app ...]` | **D** | Batch Flow D over a list of apps (argv, else `apps.txt`). Strictly serial (one board / one probe); scrapes each app's `RESULT` line into `/tmp/flowd-batch-summary.txt`, printed as `BATCH SUMMARY` at the end. The batch continues past a failed app **only because every child exit is captured explicitly** — `rc=0; x=$(...) \|\| rc=$?`, never a bare `x=$(...)`, which under this script's `set -e` takes the substitution's status and aborts the whole batch before the summary is ever printed. Summary labels: `SKIP (no build)`; `FLASH-UNVERIFIED` (`flash-jlink.sh` exit 3 — verify failed or never ran); `FLASH-FAILED` (exit 2 — probe/target connect); `FLASH-ABORTED (wrong probe)` (exit 4 — DPIDR mismatch); `FLASH-OK-READBACK-FAILED` (exit 7 — flash+verify succeeded, `flash-jlink.sh`'s own post-boot read did not connect); `FLASH-ERROR (exit N)` for any other status; `CONSOLE-READ-FAILED (exit N)` when this script's own post-boot RAM-console read cannot connect. On a non-zero flash exit it also dumps the last 20 lines of the captured `flash-jlink.sh` log, because the 6-line display grep cuts off before the verify gate's own diagnostic. |
| `apps.txt` | — | Default app list for the batch runner (one build-dir name per line). |
| `aen-bench-shared.conf` | — | Generic Kconfig fragment (all four flows): RAM-console observability (the app UART is not on USB on this bench) + `CONFIG_DCACHE=n`. No link-offset override. Add via `-DEXTRA_CONF_FILE=...`. |
| `aen-flowc-itcm.conf` | **C** | Flow-C-ONLY Kconfig HALF of the ITCM retarget: `CONFIG_USE_DT_CODE_PARTITION=n` + `CONFIG_FLASH_LOAD_OFFSET=0x0`, forcing the link base back to ITCM `0x0` for a RAM-run. Layer on top of `aen-bench-shared.conf` via `-DEXTRA_CONF_FILE=...`; never use on a Flow A/D (production MRAM) build — it silently relocates a slot0-by-design app to `0x0`. **Needs the overlay half below too** — the conf fragment alone still links into MRAM. |
| `aen-flowc-itcm.overlay` | **C** | Flow-C-ONLY devicetree HALF of the ITCM retarget: `zephyr,flash = &itcm;` (path-ref form, not `<&itcm>`) + `/delete-property/ zephyr,code-partition;`. Apply via `-DEXTRA_DTC_OVERLAY_FILE=...`, layered alongside `aen-flowc-itcm.conf`. Never use on a Flow A/D build. |

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
| `JLINK_SN` / `JLINK_SERIAL` | *(none)* | Optional SEGGER probe serial selector; set this on benches with multiple J-Links. |
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
recipe as `flash-run.sh`. The runner auto-detects the ATOC shape from the
build's own reset vector: an ITCM-linked app stages the `loadAddress
0x58000000` (M55-HE) config `flash-run.sh` uses; a slot0-XIP app (below)
stages a standalone `mramAddress` config at its OWN core's disjoint
slot0 window instead (#1069: M55-HE `0x80010000`, unchanged; M55-HP
`0x802b0000`, moved off the old shared window -- see
[`scripts/aen_atoc.py`](../../aen_atoc.py)). `jlink` stays the
debug/attach runner.

```sh
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he <your-app> --sysbuild
export SETOOLS_DIR=<...>/app-release-exec-linux   # license-gated; not shipped
export SE_UART=<your-serial-device>               # the SE-UART (host-specific)
west flash                                        # -> alif_flash -> SETOOLS
```

The runner reads `SETOOLS_DIR` / `SE_UART` (the same env vars these helpers
use), or takes `--setools-dir` / `--se-uart`. A slot0-linked app that overflows
ITCM (mramAddress `0x80010000` on M55-HE, `0x802b0000` on M55-HP since #1069)
provisions the same way — bench-proven 2026-07-19 on the M55-HE window (both
cores shared it then), a single `app-write-mram -p` run over the SE-UART
burns both the standalone app blob at its slot0 address and the signed ATOC
in one pass. The
two-blob `scripts/bench/aen/flash-jlink-mramxip.sh` (Flow D) helper remains
available as a faster SWD-only alternative that skips the SE-UART reset
race, not a requirement.

**One-off setup.** `alif_flash` is **not** in upstream Zephyr's `runners`
package — alp-sdk ships it and surfaces it through `zephyr/module.yml`'s
`runners:` list (no edit to the pinned Zephyr tree). Also `pip install fdt`
once: `app-gen-toc` needs the `fdt` Python package, which is not a Zephyr
requirement (the runner warns if it is missing).
<!-- cross-platform-lint:resume -->
