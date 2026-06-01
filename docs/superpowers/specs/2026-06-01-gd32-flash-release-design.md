# GD32 Bridge — Lean Flashable Release + Flashing SOP — Design

**Date:** 2026-06-01
**Status:** Approved design; pending implementation plan.
**Scope:** `firmware/gd32-bridge/` build/release wrapper, `scripts/flash_backends/`
+ `scripts/openocd/`, and `docs/`. No change to the transport HAL *logic* —
the `feat/gd32-transport-bringup` firmware is integrated as-is and treated as
the release foundation.

## Problem

The GD32G553 supervisor MCU on the E1M-X V2N / V2N-M1 SoMs ships (per the docs)
"pre-flashed by ALP," but in practice:

- The real firmware that makes a flashed GD32 answer over the bus — the SPI1 +
  I2C0 **slave transport HAL** — lives **only on the unmerged branch
  `feat/gd32-transport-bringup`** (3 commits), not on `dev`. `dev`'s
  `firmware/gd32-bridge/` is a scaffold whose HW-touching opcodes return
  `NOTIMPL`.
- There is **no single, public "flash a GD32 on your bench" SOP**. The steps are
  scattered across `docs/gd32-bridge.md`, `firmware/gd32-bridge/README.md`, and
  `docs/firmware-quickstart.md`, with the operational bench/HiL procedure held
  in `alplabai/alp-sdk-internal` (`HIL-PLAN.md`, `docs/internal-test-playbook.md`).
- The GD32 flash backend `scripts/flash_backends/swd_v2n_host.py` references a
  `scripts/openocd/cmsis-dap.cfg` that **does not exist**, and assumes an
  OpenOCD `target/gd32g553.cfg` that mainline OpenOCD almost certainly does not
  ship (GD32G5 is a new Cortex-M33 part). So the documented flash command would
  fail out of the box.
- The firmware has **never been run on silicon** (`alp-sdk-internal` open-TBD:
  validate IDCODE `0x6BA02477` against a real GD32G553).

## Goal

A repeatable, **documented in `alp-sdk`** path that takes the GD32 bridge
firmware from source to a flashable image and onto a chip:

1. The transport-bringup firmware is integrated into `dev` so there is one
   canonical, buildable firmware.
2. A **v0.1.0 candidate** full-flash image (`gd32-bridge.hex/.bin`,
   transports on, OTA inert) builds clean with the project toolchain and is
   checksummed.
3. The `alp-sdk` flash tooling actually works: the OpenOCD configs the backend
   references exist, and a primary SWD probe path is picked and documented.
4. A public **`docs/gd32-flashing.md`** SOP covers build → wire SWD → flash →
   verify, mirroring the *generic* parts of the internal bench procedure.

The physical flash + on-silicon verification is a **bench** activity (this
workstation has no `openocd`/`pyocd`/probe); the SOP and a handoff note prepare
it but do not claim it done.

## Non-goals

- **OTA Path-A** (`feat/gd32-ota-path-a`): deferred. It re-stacks on top once
  transports are silicon-validated. The OTA flash layout (partitioned
  bootloader + A/B slots) is explicitly HIL-pending / brick-risk and stays out.
- **Modifying transport HAL logic.** We ship the branch as the foundation; a
  deep line-by-line review of the transports is a separate concern.
- **Committing a "shipping" prebuilt binary.** Because the firmware is unproven
  on silicon, v0.1.0 is a *candidate*; no `firmware/gd32-bridge/prebuilt/` blob
  is enshrined this round (revisit post-silicon).
- **Host-driven SWD reflash (`chips/gd32_swd/`, protocol §10 Path B):** not
  wired this HW rev; external SWD probe only.

## Current state (verified 2026-06-01)

| Fact | Value |
| --- | --- |
| `feat/gd32-transport-bringup` | 3 commits (`c6b45e8`, `865efe8`, `fc38327`) off merge-base `0977435` |
| Adds | `hal/transport_hw_gd32.c`, `hal/bridge_board_config.h`, `src/transport.h`; wires `transport_spi.c` / `transport_i2c.c` / `main.c` |
| SPI1 slave (V2N) | NSS `PA8`, SCK `PA9`, MISO `PA10`, MOSI `PB15`, AF5; RX/TX on DMA0 CH1/CH2; CS mirrored to EXTI8 |
| I2C0 slave (V2N) | SCL `PA15`, SDA `PB9`, AF4; 7-bit addr `0x70`; timing derived at runtime from live APB1 |
| Default build | single full-flash image @ `0x08000000` via `toolchain/gd32g553_flash.ld`; OTA absent/inert |
| `firmware-version.txt` | `0.1.0` |
| Backends build clean? | Branch claims yes (gcc 13.x). **To be verified locally** — execution step #1 |
| Local toolchain | `arm-none-eabi-gcc 13.3.Rel1`, `cmake`, `make` present; `ninja`, `openocd`, `pyocd` **missing** |
| Vendor submodule | `vendors/gd32_firmware_library/upstream` populated, v1.5.0 |
| CI | `.github/workflows/pr-gd32-bridge-build.yml` builds stub + gd32 on PR/push to `main` (gcc 10.3), uploads gd32 artifacts |
| Flash backend | `scripts/flash_backends/swd_v2n_host.py` (openocd/pyocd, base `0x08000000`); orchestrated by `west alp-flash --helper gd32_bridge` |
| External SWD header | `SWDIO PA13` / `SWCLK PA14` + `NRST` (per `metadata/chips/gd32g553.yaml`) |
| Expected IDCODE | `0x6BA02477` |

## Design

### 1. Branch integration

- Branch off current `dev`: **`feat/gd32-flash-release`**.
- `git merge origin/feat/gd32-transport-bringup` (merge commit — preserves the
  3 commits' history). Before merging, print the conflict surface
  (`git diff 0977435..dev -- firmware/gd32-bridge/ docs/gd32-bridge.md`) to
  confirm it is near-empty; resolve any trivial overlap (most likely the
  brand-casing sweep touching comments/README).
- Tooling + docs (sections 3–4) commit onto the same branch.
- Integrate by **merging `feat/gd32-flash-release` into `dev`** (user's chosen
  default; a GitHub PR is the alternative if review-on-platform is wanted).

### 2. Build-verify + release artifact

- Build **both backends** with the pinned toolchain file:
  ```
  cmake -B build/stub -S firmware/gd32-bridge \
    -DCMAKE_TOOLCHAIN_FILE=$PWD/firmware/gd32-bridge/toolchain/arm-none-eabi.cmake \
    -DBRIDGE_HAL_BACKEND=stub  && cmake --build build/stub
  cmake -B build/gd32 -S firmware/gd32-bridge \
    -DCMAKE_TOOLCHAIN_FILE=$PWD/firmware/gd32-bridge/toolchain/arm-none-eabi.cmake \
    -DBRIDGE_HAL_BACKEND=gd32  && cmake --build build/gd32
  ```
- `gd32` build emits `build/gd32/gd32-bridge.elf/.hex/.bin`. Report
  `arm-none-eabi-size` (sanity vs 512 KB flash / 96 KB SRAM).
- Record `sha256` + byte size of the `.bin`/`.hex` in the SOP (and/or
  `firmware/gd32-bridge/CHANGELOG`-style note). **No committed prebuilt blob**;
  rely on the CI artifact for distribution until silicon-validated.
- **gcc-version risk:** CI uses gcc 10.3, local 13.3. There is no `-Werror`, so
  new `-Wshadow`/`-Wpedantic` diagnostics will not fail the build, but any new
  warnings are surfaced in the build report. If the `gd32` backend does **not**
  compile clean on 13.3, fixing that is the first execution task, ahead of
  tooling/docs.

### 3. Flash tooling (J-Link primary)

- **Primary probe: J-Link** (confirmed on the bench), connected **directly to
  the GD32's own SWD header** (`PA13`/`PA14`/NRST). The V2N host is **not** in
  the flashing loop: the host-driven SWD path (Renesas `P70`/`P71`→GD32, the
  `chips/gd32_swd/` idea) is **not wired on this HW revision**. The robust
  GD32G5 path is SEGGER's own tooling — `JLinkExe -device GD32G553MEY7TR` — which has
  built-in GD32G553 flash support and **sidesteps the open question of whether
  mainline OpenOCD can flash GD32G5** (it likely cannot stock). The flash
  backend is the generic **`swd_probe`** (an external-SWD-probe backend).
- **Extend `scripts/flash_backends/swd_v2n_host.py`** with a direct J-Link path
  alongside its existing `openocd` / `pyocd` options, keeping the same backend
  name + `flash_args` contract so `west alp-flash --helper gd32_bridge` is
  unchanged. The backend prefers `JLinkExe` when present, falling back to
  `openocd` / `pyocd`; `requires` becomes any-of `["JLinkExe", "openocd",
  "pyocd"]`. The J-Link path programs the image at base `0x08000000` and
  resets-and-runs (a small generated J-Link command script).
- **Alternative path (documented, not primary):** add **`scripts/openocd/`**
  with the interface config(s) the backend names (`cmsis-dap.cfg`, `jlink.cfg`)
  plus a GD32G553 target/flash-bank config, for CMSIS-DAP / ST-Link / OpenOCD
  users. The uncertain OpenOCD GD32G5 flash-driver support now affects only
  this alternative, **not** the critical path.
- Nothing here is runnable on this workstation (`JLinkExe` / `openocd` / `pyocd`
  + probe all absent), so the flash path is **bench-validated**, not asserted
  working — though J-Link's GD32G553 device support is well-established.

### 4. Public flashing SOP + internal mirror

- New **`docs/gd32-flashing.md`** — the missing single SOP:
  1. **Prereqs** — `arm-none-eabi-gcc 13.x`, `cmake`, submodule init, a SWD
     probe (**J-Link** primary + SEGGER J-Link software; CMSIS-DAP / ST-Link
     alternative).
  2. **Build** — exact `gd32`-backend commands → `gd32-bridge.hex`.
  3. **Wire** — GD32 programming header (`SWDIO PA13` / `SWCLK PA14` / `NRST`),
     probe connections, target power.
  4. **Flash** — both `west alp-flash <app> --helper gd32_bridge` and a
     standalone one-liner (`JLinkExe -device GD32G553MEY7TR`, or `openocd`) at base
     `0x08000000`.
  5. **Verify** — read IDCODE (expect `0x6BA02477`); then host-side
     `PING` / `GET_VERSION` via `examples/v2n/v2n-gd32-bridge-ping`.
  6. **Gotchas/recovery** — first-silicon expectations; the board-config's
     on-silicon validation points (CS-to-first-SCK setup vs EXTI+DMA-arm
     latency; I2C clock-stretch window); **validate SPI first** (point-to-point,
     clean) because the **I2C management path may be blocked by the known
     wedged BRD_I2C bus on the V2M101 bench board**.
- **Mirror generic content** from `alp-sdk-internal` (`HIL-PLAN.md`,
  `docs/internal-test-playbook.md`); rig-specific bits stay internal.
- Cross-link the new SOP from `docs/gd32-bridge.md` (Flashing section),
  `firmware/gd32-bridge/README.md`, and `docs/firmware-quickstart.md`.

## Verification: provable here vs. bench boundary

**Provable on this workstation (acceptance for this round):**

- Both backends build on gcc 13.3 with no errors; size report captured.
- `gd32-bridge.hex/.bin` produced and `sha256`/size recorded.
- `python3 firmware/gd32-bridge/tests/gen_protocol_vectors.py --check` passes
  (protocol layer unchanged / consistent).
- `python3 scripts/west_commands/alp_flash.py <app> --helper gd32_bridge
  --dry-run` prints the expected flash command against the extended backend
  (J-Link path).
- Markdown links in the new/edited docs resolve.

**Bench-only (documented for handoff, NOT done this round):**

- Physical flash via SWD probe; IDCODE read returns `0x6BA02477`.
- Host `PING` / `GET_VERSION` round-trip over **SPI** (then I2C, bus permitting).
- The board-config's on-silicon validation points.

## Risks & assumptions

- **A1 — gcc 13.3 vs CI 10.3.** Assumed clean (no `-Werror`); verified in
  step #1. Mitigation: surface/triage any new warnings; fix compile errors
  before proceeding.
- **A2 — Flashing path.** Primary is **J-Link** (`-device GD32G553MEY7TR`), which
  has established GD32G553 flash support, so OpenOCD's uncertain GD32G5 support
  is demoted to the *alternative* path only. The flash command is
  **bench-validated** (no probe/tools on this workstation), not proven here.
- **A3 — Merge is clean.** Assumed `dev` did not touch `firmware/gd32-bridge/`
  meaningfully since `0977435`; verified by the pre-merge conflict-surface diff.
- **A4 — Firmware is paper-correct only.** v0.1.0 is a **candidate**; "verified
  working" requires the bench. The SOP states this plainly.

## Deliverables

| Path | Action |
| --- | --- |
| `firmware/gd32-bridge/**` | Integrated from `feat/gd32-transport-bringup` (merge, no logic edits) |
| `scripts/flash_backends/swd_probe.py` | **Rename** `swd_v2n_host.py`→`swd_probe.py` (method key + class) **and extend** with a direct J-Link (`JLinkExe`) path |
| SoM presets ×4 + preset schema + tests + `alp_flash` comment + `v0.6-tbd` | Update `flash_method` `swd_v2n_host`→`swd_probe`; also fix the `gd32_bridge` `firmware_path` (`gd32_bridge.bin`→`gd32-bridge.bin`) |
| `scripts/openocd/` | **New** — alternative-probe (CMSIS-DAP / ST-Link / OpenOCD) configs |
| `docs/gd32-flashing.md` | **New** — the public flashing SOP |
| `docs/gd32-bridge.md`, `firmware/gd32-bridge/README.md`, `docs/firmware-quickstart.md` | Cross-link the SOP |
| (build output) | `gd32-bridge.hex/.bin` checksummed; CI artifact relied on for distribution |

## Out of scope / deferred

- OTA Path-A integration (`feat/gd32-ota-path-a`) — follow-up after silicon.
- `GET_BUILD_ID` SHA stamping (CMake TODO; reports `+0000000` today) — optional
  stretch.
- Committed prebuilt release blob — revisit post-silicon.
- Physical bench flash + silicon validation — separate bench task; this design
  produces the artifact, tooling, and SOP that make it a checklist.

## References

- `docs/gd32-bridge-protocol.md` — wire protocol (incl. §10 flashing paths).
- `docs/gd32-bridge.md` — firmware-tree overview (Build + Flashing).
- `metadata/chips/gd32g553.yaml` — pin map, SWD header, families.
- `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv` — GD32 pad allocation.
- `scripts/flash_backends/swd_v2n_host.py`, `scripts/west_commands/alp_flash.py`.
- `alp-sdk-internal/HIL-PLAN.md`, `alp-sdk-internal/docs/internal-test-playbook.md`
  (generic bench procedure to mirror; rig-specific content stays internal).
- Project memory: `project-e1m-v2m-bench-handoff` (wedged BRD_I2C on V2M101).
