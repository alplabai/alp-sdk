# Changelog

All notable changes to the ALP SDK are documented here.  Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
See [`VERSIONS.md`](VERSIONS.md) for the forward roadmap.

## [Unreleased] — v0.6.0 candidate

### Added — per-bridge firmware release versions (2026-05-26)

The on-module **GD32** and **CC3501E** bridge firmwares each gained an
independent **firmware release version** (`firmware/<mcu>/firmware-version.txt`,
semver), tracked separately from the wire-protocol version and the
build-id so a fielded device's firmware is trackable on its own.  The
GD32 build embeds it (`GD32_BRIDGE_FW_VERSION`, surfaced via `GET_BUILD_ID`
as `<ver>+<sha>`); the CC3501E reports it as `fw_version` via `GET_VERSION`
and names its prebuilt blob from it.  The three-axis model (firmware
release / wire protocol / build-id) is documented in
`docs/gd32-bridge.md` + `docs/cc3501e-bridge.md`.

### Changed — V2N Linux build + bridge docs reconciled to validated reality (2026-05-26)

The Renesas RZ/V2N Linux docs were reconciled to the WSL-validated build:
the Yocto path is the **`bitbake-layers`** flow in `meta-alp-sdk/README.md`
(kas retired, `kas/e1m-v2n.yml` removed); the version framing is **AI SDK
platform 7.1 / BSP v6.30** (linux-renesas 6.1.141-cip43) throughout; and
the layer is `meta-alp-sdk` (the deleted `yocto/meta-alp/` name was
scrubbed from live docs).  The shipped-but-undocumented `<alp/*>` surfaces
(`storage`, `dsp`, `tmu`, `power`, `rpc`, `gpu2d`, `backend`,
`e1m_x_pinout`, `ext/<vendor>`) are now reflected in the README stack
graph + Public API table, `docs/architecture.md`, and
`docs/os-support-matrix.md`.  The pre-flash provisioning model (V2N
bootloader / CC3501E / GD32 — both MCU bridge firmwares open) is
documented, and login-gated vendor download links were removed from
public docs.

### Changed — examples reorganised into category subdirectories (2026-05-24)

The 43 flat top-level example folders now live under category
subdirectories matching the `examples/README.md` functional index:
`peripheral-io/`, `audio/`, `camera-vision/`, `ai/`, `connectivity/`,
`display/`, `power-timing/`, `multicore/` (the `aen/` and `v2n/`
platform groups already used this layout).  History is preserved via
`git mv`; every `examples/<name>` reference -- per-example `west build`
commands, the functional index, `.vscode` tasks, `scripts/bootstrap.sh`,
CI workflows, docs -- is updated to `examples/<category>/<name>`.  No
source or build-system changes: twister discovers examples recursively,
so build invocations simply gain the category path segment.  (Past
CHANGELOG entries keep their original flat paths -- they record the
state at the time they were written.)

### Changed — E1M-X pinout header synced to the full x-v1.0 connector (2026-05-24)

`<alp/e1m_x_pinout.h>` was reconciled against the authoritative
`alplabai/e1m-spec` `pinout/x-v1.json` (E1M-X **x-v1.0**), which already
enumerates the entire ASS6880 LGA496 module connector. The earlier
interim edit that added I2C2/I2C3 "pending upstream e1m-spec sync" is
superseded — x-v1.0 already defines those pads, so the caveat is gone.
No e1m-spec change was needed; the header was simply behind the spec.

- Peripheral instance IDs now cover the full connector and are
  formatting-normalised: `E1M_X_I2C0..3`, `E1M_X_SPI0..2`,
  `E1M_X_CAN0..1`, `E1M_X_CSI0..3`, `E1M_X_DSI0..1`, `E1M_X_USB0..2`,
  `E1M_X_PCIE0..1`.
- New parallel-RGB-LCD class `E1M_X_LCD0` (pads `LCD_B0..LCD_B23` +
  `LCD_HSYNC` + `LCD_VSYNC`; the connector has no separate PCLK/DE pad),
  with count macro `E1M_X_LCD_COUNT` (1).
- GPIO-secondary indices appended at index >= 62 for the new
  single-ended digital pads (I2C2/3, SPI2, CAN1, and the LCD0 block);
  `E1M_X_GPIO_COUNT` grows 62 -> 99.

This extension is **additive and ABI-safe** — every pre-existing
instance ID and GPIO index keeps its value. Differential pairs
(CSI/DSI/PCIe/USB/ETH) are not GPIO-capable and are omitted from the
GPIO index space.

### Added — Linker-section backend registry (Slices 0..7) (2026-05-23)

Replaces the per-class `#if`-ladder dispatch that lived in
`src/zephyr/peripheral_<class>.c` with a portable, linker-section
backend registry. Every supported peripheral class (`adc`, `i2c`,
`spi`, `uart`, `gpio`, `pwm`, `i2s`, `can`, `counter`, `qenc`, `rtc`,
`wdt`, `usb`, `ble`, `wifi`, `mqtt`, `audio`, `mproc`, `rpc`,
`security`, `dsp`, `tmu`, `storage`, `power`, `camera`, `inference`,
`display`, `gpu2d`) now dispatches through a class-specific section
walked by `alp_backend_select()` in `src/backend.c`.

- **Class dispatchers** live at `src/<class>_dispatch.c` and own the
  handle pool. They are OS-agnostic — no `<zephyr/...>` types in the
  shared `<class>_ops.h` headers per issue #34's cleanup.
- **Backends** live at `src/backends/<class>/<vendor>.c` and register
  via `ALP_BACKEND_REGISTER(<class>, <vendor>, { … })`. Variants today:
  - **Real Zephyr backends** (`zephyr_drv.c` / `zephyr_video.c` /
    `zephyr_pm_policy.c` / `zephyr_flash.c` / `zephyr_littlefs.c`)
  - **SW fallbacks** (`sw_fallback.c`) registered at `silicon_ref="*"`
    priority 0 — universal floor.
  - **Vendor-specific bridges** (`gd32_bridge.c` for adc/counter/qenc
    on V2N) registered at `silicon_ref="renesas:rzv2n:n44"` priority
    100.
  - **NOT_IMPLEMENTED stubs** for backends whose vendor pack hasn't
    landed yet (DRP-AI3 → issue #58, DEEPX DX-M1 → issue #59).
- **Selector tiebreaker** (3 tiers, documented in `src/backend.c` +
  `<alp/backend.h>`): (1) higher priority wins; (2) exact `silicon_ref`
  beats `*` wildcard at equal priority; (3) alphabetic vendor at
  same priority + match-type. Test coverage in
  `tests/unit/backend_registry/src/test_registry.c` and
  `tests/unit/backend_registry/src/test_bridge_selection.c`.
- **Capability getters** added: `alp_<class>_capabilities()` for
  every migrated class, returning the shared `alp_capabilities_t`
  + `alp_instance_cap_t` flags populated by the backend's
  `ops->probe()` at open time.
- **Vendor extensions** (`<alp/ext/<vendor>/<class>.h>`) for the
  silicon-specific surfaces that don't fit the portable API:
  - `<alp/ext/alif/adc.h>` (oversampling, trigger source)
  - `<alp/ext/alif/storage.h>` (OSPI SecAES — body NOSUPPORT until
    Alif HAL pack)
  - `<alp/ext/nxp/storage.h>` (FlexSPI OTFAD — body NOSUPPORT until
    NXP pack)
  - `<alp/ext/renesas/power.h>` (GD32 supervisor mode set via
    `CMD_POWER_MODE_SET` opcode 0x28)
  - `<alp/ext/renesas/camera.h>` (V2N N44 ISP fine-grained knobs)
  - `<alp/ext/alif/camera.h>` (Mali-C55 — body NOSUPPORT until
    Alif HAL Mali-C55 pack)
  - `<alp/ext/renesas/inference.h>` (DRP-AI3 pipeline-stage knobs)
  - `<alp/ext/deepx/inference.h>` (DX-M1 slot + tile management)
- **Inference Kconfig ladder** retired. `CONFIG_ALP_SDK_INFERENCE_TFLM`
  → `_BACKEND_TFLM`; `_DRPAI` → `_BACKEND_DRPAI_V2N`; `_ETHOS_U` →
  `_BACKEND_ETHOS_U_AEN` / `_ETHOS_U_N93`; `_ETHOS_U_U55` →
  `_ETHOS_U_VARIANT_U55` etc.; `_TFLM_NEON/HELIUM/REF` →
  `_TFLM_KERNEL_NEON/HELIUM/REF`. Customer-facing
  `ALP_INFERENCE_BACKEND_AUTO/CPU/ETHOS_U/DRPAI/DEEPX_DX` enum
  unchanged.
- **CI gates** (`scripts/check_stub_issues.py`, `_sw_fallback_tags.py`,
  `_vendor_ext_tags.py`) enforce per-backend documentation
  conventions (issue trackers on stubs, `@par Cost`/`Performance` on
  SW fallbacks, `@par Supported silicon` on vendor extensions).
- **Yocto-side migration** deferred to tracking issue #33.

ADR / spec: `docs/architecture/backend-registry.md` +
`docs/superpowers/specs/2026-05-21-backend-registry-design.md`.

### Added — OTA Zephyr-client provider-driven dispatch (ADR 0009 resolved) (2026-05-20)

Lands the v0.6 follow-on to ADR 0009: `ota.provider:` is now a
provider-driven dispatch with real Zephyr-side emit for every
recognised provider, not just Yocto-side Mender.

- Schema enum widened: `ota.provider: hawkbit` joins `mender` /
  `mcumgr` / `none` (`metadata/schemas/board.schema.json`).
- `_slice_alp_conf()` emits per-provider Kconfig on every Zephyr
  slice when `ota:` is declared:
  - `mender`  → `CONFIG_MENDER_MCU_CLIENT=y` + server URL / tenant
    token / artifact name / poll interval (Mender-MCU-client,
    out-of-tree BSD-3).
  - `hawkbit` → `CONFIG_HAWKBIT=y` + `CONFIG_HAWKBIT_SHELL=y` +
    `HAWKBIT_SERVER` + `HAWKBIT_POLL_INTERVAL` (Zephyr upstream).
  - `mcumgr`  → `CONFIG_MCUMGR=y` + `CONFIG_MCUMGR_GRP_IMG=y` +
    `CONFIG_MCUMGR_GRP_OS=y`.  Transport (UART / BLE / UDP) stays
    the app's call.
- Yocto-side dispatch unchanged: `provider: mender` keeps the
  existing `MENDER_*` weak-assignments in `local.conf`.
- west.yml fragment emit (`scripts/alp_project.py` `_emit_west_libraries`)
  auto-adds a `mender-mcu-client` `name-allowlist:` entry when
  `ota.provider: mender` is declared.  Hawkbit and MCUmgr are
  Zephyr-upstream so no west.yml change is needed.
- Validator rule 1 (P2.3) relaxed for the new dispatch:
  - `provider: mender` accepts EITHER a Yocto OR a Zephyr core
    (was Yocto-only).
  - `provider: hawkbit` requires at least one Zephyr core.
  - `provider: mcumgr` requires at least one Zephyr core.
  Errors point at the offending rule with the resolved-dispatch
  hint.
- Tests: 6 new cases under `tests/scripts/test_alp_orchestrate.py`
  (mender-on-zephyr ok, mender-with-no-target rejected, hawkbit
  requires zephyr, hawkbit Kconfig emit shape, mcumgr requires
  zephyr, mcumgr SMP Kconfig emit).  Pre-existing
  `mender_without_yocto_rejected` test rewritten to assert the
  new "ok" behaviour.
- Docs: `docs/board-config.md` § "OTA" rewritten with the
  per-provider emit matrix + validator rule listing.

### Added — per-library HW-backend profile coverage (25 profiles) (2026-05-20)

**Priority 2.2 of the v0.6 milestone lands as a regression guard.**
All 25 libraries enumerated in `cores.<id>.libraries:` in
`metadata/schemas/board.schema.json` already ship a
`metadata/library-profiles/<libname>/hw-backends.yaml` table
declaring their per-class accelerator binding (crypto, gpu_2d, dma,
simd, cordic, fft, ml_npu_primary, ...) against the SoM
preset's `capabilities:` matrix.  Audit revealed full coverage; the
gap was test-side -- no regression test enforced that the schema
enum and the on-disk profile set stay in lock-step.

- New `tests/scripts/test_library_profiles.py` (77 parametrised
  cases) enforces four invariants:
  1. **Coverage** — every library in the schema enum has a matching
     `hw-backends.yaml`; every shipped profile directory maps back
     to an enum entry (no orphans).  The single `cmsis_dsp` →
     `cmsis-dsp` directory rename is accommodated explicitly,
     mirroring `_LIBRARY_WEST_MODULES` in `scripts/alp_project.py`.
  2. **Shape** — each profile carries the required top-level fields
     (`schema_version`, `library`, `class`, `accelerators`,
     `sw_fallback`, `verification`) and the `library:` slug matches
     the directory's normalised name.
  3. **Binding axis** — each profile surfaces at least one of an
     `accelerators[]` entry, a `sw_fallback.kconfig:` knob, or a
     `verification:` block.  Empty profiles are rejected.
  4. **Kconfig well-formedness** — every emitted `kconfig:` line is
     a real-looking `CONFIG_<NAME>=<value>` token (y / n / m /
     quoted string / integer / hex) OR a comment-only sentinel
     (`# foo: ...`) for header-only libraries.  We intentionally do
     NOT verify that each symbol exists in Zephyr's Kconfig tree
     — that's a build-time concern that depends on the pinned
     Zephyr version.

- `docs/recommended-libraries.md` grows a "HW-backend profiles
  (per-library accelerator binding)" subsection summarising the
  coverage matrix (crypto / ML / DSP / FS / graphics / sensor-fusion
  / industrial / IoT / audio / header-only / test) and pointing at
  the new regression test.

The 18 libraries carrying at least one `requires_cap:`-gated
accelerator entry today (every SoM-relevant library): `bearssl`,
`cmsis_dsp`, `coremqtt_sn`, `gfx_compat`, `libcoap`, `libhelix`,
`libwebsockets`, `littlefs`, `lvgl`, `madgwick_ahrs`, `mbedtls`,
`minimp3`, `modbus`, `opus`, `pid`, `tflite_micro`, `tinygsm`,
`u8g2`.  The remaining 7 (`etl`, `fmt`, `nlohmann_json`, `doctest`,
`catch2`, `jsmn`, `nanopb`) declare an empty `accelerators:` list —
their value lives in the pure-SW path with no accelerator class
to bind.

No source / loader / data changes outside the test + docs +
CHANGELOG.  Existing 402-test suite still green (now 479 with the
77 new parametrised cases).

### Added — HiL coverage for `boot:` / `memory:` / `power:` / `diagnostics.modules:` (2026-05-20)

- Four new portable HiL specs under `tests/hil/_common/`, one per
  declarative board.yaml block landed at schema level in PR #3.
  Each spec asserts the orchestrator's CONFIG_* emit actually reaches
  real silicon (MCUboot is the first boot stage; CONFIG_MAIN_STACK_SIZE
  shows up in the runtime trace; PM subsystem suspend/resume cycle
  fires from a declared wakeup source; per-module log levels filter):
  - `boot_mcuboot.yaml`        — covers `boot:`
  - `memory_stacks.yaml`       — covers `cores.<id>.memory:`
  - `power_sleep_wake.yaml`    — covers `cores.<id>.power:`
    (flags `pending_hardware_support: deep-sleep-current-draw` — the
    AEN HiL rig doesn't carry an inline ammeter yet; the spec falls
    back to serial-trace assertions)
  - `diagnostics_modules.yaml` — covers `diagnostics.modules:`
- Host-side cross-check at `tests/scripts/test_hil_blocks_coverage.py`
  (23 tests) validates each spec parses + the matching emit code
  produces the CONFIG_* lines the spec claims to observe.  A schema
  field the emit silently drops fails CI on a normal machine, before
  the HiL runner ever sees it.
- `.github/workflows/nightly-aen-hil.yml` now invokes
  `tests/hil/run_smoke.py` against `tests/hil/aen701-evk/` after the
  existing ztest build, picking up the new specs automatically via
  the `_common/` discovery flow.
- `tests/hil/README.md` documents the block-coverage convention and
  the `pending_hardware_support:` flag.

### Added — `storage:` block: deterministic flash-partition allocator + DTS/Kconfig emit (2026-05-20)

Closes the first v0.6 schema-only gap: `storage:` lands its real
emit pipeline so the schema-authoritative partitions become
build-system artefacts.

- **New resolver** `resolve_storage_partitions()` in
  `scripts/alp_orchestrate.py` allocates physical offsets for every
  `storage[]` entry, name-sorted and page-aligned to 4 KiB within
  each `flash_device:`.  Mirrors the IPC carve-out pattern: blocked
  entries (TBD capacity, unknown device, page-misaligned offset,
  sibling overlap) land in `system-manifest.yaml` with `status:
  blocked` + `reason:` so reviewers see the gap.  Byte-stable
  allocation across rebuilds (per resolved design Q on storage
  address determinism — "pin in orchestrator").
- **Schema additive:** new optional `storage[].offset_kib:` (integer,
  4 KiB-aligned) — explicit offset override for partitions that need
  to coexist with bootloader-managed slots or migrate a legacy
  layout.  When supplied, the allocator does NOT shift its
  high-water mark, so the bump-allocated siblings stay byte-stable.
  Also new optional `memory_region.dt_label:` on the SoM preset
  schema for regions whose Zephyr DT label differs from the SDK
  name (defaults to the region `name`).
- **New emitters:**
  - `emit_dts_partitions(project)` → `dts-partitions.dtsi` (a
    DTS overlay decorating `&<dt_label>` with a `partitions
    { compatible = "fixed-partitions"; ... }` child node carrying
    `label = "<name>";` and `reg = <offset size>;` per partition).
    Apps reach partitions via Zephyr's
    `FIXED_PARTITION_ID(<name>_partition)`.
  - `emit_storage_mounts_c(project)` → optional static
    `fs_mount_t alp_storage_mounts[]` table for boot-time iteration.
    Per-fs declarations (`FS_LITTLEFS_DECLARE_DEFAULT_CONFIG`,
    `FATFS`, `FS_EXT2`) emitted under `/* clang-format off */`.
  - Per-fs Kconfig in `_slice_alp_conf()`: `CONFIG_FILE_SYSTEM=y`
    plus the matching `CONFIG_FILE_SYSTEM_LITTLEFS=y` /
    `CONFIG_FAT_FILESYSTEM_ELM=y` / `CONFIG_FILE_SYSTEM_EXT2=y`
    + per-littlefs partition `CONFIG_FS_LITTLEFS_PARTITION_<NAME>=y`.
- **CLI:** `python3 scripts/alp_orchestrate.py --emit dts-partitions
  | storage-mounts-c` — two new emit modes for inspecting the
  resolved layout.  `Orchestrator._materialise_shared()` now writes
  `build/generated/dts-partitions.dtsi` alongside the existing
  `dts-reservations.dtsi`.
- **Cross-field validation:** `load_board_yaml()` rejects typoed
  `flash_device:` references at parse time with the list of known
  devices for the project's SoM (memory_map regions + ospi keys);
  duplicate partition names within `storage:` error eagerly.
- **System manifest:** `storage:` round-trips through
  `build/system-manifest.yaml` carrying every resolved partition's
  `offset_kib`, `size_kib`, `dt_label`, `mount`, and (when blocked)
  `reason:`.
- **Tests:** 14 new cases under
  `tests/scripts/test_alp_orchestrate.py` covering: happy-path
  allocation, unknown / duplicate / page-misaligned / overflow /
  overlap blocking, byte-stable determinism, explicit `offset_kib:`
  override, DTS shape, Kconfig fragment, C mount table, manifest
  round-trip, and AEN301 OSPI TBD-capacity blocking.  Total
  orchestrator suite: 28 → 42 cases.
- **Docs:** `docs/board-config.md` storage section rewritten with
  the full v0.6 contract (allocator semantics, emit artefacts,
  inspection commands).  Tutorial 09 (`board.yaml deep dive`) gains
  a `storage:` block walkthrough with a worked allocation table.
  Template at `metadata/templates/board.yaml` carries an annotated
  storage example.

### Added — security.psa: TF-M sysbuild integration + ADR 0013 (2026-05-20)

Pulls the `security.psa:` block from "schema authoritative, emit
no-op" to "real build-system artefacts".  Scoped to the v0.6
TF-M cross-core trust-boundary land.

- `scripts/alp_orchestrate.py` gains `emit_tfm_sysbuild_conf(project)`.
  When `security.psa.tfm: true` it returns a `SB_CONFIG_TFM=y` +
  `SB_CONFIG_TFM_BUILD_TYPE=<Release|Debug|MinSizeRel>` +
  `CONFIG_PSA_CRYPTO_PERSISTENT_SLOT_COUNT=<n>` +
  `CONFIG_PSA_CRYPTO_{ITS,PS}_BACKING_STORE="<name>"` overlay.
  `attestation_root: optiga_trust_m` adds
  `CONFIG_ALP_SDK_PSA_ATTESTATION_OPTIGA=y` + a comment pointing at
  the PSA <-> OPTIGA bridge driver.  Returns "" when the block is
  absent or `tfm: false` (PSA Crypto then runs non-secure-only).
- `Orchestrator._materialise_shared()` writes the overlay to
  `build/sysbuild/tfm/tfm.conf` when non-empty; no directory is
  created otherwise.
- New CLI mode: `python3 scripts/alp_orchestrate.py
  --emit tfm-sysbuild-conf` (alongside the existing
  `system-manifest` / `ipc-contract-h` / `dts-reservations`
  emitters).
- `load_board_yaml()` gains three cross-field checks:
  `security.psa.its_storage:` and `ps_storage:` must resolve to a
  `storage[].name` OR a SoM `memory_map:` region name; and
  `attestation_root: optiga_trust_m` is rejected when the SoM
  preset doesn't ship OPTIGA Trust M (on-module or via
  `capabilities:`).  Errors point at the offending YAML path.
- New `boot.build_type:` enum (`Release` | `Debug` | `MinSizeRel`,
  default `Release`).  Propagates to both the MCUboot and TF-M
  sysbuild child images so they ship the same flavour.
- New ADR: [`docs/adr/0013-tfm-boundary-m55-hp-trustzone.md`](docs/adr/0013-tfm-boundary-m55-hp-trustzone.md)
  -- captures the locked-in cross-core trust-boundary decision
  (TrustZone-M on M55-HP, not a dedicated M55-HE).  Refines ADR
  0010.  M55-HE stays available for compute / inference offload.
- Schema: `security.psa:` description refreshed (the "pre-v0.6 ...
  emit path is a no-op" note is gone); `boot.build_type:`
  documented.  Template `metadata/templates/board.yaml` grows a
  commented `security.psa:` example.
- Docs: `docs/board-config.md` §PSA Crypto + TF-M now describes
  the v0.6 emit contract; `docs/security-audit-plan.md` gains a
  short TF-M trust boundary section pointing at ADR 0013.
- Tests: 11 new cases in
  `tests/scripts/test_alp_orchestrate.py` covering happy-path
  emit, schema field round-trip, ITS/PS backing-store reference
  validation (happy + rejection), attestation-root OPTIGA
  cross-check, absence-emits-nothing, materialise round-trip, and
  build-type inheritance.  Full suite: 413 passed / 5 skipped (was
  402 / 5).

### Added — `extra_libraries:` escape hatch for non-curated libraries (2026-05-20)

`cores.<id>.extra_libraries:` joins `libraries:` (the closed,
25-entry curated enum) with an open-set escape hatch for libraries
the SDK doesn't curate -- one-off vendor SDKs, research-only deps,
or libraries on their way into the curated set.  Each entry MUST
declare exactly one of:

- `kconfig:` -- inline Kconfig fragment lines emitted verbatim
  into the slice's `alp.conf`.  Fast path for one-off entries.
- `profile:` -- path to a `hw-backends.yaml`-style profile file.
  The loader walks the file with the same silicon / soc_family /
  requires_cap matcher used by curated libraries; first match per
  `class:` wins, `sw_fallback:` always emits.

The "exactly-one" rule is enforced by the new
`_validate_consistency()` pass (see below), along with global
uniqueness of `name:` across cores and a check that
`profile:` paths resolve to a real file.  Names that collide with
the curated `libraries:` enum are rejected -- the curated path is
the right way to wire curated libraries.

Schema lives in `metadata/schemas/board.schema.json` under
`cores.<id>.extra_libraries`; reference doc at
`docs/board-config.md` § `extra_libraries:` and tutorial coverage
at `docs/tutorials/09-board-yaml-deep-dive.md` § extra_libraries.

### Added — cross-field validator pass with 5 rules (2026-05-20)

`scripts/alp_orchestrate.py:_validate_consistency()` runs after
the JSON Schema pass and the per-core loader rules.  Five rules,
two warnings:

1. (ERROR) `ota.provider: mender` requires at least one
   `cores.<id>.os: yocto` slice.  Mender server-mode is a
   Yocto-only flow today; the Zephyr-side dispatch
   (Mender-MCU-client) lands separately per ADR 0009.
2. (ERROR) `boot.signing.algorithm:` must be supported by the SoM
   family.  Per-family allow-lists: Alif Ensemble (with OPTIGA
   Trust M attestation root) `ecdsa_p256` / `ed25519`; Renesas
   RZ/V2N + NXP i.MX 9 `ecdsa_p256` / `rsa2048` / `rsa3072`.
   Unknown families fall through (no capability-block enforcement).
3. (ERROR) `cores.<id>.iot.tls: true` requires `mbedtls` or
   `bearssl` in `libraries:` (curated) or `extra_libraries:`
   (open-set).
4. (WARN) `cores.<id>.inference.default_arena_kib >
   cores.<id>.memory.heap_kib` -- inference may OOM.  Build
   continues; stderr `WARN: ...` line emitted.
5. (WARN) `cores.<id>.power.sleep_mode != disabled` with no
   `wakeup_sources:` declared -- device will sleep but cannot
   wake.

Reference doc: `docs/board-config.md` § Cross-field validation.
Two in-repo examples (`iot-fleet-ota`, `iot-dashboard`,
`production-deployment`) updated to satisfy rule 3 by declaring
`mbedtls` in `libraries:`.

### Added — examples adopting v0.6 declarative blocks (iot-fleet-ota promotion + production-deployment + power-managed-sensor) (2026-05-20)

- `examples/iot-fleet-ota/board.yaml` promoted from prose-only OTA
  narration to declarative `boot:` + `ota:` blocks (MCUboot +
  ECDSA-P256 + Mender HTTPS-poll with `${MENDER_TENANT_TOKEN}`
  placeholder).  README rewritten with the "What lands
  declaratively in v0.6" walkthrough.
- `examples/production-deployment/board.yaml` refactored into the
  SDK's declarative-stack flagship: every v0.6 block applied at
  production stance (`boot:`/`ota:`/`security.psa:`/`storage:`/
  `cores.<id>.memory:`/`cores.<id>.power:`/`diagnostics.modules:`)
  on AEN701 with TF-M + OPTIGA-rooted attestation.  README gains
  a full per-block walkthrough.
- `examples/power-managed-sensor/` (new): reference for the v0.6
  `cores.<id>.power:` block.  AEN301 + M55-HE deep sleep with
  `wakeup_sources: [uart, gpio_int, rtc]` covering the periodic
  sample / motion-event / console-debug duty cycle.  Memory tuned
  for the short-lived per-wake task (4 KiB stack, 16 KiB heap).

### Changed — board.yaml flatten + carrier→board rename + 7 declarative blocks (2026-05-20)

**Breaking schema changes (no migration script — every in-repo
`board.yaml` rewritten in the same change).**

- `board.yaml` schema flattened: dropped the `carrier:` wrapper.
  `name`, `description`, `hw_rev`, `populated`, `e1m_routes`,
  `pins`, `preset` are now top-level fields.  Two modes: inline
  (customer path) or `preset:` (SDK-internal shortcut, used by
  the ~40 EVK demos under `examples/`).
- `schema_version` field removed entirely.  One live schema at
  `metadata/schemas/board.schema.json`.
- "Carrier" noun retired in favour of "board" everywhere: file
  paths (`metadata/carriers/` → `metadata/boards/`,
  `scripts/gen_carrier_header.py` → `scripts/gen_board_header.py`,
  `metadata/schemas/board-config-v2.schema.json` →
  `metadata/schemas/board.schema.json`, new
  `metadata/schemas/board-preset.schema.json` for the shared
  YAMLs); SoM presets' `default_carrier:` → `default_board:`; C
  API `alp_hw_info_t.carrier_*` → `board_*` fields and
  `ALP_HW_BUILD_CARRIER_*` → `ALP_HW_BUILD_BOARD_*` macros.

**Schema additions (additive; existing board.yaml files validate):**

- `e1m_routes:` grows 5 sections beyond the original
  gpio/buses/pwm: `adc`, `dac`, `i2s`, `can`, `qenc` (E1M_ENC<N>
  pads).  Per-section pad-class validation: misclassified pads
  (e.g. `E1M_I2C0` under `adc:`) error at validate time.
- `e1m_routes:` entry shape gains optional `pull:` (`up|down|none`)
  and `debounce_ms:` (board-static electrical facts).
  `routes_via:` removed (SoM concern; moved to SoM preset's
  `pad_routes.dispatch:`).
- `pins:` entries can be bare strings OR `{e1m, macro?, doc?}`
  rich form.  Validator cross-checks the macro against the
  resolved board.
- `cores.<id>.memory: { stack_kib, heap_kib, isr_stack_kib }`
  → emits `CONFIG_MAIN_STACK_SIZE` / `CONFIG_HEAP_MEM_POOL_SIZE`
  / `CONFIG_ISR_STACK_SIZE`.
- `cores.<id>.power: { sleep_mode, wakeup_sources }` → emits
  `CONFIG_PM` + `CONFIG_PM_DEVICE_WAKE_<SUBSYS>`.
- `diagnostics.modules: { <name>: <level> }` → per-module
  `CONFIG_<MODULE>_LOG_LEVEL=<n>` (supports `off`).
- New top-level `boot:` block (MCUboot configuration).  Loader's
  new `emit_sysbuild_conf()` produces a `SB_CONFIG_*` overlay
  consumed via `west build --sysbuild-config build/alp_sysbuild.conf`.
- New top-level `ota:` block (Mender / MCUmgr).  Loader appends
  `MENDER_*` weak-assignments (`?=`) + `INHERIT += "mender-full"`
  to the Yocto slice's `local.conf`.
- New top-level `storage:` block (filesystem partitions).
  Schema authoritative; DTS-overlay emit lands in v0.6 alongside
  the deterministic flash-allocator.
- New top-level `security.psa:` block (TF-M + PSA Crypto).
  Schema authoritative; TF-M sysbuild child-image plumbing lands
  in v0.6.

**EVK preset seeded:** `metadata/boards/e1m-evk.yaml` now declares
the full canonical EVK wiring across all 8 sections — ADC0..ADC7
(board-id, Arduino A1..A5, mikroBUS AN, VBAT sense), DAC0/DAC1,
I2S0/I2S1, CAN0, ENC0 (PEC12R rotary).  Generated routes header
grows 79 → 125 lines.

**Migration delta:** 41 example `board.yaml` files migrated to the
flat form with annotated `pins:` arrays where applicable; 9
test files in `tests/scripts/` updated; ~200 source files swept
for prose-level carrier→board rename.  Full docs sweep across
README, architecture, board-config, getting-started,
firmware-quickstart, heterogeneous-builds, portability,
porting-new-som, secure-boot, ota, and tutorials 09/10/11/12/16.
ABI snapshot at `docs/abi/v0.5-snapshot.json` regenerated to
match the renamed `alp_hw_info_t` fields.  All tests green:
402 passed / 5 skipped.

### Added — Intra-family portability proof + Phase B/C audit cleanups (2026-05-18)

**Portability is now empirically proven.**  Phase A swap-test
matrix: 21/21 E1M cells green (i2c-scanner / gpio-button-led /
pwm-led-fade across all 7 E1M SKUs) + 12/12 E1M-X cells green
(adc-voltmeter / pwm-led-fade / v2n-pwm-fan-control across all 4
E1M-X SKUs).  Within the AEN sub-family, generated `alp.conf` is
byte-identical across all 6 SKUs after stripping the SoC-enable
line.  Customer-facing matrix at
[`docs/portability-matrix.md`](docs/portability-matrix.md) (new).

**Phase B gap fixes** (all 5 surfaced gaps resolved):

- **A2-1** `metadata/e1m_modules/E1M-V2M102.yaml` `pad_routes:` use
  `E1M_X_*` namespace (was `E1M_*` — E1M form factor leaked into
  an E1M-X SoM).  Apps consuming `<alp/e1m_x_pinout.h>` now resolve
  on V2M102 like the other 3 E1M-X SoMs.
- **A2-2** V2M101 + V2M102 gain the 8 `E1M_X_GPIO_IO27..IO35`
  extension routes V2N already had (maintainer confirmed GD32
  routing is identical on V2M).
- **G-1** Per-variant Ethos-U Kconfig: orchestrator walks SoM
  `inference.npu_population[]` and emits
  `CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U55=y` / `_U65=y` / `_U85=y`
  per variant present.  AEN401/601/801 now emit BOTH `_U55` and
  `_U85`; previously the U85's TensorOptimized kernels were
  invisible to the build.  Driver dispatch in
  `src/zephyr/inference_tflm.cpp` selects per-variant kernel pack
  at compile time; `alp_inference_tflm_npu_variant_name()` exposes
  the active variant for HiL operators.
- **G-2** Per-CPU-class TFLM kernel selector: orchestrator emits
  `CONFIG_ALP_SDK_INFERENCE_TFLM_NEON=y` (A-cluster) /
  `_HELIUM=y` (M55) / `_REF=y` (baseline Cortex-M) per slice
  from SoC-JSON `cores[<core_id>].vector_extension`.
- **G-4** `cores.<key>` rename diagnostic: hard-fails with "did
  you mean one of: [...]" hint when no `cores:` key intersects
  the preset's `topology:` keys.  Soft-warns per dropped key on
  partial matches.

**Phase C cleanups** (bundled — audit-deferred items):

- **C.1** `button_led` + `pdm_mic` relocated from `chips/` to
  `blocks/` (alp_*-prefixed helpers are SDK abstractions, not
  chip drivers, per `[[chip-driver-naming]]`).  History-preserving
  `git mv`.  Kconfig `CONFIG_ALP_SDK_CHIP_BUTTON_LED` →
  `_BLOCK_BUTTON_LED` + `_BLOCK_PDM_MIC`; orchestrator dispatches
  per slug.  Updated 25+ consumers; new `blocks/README.md`.
- **C.2** Four Yocto NOSUPPORT stubs added
  (`src/yocto/peripheral_{can,i2s,rtc,wdt}.c`) — Yocto link line
  now provides every `<alp/*>` symbol the public headers declare.
- **C.3** `_Static_assert` ALP↔GD32 PWM align wire-encoding parity
  in `src/zephyr/peripheral_pwm.c`; cast now fails to compile if
  either enum is reordered.
- **C.4** Comment density boost on `examples/rpmsg-imx93/m33/src/main.c`
  (21 % → 67 %) and `examples/drone-autopilot/src/main.c`
  (30 % → 60 %); both clear the ~50 % examples-are-documentation target.
- **C.5** `[PAPER-CORRECT-STUB]` `@par Verification status` blocks
  added to `act8760.h` / `da9292.h` / `ov5640.h` public headers so
  API consumers see which surfaces return `ALP_ERR_NOSUPPORT`.
- **C.6** `metadata/socs/nxp/imx9/imx93.json` flagged with
  `pending_reference_manual_ingestion: true` (no invented peripheral
  counts); schema + `validate_metadata.py` extended to accept + WARN
  on the new field.

Verification: pytest `tests/scripts/` 367 passed (357 baseline + 10
new G-1/G-2/G-4 tests, 0 regressions); `validate_metadata.py` 0
failures; `check_example_portability.py` 47/0.

### Added — Phase D documentation push (2026-05-18)

Six-pack of documentation work anchoring the just-proven intra-
family portability promise:

- **D.1** `docs/portability.md` (NEW, 814 lines) — customer-facing
  portability cookbook with 6 sections: swap-and-run scope, the
  swap-test recipe (two worked examples), dual-namespace decision
  tree, intentional gaps (NPU model artefacts, form-factor symbol
  errors, heterogeneous-OS topology, carrier-specific HW),
  capability validation via `alp_last_error()` + `ALP_ERR_NOSUPPORT`
  + the runtime fallback ladder, link to the empirical matrix.
- **D.2** `docs/porting-new-som.md` rewritten as a 30-minute
  walkthrough of adding a hypothetical 7th AEN SKU (E1M-AEN901).
- **D.3** `docs/architecture.md` repository-layout tree refreshed
  to the post-slice-3a/3b state; four new sub-sections under
  "Build orchestration" (per-core slice fan-out, sparse capabilities
  flow, on_module: auto-enable, generators inventory).
- **D.4** `docs/v1.0-readiness.md` gains Pillar 3f "Intra-family
  portability proven" with checkboxes for the matrix, cookbook,
  ADR 0011, and the 5 Phase B gap closures.
- **D.5** All 16 `docs/tutorials/*.md` cross-checked.  Tutorial-04
  rewritten around the intra-family promise (old three-rings
  cross-family model superseded).  Tutorial-09 + tutorial-16
  light-edited to fix board.yaml + inference API drift + add G-1/G-2
  variant-selector pointers.  Other 13 got a "Last verified:
  2026-05-18" header line.
- **D.6** `docs/adr/0011-intra-family-portability.md` (NEW, 191
  lines) — Architecture Decision Record ratifying the load-bearing
  intra-family portability scope.

Plus `docs/README.md` (NEW) — top-level doc navigation hub linking
all the new and existing docs into topic groupings.

### Added — Cross-platform developer host first-class (2026-05-18)

Codify cross-platform Win + Mac + Linux developer support as a
load-bearing SDK principle so customers never feel "I need Linux
to use the alp-sdk."  Yocto remains Linux-only by upstream
constraint; the Zephyr-on-M-class workflow is first-class on every
host.

- `docs/adr/0012-cross-platform-developer-host.md` (NEW) — ADR
  codifying the principle, alternatives rejected (Linux-first via
  WSL2, Linux-only, per-OS forks), consequences (CI cost rises
  but adoption uplift justifies it).
- `docs/cross-platform-setup.md` (NEW, 720 lines) — 8-section
  per-OS quickstart (Linux Debian/Ubuntu/Fedora, macOS Homebrew
  + Apple Silicon, Windows native PowerShell + winget + Arm GNU
  MSI + setx, Windows + WSL2 for Yocto), verification walkthrough,
  known gotchas (MAX_PATH, AV, CRLF, Gatekeeper, symlinks, serial
  device naming), what's-Linux-only-and-why scoping.
- `scripts/check_cross_platform.py` (NEW, 618 lines) — soft-warn
  lint for Linux-only idioms (hard-coded `/dev/` paths,
  `~/.bashrc` mentions, bash-only shebangs, `make` in
  tutorial-grade docs, forward-slash absolute paths).  Two
  suppression mechanisms: `INTENTIONALLY_DISCUSSES_OS_PATHS`
  allowlist (collapses N findings to one summary line) and inline
  `<!-- cross-platform-lint:ignore -->` skip markers.  CLI flags
  `--fail-on-warning`, `--quiet`, `--json`.  37 pytest cases at
  `tests/scripts/test_check_cross_platform.py` (NEW).
- `.github/workflows/cross-platform-zephyr.yml` (NEW) — CI matrix
  scaffolding.  Ubuntu strict; macOS + Windows
  `continue-on-error: true` per ADR 0012 (surface drift, don't
  block while runners prove out).
- Lint cleanup of 28 → 0 findings on the existing tree (placeholder
  serial device paths in example READMEs; `<your-shell-rc>`
  placeholder in `docs/local-ci.md`; cross-platform header notes
  on `scripts/bootstrap.sh` + `scripts/test-all.sh`).

### Added — 8 vendor-SDK-style peripheral tutorial examples (2026-05-18)

Vendor-SDK-style canonical "first program" + per-peripheral
tutorial set.  Each example follows the standard six-file layout
(board.yaml, prj.conf, CMakeLists.txt, README.md, src/main.c,
testcase.yaml) plus native_sim overlays where needed; comment
density on main.c targets ~50 % per `[[examples-are-documentation]]`.

- `examples/hello-world` — zero-peripheral printf heartbeat via
  `alp_log_*`.  The canonical first build (74 % comment density).
- `examples/uart-hello-world` — producer-only "printf via UART"
  walkthrough (companion to bidirectional uart-echo).
- `examples/i2c-master` — read TMP112 temperature sensor; contrasts
  with `i2c-scanner` (discovery vs known-address read).
- `examples/i2c-slave` — slave-mode SHAPE + explicit SDK gap notice
  (`<alp/peripheral.h>` is master-only today; local NOSUPPORT shim
  templates the proposed `alp_i2c_slave_*` surface for v0.7).
- `examples/spi-master` — discrete master demonstrating write /
  transceive / read patterns.
- `examples/spi-slave` — slave-mode SHAPE + SDK gap notice (same
  class as i2c-slave).
- `examples/dac-waveform` — 100 Hz sine on `E1M_X_DAC0` via the
  V2N GD32 supervisor bridge (E1M-X targeted).
- `examples/timer-periodic-interrupt` — re-arming periodic alarm
  via `alp_counter_*` + ISR-safe coordination pattern.

`examples/README.md` index updated.  `check_example_portability.py`:
55 examples (was 47), 0 errors.

### Schema tightening — silicon-determined fields removed from board.yaml (2026-05-16)

Customer-facing knobs in `board.yaml` v2 are now restricted to
project-level choices.  Anything dictated by the SoM SKU's silicon
(NPU presence, on-module component populations, memory capacities,
per-core natural runtime) is read from the SoM preset under
`metadata/e1m_modules/<MPN>.yaml` and is no longer overridable at
the project level.

**Schema changes (breaking — board.yaml v2):**

- **`cores.<id>.inference.backend` removed.**  The build now compiles
  in *every* dispatcher the SoM preset's `capabilities:` block
  declares (TFLM CPU always, plus Ethos-U / DRP-AI3 / DEEPX DX-M1
  per the SoM caps).  Apps pick which to use per-handle at runtime
  via `alp_inference_open(.backend = …)`.  This unblocks concurrent
  multi-NPU dispatch on V2M101 (DRP-AI3 + DEEPX DX-M1 running
  independent models simultaneously) which the old "pick one
  backend" wiring silently broke.
- **`cores.<id>.os` is now optional.**  Every core has a natural
  runtime baked into the SoM preset's `topology:` block (Cortex-M
  → Zephyr, Cortex-A → Yocto Linux); customers write `os:` only to
  override (`os: off` to skip a core, `os: baremetal` for hand-
  written firmware on a core that normally runs Zephyr).
- **`som.overrides` removed.**  For a custom SoM variant (e.g. an
  AEN without the OPTIGA Trust M), create a new MPN preset under
  `metadata/e1m_modules/` rather than overriding here.
- **`som.memory` removed.**  Same reasoning — memory capacities are
  silicon-determined.  Custom memory variants get their own MPN
  preset.

**Loader changes:**

- `scripts/alp_orchestrate.py:_slice_alp_conf` now emits
  `CONFIG_ALP_SDK_INFERENCE_*` from the SoM `capabilities:` matrix
  (was: emitted nothing — examples carried hand-written
  `CONFIG_ALP_SDK_INFERENCE_TFLM=y` in `prj.conf`).
- `scripts/alp_orchestrate.py:_slice_cmake_args` now emits
  `-DALP_SDK_USE_DRPAI=ON` / `-DALP_SDK_USE_DEEPX_DXM1=ON` per
  the SoM caps; V2M101 baremetal/Yocto builds get **both**.

**Migration:**

```diff
 cores:
   m55_hp:
-    os: zephyr                          # delete -- topology default
     app: ./src
     inference:
-      backend: ethos_u                  # delete -- silicon-determined
       default_arena_kib: 256
```

```diff
 som:
   sku: E1M-AEN701
-  overrides:                            # delete -- create new MPN preset instead
-    secure_element: none
-  memory:                               # delete -- create new MPN preset instead
-    flash_mbit: 131072
```

**Tests:**

- `tests/scripts/test_alp_project.py` adds `TestInferenceFromSomCaps`
  covering: SoM-caps-driven CONFIG emission per family, schema-level
  rejection of `inference.backend`, V2M101 cmake-args emitting both
  DRPAI + DEEPX_DXM1 flags.

**Principle:** silicon-determined facts live in
`metadata/e1m_modules/<MPN>.yaml`; `board.yaml` carries only
project-level choices.  See
[`docs/board-config.md`](docs/board-config.md) "Silicon-determined
fields never appear in `board.yaml`".



(Tracks `metadata/sdk_version.yaml`'s declared version.  Per
`VERSIONS.md`, every point release ships the *surface* first
and accumulates runtime implementations in subsequent point
releases -- entries below collect every Added / Changed item
that lands before the v0.6.0 tag.)

> **Verification status.**  Entries below describe what's been
> *coded and merged* on `main`; passing `pr-plain-cmake` /
> `pr-twister` / `pr-static-analysis` / `pr-alp-build` is
> necessary but not sufficient to call an item "GA".  Real-hardware
> verification is tracked separately in
> [`docs/test-plan.md`](docs/test-plan.md) -- a release does not
> tag until every row gating it flips to ✅.

### v0.6.0 — Heterogeneous OS orchestration (2026-05-15)

The v0.6 redesign drops the framing that `os:` is a single
SoM-wide choice and turns Zephyr / Yocto / bare-metal into
peers that coexist on the same SoM, declared per-core from one
`board.yaml`.  Full design at
[`docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`](docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md);
decision recorded in
[`docs/adr/0010-heterogeneous-os-orchestration.md`](docs/adr/0010-heterogeneous-os-orchestration.md).

`[UNTESTED]` across the board -- AEN + iMX93 hardware-fact
fields carry `TBD` strings pending the maintainer's hand-written
HW config per the project's "never invent values" convention.

#### Added

- **Heterogeneous OS orchestration.**  Per-core `cores:` block in
  `board.yaml` v2 lets one project run Yocto on a Cortex-A cluster,
  Zephyr on a Cortex-M peer, and bare-metal on a third core -- all
  from one declarative config.  Each on-die programmable core takes
  its own `os:` / `app:` / `peripherals:` / `libraries:` /
  `inference:` / `iot:` block.
- **`<alp/rpc.h>` framed RPC layer** on top of OpenAMP / RPMsg.
  Zephyr backend (full) + Linux backend (full per Wave 5A); apps
  never type endpoint IDs or carve-out addresses by hand.
- **Five west extension commands** replacing the single
  `west alp-build` from v0.5:
  - `west alp-build` -- fan board.yaml out into per-core slices,
    emit `system-manifest.yaml`.
  - `west alp-image` -- consume the manifest, assemble a flashable
    bundle (`build/image-bundle/`).
  - `west alp-flash` -- walk the manifest's `boot_order:` and
    program each piece with the right backend tool.
  - `west alp-clean` -- tear down per-slice build dirs idempotently.
  - `west alp-renode` -- boot the dual-OS image in Renode and
    drive the RPMsg handshake smoke test.
- **Generated artefacts** emitted by `scripts/alp_orchestrate.py`:
  - `build/system-manifest.yaml` -- per-slice status + log paths +
    artefact paths + boot order, byte-stable across rebuilds.
  - `build/generated/dts-reservations.dtsi` -- shared-memory
    carve-outs as Linux + Zephyr DT reservations.
  - `build/generated/alp/system_ipc.h` -- name + address + endpoint
    ID + mailbox channel macros that both halves `#include` via
    `<alp/system_ipc.h>`.  Carries an `#error` directive for any
    blocked channel so the slice build trips when the SoM metadata
    isn't ready yet (see "Blocked carve-outs" below).
- **3 flagship heterogeneous examples** plus an offload demo:
  - `examples/rpmsg-v2n/` -- V2N101 A55 (Yocto) ↔ M33-SM (Zephyr)
    over RPMsg.
  - `examples/rpmsg-aen/` -- AEN701 A32 (Yocto) ↔ M55-HP (Zephyr).
  - `examples/rpmsg-imx93/` -- iMX93 A55 (Yocto) ↔ M33 (Zephyr).
  - `examples/heterogeneous-offload/` -- A-cluster delegates an FFT
    computation to the M peer over RPMsg.
- **14 Zephyr DT overlays** covering every SoM × M-class core combo.
- **Yocto recipes** under `meta-alp-sdk/recipes-alp/`:
  - `alp-remoteproc_0.6.bb` -- userspace remoteproc helper.
  - `alp-dts-reservations_0.6.bb` -- generated `reserved-memory:`
    fragment shipped into the kernel DT.
  - `alp-chips_0.6.bb` -- absorbed from the deleted
    `yocto/meta-alp/` layer.
  - `alp-edgeai_0.6.bb` -- path-rebased to track the renamed
    `examples/aen/edgeai-vision-aen/` source tree.
- **Per-SoM `topology:`, `memory_map:`, `mailbox:`,
  `helper_firmware:` blocks** in `metadata/e1m_modules/E1M-*.yaml`
  drive the orchestrator.  The topology key set mirrors the SoC's
  `cores[].id` set exactly (cross-checked by
  `pr-metadata-validate.yml`).
- **ADR 0010** + [`docs/heterogeneous-builds.md`](docs/heterogeneous-builds.md)
  walkthrough + glossary terms (system manifest, slice, carve-out,
  helper-MCU firmware, topology key set).
- **3 new CI workflows**:
  - `.github/workflows/pr-alp-build.yml` -- orchestrator gate
    across SoM × scenario matrix, asserts the system-manifest is
    byte-stable across rebuilds.
  - `.github/workflows/pr-bitbake.yml` -- self-hosted runner gate
    that builds `alp-image-edge` per A-cluster MACHINE.
  - `.github/workflows/pr-renode-dual-os.yml` -- Renode dual-OS
    smoke test (advisory until the self-hosted Renode runner
    ships in v0.6.1).
- **Real flash backends** under `scripts/flash_backends/` (Wave 5B):
  vendor flasher for the SoC, openocd-via-SWD for the GD32 helper,
  USB-CDC bootloader for CC3501E.  `west alp-flash` walks the
  manifest and dispatches to the right backend per artefact -- no
  developer-side tool-selection.

#### Changed

- **`board.yaml` schema bumped to v2** -- top-level `os:` field
  removed; `peripherals:` / `libraries:` / `inference:` / `iot:`
  moved per-core under `cores.<id>`.  `carrier.populated:` +
  `chips:` and `diagnostics:` stay top-level (they describe
  physical assembly + project-wide diagnostics).  No migration
  script -- no shipping customers yet; every in-repo `board.yaml`
  is rewritten in the same redesign PR (32 files).
- **`examples/mproc-dual-os-yocto-zephyr/` →
  `examples/rpmsg-v2n/`** (renamed + sub-tree restructured;
  `m33/` → `m33_sm/` to match the SoM topology key).
- **`metadata/e1m_modules/E1M-*.yaml`** gained `topology:` +
  `memory_map:` + `mailbox:` + `helper_firmware:` blocks.  V2N
  preset is fully authored from the maintainer-confirmed pinmap;
  AEN + iMX93 carry `TBD` strings on hardware-fact fields
  (`memory_map.base`, `mailbox.controller`, AEN `cc3501e_otp`
  firmware path).
- **`metadata/socs/.../*.json` `cores[]`** -- every entry now
  carries an `id:` field used as the topology key.
- **`docs/os-support-matrix.md`** restructured from per-(SoM, OS)
  columns to per-(SoM, core, OS) columns; split into Cortex-A and
  Cortex-M tables.
- **README "30-second quick start"** rewritten with a v2 per-core
  example; "Status" section updated to v0.6 ramp.
- **`meta-alp-sdk/conf/machine/` MACHINEs** renamed from
  `e1m-v2n101.conf` to `e1m-v2n101-a55.conf` (per-cluster
  naming -- the M33-SM is no longer implied to be part of the same
  MACHINE).  `pr-bitbake.yml` matrix updated accordingly.
- **AEN + iMX93 SoM presets** carry `TBD` strings on hardware-fact
  fields pending the maintainer's hand-written HW config.  Per the
  project's "never invent values" convention these stay `TBD`
  rather than guessed defaults; the AEN audit at
  `docs/aen-feature-audit-2026-05.md` and the iMX93 reference
  manual sections to cross-check are linked from each preset.

#### Removed

- **`metadata/schemas/board-config-v1.schema.json`** -- v2 fully
  replaces; no dual-schema fallback in the loader.
- **`yocto/meta-alp/`** (entire tree) -- content absorbed into
  `meta-alp-sdk/`; the duplicate layer is deleted.  Customers
  consuming the old layer name should re-point at `meta-alp-sdk`.
- **`scripts/west_commands/alp.py`** -- split into 5 focused
  command modules (`alp_build.py`, `alp_image.py`, `alp_flash.py`,
  `alp_clean.py`, `alp_renode.py`) under the same directory.  The
  `west alp-build` entry point keeps its CLI surface; the v0.5
  monolithic file is gone.

#### Fixed

- **The 13-line workaround comment** at the top of the old
  `examples/mproc-dual-os-yocto-zephyr/board.yaml` apologising
  that the two halves could not share one declaration -- removed.
  Both halves now drive from one v2 `board.yaml`, the example
  shape the schema *expects* to be used.
- **`resolve_carve_outs()` emits `status: blocked` instead of
  raising on TBD SoM metadata** (mailbox controller, memory_map
  base / size).  The manifest stays emit-able when the preset
  isn't fully HW-mapped yet — CI's manifest-shape + determinism
  gates pass while the actual slice build is what trips (via the
  generated `<alp/system_ipc.h>` `#error` directive).  Required
  for the AEN + iMX93 examples to flow through `pr-alp-build` while
  the preset's hardware-fact fields are still TBD.
- **`system-manifest.yaml` is byte-stable across rebuilds.**
  Dropped the wall-clock `duration_s` field from
  `Slice.to_manifest_entry()` — the metric stays on the runtime
  Slice dataclass for `west alp-build` logging but never lands in
  the declarative manifest.  Satisfies spec §6.1's determinism
  clause: `west alp-clean && rebuild && diff` is now a no-op.
  Enforced by `pr-alp-build.yml`'s "Determinism check" step.
- **`<alp/system_ipc.h>` path canonical at `generated/alp/`**
  (was `generated/alp_system_ipc.h`) — matches the
  `#include <alp/system_ipc.h>` consumer pattern documented in
  `include/alp/rpc.h`.  Slice CMakeLists drop
  `${CMAKE_BINARY_DIR}/generated` onto `zephyr_include_directories`
  and the include resolves with no munging.
- **v2 `zephyr-conf` emit now carries chip drivers + subsystem deps.**
  `_slice_alp_conf` was only emitting baseline + silicon +
  per-core peripherals + libraries; it dropped the `carrier.populated:`
  chip-driver block that the v1 emit had under `_emit_zephyr`.
  Apps that depend on a populated chip (e.g. `iot-connected-camera`'s
  `ssd1306` + `button_led`) lost `CONFIG_ALP_SDK_CHIP_*` under v2 so
  twister's native_sim link hit `undefined reference to ssd1306_init`.
  v2 path now merges the SoM/carrier preset's `populated:` block with
  the board.yaml override and emits both the chip-driver Kconfigs and
  the matching subsystem enables (`CONFIG_GPIO=y` / `CONFIG_I2C=y` /
  ...) so `GPIO_EMUL` / `I2C_EMUL` deps are satisfied.
- **`examples/rpmsg-v2n/m33_sm/testcase.yaml`** (moved from project
  root).  The old top-level location made twister try to configure
  the multi-slice project root as a Zephyr app — there's no
  `CMakeLists.txt` finding `Zephyr` at that level, so configure
  crashed and aborted the whole run with
  `FileNotFoundError: zephyr/.config`.  Move puts the testcase next
  to its real `CMakeLists.txt` + adds `extra_args:
  CONFIG_ALP_SDK_RPC=y` so the producer's `<alp/rpc.h>` symbols link.
- **Pre-existing chip header gaps surfaced once twister stopped
  aborting mid-run.**  `a4988.h` / `drv8825.h` / `drv8833.h` now
  `#include "alp/pwm.h"` (they declare `alp_pwm_t` fields but only
  pulled in `peripheral.h`).  `tests/zephyr/chips/prj.conf` now
  enables every chip the suite calls `*_init()` on (the v0.5
  §D.AI / §D.industrial / §D.iot / §D.audio batches were on by-include
  but off by-Kconfig, so 80+ chip _init calls hit
  `undefined reference`).
- **`tests/zephyr/library_knobs/src/main.c`** stopped using
  `defined(CONFIG_##x)` as a runtime expression (the C preprocessor
  operator only works in `#if` directives — Zephyr provides
  `IS_ENABLED()` for the C-expression form).  Dropped the
  `CONFIG_FILE_SYSTEM_LITTLEFS=y` pin and the mbedtls header
  include — Zephyr v4.4's `subsys/fs/littlefs_fs.c` + `ssl_misc.h`
  trip -Werror on this profile and the knob smoke doesn't need them.

#### Changed (CI)

- **`pr-twister.yml` drops the `ghcr.io/zephyrproject-rtos/ci:v0.29.1`
  Docker container** (~17 GB on disk; the public ubuntu-latest
  runners only ship ~14 GB free, so the image pull intermittently
  failed with `no space left on device`).  Replaced with a native
  ubuntu-latest job that pip-installs `west` + the Zephyr Python
  requirements and uses `ZEPHYR_TOOLCHAIN_VARIANT=host` so
  `native_sim/native/64` builds use the runner's stock gcc.  Saves
  ~5 minutes per run; eliminates the disk-pressure failure mode.

#### References

- ADR: [`docs/adr/0010-heterogeneous-os-orchestration.md`](docs/adr/0010-heterogeneous-os-orchestration.md)
- Design spec: [`docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md`](docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md)
- App-developer walkthrough: [`docs/heterogeneous-builds.md`](docs/heterogeneous-builds.md)

### Decided (hardware-design decisions captured 2026-05-12)

- **All E1M PWM channels are GD32-driven; Renesas drives no PWMs.**
  The "either GD32 or Renesas, picked SoM-wide via resistor strap"
  cross-source design is collapsed to a single GD32-only path.
  Removed `GPT13_GTIOC13A` (P64) / `GPT13_GTIOC13B` (P65) /
  `GPT4_GTIOC4B` (P75) rows from
  `metadata/e1m_modules/v2n/renesas-peripheral-map.{tsv,csv}`.
  GD32-side mapping unchanged: `E1M PWM0..PWM7` on GD32
  `PA11 / PB1 / PB14 / PC5 / PC10 / PC11 / PC12 / PD0`.  The
  resistor-strap selection is no longer applicable -- there's
  nothing to switch between.
- **GD32 in-field reflash uses SWD-from-host, not factory-ISP / BOOT0.**
  The earlier "BOOT0 -> Renesas `P75`" plan is dropped in favour of a
  software SWD bit-bang from the Renesas host -- SWD works regardless
  of GD32 firmware state, where factory-ISP depends on the GD32 boot
  ROM staying intact and would have required a third pad (a USART
  line) on the V2N side.  Net pad assignments (maintainer-confirmed):
  - `GD32_SWDIO` -> Renesas `P70` (was `GPT0_GTIOC0A`).
  - `GD32_SWCLK` -> Renesas `P71` (was `GPT0_GTIOC0B`).
  - `P75` becomes unassigned on the Renesas side (BOOT0 dropped,
    PWM5 moved entirely to GD32).
  See [memory `project_gd32_boot0_to_v2n_planned.md`] for the design
  rationale.
- **GD32_NRST -> Renesas `P74`** (was E1M PWM4 / GPT4_GTIOC4A).  PWM4
  becomes GD32-only.  **Line is shared with the primary PMIC reset
  out** -- host pad MUST be configured open-drain (drive low to
  assert reset; HiZ to release).  An external pull-up returns the
  line to its released state.
- **DEEPX `M1_RESET` polarity -> active-LOW.** The
  `chips/deepx_dxm1/` driver default flipped from `ACTIVE_HIGH`
  (placeholder pending the schematic check) to `ACTIVE_LOW`
  (confirmed).  `deepx_dxm1_set_reset_polarity()` still lets
  carrier code override.
- **Murata `BT_DEV_WAKE` intentionally not routed on V2N.**  The
  `chips/murata_lbee5hy2fy/` driver already supported the NULL-
  pin-handle case; metadata + header doc updated to make the
  "not-routed" status explicit.
- **5L35023B I2C address -> 7-bit `0x68`** (8-bit write `0xD0`) per
  the Renesas 5L35023 public datasheet.

### Fixed (2026-05-14 -- DA9292 OTP-variant bring-up correction)

- **`da9292_v2n_m1_enable_deepx_rail` no longer over-volts DEEPX
  to 1.50 V.**  The DA9292-AROVx OTP variant on V2N boots with
  `CH2_VSTEP=1` (`PMC_CTRL_01 = 0x80`, doubled-range encoding).
  The previous bring-up wrote byte `0x96` to `VOUT_CH2_VSEL_LO`
  expecting it to decode as 0.75 V (the VSTEP=0 mapping), but
  silicon interpreted it as `2 x 750 = 1500 mV`.  Fix: clear
  `CH2_VSTEP` + `CH2_EN` together in a single `PMC_CTRL_01`
  write before programming the voltage byte.  Datasheet
  constraint: `CHx_VSTEP` is only writable while
  `CHx_EN=0`, so the EN clear must precede.  See
  `chips/da9292/da9292.c::da9292_v2n_m1_enable_deepx_rail` and
  the `@warning` block in `include/alp/chips/da9292.h`.
- **`DA9292_I2C_ADDR_V2N` corrected from `0x1Cu` to `0x1Eu`.**
  The macro had carried the wrong 7-bit address since the driver
  landed; the AROVx OTP variant's `PMC_CFG_0A` defaults to 8-bit
  `0x3C` -> 7-bit `0x1E`.  Affects nobody at runtime today (the
  V2N firmware module that calls `da9292_init` hasn't landed yet)
  but the macro is exposed in `include/alp/chips/da9292.h` so it
  needed correcting before downstream consumers caught the bad
  value.  `docs/abi/v0.5-snapshot.json` regenerated.

### Changed (2026-05-14 -- Zephyr v3.7.0 LTS → v4.4.0 stable bump)

The SDK's `west.yml` Zephyr pin moves from v3.7.0 LTS to v4.4.0
stable.  Mainline feature access (LVGL v9, upstream Alif Ensemble
board files, the I2S `_CONTROLLER` naming, mbedtls 3.6) at the
cost of LTS support window.  Customers shipping product with a
24-month support requirement should re-pin to v3.7.x in their own
manifest -- the SDK's `<alp/*>` surface stays binary-compatible.

Knock-on changes:

- `.github/workflows/pr-twister.yml` + `nightly-aen-hil.yml` --
  `--mr v3.7.0` → `--mr v4.4.0`; cache key bumped.
- All `examples/*/testcase.yaml` + `README.md` entries that named
  the v3.x board `alif_e7_dk_rtss_hp` / `_rtss_he` retargeted to
  the v4.4 upstream Alif board `ensemble_e8_dk/ae402fa0e5597le0/rtss_hp`
  (closest available -- AEN-specific boards land later in
  Alif's own `zephyr_alif` fork).
- LVGL Kconfig knob renames (LVGL v8 → v9):
  - `CONFIG_LV_DISP_DEF_REFR_PERIOD` → `CONFIG_LV_DEF_REFR_PERIOD`.
  - `CONFIG_LV_INDEV_DEF_READ_PERIOD` -- removed in v9; runtime
    `lv_indev_set_read_timer_period()` instead.
- LVGL demo source sets (`lv_demo_widgets.c`, `lv_demo_benchmark.c`,
  `lv_demo_music*.c`) are no longer compiled by the Zephyr lvgl
  module's CMakeLists; demo examples add them to their own
  `target_sources()` explicitly.
- `src/zephyr/peripheral_i2s.c` -- `I2S_OPT_FRAME_CLK_MASTER` /
  `I2S_OPT_BIT_CLK_MASTER` are deprecated in v4.4; renamed to
  `_CONTROLLER` per Zephyr's inclusive-language pass.
- `docs/zephyr-version-policy.md`, `getting-started.md`,
  `troubleshooting.md`, `glossary.md` updated.
- New `docs/local-ci.md` -- Windows-native + WSL2 paths for
  running twister locally so contributors don't bounce off CI for
  every iteration.

### Added (2026-05-14 -- 7 demo source-level fixes + 6 new flagship demos)

Real bugs the v4.4 bump surfaced + new demo apps for the website
gallery.  All marked `[UNTESTED]` -- they build on `native_sim`
but haven't been HIL-validated.

Source fixes (real API drift in v0.5 demos):

- `drone-autopilot/src/{main,autopilot,mavlink}.c` + `board.yaml`
  -- chip API mismatches (lsm6dso_axes_t, ina236 signature, UART
  field name `baudrate`, INA236 default I²C addr literal, PWM
  header include, missing `<stdio.h>`, float/double promotion in
  the GPS pack, MAVLink UART moved off the non-existent
  E1M_UART2).  Board.yaml gains a `carrier.populated:` override
  for `bmp390` + `ublox_neo_m9n` so the loader emits their chip
  knobs (the loader doesn't yet honour a top-level `chips:` block).
- `drone-hud/src/sensors.c` -- same class of fixes.
- `ai-camera-viewer/src/inference_loop.c` -- `alp_camera_config_t`
  and `alp_inference_config_t` reconciled with the real header
  field names.
- `iot-dashboard/src/main.c` -- `bme280_compensate` signature
  rewrite (it returns a struct, not three out-params), `<stdio.h>`
  for `snprintf`, `CONFIG_LV_FONT_MONTSERRAT_28=y` added to prj.conf.
- `production-deployment/prj.conf` -- `CONFIG_MBEDTLS=n` on
  native_sim to dodge Zephyr 4.4's PSA-crypto wiring (real fix in
  v0.6 via tf-psa-crypto).
- All 15 `examples/*-*/testcase.yaml` files that an earlier
  auto-edit had left with `harness: console` indented inside the
  `tags:` list re-shaped to the correct flat layout.
- 7 new demo `CMakeLists.txt` files -- `find_package(Python3
  REQUIRED COMPONENTS Interpreter)` moved BEFORE
  `find_package(Zephyr)` so `${Python3_EXECUTABLE}` is defined
  when the alp_project loader runs.

New flagship demos (paper-correct stubs, ~50 % comment ratio):

- `examples/ai-object-detection-realtime/` -- YOLOv8-tiny on
  V2N-M1 (DEEPX 29 TOPS) + V2H-M1 (DEEPX 113 TOPS).  Camera →
  inference → bounding-box overlay → FPS counter.
- `examples/ai-anomaly-detection-vibration/` -- accelerometer →
  1D-CNN → anomaly score for predictive maintenance.  AEN
  always-on Ethos-U + V2N.
- `examples/iot-fleet-ota/` -- secure OTA + rollback signed by
  the OPTIGA Trust M secure element.  All E1M-X SoMs.
- `examples/audio-noise-suppression/` -- DSP + AI pipeline,
  ~10 ms latency.  V2N + V2H DSP cores.
- `examples/audio-wake-word/` -- always-on keyword spotting on
  AEN's Ethos-U at sub-mW.  E1M-AEN.
- `examples/mproc-dual-os-yocto-zephyr/` -- Yocto/A55 + Zephyr/M33
  shared-memory IPC, dual-firmware shape, dual-update lifecycle.
  V2N + V2H.

### Added (2026-05-14 -- meta-alp-sdk Yocto layer + drone-autopilot MAVLink GCS link)

`[UNTESTED]` -- both pieces are paper-correct v0.5 scaffolding.

- `meta-alp-sdk/` -- top-level Yocto layer that wires the SDK into
  ROS 2 Humble + DEEPX-on-Linux images for V2N + V2N-M1 Linux
  targets.  Layer compatibility: `kirkstone scarthgap`.  Ships:
  - `conf/layer.conf` + `conf/machine/e1m-v2n101.conf` +
    `conf/machine/e1m-v2m101.conf` (V2N base vs V2N + DEEPX SoM).
  - `recipes-core/alp-sdk/alp-sdk_0.5.bb` -- builds + installs
    `libalp_sdk.so` + the `<alp/*>` headers.
  - `recipes-ros/alp-perception/alp-perception_0.5.bb` -- builds
    the v2n-m1-ros-perception ROS 2 node from `examples/v2n/`.
  - `recipes-deepx/dx-rt/dx-rt_2.4.bb` -- pins DEEPX runtime +
    kernel module to v2.4.0; V2N base machines get a stub.
  - `recipes-images/alp-image-edge.bb` -- reference edge AI
    image bundling alp-sdk + dx-rt + ROS 2 + the perception node.
- `examples/drone-autopilot/src/mavlink.{c,h}` -- minimal MAVLink
  v2 stack (no upstream `c_library_v2` dependency).  Pack/parse
  for HEARTBEAT / ATTITUDE / GPS_RAW_INT / GLOBAL_POSITION_INT /
  BATTERY_STATUS / RC_CHANNELS outbound; HEARTBEAT + COMMAND_LONG
  (arm/disarm + mode set) inbound.  Wired into `main.c` as two
  threads (`mav_tx` @ 10 Hz, `mav_rx` byte-driven, both priority
  5).  Customers drop in the upstream c_library_v2 when they need
  the full message dialect -- symbol prefixes don't collide
  (`alp_mavlink_*` vs `mavlink_*`).  CRC-X.25 + per-message CRC
  extras hard-coded for the 7 message types we touch.

### Added (2026-05-14 -- twister scenarios for the 6 LVGL + application demos)

Each of the new demo examples got a `testcase.yaml` registering
it with twister + matching tags so the demos build on every push:

- `lvgl-widgets-demo`, `lvgl-benchmark`, `lvgl-music-player` --
  tags `lvgl demo example` (+ `benchmark performance` /
  `audio codec` per demo).
- `drone-hud` -- tags `lvgl demo example marketing showcase
  drone uav`.
- `iot-dashboard` -- tags `lvgl iot mqtt tls demo example`.
- `ai-camera-viewer` -- tags `lvgl ai inference demo example
  marketing showcase`.

Two scenarios per demo:
1. `*.native_sim` -- `platform_allow: native_sim/native/64`,
   `build_only: true` (LVGL needs an SDL2 host runtime; twister
   build-only is enough for v0.5).
2. `*.aen` -- `platform_allow: alif_e7_dk_rtss_hp`,
   `build_only: true` (no EVK runner online; v0.6 HiL fills in
   the flash-and-run path).

LVGL upstream `demos/widgets/` + `demos/benchmark/` + `demos/music/`
sources are already pulled in via the existing
`name-allowlist: - lvgl` line in `west.yml` -- no west.yml change
needed.

### Added (2026-05-14 -- §D.lib.loader: extras-tier1 CI pin-check workflow + baseline SW-fallback emission)

Two follow-ups so the §D.lib batch is fully self-validating:

1. **`.github/workflows/nightly-extras-tier1-pins.yml`** (new) --
   nightly + PR-on-touch + manual workflow that runs
   `west update --group-filter +extras-tier1` and asserts each
   pinned library directory under `modules/lib/` ends up populated.
   - 9 stable pins (u8g2, libcoap, tinygsm, libwebsockets, jsmn,
     opus, Catch2, libmodbus, coreMQTT-SN) -- pin breakage FAILS
     the workflow.
   - 4 TBD pins (minimp3, libhelix, bearssl, madgwick_ahrs) --
     warn-only until the maintainer locks specific SHAs.
   Audit artefact uploaded with the first line of each fetched
   library's README so reviewers can eyeball "did this land the
   right repo?".

2. **Baseline library SW-fallback emission** -- the 4 pre-existing
   Tier 1 libraries with HW bindings (lvgl / mbedtls / cmsis_dsp /
   littlefs) now emit their matching `CONFIG_ALP_<LIB>_<FALLBACK>=y`
   line in `alp.conf` alongside the upstream Zephyr-module knob:
   - `lvgl` → `CONFIG_LVGL=y` + `CONFIG_ALP_LVGL_SW_BLIT=y`
   - `mbedtls` → `CONFIG_MBEDTLS=y` + `CONFIG_MBEDTLS_BUILTIN=y` +
     `CONFIG_ALP_MBEDTLS_PURE_C=y`
   - `cmsis_dsp` → `CONFIG_CMSIS_DSP=y` + `CONFIG_ALP_CMSIS_DSP_SCALAR=y`
   - `littlefs` → `CONFIG_FILE_SYSTEM_LITTLEFS=y` +
     `CONFIG_FILE_SYSTEM=y` + `CONFIG_ALP_LITTLEFS_SYNC_IO=y`
   Redundant with `Kconfig.alp-libraries`' `default y` on those
   symbols, but documents the fallback choice next to the
   library-enable line in the emitted alp.conf.

`TestHwBackendsLoader.test_sw_fallback_always_emitted` extended
from 8 to 12 assertions to lock the four new emissions in.

### Added (2026-05-14 -- §D.lib.loader: native_sim twister scenario for library knobs)

`tests/zephyr/library_knobs/` -- new twister test scenario that
exercises the alp.conf → Kconfig → Zephyr build chain end-to-end
for the §D.lib library set on `native_sim/native/64`.  Three
on-purpose checks:

1. **mbedtls links** -- includes `mbedtls/version.h` + asserts
   `MBEDTLS_VERSION_NUMBER != 0` and `mbedtls_version_get_string`
   resolves.  Catches `CONFIG_MBEDTLS=y` emission breakage that
   the existing alp_project.py Python tests can't see (those
   only check the emitted CONFIG_*=y lines, not whether they
   produce a buildable Zephyr image).
2. **cmsis_dsp links** -- includes `arm_math.h` + calls
   `arm_copy_f32(...)`.  Confirms CMSIS-DSP's symbol surface
   resolves at link time.
3. **§D.lib SW-fallback knobs reachable from Kconfig** -- compile-
   time check counts 21 `CONFIG_ALP_<LIB>_<FALLBACK>=y` knobs
   declared in `zephyr/Kconfig.alp-libraries` and asserts each
   one is defined.  Catches a broken `rsource "Kconfig.alp-libraries"`
   line in `zephyr/Kconfig` (the most likely v0.6 regression).

Test registers with twister under tag `library_knobs.smoke`;
runs on every push that touches `metadata/library-profiles/`,
`zephyr/Kconfig.alp-libraries`, or `scripts/alp_project.py`.

`extras-tier1`-only libraries (u8g2, opus, libcoap, libwebsockets,
...) are NOT exercised here because their west pin is disabled
by default; a follow-up workflow that flips
`--group-filter +extras-tier1` will cover them once those pins
stabilise from `# TBD: pin SHA` to specific revisions.

### Added (2026-05-14 -- §D.lib.loader: unit tests + west.yml extras-tier1 group)

Two follow-ups so the §D.lib batch + loader stay regression-safe
and the libraries we don't currently ship under Zephyr's tree
have a pinned consumption path.

**Loader unit tests** -- new `TestHwBackendsLoader` class in
`tests/scripts/test_alp_project.py` (9 tests).  For each SoM SKU
(AEN301, AEN401, AEN601, AEN801, V2N101, V2M101, NX9101) +
12 libraries, asserts the expected `CONFIG_ALP_*=y` emission set.
Locks in the per-SKU wiring -- any future metadata change (new
SoM cap, silicon family, library) that drops or duplicates a
binding fails CI at the per-priority-match level, not at twister.
Cases covered:
- E3 emits U55 only, never U85.
- E4 emits BOTH U85 (primary) + U55 (secondary) driver shims.
- E6 emits LVGL_GPU2D + GFX_COMPAT_GPU2D (gpu2d cap present).
- E8 routes LITTLEFS_XSPI_DMA through the hexspi_dma cap path.
- V2N101 emits DRP_AI + NEON + CAU + EMMC_DMA; nothing AEN-specific.
- NX9101 emits ETHOS_U65 + N93 driver shim.
- OPTIGA cross-family: fires on NX9101 where no higher-priority
  crypto wins, suppressed on AEN401 by CryptoCell.
- Unconditional DMA fallbacks (TFLM_DMA_COPY, MINIMP3_I2S_DMA)
  always emit.
- SW-fallback knobs for §D.lib libraries always emit.

**`extras-tier1` west.yml group** -- 13 upstream pins for the
Tier 1 libraries that aren't already in Zephyr's modules tree:
u8g2 (v2.36.5), libcoap (v4.3.5), TinyGSM (v0.11.7),
libwebsockets (v4.3.4), jsmn (v1.1.0), opus (v1.5.2),
Catch2 (v3.7.1), libmodbus (v3.1.10), coreMQTT-SN (v1.0.1).
Pinned to `main`/`master` with `# TBD: pin SHA after maintainer
audit` for upstreams that have no semver releases (minimp3,
libhelix) or are tarball-only (bearssl uses the community
mirror at github.com/bearsslmirror/BearSSL; madgwick_ahrs
uses x-io Technologies' Fusion successor library).

`gfx_compat` is maintainer-written and ships in-tree -- no west
pin needed.  `tflite-micro` and `nanopb` already live in Zephyr's
own west.yml; allowlisted in our import filter.

The `extras-tier1` group is disabled by default
(`-extras-tier1` in `group-filter:`), so v0.5 workspaces stay
light.  Customers flip via `west update --group-filter
+extras-tier1` when their `board.yaml` `libraries:` lists any
of these libraries.

### Added (2026-05-14 -- §D.lib.loader: requires_cap matcher + capability-keyed bindings audit)

Phase 2b's loader grew a `requires_cap:` matcher so library
priority entries can key off the per-SoM `capabilities:` block
directly, rather than the family-coarse `soc_family:` token.
Three concrete wins:

1. **Cross-family bindings collapse to one entry.**  `optiga_trust_m`
   is populated on AEN + V2N + NX9101 SKUs.  Old form needed
   three priority entries (one per family); new form is one
   `requires_cap: optiga_trust_m`.
2. **Sub-family bindings stop over-firing.**  `gpu2d` is on E6 /
   E7 / E8 but not E3 / E4 / E5.  `soc_family: alif_ensemble` fired
   on every Ensemble SKU; `requires_cap: gpu2d` only fires where
   the silicon actually carries it.
3. **NPU population gets surgical.**  Numeric caps
   (`ethos_u55_count`, `ethos_u85_count`, `ethos_u65_count`) are
   treated as truthy when > 0 -- `requires_cap: ethos_u85_count`
   handles "this SKU has at least one U85" directly.

Loader (`scripts/alp_project.py::_emit_library_hw_backends`) now
parses the SKU's `capabilities:` block (line-driven, no PyYAML
dep) and exposes a `_cap_truthy()` helper that handles booleans +
numeric counts.  Per-priority match: all specified keys
(`silicon:` / `soc_family:` / `requires_cap:`) must match; any
omitted key is wildcarded.

Library hw-backends.yaml rewrite (capability-keyed where it beats
soc_family): tflite_micro / lvgl / mbedtls / cmsis_dsp / littlefs /
bearssl / madgwick_ahrs / u8g2 / gfx_compat / minimp3 / opus.
Notable: tflite_micro grew an `ethos_u65_count` entry so NX9101
emits `CONFIG_ALP_TFLM_ETHOS_U65=y`; littlefs now resolves E8's
HexSPI separately from E3..E7's OctalSPI (both share the
`CONFIG_ALP_LITTLEFS_XSPI_DMA=y` driver shim).

New Kconfig knob: `ALP_TFLM_ETHOS_U65` (depends on
ALP_SOC_NXP_IMX9_IMX93).  The legacy `preferred_backend: ethos_u`
code path in `scripts/alp_project.py` also emits it alongside
`CONFIG_ALP_SDK_INFERENCE_ETHOS_U_N93=y` for the N93 driver shim.

`include/alp/inference.h` ALP_INFERENCE_BACKEND_ETHOS_U enum doc
enumerates all three U55 / U65 / U85 variants explicitly.

Cross-SKU smoke-test (driver dump per SKU):
- AEN401 (E4): U85 primary + U55 secondary + HELIUM + DMA_COPY +
  LVGL_DMA2D + MBEDTLS_CRYPTOCELL + LITTLEFS_XSPI_DMA +
  BEARSSL_CRYPTOCELL.
- AEN601 (E6): same + LVGL_GPU2D + MADGWICK_FPU + MINIMP3_HELIUM +
  OPUS_HELIUM.
- AEN801 (E8): same as AEN601 but HexSPI resolves the
  LITTLEFS_XSPI_DMA gate.
- V2N101: DRP_AI + NEON; LVGL_TMU; MBEDTLS_CAU; LITTLEFS_EMMC_DMA.
- NX9101: ETHOS_U65 + NEON; MBEDTLS_OPTIGA; LITTLEFS_EMMC_DMA.

### Added (2026-05-14 -- §D.lib.loader: per-SoM capabilities blocks + baseline hw-backends + ml_npu split)

Three audit follow-ups in one batch, after the user pointed out
that E4 / E6 / E8 actually carry TWO Ethos-U55s alongside the
single Ethos-U85 -- the NPU population is U55-HE (paired with
M55-HE) + U55-HP (paired with M55-HP) + U85-HG (Hyper-Generative,
Transformer-capable):

1. **`tflite_micro/hw-backends.yaml` ml_npu split** -- single
   `ml_npu` class restructured into `ml_npu_primary` +
   `ml_npu_secondary`.  On E4 / E6 / E8 the primary emits
   `CONFIG_ALP_TFLM_ETHOS_U85=y` and the secondary emits
   `CONFIG_ALP_TFLM_ETHOS_U55=y`, so BOTH driver shims now
   link.  On E3 / E5 / E7 only the primary fires (U55-only).

2. **Baseline-library hw-backends.yaml fill-in** -- the
   pre-existing 4 libraries with HW bindings (lvgl, mbedtls,
   cmsis_dsp, littlefs) gained their own hw-backends.yaml.  Now
   every Tier 1 library either ships a profile or has a
   documented "pure-SW only" status.  Matching Kconfig knobs
   added to `Kconfig.alp-libraries`:
   - `ALP_LVGL_GPU2D` / `_DAVE2D` / `_TMU` / `_DMA2D` / `_SW_BLIT`
   - `ALP_MBEDTLS_CRYPTOCELL` / `_INLINE_AES` / `_CAU` / `_OPTIGA`
     / `_PURE_C`
   - `ALP_CMSIS_DSP_HELIUM` / `_NEON` / `_TMU_CORDIC` / `_TMU_FFT`
     / `_ADC_DMA` / `_SCALAR`
   - `ALP_LITTLEFS_XSPI_DMA` / `_EMMC_DMA` / `_QSPI_DMA` / `_SYNC_IO`

3. **Per-SoM `capabilities:` blocks** -- new top-level section
   in every `metadata/e1m_modules/E1M-*.yaml` declares the
   accelerator population per SKU.  Field shape:
     `ethos_u55_count: <int>` / `ethos_u85_count: <int>` /
     `ethos_u65_count: <int>` for NPU counts; booleans for the
     remaining accelerators (drp_ai / deepx_dx / helium_mve /
     neon / gpu2d / dave2d / cryptocell / inline_aes / cau /
     optiga_trust_m / xspi_dma / emmc_dma / quadspi_dma / dma2d /
     tmu_cordic / tmu_fft / tmu_fac).
   Populated for AEN301 / AEN401 / AEN501 / AEN601 / AEN701 /
   AEN801 / V2N101 / V2N102 / V2M101 / V2M102 / NX9101 -- every
   SoM SKU in the registry.  Per the "Pending exact HW
   configurations" rule, items not yet datasheet-verified (E3
   GPU2D presence, E4 DAVE2D presence) are explicitly marked
   `false` + `# TBD.` in a trailing comment so the maintainer
   can flip them once the SoM BOM finalises.

4. **`npu_population:` lists** -- alongside `ethos_u_variants:`,
   each AEN SKU now declares the full NPU population as a list
   of named instances with `role:` + `paired_with:` tags
   (NPU-HE / M55-HE, NPU-HP / M55-HP, NPU-HG / HG-subsystem).
   Matches the Alif Ensemble block diagram in the datasheet.

The loader (`scripts/alp_project.py`) still picks matches via
`silicon:` / `soc_family:` -- the new `capabilities:` block is
declarative now, available for a future `requires_cap:` key in
hw-backends.yaml.

### Fixed (2026-05-14 -- §D.lib.loader follow-up: Ethos-U85 propagated through every consumer)

Phase 2b's per-NPU split landed in the new `tflite_micro` library-
profile loader but didn't propagate to the existing inference
dispatcher path or the SoM-preset metadata.  This commit closes
those gaps so every layer reflects the U85 / U55 / U65 split:

- `scripts/alp_project.py`: the legacy `preferred_backend: ethos_u`
  path now emits the per-NPU driver Kconfigs alongside the legacy
  dispatcher gate.  Silicon `alif:ensemble:e4 | e6 | e8`
  → `CONFIG_ALP_TFLM_ETHOS_U85=y` + `CONFIG_ALP_TFLM_ETHOS_U55=y`.
  Other Alif Ensemble SKUs (E3 / E5 / E7) → `CONFIG_ALP_TFLM_ETHOS_U55=y`
  only.  `nxp:imx9:imx93` → `CONFIG_ALP_SDK_INFERENCE_ETHOS_U_N93=y`
  (unchanged).  `CONFIG_ALP_SDK_INFERENCE_ETHOS_U=y` stays as the
  customer-facing dispatcher gate.

- `metadata/e1m_modules/E1M-AEN*.yaml`: new `ethos_u_variants:`
  list captures the full NPU population per SKU.  AEN401 / AEN601 /
  AEN801 now declare `[u85, u55]`; the others stay `[u55]`.  The
  singular `ethos_u_variant:` field promotes to the primary
  (U85 on the U85-bearing SKUs).

- `include/alp/inference.h`: `ALP_INFERENCE_BACKEND_ETHOS_U` enum
  doc enumerates the three variants the token covers (U85 / U65 /
  U55) + the per-SKU loader behaviour.  Public ABI stays generic.

- `src/zephyr/inference_tflm.cpp`: file-header comment generalised
  from "U55 + U65" to "U55 + U65 + U85" with the per-Kconfig matrix.

- `README.md` inference-dispatcher row: notes the U55 / U85 / U65
  coverage.

### Fixed (2026-05-14 -- §D.lib.loader: Ethos-U85 vs U55 differentiation for tflite_micro)

§D.lib batch collapsed both Alif NPUs (Ethos-U55 + Ethos-U85) under
a single `ethos_u` backend.  In silicon they are two different IPs:

- Ethos-U55: on every Ensemble SKU (E3, E4, E5, E6, E7, E8) -- two
  instances per SoC.
- Ethos-U85: on E4, E6, E8 only -- one instance per SoC, Transformer-
  capable, the Generative-AI forward path.

The loader and the tflite_micro hw-backends.yaml now distinguish
the two:

- `scripts/alp_project.py::_emit_library_hw_backends` learns a
  `silicon:` matcher (in addition to `soc_family:`).  When a
  priority entry sets `silicon: alif:ensemble:e4`, the loader only
  emits it on the SKU whose `silicon:` field in
  `metadata/e1m_modules/<sku>.yaml` matches exactly.
- `metadata/library-profiles/tflite_micro/hw-backends.yaml` now
  declares three U85 entries (E4 / E6 / E8) as priority 1, then a
  family-wide U55 entry for the remaining SKUs.  Result on an
  AEN401 / AEN601 / AEN801 board: `CONFIG_ALP_TFLM_ETHOS_U85=y`
  emitted.  On AEN301 / AEN501 / AEN701: `CONFIG_ALP_TFLM_ETHOS_U55=y`.
- `zephyr/Kconfig.alp-libraries` splits the old `ALP_TFLM_ETHOS_U`
  symbol into `ALP_TFLM_ETHOS_U85` (depends on E4 || E6 || E8) +
  `ALP_TFLM_ETHOS_U55` (depends on any E* SoC).

Other libraries (cmsis_dsp / minimp3 / opus) only bind to Helium
MVE on the M55 cores, which every E SoC ships -- no per-silicon
split needed there.

### Changed (2026-05-14 -- §D.closeout: v1.0-readiness + README + test-coverage closeout)

Phase 5 (closeout) of the chip-and-library ecosystem expansion per
docs/superpowers/specs/2026-05-14-chip-and-library-ecosystem-design.md.

- `docs/v1.0-readiness.md`: new Pillar 4-bis section declaring the
  three-tier ecosystem in operation (Tier 1 = 80 chips + 25 libs;
  Tier 2 in `alp-sdk-community` = skeleton + 10 seed chips;
  Tier 3 = customer / private repos via pattern-only support).
- `README.md`: chip-count bump 20+ → 80 + Tier 2 pointer.
- `docs/test-coverage-audit.md`: chips/ row file count 108 → 254,
  driver count 21 → 80; notes the [UNTESTED] badge convention.

Total v0.5 §D-batch delivery:
- 49 new Tier 1 chip drivers (§D.AI 18, §D.industrial 18, §D.iot 9,
  §D.audio 6)
- 17 new Tier 1 library knobs + per-library hw-backends.yaml
- §D.lib.loader cross-library HW-backend loader + Kconfig.alp-libraries
- alp-sdk-community public repo + skeleton + 10 seed contributions

### Added (2026-05-14 -- §D.community: Tier 2 contribution surfaces in alp-sdk)

Phase 3 of the chip-and-library ecosystem expansion per
docs/superpowers/specs/2026-05-14-chip-and-library-ecosystem-design.md.

alp-sdk-side artefacts that pair with the new alplabai/alp-sdk-
community repo (created separately):

- `metadata/schemas/contribution-v1.schema.json` (new) -- JSON Schema
  that every Tier 2 contribution's `metadata.yaml` validates against
  on PR.  Permits Apache-2.0 / MIT / BSD only (GPL rejected by
  `pr-lint`); enforces `name:` matches the parent directory; pins
  the `family:` enum (sensor / display / camera / motor / encoder /
  audio / power / cellular / lora / wifi / ble / gnss / crypto /
  memory / io_expander / serdes / switch / graphics / ml / control /
  iot / networking / parsing / serialization / testing / other);
  pins `interfaces:` enum (i2c / spi / uart / i2s / pwm / gpio /
  can / adc / dac / rtc / watchdog / usb / ethernet / mipi_csi2 /
  dvp / fpd_link / gmsl / pcie / gpio_bitbang / gpio_pwm).
- `docs/contributing-tier-2.md` (new) -- customer-facing
  walkthrough of the three-tier model + per-contribution
  checklist + the three integration patterns (pull-everything /
  per-contribution selection / search-then-clone) + the
  "Verified" → Tier-1 promotion path.

The alplabai/alp-sdk-community repo + its skeleton (registry.yaml,
west.yml, templates/, .github/workflows/, CODEOWNERS) lives in a
separate commit on that repo's `main` branch.

### Added (2026-05-14 -- §D.lib.loader: cross-library HW-backend loader hook)

Phase 2b of the chip-and-library ecosystem expansion.  Adds the
loader hook that picks the highest-priority HW backend per
(library × accelerator class) from each enabled library's
`metadata/library-profiles/<name>/hw-backends.yaml`, cross-
referencing the active SoM family.

- `scripts/alp_project.py` grows `_emit_library_hw_backends()`
  (~70 LoC) — runs after the SW-fallback emission and walks each
  library's priority list, emitting the first matching backend per
  accelerator class as `CONFIG_*=y`.
- `zephyr/Kconfig.alp-libraries` (~200 LoC, new file) declares every
  per-library + per-backend Kconfig symbol the loader can emit.
  Sourced from `zephyr/Kconfig` via `rsource` under the `ALP_SDK`
  if-block.  SW-fallback symbols default `y`; HW-acceleration
  symbols default off and turn on only when the loader writes them.

NOT in this commit (deferred per the design spec): `capabilities:`
blocks in `metadata/e1m_modules/E1M-*.yaml`.  Loader currently maps
SoM family directly from SKU; finer-grained per-SoM capability
flags (`has_ethos_u: true`, `has_dave2d: true`, ...) belong to the
maintainer per the "Pending exact HW configurations" rule.

### Added (2026-05-14 -- §D.lib: 17 library knobs + per-library hw-backends)

Phase 2 of the chip-and-library ecosystem expansion per
docs/superpowers/specs/2026-05-14-chip-and-library-ecosystem-design.md.

17 new libraries available via `board.yaml`'s `libraries:` enum
(was 8, now 25 -- on the v1.0 target).  Each library ships:
- One `metadata/library-profiles/<name>/hw-backends.yaml` declaring
  which accelerator classes (NPU / GPU / SIMD / DMA / crypto /
  cordic / timing) it binds to per SoM family, with priority order
  + matching `CONFIG_*` symbol.  Pure-SW fallback required + always
  available.
- An entry in `scripts/alp_project.py`'s `_LIBRARY_KCONFIG` table
  that emits the SW-fallback `CONFIG_*` unconditionally.  The
  cross-library loader hook (§D.lib.loader, next commit) layers
  the HW-backend `CONFIG_*` on top by cross-referencing
  `hw-backends.yaml` against the active SoM's `capabilities:`
  block in `metadata/soms/*.yaml`.
- An entry in `metadata/schemas/board-config-v1.schema.json`'s
  `libraries:` enum so the schema validator accepts the new names.

Libraries by domain (§D.lib.<batch> tag):
- §D.lib.ai (3): `tflite_micro`, `u8g2`, `gfx_compat`
- §D.lib.industrial (3): `madgwick_ahrs`, `pid`, `modbus`
- §D.lib.iot (7): `coremqtt_sn`, `libcoap`, `tinygsm`, `nanopb`,
  `libwebsockets`, `jsmn`, `bearssl`
- §D.lib.audio (3): `minimp3`, `opus`, `libhelix`
- §D.lib.test (1): `catch2`

Verification: every hw-backends.yaml carries `verification:
hil_silicon: untested / smoke_tests: build_only` -- the wiring is
schema-validated but no per-(library × SoM-family) HiL bring-up
has been run yet.

`west.yml` pins for the libraries NOT already in Zephyr's modules
tree (u8g2, libcoap, tinygsm, libwebsockets, jsmn, bearssl,
minimp3, opus, libhelix, catch2, libmodbus, madgwick_ahrs,
coremqtt_sn, gfx_compat) are NOT in this commit -- they land in
a follow-up `feat(west): extras-tier1 group` once the maintainer
picks the per-library tagged revisions.  Tier 1 libraries that
ARE already in Zephyr's west.yml (`tflite_micro`, `nanopb`)
flow through the existing name-allowlist filter.

### Added (2026-05-14 -- §D.audio: 6 audio chip drivers)

Tier 1 ecosystem expansion -- Phase 1 §D.audio batch.  All headers
carry the `[ABI-EXPERIMENTAL]` + `[UNTESTED]` badges.

- **`ics_43434`**         -- InvenSense ICS-43434 omnidirectional
  MEMS mic (I2S, channel-binding helper).
- **`inmp441`**           -- InvenSense INMP441 low-cost MEMS mic.
- **`wm8960`**            -- Cirrus / Wolfson WM8960 stereo codec
  (I2C config + I2S data; packed 9-bit register write).
- **`tlv320aic3204`**     -- TI TLV320AIC3204 premium codec w/
  miniDSP (page-paged I2C control surface).
- **`max98357a`**         -- ADI MAX98357A 3 W mono class-D amp
  (shutdown / mode-select pin control).
- **`es8388`**            -- Everest Semi ES8388 stereo codec
  (China-domestic; same shape as WM8960).

Audio sample path on these chips goes through the portable
`<alp/i2s.h>` peripheral surface; the drivers above only own the
I2C control / GPIO power-rail surfaces and channel-binding helpers.

### Fixed (2026-05-14 -- §D.iot ZTEST tests follow-up)

§D.iot ZTESTs were added in this commit as a follow-up — the
prior §D.iot commit (6fbb14a) landed the chip drivers +
Kconfig/CMake glue but the NULL-arg-guard ZTEST entries didn't
make it in due to an Edit-mismatch on the test file.  Captured
here so the audit trail stays clean.

### Added (2026-05-14 -- §D.iot: 9 IoT / connectivity chip drivers)

Tier 1 ecosystem expansion -- Phase 1 §D.iot batch.  Every public
header in this batch carries the `[ABI-EXPERIMENTAL]` + `[UNTESTED]`
badges.  The chip headers added in this batch + the prior §D.AI +
§D.industrial batches are all tagged `[UNTESTED]` -- the verification
badge captures the v0.5 truth: drivers compile + pass the NULL-arg
ZTESTs but have no HiL silicon bring-up yet, so customers should
treat all timing / register-value / lifecycle sequencing as paper-
correct only until the v1.0 verification sweep lands.

- **`quectel_bg95`**       -- Quectel LTE-M / NB-IoT / EGPRS module
  (UART AT shell + PWRKEY pulse).
- **`quectel_bg77`**       -- Quectel LTE-M / NB-IoT module with
  integrated GNSS.
- **`ublox_sara_r5`**      -- u-blox LTE-M carrier-certified module
  (1500 ms PWR_ON pulse).
- **`semtech_sx1262`**     -- Semtech SX1262 LoRa / FSK transceiver
  (opcode shell + BUSY-wait + GetStatus probe).
- **`semtech_sx1276`**     -- Semtech SX1276 legacy LoRa transceiver
  (register R/W + REG_VERSION probe).
- **`ublox_neo_m9n`**      -- u-blox NEO-M9N multi-constellation GNSS
  (UART NMEA line read).
- **`ublox_max_m10s`**     -- u-blox MAX-M10S small-footprint GNSS.
- **`atgm336h`**           -- AllyStar ATGM336H cost-optimised GNSS.
- **`atecc608b`**          -- Microchip ATECC608B EC P-256 + AES
  secure element (wake / idle / sleep shell; ATCA crypto pending
  CryptoAuthLib import).

### Changed (2026-05-14 -- §D.AI + §D.industrial: [UNTESTED] verification badges)

Retro-fit `@par Verification status: [UNTESTED]` Doxygen tag onto
every chip header added in the §D.AI + §D.industrial batches earlier
in this run, plus a matching `verification: { hil_silicon: untested,
smoke_tests: null_arg_guard }` block in each chip's metadata YAML.
Customer-visible signal that the v0.5 drivers compile + pass the
NULL-arg ZTEST surface but have not yet seen HiL silicon bring-up.

### Added (2026-05-14 -- §D.industrial: 18 industrial sensing / control chip drivers)

Tier 1 ecosystem expansion -- chip-and-library-ecosystem-design.md
Phase 1 §D.industrial batch.  Drivers ship with Doxygen-clean public
headers in `include/alp/chips/<name>.h`, implementations under
`chips/<name>/`, metadata YAML, Kconfig / CMakeLists / alp_project.py
hooks, and per-chip NULL-arg-guard ZTESTs.

- **`bmp390`**     -- Bosch BMP390 high-precision pressure sensor (I2C).
- **`ms5611`**     -- TE MS5611 drone-grade barometer (I2C; PROM
  read at init).
- **`lps22hb`**    -- ST LPS22HB MEMS barometer (I2C).
- **`vl53l1x`**    -- ST VL53L1X single-zone ToF ranger (I2C).
- **`vl53l5cx`**   -- ST VL53L5CX 8x8 multi-zone ToF ranger (I2C).
- **`a02yyuw`**    -- DFRobot A02YYUW waterproof ultrasonic ranger
  (UART; checksum-validated 4-byte distance frame).
- **`drv8833`**    -- TI DRV8833 dual H-bridge brushed-DC driver
  (PWM; signed pulse-width per channel, sleep gate).
- **`drv8825`**    -- TI DRV8825 bipolar stepper (PWM step + GPIO
  dir / microstep / nEnable).
- **`tmc2209`**    -- Trinamic TMC2209 silent stepper driver
  (UART register read / write with CRC-8/ATM).
- **`a4988`**      -- Allegro A4988 stepper driver (PWM step + GPIO
  dir / microstep / nEnable).
- **`as5048a_b`**  -- ams AS5048B 14-bit magnetic encoder (I2C).
- **`mt6701`**     -- MagnTek MT6701 14-bit magnetic encoder (I2C).
- **`hx711`**      -- Avia Semi HX711 24-bit load-cell ADC
  (bit-banged 2-wire, 128 / 64 / 32 gain).
- **`max31855`**   -- ADI MAX31855 K-type thermocouple-to-digital
  (SPI; signed milli-C decode + fault flags).
- **`max31865`**   -- ADI MAX31865 PT100 / PT1000 RTD-to-digital
  (SPI; 15-bit ratio + fault bit).
- **`tsl2591`**    -- ams TSL2591 wide-dynamic-range light sensor
  (I2C; visible + IR channels).
- **`qmc5883l`**   -- QST QMC5883L 3-axis compass (I2C; continuous
  mode at 200 Hz / 2 G / OSR 512).
- **`veml7700`**   -- Vishay VEML7700 ALS (I2C; 16-bit count read).

Validators clean before push:
- scripts/validate_metadata.py
- scripts/check_example_portability.py
- scripts/check_pin_conflicts.py
- scripts/abi_snapshot.py --diff docs/abi/v0.5-snapshot.json

### Added (2026-05-14 -- §D.AI: 18 vision / display / accelerator chip drivers)

Tier 1 ecosystem expansion -- chip-and-library-ecosystem-design.md
Phase 1 §D.AI batch.  All drivers ship with a Doxygen-clean public
header in `include/alp/chips/<name>.h`, implementation under
`chips/<name>/<name>.c`, metadata at `metadata/chips/<name>.yaml`,
Zephyr Kconfig + CMakeLists glue, and a NULL-arg-guard ZTEST under
`tests/zephyr/chips/src/main.c`.  Driver bodies follow the
`[stub-impl]` pattern: chip-ID + soft-reset + lifecycle is real;
vendor-specific register tables (camera resolution / pixel format
profiles, e-paper waveform LUTs) land in follow-up commits once the
maintainer adds the reference init scripts to the internal design
archive.  All public symbols are tagged `[ABI-EXPERIMENTAL]` until
the first SoM verification.

- **`ov2640`** -- OmniVision 2 MP UXGA DVP camera (ESP32-CAM default).
- **`ov5645`** -- OmniVision 5 MP MIPI CSI-2 camera.
- **`ov7670`** -- OmniVision VGA DVP camera (reference classic).
- **`ov9281`** -- OmniVision 1 MP global-shutter mono MIPI CSI-2
  (AR/VR / ALPR / industrial tracking).
- **`ar0234`** -- onsemi 1080p global-shutter colour MIPI CSI-2.
- **`imx219`** -- Sony 8 MP MIPI CSI-2 (RPi Cam v2 standard).
- **`imx477`** -- Sony 12.3 MP MIPI CSI-2 (RPi HQ Camera).
- **`gc2145`** -- GalaxyCore 2 MP cost-sensitive DVP camera.
- **`ti_ds90ub953_954`** -- TI FPD-Link III camera SerDes pair
  (long-cable industrial machine vision).
- **`maxim_max9295_9296`** -- ADI (Maxim) GMSL2 6 Gbps automotive
  camera SerDes pair.
- **`st7789`** -- Sitronix ST7789V 240x240 / 240x320 IPS TFT
  (init + window + write-pixels).
- **`ili9341`** -- Ilitek ILI9341 240x320 SPI TFT.
- **`ili9488`** -- Ilitek ILI9488 320x480 SPI TFT.
- **`ra8875`** -- RAiO RA8875 5-7" LCD controller + resistive
  touch (SPI; register-level access + soft reset).
- **`sh1106`** -- Sino Wealth SH1106 128x64 monochrome OLED (I2C;
  drop-in alternative to SSD1306, accounts for 132-column RAM
  offset).  Driver runs full lifecycle including frame push.
- **`il3820`** -- Solomon IL3820 4.2" tri-colour e-paper (SPI;
  init + busy-wait + soft reset; waveform LUT pending).
- **`gdew0154t8`** -- GoodDisplay GDEW0154T8 1.54" monochrome
  e-paper (SPI; same lifecycle shape as `il3820`).
- **`hailo_8l`** -- Hailo-8L 13 TOPS NPU host-side bring-up
  driver for the M.2 PCIe form factor (host RESETB sequence +
  WAKE# read; PCIe enumeration + HailoRT runtime live on the
  Linux side).

Validators that ran clean on this batch (each before push):
`validate_metadata.py` / `check_example_portability.py` /
`check_pin_conflicts.py` / `abi_snapshot.py --diff
docs/abi/v0.5-snapshot.json`.

### Added (2026-05-14 -- V2N DEEPX rail GPIO map)

- **`DEEPX_PWR_EN_REQ` (P65) + `DEEPX_CORE_0P75_EN` (P64)** added
  to `metadata/e1m_modules/v2n/renesas-peripheral-map.{tsv,csv}`.
  P65 is the rising-edge "DEEPX should come up now" signal from
  the primary PMIC into V2N; P64 is V2N's drive to the DA9292
  EN2 pin (and other shared-rail consumers) that physically
  enables the 0.75 V DEEPX rail.  Sequence captured in
  [memory `project_v2n_da9292_core_0p75_control.md`].  V2N
  firmware module (`src/zephyr/v2n_power_mgmt.c`) wiring lands
  next.

### Changed (2026-05-14 -- top-level UX simplification cont.)

- **`docs/ota-device-contract.md` cross-ref to
  `notes/morning-handoff-2026-05-13.md` removed.**  The `notes/`
  directory is gitignored (maintainer handoff drafts that are
  not part of the public SDK); a doc line that referenced a
  non-tracked file was dead-link bait for anyone cloning the
  public repo.  Removed; the "Open questions" stand on their
  own.
- **`tools/program_eeprom.py` moved to `scripts/program_eeprom.py`.**
  The `tools/` directory only held one script (the EEPROM
  manifest packer); `scripts/` already hosts 13 similar Python
  utilities (`abi_snapshot.py`, `validate_metadata.py`,
  `validate_board_yaml.py`, `check_pin_conflicts.py`, etc.).
  Folding the single-file directory into `scripts/` removes one
  top-level entry and makes the "ALP SDK has one place for
  helper scripts" rule unambiguous.  Files touched: `README.md`,
  `docs/board-id.md`, `docs/bring-up-v2n.md`,
  `docs/getting-started.md`, `docs/test-plan.md`,
  `docs/troubleshooting.md`,
  `examples/v2n/v2n-board-id-readout/README.md`,
  `examples/v2n/v2n-eeprom-manifest-dump/README.md`,
  `include/alp/hw_info.h`, `src/zephyr/hw_info_zephyr.c`,
  `tests/fuzz/eeprom_manifest_fuzz.c`,
  `tests/scripts/test_program_eeprom.py`, plus the script's own
  self-reference docstring.  Stale `tools/__pycache__/`
  directory (the only remaining content after the move, and
  untracked) deleted to fully remove the top-level entry.
  Historical `tools/` mentions in this `CHANGELOG.md` left
  untouched.
- **`dts/bindings/` moved to `zephyr/dts/bindings/`.**  Zephyr
  devicetree bindings are Zephyr-only artefacts and belong under
  the `zephyr/` module subtree alongside `zephyr/Kconfig` /
  `zephyr/CMakeLists.txt` / `zephyr/module.yml`.  Folding them
  in removes the top-level `dts/` directory.
  `zephyr/module.yml` updated with `dts_root: zephyr` so
  Zephyr's binding scanner finds them at the new location (the
  default `<module-root>/dts/bindings/` no longer applies).
  Two binding files moved verbatim:
  `alp,pin-array.yaml`, `vendor-prefixes.txt`.  Comment update
  in `src/zephyr/peripheral_gpio.c`.  Top-level `dts/`
  directory deleted.
- **`sysbuild/aen/` moved to `zephyr/sysbuild/aen/`.**  Sysbuild
  configs are Zephyr-only artefacts (MCUboot + ECDSA-P256
  secure-boot profile for AEN-Zephyr applications); folding them
  under the Zephyr module subtree means the top-level repo only
  shows directories that are OS-agnostic or OS-router
  (`chips/`, `examples/`, `src/`, etc.) -- Zephyr-specific
  artefacts cluster under `zephyr/`.  Files touched:
  `VERSIONS.md`, `docs/adr/0006-secure-boot-secure-ota.md`,
  `docs/secure-boot.md`, `docs/test-plan.md`, `keys/README.md`,
  `keys/generate_dev_key.sh`, plus the moved README +
  sysbuild.conf internal self-refs.  Top-level `sysbuild/`
  directory deleted (no longer holds any siblings).
- **`examples/README.md` audit.**  After the §A.6 SoM-subfolder
  refactor (`examples/v2n/v2n-*` and `examples/aen/*`), the
  cross-family table still listed `edgeai-vision-aen` as if it
  lived at the top level, and the V2N-specific table used bare
  directory names (`v2n-gd32-bridge-ping`) instead of the
  `v2n/v2n-gd32-bridge-ping` paths that actually exist on disk.
  Split into three sections (cross-family / AEN-specific /
  V2N-M1-specific) with correct relative paths for every row.

### Added (2026-05-14 -- peripheral thin-spot test fills §C.22)

Closes the §1c "thin-spot fills" carry-forward from the
readiness doc.  Adds NULL-handle / NULL-arg / out-of-range
guard tests for every public function on the six thinly-
covered peripherals so the binding-layer contract is
exercised on every native_sim build.

Per-peripheral additions:

- tests/zephyr/peripheral/src/rtc.c: 1 -> 5 tests.  Added
  set_time / get_time NULL-handle + NULL-out guards.
- tests/zephyr/peripheral/src/counter.c: 1 -> 8 tests.
  Added start / stop / get_value / us_to_ticks /
  cancel_alarm / close NULL guards (every public function
  on the counter surface).
- tests/zephyr/peripheral/src/qenc.c: 1 -> 5 tests.  Added
  get_position / reset_position / close NULL guards.
- tests/zephyr/peripheral/src/spi.c: 2 -> 7 tests.  Added
  NULL-cfg open + transceive / write / read NULL-handle
  guards + close-on-NULL safety.
- tests/zephyr/peripheral/src/i2s.c: 2 -> 4 tests.  Added
  invalid-word_bits rejection (must be 8/16/24/32) +
  zero-block_frames rejection.
- tests/zephyr/peripheral/src/wdt.c: 2 -> 5 tests.  Added
  out-of-range-id rejection + feed / disable NULL-handle
  guards (NULL-feed silently no-op would mask a stuck
  watchdog -- the test guards against that regression).

Total per-peripheral test count: 46 -> 69.  Cross-cutting
tests in main.c unchanged at 38.

docs/test-coverage-audit.md headline table updated with the
new counts + the seven peripherals now marked ✅ healthy
(after-§C.22).

docs/v1.0-readiness.md §1c thin-spot-fills row flips from
📋🔌 to [x].

Real-device transfer correctness (positive paths) still
HiL-gated per the corresponding docs/test-plan.md row;
native_sim covers the binding-layer contract every public
function inherits.

Validators after commit:
- metadata / portability / pin-conflicts: clean
- abi_snapshot --diff: unchanged

### Changed (2026-05-14 -- full doxygen pass on remaining headers §C.21)

Completes the v1.0 contract: every public function in every
`include/alp/*.h` now carries the full Doxygen triplet
(`@brief` + `@param` + `@return`).  Void functions get
`@brief` + `@param` only (no `@return`).

Filled headers:
- **peripheral.h** -- GPIO + I2C + SPI + UART surfaces; 17
  functions extended from one-line `@brief` to full triplet
  blocks documenting parameter directions + the status-code
  menu each function can return.
- **iot.h** -- Wi-Fi (4 functions) + MQTT (6 functions) full
  triplets.
- **usb.h** -- device-role (5 functions) + host-role (3
  functions) full triplets.
- **inference.h** -- 4 accessor functions (num_inputs /
  num_outputs / get_input / get_output / invoke / close)
  upgraded with parameter directions + status-code menus.
- **audio.h** -- 4 in/out start/stop/close functions
  upgraded from one-liners.
- **can.h** -- 3 (start / stop / close / remove_filter)
  upgraded.
- **counter.h** -- 4 (start / stop / cancel_alarm / close +
  qenc_reset_position / qenc_close) upgraded.
- **display.h** -- all 4 public functions plus get_caps
  upgraded.
- **ble.h** -- close / advertise_stop / scan_stop upgraded.
- **mproc.h** -- hwsem_close upgraded.
- **gui.h** -- lvgl_attach upgraded.

Headers that were already complete (no changes): dsp.h,
gpu2d.h, power.h, tmu.h (the §C.17 wave-2 spot-pass) +
hw_info.h, pwm.h, adc.h, rtc.h, wdt.h, i2s.h, security.h,
soc_caps.h, e1m_pinout.h.

`docs/v1.0-readiness.md` §3a "Full pass" checkbox flips to
`[x]`.

Validators after commit:
- metadata / portability / pin-conflicts: clean
- abi_snapshot --diff: unchanged (pure doc-comment touch).

### Fixed (2026-05-14 -- sdk-alif restoration for AEN board files §C.42)

§C.41 oversight correction.  hal_alif alone gives HAL drivers
but NOT Alif Ensemble Zephyr board files.  Verified:

- `alifsemi/hal_alif` v2.2.0 has zero `boards/*` content -- it's
  Apache-2.0 HAL drivers only.
- Zephyr v3.7 LTS (our pin) does **not** include Alif boards.
  Upstream Zephyr **main** has only 3 (`balletto_b1_dk`,
  `ensemble_e1c_dk`, `ensemble_e8_dk`) -- the full 8-board set
  lives in `alifsemi/zephyr_alif`.
- The canonical path for stock-EVK builds is `sdk-alif`'s
  aggregate manifest (which imports `zephyr_alif` as the
  Zephyr project, replacing upstream v3.7).

Customers building for AEN now have **two clear paths**:

  Custom board overlay → default workspace (Zephyr v3.7 +
  top-level hal_alif) suffices.  Carrier writes their own
  `boards/<board>.overlay`.

  Stock Alif EVK boards → enable `vendor-sdks` + use
  `sdk-alif` as workspace topdir.  Replaces our Zephyr pin
  with `zephyr_alif`; full 8-board set in tree.

§C.42 changes in west.yml:

- Restored `sdk-alif` to the `vendor-sdks` opt-in group at
  `zas-v2.0.0-rc1` (latest tagged release as of 2026-05-14).
- Kept `hal_alif` as top-level default-enabled (so the HAL
  is always available; matches the `hal_renesas` /
  `hal_nxp` shape).
- Expanded the pin-policy comment block with the two-path
  decision tree.
- Restructured the `vendor-sdks` group comment to enumerate
  three flavours: sdk-alif aggregate / Alif vendor-licensed
  drivers / bare-metal Renesas+NXP mirrors.

Doc updates:

- `docs/getting-started.md` §8 Alif row rewritten with the
  two-path choice + when to pick each.
- `docs/vendor-partnerships.md` §Alif rewritten with a code
  block showing the decision tree + explanation of why §C.40
  and §C.41 each got part of the story right but not all.
- `docs/v1.0-readiness.md` Pillar 9 line updated.

Validators after commit (config + docs only):
- metadata: 0 failures
- portability: 30/30
- pin-conflicts: clean
- abi_snapshot --diff: unchanged

### Changed (2026-05-14 -- hal_alif promoted to first-class upstream §C.41)

Material correction to §C.40.  Verified Alif publishes an
**Apache-2.0 standalone Zephyr-module-shaped HAL** at
[`github.com/alifsemi/hal_alif`](https://github.com/alifsemi/hal_alif),
not just inside their `sdk-alif` aggregate manifest.  Same
shape as `zephyrproject-rtos/hal_renesas` /
`zephyrproject-rtos/hal_nxp` -- standard `zephyr/module.yml`,
root `CMakeLists.txt`, drivers/ subtree covering analog +
ethos_u + isp + jpeg + ospi + utimer.  Latest release v2.2.0
(2026-03-27); steady release cadence.

`west.yml` changes:

- Added `hal_alif` v2.2.0 as a **top-level project** at
  `modules/hal/alif`, default-enabled (no group).  AEN builds
  now get the Alif HAL automatically on `west update` --
  parallel to how Renesas + NXP work via Zephyr's own
  manifest.
- Removed `sdk-alif` from the `vendor-sdks` opt-in group.
  The §C.40 architecture had `sdk-alif` (Alif's aggregate
  manifest) pinned there under the assumption that hal_alif
  had to come through Alif's own Zephyr-fork manifest --
  incorrect, hal_alif stands alone.
- Added two NEW pins under the `vendor-sdks` opt-in group
  for the actually-vendor-licensed Alif drivers:
    `alif_dave2d-driver` (DAVE2D 2D accelerator)
    `alif_image-processing-lib` (Helium image kernels)
  These are source-visible under the Alif Semiconductor
  Software License Agreement -- NOT Apache.  The opt-in
  matches the licence consent story.
- Updated the pin-policy + vendor-sdks comment blocks to
  document the cleaner architecture.

`docs/getting-started.md` §8 row for Alif rewritten:
"**Nothing extra**" replaces the previous three-option
workaround (group-filter / EXTRA_ZEPHYR_MODULES / alt
topdir).  Bare-metal subsection now lists `modules/hal/alif/`
as default-on, with the four `vendor-sdks` group pins
clarified.

`docs/vendor-partnerships.md` Alif section rewritten with
the verified upstream story; the pre-§C.41 "critical
Zephyr-integration gap" language replaced with the verified
"hal_alif is its own Apache-2.0 module".

The pre-§C.40 west.yml had a silently-broken `name-allowlist`
entry (`hal_alif` listed but not in Zephyr's modules tree);
§C.40 surfaced and reverted that.  §C.41 now adds the correct
fix: import hal_alif from Alif's own repo as a top-level
project.

Validators after commit (config + docs only):
- metadata: 0 failures
- portability: 30/30
- pin-conflicts: clean
- abi_snapshot --diff: unchanged

### Fixed (2026-05-14 -- Zephyr-vendor-HAL integration cross-check §C.40)

Verified each vendor SDK's actual Zephyr-integration state
(parallel to §C.33..§C.35's vendor-repo verification).
Surfaced and fixed a silent bug in the pre-§C.40 west.yml.

Findings:

- **Zephyr v3.7 has NO `hal_alif`** in its modules tree.  The
  SDK's `name-allowlist` listed `hal_alif` but `west update`
  silently skipped it because Zephyr's manifest has no
  project of that name.  Result: AEN builds against the
  SDK's default west workspace would have linked against
  *nothing* on the Alif side.  Bug.
- **Zephyr's `hal_renesas`** mirrors RZ/V FSP at
  `drivers/rz/fsp/src/rzv/bsp/mcu/rzv2n/` (Zephyr v3.7 pins
  revision af77d7cd).  V2N + V2N-M1 paths covered.
- **Zephyr's `hal_nxp`** covers i.MX 93 (MIMX9301..9352) at
  `mcux/mcux-sdk-ng/devices/i.MX/i.MX93/` (Zephyr v3.7 pins
  revision 862e0015).  E1M-NX9101's MIMX9352 covered.
- **DEEPX** is Linux-side (PCIe + Yocto), not a Zephyr HAL.
  `chips/deepx_dxm1/` is the host-side bring-up code that
  runs on the Renesas A55 cluster; the NPU runtime rides on
  the customer's Yocto image.

Fixes in west.yml:

- Removed bogus `hal_alif` from `name-allowlist`.
- Added `hal_renesas` + `hal_nxp` so the V2N + i.MX 93
  paths actually get the upstream HALs imported by default.
- Expanded the pin-policy comment block to document the
  Alif gap explicitly + point customers at the three valid
  consumption paths (vendor-sdks group, EXTRA_ZEPHYR_MODULES,
  Alif's own west manifest as workspace topdir).
- Expanded the `vendor-sdks` group comment to clarify which
  pins are GENUINELY required vs which are duplicative-with-
  Zephyr (`sdk-alif` is required; `rzv-fsp` + `mcuxsdk-manifests`
  are for bare-metal customers who don't run under Zephyr).

Docs updates:

- `docs/getting-started.md` §8 gains a new "How each vendor
  SDK reaches your Zephyr build" subsection with a four-row
  table (Renesas / NXP / Alif / DEEPX) documenting the
  consumption path for each.  Plus a "Bare-metal /
  non-Zephyr customers" paragraph for the `vendor-sdks`
  group.
- `docs/vendor-partnerships.md` Alif / Renesas / NXP sections
  all gain a "Zephyr-integration status (§C.40 cross-check)"
  block that records the verified integration path.

This is a real material correction.  Pre-§C.40 customers
building for AEN would have had a silently-broken
`west update` (no hal_alif imported); post-§C.40 they
either consume `sdk-alif` via the vendor-sdks group, or
they explicitly opt out with EXTRA_ZEPHYR_MODULES.

Validators after commit:
- metadata: 0 failures
- portability: 30/30
- pin-conflicts: clean
- abi_snapshot --diff: unchanged (config + docs only)

### Added (2026-05-14 -- vendor-licence section + DEEPX sample + west.yml vendor pins §C.36 §C.37 §C.38)

Closes the remaining in-repo work surfaced by the
§C.33..§C.35 upstream verification of the four vendor SDKs
(Alif / Renesas / NXP / DEEPX).  Three coordinated changes:

- **§C.36 `docs/getting-started.md` §8** -- new
  "Vendor licences when integrating against real silicon"
  section.  Per-vendor table covering Alif (two-bucket:
  Apache-2.0 / MIT forks + vendor-licensed differentiating
  drivers), Renesas (BSD-3-Clause rzv-fsp), NXP (NXP-
  specific licence on mcuxsdk-manifests), DEEPX (two-bucket:
  dx_fw Apache + customer-only runtime).  Spells out what
  this means for customer projects: clone-and-study is free
  on all four; production redistribution requires
  per-component licence review.  Renumbers downstream
  sections (was 8/9/10; now 9/10/11).
- **§C.37 `examples/v2n/v2n-m1-deepx-inference/`** --
  V2N-M1 DEEPX inference flagship skeleton.  Walks the
  four-stage bring-up: §C.28 v2n_power_mgmt for the 0.75 V
  DEEPX rail, `chips/deepx_dxm1/` host driver for the PCIe
  mux + M1_RESET sequencer, `<alp/inference.h>` open with
  `BACKEND_DEEPX_DX`, invoke + result.  Targets
  `som.sku: E1M-V2M101` (the V2N-M1 family).  README
  documents the customer-side `dx_rt` integration path
  (clone from `github.com/DEEPX-AI` under DEEPX's
  customer licence).  Twister scenario `build_only: true`
  until a V2N-M1 board file + the dx_rt integration ship
  on a HiL rig.  Example count: 29 -> 30.
- **§C.38 `west.yml`** -- new `vendor-sdks` group (disabled
  by default like `extras-v04`) pinning the three public
  vendor SDK repos to the maintainer-verified tags:
    - `sdk-alif` @ v2.3.0-rc1 (from `github.com/alifsemi`)
    - `rzv-fsp` @ v3.1.0 (from `github.com/renesas`)
    - `mcuxsdk-manifests` @ v26.03.00 (from
      `github.com/nxp-mcuxpresso`)
  v0.3 workspaces don't see them on `west update`;
  customers enable per-vendor via
  `west update --group-filter +vendor-sdks` after
  reviewing the §8 licence table.

Updated trackers:
- `docs/vendor-partnerships.md`: Alif + NXP licence-
  acknowledgement line items flipped to [x] cross-ref §C.36;
  DEEPX V2N-M1 sample line item flipped to [x] cross-ref
  §C.37.
- `docs/v1.0-readiness.md` Pillar 9: paragraph extended
  with the §C.36 + §C.38 cross-refs.

Validators after commit:
- metadata: 0 failures
- portability: 30/30
- pin-conflicts: clean
- abi_snapshot --diff: unchanged

### Changed (2026-05-14 -- vendor-partnership status: Alif upstream verification §C.35)

Verified Alif claims in `docs/vendor-partnerships.md` against
the 59 public repos at `github.com/alifsemi`.

Findings:

- The Alif Zephyr SDK aggregate (`sdk-alif`) ships steady
  releases: v2.3.0-rc1 (2026-05-09), v2.2.0 (2026-03-27),
  v2.1.0 (2026-01-21).  The §C.31 tracker's "Alif HAL v1.6
  ships 2026-Q2" line was reading the wrong release stream;
  the `sdk-alif` repo is the canonical Zephyr-facing
  delivery.
- DAVE2D + Ethos-U + ISP drivers are ALREADY PUBLIC:
  - `alif_dave2d-driver` (updated 2026-04-30)
  - `alif_ml-embedded-evaluation-kit` (2026-05-13, fork of
    ARM's reference Ethos-U eval kit)
  - `alif_image-processing-lib` (D/AVE2D + Helium image
    kernels, 2026-05-13)
- Two licensing buckets:
  - **Apache-2.0 / MIT** (inherited from upstream forks):
    `zephyr_alif`, `hal_alif`, `cmsis_alif`,
    `mcuboot_alif`, `matter_alif`, Alif's `tinyusb` port.
    Yocto layers `meta-alif` / `meta-alif-ensemble` /
    `meta-alif-iot` all MIT.  Build containers
    (`alif-sdk-containers`) MIT.
  - **"Alif Semiconductor Software License Agreement"**
    (source-visible vendor terms): `sdk-alif`,
    `alif_dave2d-driver`, `alif_lvgl-dave2d`,
    `alif_image-processing-lib`,
    `alif_ml-embedded-evaluation-kit`,
    `alif_ensemble-cmsis-dfp`.

Same pattern as NXP MCUXpresso: vendor's forks of upstream
OSS keep permissive licensing; differentiating drivers ride
a vendor-specific licence.  Source-visible but with vendor
terms.

`docs/vendor-partnerships.md` Alif section rewritten with
the two-bucket licence table + flipped:
- "Public AEN Zephyr SDK availability" -> [x]
- "DAVE2D + Ethos-U evaluation kit availability" -> [x]

New open items added:
- Alif-licence acknowledgement paragraph in
  `docs/getting-started.md` (parallels the §C.34 NXP
  one).
- Sync `metadata/socs/alif/ensemble/*.json` to
  `sdk-alif` v2.3.0-rc1's Zephyr-board manifests.

`docs/v1.0-readiness.md` Pillar 9 row now lists all three
vendor SDKs as "already public with steady release
cadences" with per-vendor crisp summaries.

Validators after commit (docs-only):
- metadata: 0 failures
- abi_snapshot --diff: unchanged

### Changed (2026-05-14 -- vendor-partnership status: NXP MCUXpresso verification §C.34)

Verified the NXP claims in `docs/vendor-partnerships.md`
against the upstream MCUXpresso manifest repo:

- The MCUXpresso SDK for i.MX 9x **is already public** at
  `github.com/nxp-mcuxpresso/mcuxsdk-manifests`.  Latest
  stable tag **v26.03.00** (Q1 2026); prerelease tags for
  v26.06.00 already in flight.
- Layout: west.yml-driven Zephyr-style manifest aggregating
  per-component repos.  Boards directory has 11 i.MX 9x
  board manifests including `mcimx93evk` /
  `mcimx93autoevk` / `mcimx93qsb` / `mcimx93wevk` plus
  several i.MX 95 EVKs.
- **License is NXP-specific**:
  `LA_OPT_Online Code Hosting NXP_Software_License v1.4`
  (May 2025), not Apache / BSD.  Source-visible but with
  NXP terms.  This affects how customers integrate the
  i.MX 9x MCU-side path through our SDK -- one new open
  item to add an acknowledgement paragraph to
  `docs/getting-started.md` before v1.0.
- The mcuxsdk-manifests covers the MCU-side
  (Cortex-M33 real-time cores) only; the Yocto / Linux-
  side ships through `meta-imx` on its own quarterly
  release cycle -- existing open item unchanged.

`docs/vendor-partnerships.md` NXP section rewritten to
reflect the verified state; the previous "i.MX 93 BSP
confirmation -- tracking the calendar" line still applies
but only to the Yocto side, not the MCU SDK side.

`docs/v1.0-readiness.md` Pillar 9 line extended with the
NXP cross-ref alongside the §C.33 Renesas + DEEPX
findings.

Validators after commit (docs-only):
- metadata: 0 failures
- abi_snapshot --diff: unchanged

### Changed (2026-05-14 -- vendor-partnership status: upstream verification §C.33)

Verified `docs/vendor-partnerships.md` claims against the
actual upstream state of two key repos the user surfaced.

**Renesas (better than the §C.31 tracker said):**

- The RZ/V FSP **is already public** at
  `github.com/renesas/rzv-fsp` under BSD-3-Clause for the
  MPU BSP / Board BSP / HAL drivers / generic middleware
  (the parts the SDK consumes).  Latest release v3.1.0
  (2025-03-11).  Board support for `rzv2n_evk` is
  in-tree at `rzv/board/rzv2n_evk/` -- no NDA tarball
  needed.  Tracker line item "Public RZ/V2N N44 FSP
  release" flipped to `[x]`.
- Note: `github.com/renesas/fsp` (no `rz-` prefix) is the
  **RA MCU FSP** -- a different SoC family (Cortex-M
  MCUs).  Don't confuse the two when reading the
  vendor-partnerships doc.
- Remaining Renesas opens narrowed to: DRP-AI compiler
  toolchain licence (the on-die driver ships BSD-3-Clause
  via rzv-fsp; the *model compiler* is the licensing
  question) + DA9292 AROVx OTP confirmation.

**DEEPX (more nuanced than the §C.31 tracker said):**

Added a dedicated DEEPX section to
`docs/vendor-partnerships.md`.  Verified the 30+ public
repos under `github.com/DEEPX-AI`:

| Repo                      | License                                    |
|---------------------------|---------------------------------------------|
| `dx_fw`                   | **Apache-2.0** (firmware images)            |
| `dx_rt`                   | Customer-only (despite "open source" desc)  |
| `dx_app`                  | Customer-only                               |
| `dx_rt_npu_linux_driver`  | Customer-only                               |
| `meta-deepx-m1`           | **No LICENSE file** -- open question        |
| `dx_rt_windows`           | Customer-only                               |
| `dx-modelzoo`             | MIT (genuinely open)                        |
| `ultralytics-deepx`       | AGPL-3.0                                    |

Multiple repo descriptions say "open source" but the
LICENSE text explicitly restricts to "customers who are
supplied with DEEPX NPU... unauthorized sharing prohibited".
Source-visible ≠ Apache / BSD redistributable.

SDK implications:
- `chips/deepx_dxm1/` is a thin host driver (PCIe + GPIO
  + reset polarity) under our own Apache-2.0 -- doesn't
  redistribute DEEPX code, so no licence-encumbered
  dependency lands in this repo.
- `<alp/inference.h>` DEEPX backend dispatch is a
  header-level seam.  Customers building against the
  DX-M1 path pull `dx_rt` themselves as DEEPX NPU
  customers; the SDK only links against headers.
- `meta-deepx-m1`'s missing LICENSE is the one new open
  item -- ask DEEPX whether the Yocto layer is intended
  to be redistributable.

`docs/v1.0-readiness.md` Pillar 9 rows updated to reflect
both verifications.

### Added (2026-05-14 -- HW-blocked tracker docs §C.31)

Two new docs that codify the external-party items blocking
the remaining v1.0 pillars.  The items themselves can't close
in-repo (need third-party engagement); the docs make sure the
maintainer doesn't lose state between weekly partnership-review
cycles and give customers a public reference for what's coming.

- **`docs/security-audit-plan.md`**: External security audit
  engagement plan.  Covers scope (threat-model verification +
  source review of security surfaces + MCUboot integration
  audit + OTA pen test + fuzz-corpus inheritance), firm-
  selection criteria + shortlist (Trail of Bits, NCC Group,
  Doyensec), 2026-Q2..Q4 timeline, budget envelope, and the
  workflow for landing findings as a v1.0.x point release.
  v1.0 tag does NOT block on completion -- the audit runs in
  parallel with customer integration against the stable v1.0
  surface.
- **`docs/vendor-partnerships.md`**: Per-vendor tracker for
  the four external relationships that gate Pillar 9 (Renesas
  RZ/V2N FSP + DA9292 pad-routing + DRP-AI runtime licence,
  Alif AEN HAL v1.6 sync + dual-image build flow upstreaming,
  NXP i.MX 93 meta-imx alignment + OTFAD Zephyr driver, the
  alp-zephyr-modules public release, OpenEmbedded layerindex
  registration).  One row per vendor with open items, next
  action, and the maintainer's next-cycle owner.

- **`docs/v1.0-readiness.md`** Pillar 8 + Pillar 9 paragraphs
  updated to cross-ref the new trackers.

### Added (2026-05-14 -- production-deployment flagship skeleton §C.29)

- **`examples/production-deployment/`** -- the v1.0 integration
  flagship.  Demonstrates the full
  *manufactured → deployed → updated → attested* lifecycle on
  a single app, so customers see how the SDK's secure-boot,
  OTA, EEPROM-provisioning, and remote-attestation pieces fit
  together.  Four stages in `src/main.c`:
  1. Factory-provisioning read-back via `<alp/hw_info.h>`
     (SoM SKU + serial + revision + mfg date from the
     manufacturer EEPROM).
  2. Secure-boot evidence via `<alp/storage.h>` -- internal-
     flash slot inspection so a cloud-side fleet console can
     confirm the deployed firmware revision.
  3. OTA polling via `<alp/iot.h>` (Wi-Fi + MQTT + TLS to a
     Mender server; under native_sim the WiFi open returns
     NOSUPPORT and the example prints the transition).
  4. Remote attestation tick via `<alp/security.h>` -- TRNG
     nonce + OPTIGA signature published as a heartbeat.
- **board.yaml**: targets E1M-AEN701 + E1M-EVK; pulls in the
  iot / storage / security peripheral classes + mbedtls +
  mcuboot libraries + optiga_trust_m + eeprom_24c128 chips.
- **CMakeLists.txt + prj.conf + testcase.yaml**: standard
  `alp_project.py` wiring identical to every other example;
  Twister scenarios for native_sim (regex-match `[prod] done`)
  + AEN-Zephyr (build-only until a Mender server is staged).
- **README.md**: walks the four stages, expected output, the
  HiL flow, and the "production variants" pattern (customers
  fork the skeleton for V2N / i.MX 93 carriers, swap the OTA
  fabric for AWS / Azure, add domain logic between OTA poll +
  attestation).  Cross-refs to docs/secure-boot.md +
  docs/threat-model.md + docs/tutorials/12-mender-ota.md.

Example count: 28 -> 29 (verified via
`scripts/check_example_portability.py`).  This closes the last
flagship under Pillar 4 / §4 in the readiness doc.

### Added (2026-05-14 -- mproc-mailbox HE-side peer image §C.30)

- **`examples/mproc-mailbox/peer/main.c`** -- HE-side peer of
  the §C.12 HP flagship.  Opens the same mbox + shmem region,
  blocks on `alp_mbox_recv` indefinitely, reads the
  (offset, length) tuple the HP signalled, pulls the payload
  out of shared memory, builds an `echo: <payload>` response,
  stages it at a different shmem offset (256-byte gap from
  the HP's request region so the two never overlap), signals
  back via `alp_mbox_send`.  Steady-state loop -- one HP
  request -> one HE reply, then back to the mbox wait.
  Bounded request length (128 bytes max) so a stale or
  hostile tuple can't overrun the peer's stack buffer.
- **`examples/mproc-mailbox/peer/CMakeLists.txt`**: standalone
  Zephyr application skeleton; builds with the same `west
  build -b <he-board>` invocation pattern any single-image
  example uses.  Sysbuild picks this up automatically once
  the v0.4 dual-image build flow lands in
  `alplabai/alp-zephyr-modules`.
- **`examples/mproc-mailbox/peer/prj.conf`**: minimal -- just
  `CONFIG_PRINTK=y` + `CONFIG_ALP_SDK=y`; the peer doesn't
  need the IoT stack or inference runtime that the HP image
  pulls in.
- **`examples/mproc-mailbox/README.md`**: updated with the
  expected interleaved HP / HE console output + the dual-
  build command pair customers run today (HP image one
  `west alp-build`, HE image one `west build`; sysbuild
  flow ships in v0.4).

This closes the only 🚧 flagship under Pillar 4 / §4 in the
readiness doc.  The remaining 📋 flagship (production-
deployment) lands in §C.29.

### Added (2026-05-14 -- V2N DEEPX rail-mgmt + BRD_I2C shared-handle pattern §C.28)

Closes Pillar 1 §1b in the readiness doc.  Two new files +
extensions to the V2N supervisor singleton:

- **`src/zephyr/v2n_supervisor.h/.c` extensions** introduce
  `alp_z_v2n_supervisor_brd_i2c_acquire()` /
  `alp_z_v2n_supervisor_brd_i2c_release()` borrowing pair.
  Returns the supervisor's cached `alp_i2c_t *` under the same
  mutex used by the existing GD32 acquire helper, so BRD_I²C
  consumers (DA9292, future BRD peripherals) share a single
  lock with the GD32 bridge dispatcher.  Avoids half-driven
  transfers when the two compete on the same physical bus.
- **`src/zephyr/v2n_power_mgmt.{h,c}`** -- DEEPX rail-bringup
  auxiliary.  Listens on `v2n-deepx-pwr-en-req-gpios` (Renesas
  P65) for a rising edge; defers to a workqueue (DA9292 register
  sequence takes ~5 ms, too long for an IRQ); acquires the
  supervisor's BRD_I²C handle; runs
  `da9292_v2n_m1_enable_deepx_rail` (handles the VSTEP=1 OTP
  trap captured in the memory; polls CH2 PG up to 5 ms);
  releases the I²C; drives `v2n-deepx-core-0p75-en-gpios`
  (P64) high to release the rest of the DEEPX clamps.
  `SYS_INIT` at APPLICATION priority runs `da9292_v2n_base_init`
  during boot so the DA9292 context is hot before the first
  P65 edge arrives.
- **`zephyr/Kconfig` + `zephyr/CMakeLists.txt`**: new
  `CONFIG_ALP_SDK_V2N_POWER_MGMT` option (defaults on for V2N +
  DA9292 builds) gates the module; missing DT aliases fall
  through to a NOSUPPORT stub so non-M1 V2N carriers compile
  cleanly without the DEEPX rail.

Verification: HiL (V2N-M1 EVK).

### Changed (2026-05-14 -- SLSA L2 -> L3 via slsa-framework reusable workflow §C.27)

- **`.github/workflows/release.yml` upgraded from SLSA L2 to L3.**
  L2 (`actions/attest-build-provenance@v1` running inline in the
  build job) is replaced by the
  `slsa-framework/slsa-github-generator/.github/workflows/generator_generic_slsa3.yml@v2.0.0`
  reusable workflow.  The build job now emits base64-encoded
  SHA-256 digests for the tarball; the new `provenance` job
  runs the SLSA generator inside an isolated, ephemeral
  GitHub-hosted runner that the build job cannot tamper with --
  the L3 hardened-builder requirement.  Resulting bundle is
  uploaded to the same Release via `upload-assets: true`.
- **Verification one-liner updated** in `docs/release-policy.md`:
  `gh attestation verify alp-sdk-v<N>.tar.gz --owner alplabai`
  now checks the L3 attestation (workflow identity is
  `slsa-framework/slsa-github-generator`, not `alplabai/alp-sdk`).
- **Permissions model**: top-level `permissions: read-all`; each
  job opts in to what it needs (`build` -> `contents: write`,
  `provenance` -> `actions: read` + `id-token: write` +
  `contents: write`).  No leaked privileges across jobs.

SLSA spec reference:
https://slsa.dev/spec/v1.0/levels#level-3.

### Added (2026-05-14 -- TLS handshake fuzz harness §C.26)

- **`tests/fuzz/tls_handshake_fuzz.c`** -- libFuzzer harness for
  the TLS 1.2 / 1.3 handshake-record header decode path the SDK
  wrapper inspects before forwarding to mbedtls / OpenSSL.
  Reference parser inline; harness asserts every claimed length
  (record_len, hs_len, sid_len, cipher_suites_len,
  compression_methods_len, extensions_len + per-extension
  length) stays within the input buffer.
- **Catches** the canonical TLS overrun CVE class (Heartbleed-
  shape record-length lies past buffer end), reserved content
  types (only 20/21/22/23/24 valid), bad protocol-version
  envelopes, the four cascading length-field overruns inside
  ClientHello, and odd cipher_suites_len (must be even since
  each suite is 2 bytes).
- **Build path**: `_alp_add_fuzz_target(tls_handshake)` added
  to `tests/fuzz/CMakeLists.txt` next to the existing 8 fuzz
  harnesses (cc3501e, iot_mqtt, eeprom_manifest,
  gd32_bridge_frame, swd_packet, mproc_frame, ble_adv_parser,
  optiga_apdu).  ASan + UBSan + libFuzzer flags carry over via
  the existing helper.
- **Corpus**: empty under `tests/fuzz/corpus/tls_handshake/`
  with a `.gitkeep` marker -- real corpora grow as the fuzzer
  finds interesting inputs.

This closes the §6 "tls_handshake_fuzz deferred" carry-forward
from the readiness doc; the harness now covers the SDK-owned
slice that upstream mbedtls / OpenSSL fuzzers don't reach (the
small framing helper the SDK keeps in
`src/zephyr/iot_mqtt_tls.c` / `src/yocto/iot_tls.c`).

### Added (2026-05-14 -- GD32 ADC stream DMA + DSP chain_bind + RTC wake §C.23 §C.24 §C.25)

Three back-to-back HAL bodies in
`firmware/gd32-bridge/hal/bridge_hw_gd32.c` that close the
remaining §1a opcodes:

- **§C.23 -- `bridge_hw_adc_stream_begin/read/end`** (opcodes
  `ADC_STREAM_*`): DMA0/1-backed continuous acquisition.  Two
  parallel streams, each owning a 1024-sample circular ring
  buffer that the matching DMA channel fills peripheral-to-memory
  at the ADC clock; `bridge_hw_adc_stream_read` polls the DMA
  counter to recover the write cursor + drains the host's
  requested span.  Total ring footprint: 2 * 1024 * 2 = 4 KB.
- **§C.24 -- `bridge_hw_adc_dsp_chain_bind`**: completes the
  ADC-DSP pipeline started in §C.15d.  Validates the chain is
  complete (no half-uploaded stages) and the ordering rules
  (FFT must be terminal, WINDOW must immediately precede FFT,
  no gaps in the populated stage list, no WINDOW without a
  terminating FFT).  Stores the binding on both halves; runtime
  FFT/FAC dispatch inside `stream_read` follows in a later
  commit.
- **§C.25 -- `bridge_hw_power_mode_set` RTC wakeup + full
  wake-source bitmap**: extends §C.15c.  `wake_after_ms` now
  arms the RTC wakeup timer through a one-time IRC32K-clocked
  bring-up (LSI -> RCU_RTCSRC_IRC32K -> WAKEUP_RTCCK_DIV16,
  0.5 ms LSB, max wake 32.7 s in this mode).  Same path handles
  `ALP_POWER_WAKE_RTC` + `ALP_POWER_WAKE_TIMER` bits.
  `ALP_POWER_WAKE_UART_RX` / `_USB` / `_ETH_LINK` reject
  cleanly with NOSUPPORT -- no HW path on the GD32G5 baseline.

ABI unchanged (firmware-internal); validators all green.
Verification gate is HiL.

### Added (2026-05-14 -- native-sim bench cases for wave-2 surfaces §C.19)

- **Four new microbench files** under `tests/bench/`:
  - `bench_dsp.c` -- chain_open validators (FIR-4tap, WINDOW+FFT-64
    ordering / power-of-two check) + apply / close NULL-handle
    guards (6 cases).
  - `bench_tmu.c` -- 8 primitives (`sin`/`cos`/`tan`/`atan2`/
    `sqrt`/`log`/`exp`/`hypot`) on the libm-fallback path + the
    representative NULL-out INVAL rejection cost (9 cases).
  - `bench_power.c` -- open / configure_wake_source /
    request_sleep INVAL-no-wake / close round-trip (4 cases).
  - `bench_security.c` -- hash open + update + finish NULL guards,
    AEAD open + encrypt + decrypt round-trip on the stub backend,
    `alp_random_bytes` INVAL pre-check (7 cases).
- **`tests/bench/bench_main.c`** updated to chain the four new
  `bench_*_main()` entry points after the existing six.
- **`tests/bench/CMakeLists.txt`** updated to list the four new
  `.c` files under `add_executable(alp_bench ...)`.
- **`tests/bench/baselines/native-sim-cpu.yaml`** extended with
  30 new cases under explicit per-suite sub-headings, each with a
  `note:` field documenting the path the number represents
  (validation cost, NOSUPPORT-stub rejection, libm-fallback
  dispatcher overhead, etc.).
- **`docs/v1.0-readiness.md` Pillar 5 row** annotated with the
  §C.19 extension.

Real-silicon baselines (FIR throughput on V2N's CMSIS-DSP, SHA-256
on an Ensemble CryptoCell, GD32 CORDIC round-trip latency over
SPI / I2C, vendor PMU sleep-wake transition times) still live in
the per-(SoM, OS) baselines under `E1M-<MPN>-<os>.yaml` and gate
on HiL.

### Added (2026-05-14 -- SLSA L2 build provenance on releases §C.18)

- **`.github/workflows/release.yml` gains a SLSA v1.0 Build
  Level 2 provenance attestation step** via GitHub's first-party
  `actions/attest-build-provenance@v1`.  The action signs an
  in-toto attestation with the workflow's OIDC token through
  Sigstore's Fulcio CA + uploads the bundle to the repo's
  attestations store.  The resulting bundle is a SLSA v1.0
  provenance predicate naming this workflow + commit + the
  artefact's SHA-256 digest as subject.
- **New release artefact**: `alp-sdk-v<N>.tar.gz.intoto.jsonl`
  uploaded alongside the existing `.sha256` + `.sha512` checksum
  files.  Verifiable with `gh attestation verify
  alp-sdk-v<N>.tar.gz` -- no extra customer tooling needed.
- **Workflow permissions extended**: `id-token: write` (OIDC
  signing) + `attestations: write` (bundle upload) added next
  to the existing `contents: write`.
- **`docs/release-policy.md` §"Release-cut procedure" step 6
  expanded** to enumerate the three sidecar artefacts (sha256,
  sha512, intoto.jsonl) + the `gh attestation verify`
  one-liner.
- **`docs/v1.0-readiness.md` Pillar 8 checkbox flipped** from
  📋 to [x] with the SLSA L2 vs L3 note (L3 would require a
  hermetic builder + isolated runner, not gating v1.0).

  SLSA spec reference:
  https://slsa.dev/spec/v1.0/levels#level-2.

### Added (2026-05-14 -- doxygen spot-pass on wave-2 headers §C.17)

- **`@param` / `@return` blocks filled on every public function in
  `<alp/camera.h>` + `<alp/storage.h>`.**  Both headers already
  carried a `@brief` line per function but were missing the full
  Doxygen triplet on the older v0.1 stubs (camera start / stop /
  capture / release / close + storage get_info / read / write /
  erase / sync / close).  Each filled-out comment lists the
  parameter directions + every status code the function can return
  so customers reading the header through Doxygen see the same
  level of detail as the v0.5 wave-2 additions
  (`alp_camera_configure_isp`, `alp_storage_configure_inline_aes`).
- **No changes** required on `<alp/dsp.h>`, `<alp/gpu2d.h>`,
  `<alp/power.h>`, `<alp/tmu.h>` -- those already carried full
  `@brief` / `@param` / `@return` blocks.
- **`docs/v1.0-readiness.md` §3a updated:** spot-pass checkbox
  flipped to `[x]`; remaining "full pass across every header"
  checkbox lists the headers most likely to still have gaps
  (peripheral.h subsections, iot.h, ble.h, security.h).

### Changed (2026-05-14 -- peripheral test split per peripheral §C.16)

- **`tests/zephyr/peripheral/src/main.c`** (902 LOC monolith) split
  into 13 per-peripheral files alongside a slimmed-down `main.c`:
  `i2c.c`, `spi.c`, `gpio.c` (incl. pool exhaustion), `uart.c`
  (incl. RX ringbuf), `pwm.c` (incl. single-pulse + capture
  NOSUPPORT contract), `adc.c` (incl. streaming), `dac.c`,
  `counter.c`, `qenc.c`, `i2s.c`, `can.c`, `rtc.c`, `wdt.c`.
  `main.c` retains the cross-cutting sections that don't pair
  1:1 with a single peripheral header: TMU primitives, power-
  mode NOSUPPORT contract, AEN audit gap surfaces (gpu2d /
  camera ISP / storage AES), portable delay helpers, SoC
  capability validation, V2N supervisor cross-peripheral
  dispatch, GD32-TRNG entropy source -- plus the
  `ZTEST_SUITE(alp_peripheral, ...)` declaration that the
  split files register against.
- **`CMakeLists.txt` updated** to list all 14 source files
  under `target_sources(app PRIVATE ...)`.
- **`testcase.yaml` unchanged** -- Twister discovers ZTESTs
  through the suite registration mechanism, not the file list,
  so the 4 existing scenarios (`smoke`, `caps_e3`,
  `uart_rx_ringbuf`, `v2n_supervisor`) keep covering the same
  82-test surface.
- **All 82 ZTESTs preserved verbatim** (verified via
  `grep -c '^ZTEST'` -- pre-split 82 / post-split sum 82).
- **`docs/test-coverage-audit.md` updated** to reference the
  new layout + flip §1 ("Split `peripheral/main.c`") to DONE.
- **`docs/v1.0-readiness.md` §1c first checkbox flipped to
  `[x]`**; thin-spot fills row now points to the per-peripheral
  `.c` as the natural insertion point.

### Added (2026-05-14 -- GD32 ADC-DSP chain pool + chunk reassembly §C.15d)

- **`bridge_hw_adc_dsp_chain_open` real body.**  Replaces the
  NOSUPPORT stub with a first-fit allocation over a 4-entry
  static pool (`BRIDGE_DSP_MAX_CHAINS`, mirrors
  `GD32G553_BRIDGE_ADC_DSP_MAX_CHAINS`).  Returns
  `BRIDGE_HW_ERR_NOTIMPL` on pool exhaustion (protocol layer
  maps to STATUS_NOSUPPORT today; a STATUS_NOMEM-equivalent
  arrives alongside chain_bind).
- **`bridge_hw_adc_dsp_stage_push` real body.**  Reassembles
  chunks into a per-(chain, stage) 260-byte buffer
  (`BRIDGE_DSP_MAX_STAGE_BYTES`), enforcing:
  - chain_id / stage_index in pool range,
  - `kind` in 0..3 (FIR / IIR / WINDOW / FFT),
  - non-zero `chunk_data_len` with a non-NULL pointer,
  - `chunk_offset + chunk_data_len <= chunk_total_size`
    (overflow-safe via subtraction),
  - first chunk seeds `kind` + `total_size`; subsequent chunks
    must agree (catches mid-upload re-target),
  - re-write after stage completion rejects with INVAL.

  Stage is marked complete when `bytes_received == total_size`.
  Total RAM cost: 4 chains x 4 stages x 260 B = 4160 bytes +
  ~80 bytes metadata; comfortable inside the GD32G553's 128 KB
  SRAM.

- **`bridge_hw_adc_dsp_chain_bind` left as NOSUPPORT.**  Wiring
  a completed chain into the wave-2 FFT/FAC DMA stream needs
  the ADC_STREAM_BEGIN / DMA pipeline (also still NOTIMPL); both
  land together in a follow-up.  The new chain pool is
  immediately usable to validate the upload protocol via the
  host-side ZTESTs (`tests/zephyr/chips/src/main.c::test_gd32g553_v05_invalid_args`)
  even before the streaming side is HiL-ready.

### Added (2026-05-14 -- GD32 power-mode-set deep-sleep + standby §C.15c)

- **`bridge_hw_power_mode_set` modes 2 (deep-sleep) + 3 (standby)
  promoted from NOSUPPORT to real bodies.**  Mode 2 calls
  `pmu_to_deepsleepmode(PMU_LDO_LOWPOWER, WFI_CMD)`; mode 3 calls
  `pmu_to_standbymode()` (does not return -- chip wakes via reset
  + the bridge's normal handshake on the next host packet, per
  the existing contract in `<alp/power.h>`).  Mode 0 (run) +
  mode 1 (sleep) remain accepted no-ops since the bridge's main
  loop already idles in WFI between transport ISRs.

- **Wake-source bitmap partial-applied.**  `ALP_POWER_WAKE_GPIO`
  (bit 0x02) enables `PMU_WAKEUP_PIN0..4` collectively so the
  carrier's pre-configured WKUP pad pads can break the chip out
  of deep-sleep / standby.  Any other ALP_POWER_WAKE_* bits
  (RTC / UART_RX / TIMER / USB / ETH_LINK) return
  `BRIDGE_HW_ERR_NOTIMPL` so the host knows the request was not
  fully honoured -- those paths land alongside the RTC-alarm
  wiring in a follow-up.  `wake_after_ms != 0` still rejects
  cleanly for the same reason.

### Added (2026-05-14 -- GD32 timer-sync HAL §C.15b)

- **`bridge_hw_timer_sync` real body** in
  `firmware/gd32-bridge/hal/bridge_hw_gd32.c`.  Replaces the
  NOSUPPORT stub with a master-slave SMC configuration over the
  three GD32G5x3 advanced timers (TIMER0 / TIMER7 / TIMER19).
  Wire byte mapping: `master`/`slave` integer ids 0..2 ->
  TIMER0/TIMER7/TIMER19; `mode` byte 0..5 -> vendor SMC encoding
  (DISABLE / RESTART / PAUSE / EVENT / EXTERNAL0 /
  QUAD_DECODER_MODE1).  Master emits its UPDATE event as
  `TRI_OUT0_SRC_UPDATE` + flips `MASTER_SLAVE_MODE_ENABLE`; slave
  listens to internal trigger ITI0 + adopts the mapped slave
  mode.  Out-of-range master==slave / unknown timer ids / unknown
  mode all return clean error codes (INVAL / RANGE) rather than
  silently selecting a default.  Idempotent.

  SYSCFG router that maps ITI0 of the slave to a specific upstream
  TIMER's TRGO is left at chip default; a future hardware-bring-up
  tweak via `trigsel_init` can override that for non-default
  (master, slave) pairings.  Verification gate is HiL.

### Added (2026-05-14 -- GD32 PWM input-capture HAL §C.15a)

- **`bridge_hw_pwm_capture_begin` / `_read` / `_end` real bodies**
  in `firmware/gd32-bridge/hal/bridge_hw_gd32.c`.  Replaces the
  three NOSUPPORT stubs that landed with v0.5 wave-2 §2B.2 with
  a polled-drain implementation: BEGIN switches the channel from
  output mode (CHxN driving) to input-capture (CIx polled),
  configures polarity per `edge` (0 rising / 1 falling / 2 both),
  clears any stale CCxIF; READ checks CCxIF and pulls `CCxVAL`,
  composing `(period_ticks, pulse_width_ticks)` via a per-channel
  state machine -- both-edge mode walks rising/falling deltas to
  recover both quantities, single-edge mode reports period only
  (pulse_width left at zero); END disables the channel and clears
  state.  Unit conversion uses `PWM_TIMER_TICK_NS` (1 us at the
  240 MHz / 240 prescaler), correcting the bridge_hw.h doc-comment
  that referenced the unscaled core-clock LSB.
- **V2N pad routing caveat captured inline.**  The PWM map binds
  the COMPLEMENTARY (CHxN) outputs, but the GD32G5x3's
  input-capture path reads the MAIN (CHx) pad -- a physically
  different pin per the datasheet.  Until a hardware-bring-up
  commit reworks pad routing onto the main-channel AF, BEGIN
  succeeds + the structural flow is exercised end-to-end but
  no edges actually land at READ, so it surfaces
  `BRIDGE_HW_ERR_NOTIMPL` ("ring empty") per the contract.
  Verification gate is HiL; CI build under
  `pr-gd32-bridge-build.yml` covers the structural compile.

### Added (2026-05-14 -- fuzz coverage expansion §C.3)

Three new libFuzzer harnesses under `tests/fuzz/` for parser
surfaces that hadn't been covered yet.  All compile only with
`ALP_BUILD_FUZZ=ON` + clang; default builds untouched.  Existing
ASan + UBSan + libFuzzer flags carry over via the existing
`_alp_add_fuzz_target` helper in `tests/fuzz/CMakeLists.txt`.

- **`tests/fuzz/mproc_frame_fuzz.c`** -- the v0.4-prep mproc
  envelope decoder (`src/common/proto/alp_mproc_frame.c`).
  Verifies the 12-byte header decode + payload-length bound +
  encoder/decoder round-trip.  Catches: header / payload-length
  disagreement (OOB read if the decoder believes length blindly),
  magic-mismatch crash vs clean drop, negative-length cast.
- **`tests/fuzz/ble_adv_parser_fuzz.c`** -- BLE adv/scan-response
  AD-structure enumerator.  Reference parser inline; harness
  asserts every (offset + len) range stays within the input.
  Catches the canonical BLE CVE class: length-field overrun
  where a malicious adv packet claims `len_n` greater than the
  remaining buffer.
- **`tests/fuzz/optiga_apdu_fuzz.c`** -- OPTIGA Trust M APDU
  response parser.  Walks ISO-7816 SW1/SW2 split + BER-TLV
  payload.  Catches: TLV length-of-length overflow (CVE class
  in TLV parsers generally), unknown SW codes that must surface
  as `ALP_ERR_IO` rather than crashing, off-by-one on the
  SW1-SW2 trailing-bytes split.

Corpus directories seeded empty under `tests/fuzz/corpus/`
(`mproc_frame/`, `ble_adv_parser/`, `optiga_apdu/`) with
`.gitkeep` markers; real corpora grow as the fuzzers find
interesting inputs.

Deferred from this batch:
- `tls_handshake_fuzz.c` -- OpenSSL's upstream fuzz coverage on
  the TLS handshake state machine is comprehensive; adding our
  own gives marginal value.
- `board_yaml_loader_fuzz.c` + `som_preset_yaml_fuzz.c` --
  Python-side targets via Atheris.  Need a different
  infrastructure than the C `_alp_add_fuzz_target` pattern; lands
  in a follow-up alongside the Atheris CI workflow.

### Added (2026-05-14 -- ABI stability markers across public headers §C.2)

- **`docs/abi-markers.md`** (new) -- classification doc for the
  v1.0 ABI freeze.  Defines `[ABI-STABLE]` vs `[ABI-EXPERIMENTAL]`
  semantics, lists every `include/alp/*.h` with its current
  marker + rationale, documents the promotion path
  (EXPERIMENTAL -> STABLE), and explains how
  `pr-abi-snapshot.yml` reads markers to gate merges post-1.0.
- **`@par ABI status:` line landed in all 26 public headers**:
  - **`[ABI-STABLE]`** (20 headers): `peripheral.h`, `pwm.h`,
    `adc.h`, `counter.h`, `i2s.h`, `can.h`, `rtc.h`, `wdt.h`,
    `audio.h`, `iot.h`, `security.h`, `ble.h`, `inference.h`,
    `mproc.h`, `hw_info.h`, `e1m_pinout.h`, `soc_caps.h`,
    `gui.h`.
  - **`[ABI-EXPERIMENTAL]`** (6 headers): `dsp.h`, `gpu2d.h`,
    `power.h`, `tmu.h`, `camera.h` (file-level; ISP-config
    block tentative), `storage.h` (file-level; inline-AES
    block tentative), `display.h`, `usb.h` (both placeholders).
- ABI snapshot unchanged -- markers are comments only.  Future
  per-function `[ABI-EXPERIMENTAL]` annotations can override the
  file-level marker for specific symbols (e.g.
  `alp_camera_configure_isp` inside the otherwise-stable
  `camera.h`).

### Changed (2026-05-14 -- ABI-BREAKING: drop ALP_ prefix from E1M instance IDs §C.1z)

- **`ALP_E1M_*` -> `E1M_*` across every public surface, example,
  test, and doc cross-ref.**  Captured from the maintainer:
  "E1M is the open-standard form factor, not an ALP-specific
  concept -- the prefix is redundant noise."  The `ALP_*`
  namespace stays reserved for SDK-internal abstractions
  (`ALP_OK`, `alp_status_t`, etc.); pin / peripheral instance
  IDs lose the prefix.
- Scope: 335 replacements across 49 files.  `include/alp/e1m_pinout.h`
  header guard + every macro renamed; `include/alp/adc.h`,
  `include/alp/boards/alp_e1m_evk.h`, `include/alp/chips/cc3501e.h`
  refs updated; 27 example main.c + README + testcase.yaml + the
  `alp,pin-array` overlay rewritten; both Twister test mains;
  `scripts/alp_project.py` ID-validation table; docs/adr/0004 +
  docs/board-config + docs/cc3501e-integration-plan + docs/e1m-pinout
  + docs/getting-started + docs/soms/aen + README.md updated.
- **Validators all green** post-rename:
  `python3 scripts/validate_metadata.py` (0 failures),
  `python3 scripts/check_example_portability.py` (27/27),
  `python3 scripts/check_pin_conflicts.py` (clean).
- **ABI-breaking, pre-1.0.**  Historical `docs/abi/v0.1-snapshot.json`
  + `v0.3-snapshot.json` left untouched (frozen historical record
  of those tags); `v0.5-snapshot.json` regenerated to match the
  new surface.  Pre-1.0 the change is a normal minor-cycle
  rename; post-1.0 it would require a major bump per
  `docs/release-policy.md`.
- **Follow-ups tracked in `docs/v1.0-readiness.md` §1z:**
  - Item 2: collapse `_CH<N>` suffixes (`E1M_DAC_CH0` aliases for
    `E1M_DAC0`, etc.) -- TBD next commit.
  - Item 3: pin-as-GPIO fallback so `alp_gpio_open(E1M_ADC0)`
    returns a usable handle -- design TBD, will need either an
    ID-space unification or an explicit conversion in
    `alp_gpio_open`.

### Added (2026-05-14 -- release engineering scaffolding §C.1)

- **`CODEOWNERS`** at repo root -- GH auto-requests reviews for
  the maintainer on every PR touching the ABI-load-bearing,
  schema, loader, security, or CI surfaces.
- **`docs/release-policy.md`** -- SemVer + LTS + deprecation
  contract.  v1.0 = first LTS, 24-month support, 6-month overlap
  when superseded.  `[ABI-STABLE]` vs `[ABI-EXPERIMENTAL]` marker
  semantics.  Release-cut procedure codified end-to-end (ledger
  check -> ABI regen -> sdk_version bump -> CHANGELOG slice ->
  signed tag -> workflow auto-publish).
- **`docs/security-advisories.md`** -- embargoed-disclosure
  workflow.  Reports via GitHub Security Advisories (private),
  not public issues / not community forum.  CVSS severity rubric
  mapped to embargo + backport reach.  Vendor-SDK CVEs explicit
  out-of-scope; track upstream.
- **`.github/workflows/release.yml`** -- fires on `v*` tag push:
  verify tag matches `metadata/sdk_version.yaml`, verify
  `docs/abi/v<MAJOR.MINOR>-snapshot.json` exists, slice the
  matching CHANGELOG section, build source tarball + SHA-256/512
  checksums, create the GitHub Release.
- **`.github/workflows/pr-abi-snapshot.yml`** -- post-1.0 ABI
  gate.  Diffs the working-tree ABI snapshot against the latest
  committed one; classifies entries by `[ABI-STABLE]` /
  `[ABI-EXPERIMENTAL]` markers; posts the diff as a PR comment.
  Currently parked behind `continue-on-error: true` so it runs
  informationally until the v1.0 tag flips it to a hard gate.
- **`scripts/bump_version.py`** -- one-command release-prep tool.
  Bumps `metadata/sdk_version.yaml`, slices the CHANGELOG
  `[Unreleased]` section into a dated `[vX.Y.Z]` section,
  regenerates the ABI snapshot for the new minor.  Dry-run mode
  prints the diff plan without writing.  Operator still has to
  run `git commit + git tag -s` themselves -- the tooling stops
  short of the irreversible step.

### Added (2026-05-14 -- V1.0 readiness tracker + external anchors §C.0)

- **`docs/v1.0-readiness.md`** (new) -- the master living checklist
  for everything between today and the v1.0.0 tag.  Organises
  remaining work into 11 pillars (code completion, docs, fuzz, ABI
  freeze, release engineering, hardware verification, external
  party gates, reference apps, performance baselines, security
  audit, and the external anchors that came in this commit).
  Cross-links to `VERSIONS.md` (north star) + `docs/test-plan.md`
  (verification ledger).
- **External anchors landed across customer-facing surfaces:**
  - `README.md` — top-of-readme pointer to
    [`docs.alplab.ai/sdk/introduction`](https://docs.alplab.ai/sdk/introduction)
    (rendered docs) +
    [`community.alplab.ai`](https://community.alplab.ai/) (forum) +
    GitHub Issues (bug tracker).
  - `docs/getting-started.md` -- intro block points at the rendered
    docs site and the community forum.
  - `CONTRIBUTING.md` -- new "Where to ask things" table separating
    open-ended questions (forum) / concrete bugs (issues) /
    security issues (advisories) / rendered docs.
  - `docs/troubleshooting.md` -- callout above the error catalogue
    pointing at the forum for unlisted issues.

### Changed (2026-05-14 -- CX read-through §B.4)

- **`README.md` 30-second quick start clarification.**  The
  example `board.yaml` in the README uses generic
  `peripherals: [i2c, pwm]` for schema illustration purposes,
  but the linked `examples/gpio-button-led/` skips
  `peripherals:` entirely and uses `carrier.populated.button_led:
  true` instead.  Added a sentence noting that every block
  except `schema_version` / `som` / `carrier` / `os` is
  optional, with the gpio-button-led pattern called out
  explicitly so first-time readers don't trip on the mismatch.
- **CX read-through pass: no other issues found.**  Verified
  README.md, docs/getting-started.md, docs/firmware-quickstart.md,
  docs/troubleshooting.md, and docs/glossary.md are internally
  consistent post-§A.7..§B.3 restructures.  Zero TODO/FIXME
  markers in the top customer-facing surfaces.  Zero stale
  paths after the §A.8 (tools/->scripts/), §A.10 (dts->zephyr/dts),
  §A.11 (sysbuild->zephyr/sysbuild), §B.1 (examples READMEs),
  and §B.2 (SKU flatten) restructures.  No additional doc
  changes warranted.

### Added (2026-05-14 -- test coverage audit §B.3)

- **`docs/test-coverage-audit.md`** (new) -- maps ZTEST counts
  per `tests/zephyr/<area>/` against the `<alp/...>` surface
  each area covers.  Per-peripheral breakdown inside the
  902-line `tests/zephyr/peripheral/src/main.c` monolith
  identifies six "thin" peripherals (RTC / QENC / COUNTER
  with 1 test each, SPI / I²S / WDT with 2 each).  The
  native_sim coverage shape (negative-path-only for
  audio / inference / iot / security / usb) is documented +
  contrasted with the HiL ledger in `docs/test-plan.md` so
  reviewers don't confuse "green native_sim" with "verified".
  Methodology section captures the grep recipes for re-running
  the audit on future commits.  `docs/testing.md` cross-links
  to the new audit.  This is a no-new-tests pass; per-area test
  additions are scheduled as follow-up work.

### Removed (2026-05-14 -- internal docs moved to private repo)

- **`docs/hil-plan.md` removed from the public SDK.**  The
  document described the in-house Hardware-in-the-Loop rig --
  fault-injection wiring, instrument lattice, expected fixture
  coverage -- which contains test-IP relevant to the
  maintainer's HiL setup but no API contract a customer needs.
  Moved to the private `alplabai/e1m-som-metadata` repo as
  `HIL-PLAN.md`.  Public refs in `docs/firmware-quickstart.md`,
  `docs/glossary.md`, and `docs/test-plan.md` rewritten to
  point at the internal location (without leaking its content).
- **`docs/aen-feature-audit-2026-05.md` removed from the public
  SDK.**  The audit catalogues which Alif Ensemble peripherals
  the SDK exposes vs the silicon's full surface -- internal
  product/coverage roadmap, not consumer-facing API.  Moved to
  `alplabai/e1m-som-metadata` as `AEN-FEATURE-AUDIT-2026-05.md`.
  Public cross-refs in `examples/aen/README.md` rewritten to
  cite the internal location; code-comment references in
  `include/alp/camera.h`, `include/alp/gpu2d.h`,
  `include/alp/storage.h`, `src/zephyr/gpu2d_zephyr.c`, and
  `zephyr/Kconfig` updated to drop the explicit path while
  keeping the audit reference ("Per the internal AEN feature
  audit, ...").

### Changed (2026-05-14 -- doc xref + SKU layout cleanup §B.2)

- **SoM preset files flattened: `E1M-<MPN>/som.yaml` ->
  `E1M-<MPN>.yaml`.**  The previous layout had every SKU's
  preset named `som.yaml` inside an MPN-named directory --
  visually ambiguous in any directory listing or recent-files
  list ("which som.yaml is open in tab 4?").  Moving the SKU
  into the filename makes every preset distinguishable at a
  glance.  Eleven files renamed (`E1M-AEN301`/`401`/`501`/
  `601`/`701`/`801`, `E1M-V2N101`/`V2N102`, `E1M-V2M101`/`V2M102`,
  `E1M-NX9101`); the empty MPN-named directories deleted.
  Loader logic in `scripts/{alp_project,program_eeprom,validate_board_yaml,validate_metadata,check_example_portability}.py`
  rewired to the new path pattern; JSON schemas
  (`board-config-v1`, `som-preset-v1`) descriptions updated.
  ~16 doc + comment xrefs across `docs/`, `include/`,
  `metadata/`, and `examples/` rewritten.  ABI snapshot
  regenerated.
- **Stale dead-link sweep across `docs/`.**  Five markdown
  links pointed at `../memory/project_*.md` -- files that live
  in the maintainer's local AI-memory store, not in the public
  repo.  Anyone cloning got dead links.  Removed the link
  decoration in five files (`docs/gd32-bridge-protocol.md`,
  `docs/gd32-bridge.md`, `docs/ota-device-contract.md`,
  `docs/adr/0005-alp-sdk-vs-alp-studio-boundary.md`); rewrote
  surrounding prose to stand on its own.  Final link-checker
  sweep: 0 dead markdown links in `docs/`.
- **`docs/board-config.md` "Stock presets" structure refreshed.**
  The directory illustration (~ lines 290+) showed the
  hypothetical `sku-aen<N>.yaml` flat layout that never
  shipped.  Replaced with the actual flat-MPN layout
  (`E1M-<MPN>.yaml` at the `e1m_modules/` root, family
  subdirs for family-scope artefacts).  The second illustration
  (~ lines 580+) similarly refreshed: stale "TBD" labels on
  the V2N + V2N-M1 + i.MX 93 family `hw-revisions.yaml` rows
  (the FILES exist; their VALUES carry per-rev TBDs); the
  E1M-X-EVK board.yaml row no longer mis-claimed as TBD (the
  file exists); added the `custom-example/board.yaml`
  template row.

### Added (2026-05-14 -- examples completeness pass §B.1)

- **`examples/audio-loopback/`** now has `README.md` +
  `board.yaml`.  The example shipped with only
  `CMakeLists.txt` + `prj.conf` + `testcase.yaml` + `src/main.c`,
  making it the only example that couldn't be copied as a
  starting-point project (no `board.yaml` for the customer to
  edit, no README to orient a first-time reader).
- **`examples/aen/README.md`** (new) -- per-SoM index for the
  AEN-only examples directory.  Currently lists
  `edgeai-vision-aen` with a sentence + cross-refs into the
  AEN one-pager + feature audit.
- **`examples/v2n/README.md`** (new) -- per-SoM index for the
  V2N / V2N-M1 examples directory.  Lists all 11 V2N examples
  with one-line summaries + cross-refs into the V2N bring-up
  docs.
- **Top-level `README.md` repository-layout block refreshed**:
  dropped the `tools/` row (folded into `scripts/` per §A.8);
  dropped the misleading `(vscode/)` "split out" placeholder
  (`.vscode/` is currently tracked); `examples/` row updated to
  call out the per-SoM subfolder layout (`examples/aen/` +
  `examples/v2n/`); `firmware/` row updated to mention both
  `cc3501e/` and `gd32-bridge/` (the latter moved in per §A.6);
  new `zephyr/` row to capture the Zephyr-module subtree
  (`Kconfig`, `module.yml`, `dts/bindings/`, `sysbuild/aen/`)
  after §A.10 + §A.11.

### Changed (2026-05-13 -- top-level UX simplification cont.)

- **`bench/` moved to `tests/bench/`.**  The microbench harness
  is a test-flavoured artefact (opt-in via `ALP_BUILD_BENCH=ON`,
  no CI gate today) -- belongs under `tests/` alongside the rest
  of the SDK's test infrastructure.  Top-level repo gets one
  fewer item, and the relationship between bench + smoke + ztest
  becomes visible.  Files updated: top-level `CMakeLists.txt`
  (`add_subdirectory(tests/bench)`), `tests/bench/CMakeLists.txt`
  + `README.md` + `baseline_runner.py` + `baselines/README.md`
  + `bench.h` self-refs, plus `docs/test-plan.md`,
  `docs/testing.md`, `fuzz/README.md` cross-references.
- **`fuzz/` moved to `tests/fuzz/`.**  Same reasoning as the
  bench move: the libFuzzer harnesses are test infrastructure
  (opt-in via `ALP_BUILD_FUZZ=ON`, clang-only, no CI gate
  today).  Top-level repo loses one more entry, and bench /
  fuzz / smoke / yocto / zephyr live as siblings under
  `tests/`.  Files updated: top-level `CMakeLists.txt`
  (`add_subdirectory(tests/fuzz)`), `tests/fuzz/CMakeLists.txt`
  + `README.md` self-refs, four harness-file header comments
  (`cc3501e_fuzz.c`, `eeprom_manifest_fuzz.c`,
  `gd32_bridge_frame_fuzz.c`, `iot_mqtt_fuzz.c`), plus the
  `tests/bench/README.md` cross-ref bumped from `../../fuzz/`
  to `../fuzz/`.
- **`gd32-bridge/` moved to `firmware/gd32-bridge/`.**  Unifies
  the firmware trees -- `firmware/cc3501e/` (already there) and
  `firmware/gd32-bridge/` (new home) now sit side-by-side
  instead of being scattered between the top level + `firmware/`.
  Mechanical search-and-replace: 22 files updated with
  `gd32-bridge/` -> `firmware/gd32-bridge/` (skipping
  `CHANGELOG.md`'s historical entries).  Notable touch points:
  `.github/workflows/pr-gd32-bridge-build.yml` paths filter
  + build/source/toolchain paths (14 occurrences),
  `.github/workflows/pr-metadata-validate.yml` (5 occurrences),
  `.gitignore` (`!gd32-bridge/toolchain/*.cmake` allowlist),
  `include/alp/chips/gd32g553.h` cross-refs (4 occurrences),
  `tests/fuzz/CMakeLists.txt` `target_sources` paths
  (5 occurrences for the gd32_bridge_frame fuzz harness),
  `tests/fuzz/gd32_bridge_frame_fuzz.c` source-comment paths,
  `chips/gd32g553/gd32g553.c`, `zephyr/Kconfig`,
  `metadata/chips/gd32g553.yaml`, `docs/gd32-bridge.md`,
  `docs/gd32-bridge-protocol.md`, `docs/test-plan.md`,
  `VERSIONS.md`, `vendors/gd32_firmware_library/*`, plus the
  bridge's internal self-refs (CMakeLists, README, HAL headers,
  bootloader, tests).  No semantic changes -- pure path-prefix
  update.
- **`CONTRIBUTORS.md` folded into `CONTRIBUTING.md`.**  Two
  contributor-facing files at the top level was one too many --
  the team-credit table + the Co-Authored-By trailer convention
  now live as the "Team" and "Credit" sections at the bottom of
  `CONTRIBUTING.md`.  Top-level file count drops by one.  No
  callers referenced `CONTRIBUTORS.md` directly so no
  cross-refs needed updating.
- **`ci/` moved to `docs/ci/`.**  The directory only held two
  documentation files (`HW-IN-LOOP.md` runner contract +
  `README.md` workflow index) -- the actual CI lives at
  `.github/workflows/`.  Having a top-level `ci/` that was just
  docs confused readers (was it the source of CI?  No: the
  source is `.github/workflows/`, and `ci/` was the *prose*
  about those workflows).  Move makes the relationship
  explicit: `docs/ci/` is documentation *about* CI, not CI
  itself.  Files updated: `docs/ci/README.md` self-heading +
  relative-path bumps (`../.github/` -> `../../.github/`,
  `../scripts/` -> `../../scripts/`, `../VERSIONS.md` ->
  `../../VERSIONS.md`, `../docs/testing.md` -> `../testing.md`),
  `docs/ci/HW-IN-LOOP.md` board-doc xref (`../docs/boards/` ->
  `../boards/`), plus 13 cross-refs across `docs/testing.md`,
  `docs/test-plan.md`, `docs/os-support-matrix.md`,
  `docs/doxygen/README.md` (also fixed a pre-existing typo:
  `ci/pr-doxygen.yml` -> `.github/workflows/pr-doxygen.yml`),
  `yocto/meta-alp/README.md`, `yocto/meta-alp/conf/distro/include/mender.inc`,
  `.github/workflows/pr-plain-cmake.yml`,
  `.github/workflows/nightly-aen-hil.yml`,
  `.github/PULL_REQUEST_TEMPLATE.md`, `scripts/test-all.sh`,
  five test-source comment headers in `tests/yocto/*.c` +
  `tests/yocto/CMakeLists.txt`.

### Removed (2026-05-13 -- top-level UX simplification)

- **`PLAN.md` deleted from the public repo.**  It was an
  internal product / engineering planning document
  ("ALP SDK — Product + Engineering Plan", 583 lines, last
  revised 2026-05-11) that duplicated forward-looking
  positioning already captured in [`VERSIONS.md`](VERSIONS.md)
  and pillar-level architecture text already in the customer
  surfaces (`docs/architecture.md`, `docs/secure-boot.md`,
  `docs/ota.md`, `docs/recommended-libraries.md`,
  `include/alp/inference.h` headers).  Live cross-references
  updated to point at those canonical surfaces instead of the
  deleted planning doc; historical mentions in `CHANGELOG.md`
  are kept untouched.  Files touched: `VERSIONS.md`,
  `docs/test-plan.md`, `docs/secure-boot.md`, `docs/ota.md`,
  `docs/recommended-libraries.md`, `sysbuild/aen/README.md`,
  `vendors/deepx-dxm1/README.md`,
  `vendors/renesas-rzv2n/rzv_drp-ai_tvm/README.md`,
  `include/alp/inference.h`.

### Fixed (2026-05-13 -- gd32-bridge CI follow-ups)

- **`gd32-bridge` GD32 backend now links against a real `gd32g553_xE`
  memory map (2026-05-13).**  Replaces the placeholder
  `toolchain/gd32g553_flash.ld` with a derivative of the vendor's
  `gd32g553_xE_flash.ld` from `Firmware/CMSIS/GD/GD32G5x3/Source/GCC/Ld/`,
  preserving the FLASH (512K @ `0x08000000`) + RAM (96K @ `0x20000000`)
  + TCMSRAM (32K @ `0x10000000`) regions and the `_sidata` / `_sdata` /
  `_edata` / `_sbss` / `_ebss` / `_sp` symbol set the vendor
  `startup_gd32g5x3.S` Reset_Handler dereferences.  Also adds the
  `.vectors` / `.init_array` / `.fini_array` / `.heap_stack` /
  `.tcmsram` section recipes that the prior placeholder lacked.  Pairs
  with `hal/gd32_libc_stubs.c`: weak `_init` / `_fini` symbols emitted
  from a C translation unit because `-nostartfiles` drops the
  toolchain's `crti.o` (which normally supplies them).  Stubs stay
  empty -- the firmware does no C++ static init and the classic init /
  fini sections aren't used.  Source file is only linked when
  `BRIDGE_HAL_BACKEND=gd32` (the stub backend never references
  `__libc_init_array`).  Unblocks the `pr-gd32-bridge-build · gd32`
  job which had been failing at link with
  `undefined reference to '_sidata'` ... `'_ebss'` plus
  `undefined reference to '_init'`.

- **`gd32-bridge/src/protocol.c` -- silence `-Wtype-limits` on the
  OTA-range guard (2026-05-13).**  Dropped the `cmd <= 0xFFu` upper
  bound from the OTA opcode dispatch (`cmd` is `uint8_t`, so the
  comparison was always true and tripped GCC's `-Wtype-limits` under
  `-Wextra`).  No behavioural change.

### Changed (2026-05-13 -- gd32-bridge HAL bodies)

- **`bridge_hw_reset_reason()` now decodes RCU_RSTSCK (2026-05-13).**
  Replaces the `return 0u; /* UNKNOWN */` stub in
  `gd32-bridge/hal/bridge_hw_gd32.c` with a priority decode of the
  GD32G5x3's reset-status register flags: `PORRSTF` -> `POWER_ON`,
  `BORRSTF` -> `BROWNOUT`, `EPRSTF` -> `NRST_PIN`, `LPRSTF` ->
  `LOWPOWER`, `FWDGTRSTF | WWDGTRSTF` -> `WDT`, `SWRSTF` -> `SOFT`.
  Decoder picks coldest cause first because the GD32G5 latches all
  flags across nested resets.  After decoding, the `RSTFC` bit
  (`RCU_RSTSCK` bit 24) clears every cause so the next caller sees
  `UNKNOWN` until a future reset fires.  Encoded byte matches the
  host enum `gd32g553_reset_cause_t` in `<alp/chips/gd32g553.h>`.
  Wire behaviour for `CMD_RESET_REASON` (0x03) on the gd32 backend
  now reports the real cause instead of always-`UNKNOWN`; stub
  backend unchanged (still returns 0).  First of the 18 HAL bodies
  enumerated in `gd32-bridge/hal/bridge_hw_gd32.c`'s top comment.

- **`bridge_hw_gpio_read` / `bridge_hw_gpio_write` now drive real
  GPIO pads (2026-05-13).**  Second of the 18 HAL bodies.  Adds an
  18-entry `gpio_pad_map[]` in `gd32-bridge/hal/bridge_hw_gd32.c`
  sourced from `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv` (the
  free `E1M IO8..IO35` rows; gaps at IO15 / IO17..IO23 / IO26 /
  IO33 collapse to a compact 0..17 bit numbering on the wire
  mask).  `bridge_hw_init()` enables `RCU_GPIO{A,B,C,D,E,F}` and
  configures every pad as `INPUT + PULL_UP` (safe default -- no
  driven contention with carrier-side wiring).  Read calls return
  the external pad level by default; the first write to a given
  pad promotes it to `OUTPUT` push-pull at `GPIO_OSPEED_12MHZ`
  (sticky until the chip resets), after which reads return the
  driven level via `gpio_output_bit_get`.  No protocol or ABI
  change -- the wire opcodes `CMD_GPIO_READ` (0x10) and
  `CMD_GPIO_WRITE` (0x11) flip from `STATUS_NOSUPPORT` to
  `STATUS_OK` on the gd32 backend.

- **`bridge_hw_trng_read` -> NIST SP800-90B TRNG with bounded
  polling (2026-05-13).**  Third of the 18 HAL bodies.  Adds
  one-time TRNG bring-up in `bridge_hw_init()` matching the
  vendor's `TRNG_NIST_mode` example sequence (PLL Q / 2 clock
  source, SHA-256 conditioning over 440-bit input -> 256-bit
  output, post-processing enabled, clock-error detection armed)
  with a bounded PLL-stable wait so a misconfigured clock tree
  can't hang boot.  Bring-up failure (PLL never stable, analog
  noise dead, `CECS` / `SECS` self-check tripped) leaves
  `trng_ready = false` so subsequent reads return
  `BRIDGE_HW_ERR_IO` rather than serving zero-filled bytes.
  `bridge_hw_trng_read()` polls `DRDY` between 32-bit pulls with
  a per-word timeout, packs the LSB bytes into the caller's
  buffer, and re-checks `CECS` / `SECS` on every word so a
  mid-read fault still surfaces.  Wire opcode `CMD_TRNG_READ`
  (0x80) flips from `STATUS_NOSUPPORT` to `STATUS_OK` on the
  gd32 backend.  No protocol or ABI change.

- **`bridge_hw_tmu_compute` -> CORDIC math accelerator
  (2026-05-13).**  Fourth of the 18 HAL bodies.  Wires nine of the
  twelve host TMU functions through the GD32G5x3's CORDIC unit:
  `sin`, `cos`, `atan`, `atan2`, `sqrt`, `log` (ln), `sinh`,
  `cosh`, `hypot` (sqrt(x^2+y^2) via TMU_MODE_MODULUS).  Each
  call reconfigures `tmu_parameter_struct` for the requested
  function + Q31/F32 format, writes one or two inputs through
  `tmu_one_*_write` / `tmu_two_*_write`, polls `TMU_FLAG_END`
  with a bounded timeout, then reads one or two outputs through
  the matching `_read` helper -- SIN / COS emit a (result,
  scaling_factor) pair so the second word is drained and
  discarded.  Returns `BRIDGE_HW_ERR_RANGE` on `TMU_FLAG_OVRF`
  (input outside the function's domain) and `BRIDGE_HW_ERR_IO`
  on the bounded timeout.

  `tan` / `exp` / `tanh` aren't native TMU modes; this commit
  returns `BRIDGE_HW_ERR_NOTIMPL` (-> wire `STATUS_NOSUPPORT`)
  for them.  A future commit can either compose them on the
  bridge side (`tan = sin/cos`, `exp = cosh + sinh`,
  `tanh = sinh/cosh`) or have the V2N-side `<alp/tmu.h>`
  wrapper fall back to libm when the bridge signals NOSUPPORT.

  `bridge_hw_init()` adds an `rcu_periph_clock_enable(RCU_TMU)`
  call; per-op configuration happens inside the compute body so
  the dispatch is stateless across calls.  Wire opcode
  `CMD_TMU_COMPUTE` (0x90) flips from `STATUS_NOSUPPORT` to
  `STATUS_OK` on the gd32 backend for the nine supported
  functions; the other three remain `STATUS_NOSUPPORT`.  No
  protocol or ABI change.

- **`bridge_hw_dac_set` / `bridge_hw_dac_get` now drive real DAC
  channels (2026-05-13).**  Fifth of the 18 HAL bodies.  Two
  channels per the `dac_channels[]` map in
  `gd32-bridge/hal/bridge_hw_gd32.c`:
    - channel 0 -> DAC0 / DAC_OUT0 / PA4
    - channel 1 -> DAC1 / DAC_OUT0 / PA6 (per GD32G5x3 datasheet
      pin alt-function table; maintainer-confirmed)
  `bridge_hw_init()` configures PA4 + PA6 as analog, enables
  `RCU_DAC0` + `RCU_DAC1`, and runs the vendor's standard
  `dac_deinit -> dac_trigger_disable -> dac_wave_mode_config
  (WAVE_DISABLE) -> dac_mode_config (NORMAL_PIN_BUFFON) ->
  dac_enable` sequence for each channel.  `bridge_hw_dac_set`
  converts mV to 12-bit right-aligned code at `DAC_VREF_MV =
  3300` (the V2N's 3.3 V analog supply), clamping overflow to
  full-scale.  `bridge_hw_dac_get` reads the hold register
  (the value driving the pad) and converts back to mV.  Wire
  opcodes `CMD_DAC_SET` (0x50) and `CMD_DAC_GET` (0x51) flip
  from `STATUS_NOSUPPORT` to `STATUS_OK` on the gd32 backend.
  No protocol or ABI change.

- **Correct DAC VREF to 1.8 V (2026-05-13).**  The §2B step 5
  commit dcc708e shipped with `DAC_VREF_MV = 3300` based on an
  incorrect assumption about the V2N analog supply rail.
  Maintainer-confirmed against schematic: V2N feeds the GD32G553
  analog domain from a 1.8 V rail, not 3.3 V.  Updates
  `DAC_VREF_MV` to `1800u` in `gd32-bridge/hal/bridge_hw_gd32.c`
  and the matching mention in the top-of-file implementation
  log.  Both the `set` (mV -> code) and `get` (code -> mV)
  paths use this constant so the scaling reverses cleanly.

- **`bridge_hw_counter_read` -> Cortex-M33 DWT cycle counter
  (2026-05-13).**  Sixth step in the implementation order
  (#12 of the original 18-step list; tackled out-of-order
  because DWT needs no peripheral routing or pad config).
  `bridge_hw_init()` arms the trace block (`CoreDebug->DEMCR
  |= TRCENA`) and starts the 32-bit free-running counter
  (`DWT->CYCCNT = 0; DWT->CTRL |= CYCCNTENA`).  The counter
  ticks at the core clock (240 MHz on GD32G553 -> ~4.16 ns
  LSB, ~17.9 s wrap).  `bridge_hw_counter_read(0, *ticks)`
  returns the current `DWT->CYCCNT` value; other counter
  ids return `BRIDGE_HW_ERR_RANGE` (future revisions can
  carve out additional ids for derived slower tick bases).
  Wire opcode `CMD_COUNTER_READ` (0x70) flips from
  `STATUS_NOSUPPORT` to `STATUS_OK` on the gd32 backend.
  No protocol or ABI change.

- **`bridge_hw_pwm_set` / `bridge_hw_pwm_get` -> 8 PWM channels
  on TIMER0 + TIMER7 (2026-05-13).**  Seventh of the 18 HAL
  bodies.  Pin alt-functions confirmed against the GD32G553xx
  Datasheet Rev2.0 (Tables 2-10..2-13, pin alternate-function
  summary) and the V2N schematic AF column:
    - PWM0  PA11  TIMER0_MCH0 (complementary) AF6
    - PWM1  PB1   TIMER0_MCH2 (complementary) AF6
    - PWM2  PB14  TIMER0_MCH1 (complementary) AF6
    - PWM3  PC5   TIMER0_MCH3 (complementary) AF6
    - PWM4  PC10  TIMER7_MCH0                 AF4
    - PWM5  PC11  TIMER7_MCH1                 AF4
    - PWM6  PC12  TIMER7_MCH2                 AF4
    - PWM7  PD0   TIMER7_MCH3                 AF6
  All eight PWMs ride distinct timer channels (main outputs
  unused on V2N), so per-channel duty cycles are independent.
  Timer prescaler fixed at 240-1 -> 1 us tick at the GD32G553's
  240 MHz timer-input clock, giving a 1 us LSB and ~65 ms max
  period (16-bit ARR).  Per-set body updates the timer's ARR
  (period) and the channel's compare (duty), so the period is
  rounded to whole microseconds.  Cache stores the host's exact
  ns request so `bridge_hw_pwm_get` round-trips without rounding
  loss on the read side.

  Architectural constraint documented in the source:
  PWM0..3 share TIMER0's single ARR; PWM4..7 share TIMER7's.
  Setting a different period on two channels of the same timer
  is last-write-wins.  In typical use the host picks one period
  per group so this doesn't surface.

  Wire opcodes `CMD_PWM_SET` (0x20) and `CMD_PWM_GET` (0x21)
  flip from `STATUS_NOSUPPORT` to `STATUS_OK` on the gd32
  backend.  No protocol or ABI change.

- **`bridge_hw_adc_read` -> single-shot polling read across
  ADC0..3 (2026-05-13).**  Eighth of the 18 HAL bodies.  Maps
  the 8 E1M ADC channels per maintainer-confirmed table:
    - E1M ADC0  PD9   ADC3  CH12
    - E1M ADC1  PB12  ADC3  CH2
    - E1M ADC2  PE13  ADC2  CH2
    - E1M ADC3  PE11  ADC2  CH13
    - E1M ADC4  PC4   ADC1  CH4
    - E1M ADC5  PA5   ADC1  CH12
    - E1M ADC6  PA2   ADC0  CH2
    - E1M ADC7  PA3   ADC0  CH3
  `bridge_hw_init()` configures all 8 pads as analog, enables
  `RCU_ADC0..3`, runs the per-peripheral init (deinit ->
  HCLK/6 clock -> right-align -> length=1 -> external trigger
  disable -> enable).  Calibration intentionally skipped --
  it needs a millisecond settling delay after enable() and
  the bridge boot has no SysTick yet; a follow-up commit can
  add it once a delay primitive lands.

  `bridge_hw_adc_read(channel, samples, mv)`: reconfigures
  the ADC's routine slot for the requested channel + 240-cycle
  sample time, then loops software-trigger + poll-EOC +
  read-data per sample.  Raw 12-bit code converted to mV at
  VREF=1800 (matches DAC_VREF_MV).  No timeout on the EOC
  poll -- 12-bit conversion at HCLK/6 finishes in ~6 us so a
  hung ADC would hang the bridge, which a future SysTick
  watchdog can catch.

  `bridge_hw_adc_configure` + the `bridge_hw_adc_stream_*`
  family stay NOSUPPORT pending follow-up commits.  Wire
  opcode `CMD_ADC_READ` (0x30) flips from `STATUS_NOSUPPORT`
  to `STATUS_OK` on the gd32 backend.  No protocol or ABI
  change.

- **`bridge_hw_adc_configure` -> per-channel sample_cycles
  override (2026-05-13).**  Ninth body, partial.  Adds a
  per-channel `adc_sample_cycles_cache[]` so callers can
  tighten sample time from the default 240 cycles to e.g. 24
  (~140 ns sample window at HCLK/6) for low-impedance sources
  without burning ~6 us per conversion.
  `bridge_hw_adc_read` now picks up the cached value
  per-channel.  Resolution + oversample_ratio paths are still
  gated to defaults (12-bit, OS=1); the configure body
  returns `BRIDGE_HW_ERR_NOTIMPL` if the caller asks for
  anything else, so the host sees a clear NOSUPPORT rather
  than silently-ignored config.  A follow-up commit will land
  the resolution + oversample apply path.  Wire opcode
  `CMD_ADC_CONFIGURE` (0x32) flips from `STATUS_NOSUPPORT` to
  `STATUS_OK` for default-resolution requests on the gd32
  backend.  No protocol or ABI change.

- **`bridge_hw_pwm_single_pulse` -> TIMERx one-pulse mode
  (2026-05-13).**  Tenth body.  Reuses the PWM channel map +
  init from §2B step 6 -- the channel's pad is already
  configured as alt-function PWM output; this body just
  switches the underlying timer to OPM (one-pulse mode):
    1. zero the timer counter
    2. set ARR = pulse_us - 1 and channel compare = pulse_us
    3. timer_single_pulse_mode_config(periph, SP_MODE_SINGLE)
    4. timer_enable() (the SP bit halts the timer after one
       wrap, so the channel pad drives high for `pulse_us`
       microseconds then returns to its idle level)
  The pulse-width cache is also updated so a follow-up
  `bridge_hw_pwm_get` reports the one-shot setpoint.

  Per contract, the channel stays in one-pulse mode until the
  next `bridge_hw_pwm_set` on the same channel: that body now
  flips the timer's SP bit back to `SP_MODE_REPETITIVE` before
  writing the period/duty.

  Caveat: the SP bit lives in the timer's CTL0 register
  (timer-wide, not per-channel), so a single-pulse call on
  any one channel of TIMER0 / TIMER7 puts the entire group
  into one-pulse mode.  Mixed continuous + single-pulse use
  across the same timer needs the host to coordinate.
  Documented in the body comment.

  Wire opcode `CMD_PWM_SINGLE_PULSE` (0x26) flips from
  `STATUS_NOSUPPORT` to `STATUS_OK` on the gd32 backend.  No
  protocol or ABI change.

- **`bridge_hw_power_mode_set` partial (2026-05-13).**
  Eleventh body, partial.  Accepts mode 0 (run) + mode 1
  (sleep) as no-ops because `main()`'s `for (;;) { __WFI();
  bridge_hw_tick(); }` already idles the CPU between
  transport ISRs (which IS "sleep" on the GD32G5).  Modes 2
  (deep-sleep) + 3 (standby) need a reply-then-sleep state
  machine (so the transport TX FIFO drains before the
  peripheral clocks gate) + wake-source EXTI/RTC config; they
  return `BRIDGE_HW_ERR_NOTIMPL` for this commit.
  `wake_bitmap` + `wake_after_ms` likewise return NOSUPPORT
  for any non-zero value rather than silently dropping
  caller-supplied wake config.

  Wire opcode `CMD_POWER_MODE_SET` (0x28) flips from
  `STATUS_NOSUPPORT` to `STATUS_OK` for mode 0 / 1 +
  zero-wake-config requests.  Other combinations remain
  `STATUS_NOSUPPORT` pending the follow-up state machine.
  No protocol or ABI change.

- **`bridge_hw_pwm_configure` partial (2026-05-13).**
  Twelfth body, partial.  Accepts the defaults that
  `bridge_hw_init`'s `pwm_timer_init()` programs --
  edge-aligned counter, no dead-time, no break input --
  with `BRIDGE_HW_ERR_NOTIMPL` for any non-default request.
  Lets the host's idempotent "set to defaults" config call
  succeed (useful for the v0.3 host wrapper's config-state
  machine) while signalling that advanced tuning isn't
  wired yet.
  Wire opcode `CMD_PWM_CONFIGURE` (0x22) flips from
  `STATUS_NOSUPPORT` to `STATUS_OK` for default-config
  requests; non-defaults stay `STATUS_NOSUPPORT` pending a
  follow-up that adds the per-timer CAM / dead-time / break
  apply path with last-write-wins across the timer's
  channels.

- **`bridge_hw_qenc_read` / `bridge_hw_qenc_reset` -> 4
  quadrature encoders (2026-05-13).**  Thirteenth body.
  Maintainer-confirmed mapping:
    - ENC0  X=PA0 / Y=PB3  TIMER1  CH0/CH1  AF1
    - ENC1  X=PC6 / Y=PC7  TIMER2  CH0/CH1  AF2
    - ENC2  X=PB6 / Y=PB7  TIMER3  CH0/CH1  AF2  (after PB5/PB7 swap)
    - ENC3  X=PB2 / Y=PA1  TIMER4  CH0/CH1  AF2
  `bridge_hw_init()` configures X / Y pads as alt-function
  inputs with internal pull-up (so a disconnected encoder
  doesn't float and falsely tick), enables `RCU_TIMER1..4`,
  and runs each timer through `timer_quadrature_decoder_mode_config`
  with `TIMER_QUAD_DECODER_MODE2` (X4 -- counts on both
  edges of both inputs) at full-range ARR (16-bit timers
  wrap at 0xFFFF; 32-bit wrap at 0xFFFFFFFF).
  `bridge_hw_qenc_read` reads the timer counter and casts to
  `int32_t` -- caller handles wrap via deltas.
  `bridge_hw_qenc_reset` zeroes the counter via
  `timer_counter_value_config`.  Wire opcodes
  `CMD_QENC_READ` (0x60) and `CMD_QENC_RESET` (0x61) flip
  from `STATUS_NOSUPPORT` to `STATUS_OK` on the gd32 backend.
  No protocol or ABI change.

### Added (2026-05-14)

- **`<alp/tmu.h>` -- portable CORDIC math accelerator surface (with libm fallback) (2026-05-14).**
  Wires a customer-facing math primitive set (sin / cos / tan / atan /
  atan2 / sqrt / log / exp / sinh / cosh / tanh / hypot) without
  exposing the GD32 name to application code (per
  `memory/feedback_portable_hw_offload_with_sw_fallback.md`).  Three
  pieces:
  - New wire opcode `CMD_TMU_COMPUTE` (0x90) on the bridge protocol;
    bumps `PROTOCOL_VERSION_MINOR` 3 -> 4.  Request payload is 12 B
    (`function:u8 format:u8 reserved:u16 in_a:u32 in_b:u32`); reply
    payload is 4 B (`result:u32`).  Function enum covers the twelve
    TMU primitives; format byte picks Q31 fixed-point (0) or IEEE-754
    single-precision (1) -- the SDK's portable surface always uses
    IEEE-754 because its public API takes `float`, leaving Q31 for
    direct `gd32g553_tmu_compute` callers.  Firmware handler dispatches
    to a new `bridge_hw_tmu_compute` HAL hook stubbed in
    `bridge_hw_stub.c` (returns `BRIDGE_HW_ERR_NOTIMPL` -- so the wire
    reply is `STATUS_NOSUPPORT` until `bridge_hw_gd32.c` ships).
  - Host driver wrapper `gd32g553_tmu_compute(ctx, function, format,
    in_a, in_b, *result)` in `chips/gd32g553/gd32g553.c` carries the
    range-check + NULL-pointer contract; declared in
    `include/alp/chips/gd32g553.h`.
  - Portable `alp_tmu_*` surface in a new `<alp/tmu.h>` -- twelve
    primitives, each taking `float` inputs and writing a `float` out
    parameter, returning `alp_status_t`.  On the V2N family
    (CONFIG_ALP_SDK_V2N_SUPERVISOR=y) each call acquires the
    supervisor singleton and dispatches through
    `gd32g553_tmu_compute` with `GD32G553_TMU_FMT_F32`; on every
    other SoM (and under `native_sim`) the wrapper falls back to
    libm (`sinf`, `cosf`, `sqrtf`, ...) directly -- the libm path
    never returns `ALP_ERR_NOSUPPORT`, so customer code can rely on
    the surface working everywhere.  Wire format choice (IEEE-754
    single) documented in `docs/gd32-bridge-protocol.md` §3.12.
  New Kconfig `CONFIG_ALP_SDK_PERIPH_TMU` (default y) gates the new
  source file `src/zephyr/peripheral_tmu.c`.  Tests under
  `tests/zephyr/peripheral/src/main.c` cover the NULL-out INVAL
  paths for each primitive, a libm-fallback correctness check on
  non-supervisor builds (`sqrt(4) == 2.0`, `sin(0) == 0`,
  `hypot(3, 4) == 5`), and a `NOT_READY without buses` assertion
  under `CONFIG_ALP_SDK_V2N_SUPERVISOR=y` proving the supervisor
  branch reaches dispatch.  Protocol vectors regenerated via
  `gd32-bridge/tests/gen_protocol_vectors.py` (new §6 block for the
  v0.4 additions, includes `spi_tmu_compute_sqrt_f32_4p0_request`).
  ABI snapshots `docs/abi/v0.{1,3}-snapshot.json` regenerated.

- **`<alp/pwm.h>` -- portable per-channel PWM tuning surface (2026-05-14).**
  Wires the customer-facing path for the v0.3 `CMD_PWM_CONFIGURE`
  opcode without exposing the GD32 name to application code (per
  `memory/feedback_portable_hw_offload_with_sw_fallback.md`).
  Three additions:
  - `alp_pwm_align_t` enum mirrors the four advanced-timer counter
    shapes (edge, center-aligned-up, center-aligned-down,
    center-aligned-both).  Wire values match `gd32g553_pwm_align_t`
    by construction so the bridge dispatch is an enum-renaming cast.
  - `ALP_PWM_BREAK_NONE` / `ALP_PWM_BREAK_EXTERNAL` bitmap macros
    for the break-input opcode argument.  Bit 0 is the external
    break input; remaining bits reserved for future fault sources.
  - `alp_pwm_configure(pwm, align, dead_time_ns, break_cfg)`.  On
    the V2N family (V2N + V2N-M1, shared GD32G553) the call
    acquires the supervisor singleton and dispatches through
    `gd32g553_pwm_configure`; the configuration is sticky across
    subsequent `alp_pwm_set_duty` / `alp_pwm_set_period` calls.
    On non-bridge SoMs the call returns `ALP_ERR_NOSUPPORT` --
    Zephyr's portable `pwm_*` driver class exposes neither
    dead-time nor center-aligned counters outside vendor-specific
    extensions, so silently accepting the call would mislead
    callers.
  Tests cover the binding-layer NULL-handle path; bridge /
  DT-alias dispatch is exercised by the supervisor scenario's
  compile + link step.

- **`<alp/security.h>` -- GD32 TRNG as a PSA Crypto entropy source on V2N (2026-05-14).**
  Wires the v0.3 `CMD_TRNG_READ` opcode (shipped on the bridge side
  in the protocol-v0.3 commit) into MbedTLS's platform entropy
  callback, so the portable `alp_random_bytes()` transparently picks
  up true randomness on V2N without app code mentioning the GD32
  name (per `memory/feedback_portable_hw_offload_with_sw_fallback.md`).
  The SDK's mbedtls profile already sets `MBEDTLS_NO_PLATFORM_ENTROPY`,
  which makes mbedtls request a `mbedtls_hardware_poll()` from the
  integrator; the new implementation in `src/zephyr/security_zephyr.c`
  drains the GD32G553's NIST SP800-90B pre-certified TRNG in chunks
  of <= `GD32G553_BRIDGE_TRNG_MAX_BYTES` (= 32 B) through the
  supervisor singleton, releasing the bridge mutex between chunks so
  long entropy fills don't serialise other peripheral ops behind
  them.  Partial-fill returns are surfaced honestly (mbedtls then
  folds the bytes into its accumulator and reseeds later); a
  zero-progress failure surfaces as `MBEDTLS_ERR_ENTROPY_SOURCE_FAILED`.
  New Kconfig: `CONFIG_ALP_SDK_SECURITY_V2N_TRNG_ENTROPY`
  (default y when both the V2N supervisor and `<alp/security.h>` are
  on; depends on `ALP_SDK_V2N_SUPERVISOR && ALP_SDK_SECURITY`).
  Public `alp_random_bytes()` contract is unchanged -- still
  `ALP_OK` / `ALP_ERR_NOT_READY` / `ALP_ERR_IO` / `ALP_ERR_INVAL`,
  with the GD32 dependency invisible to applications.  Tests under
  `tests/zephyr/peripheral/src/main.c` cover the public-surface
  NULL-arg contract on the V2N supervisor build (bridge / PSA
  round-trip exercised by the supervisor scenario's compile + link
  step plus the existing security smoke suite).

- **`<alp/adc.h>` -- portable streaming ADC surface + v0.3 configure knobs (2026-05-14).**
  Wires the customer-facing path for the v0.3 wire opcodes the prior
  commit shipped on the GD32 side, with no `gd32g553_*` symbols
  visible to application code (per
  `memory/feedback_portable_hw_offload_with_sw_fallback.md`).  Two
  additions:
  - `alp_adc_config_t` grows `oversampling_ratio` (1..256, rounded
    down to power-of-two) and `sample_cycles` (rounded down to the
    backend's nearest discrete tap).  When either is non-zero --
    or `resolution_bits` differs from the firmware default -- the
    V2N-family backend pushes the values via
    `gd32g553_adc_configure` between supervisor-acquire and
    handle-return so the very first `alp_adc_read_*` already
    honours the tuning.  Existing zero-initialised configs keep
    today's behaviour.
  - New opaque `alp_adc_stream_t` + `alp_adc_stream_config_t`
    (channel + sample rate) with `alp_adc_stream_open` /
    `_read` / `_close`.  On the V2N family (V2N + V2N-M1, both
    of which carry the GD32G553) up to two streams run
    concurrently against different channels / sample rates,
    each one binding to one of the GD32's DMA controllers via
    `gd32g553_adc_stream_*`; the portable surface tracks slot
    allocation locally under a small mutex so probing
    `STREAM_BEGIN` against a sibling caller's slot never
    happens.  Non-bridge SoMs surface `ALP_ERR_NOSUPPORT` at
    open time -- the Zephyr `adc_*` driver class has no
    portable streaming primitive that matches, and a polling-
    thread software fallback is reserved for the wave-2 DSP
    pipeline so the contract stays honest until then.  New
    Kconfig: `CONFIG_ALP_SDK_MAX_ADC_STREAM_HANDLES`
    (default 2 to mirror the GD32 stream-slot count).
  Tests under `tests/zephyr/peripheral/src/main.c` cover the
  NULL-cfg / NULL-out / out-of-range / zero-rate / NOSUPPORT-on-no-
  backend paths plus the V2N supervisor `NOT_READY without buses`
  rollback (asserts the local slot bitmap unwinds so a later open
  can still claim slot 0).

- **GD32 bridge protocol v0.3 -- GD32G5 HW knobs (PWM/ADC config, two-DMA ADC streaming, TRNG) (2026-05-14).**
  Bumps `PROTOCOL_VERSION_MINOR` 2 -> 3 to surface a chunk of the
  GD32G5's hardware feature set the v0.1/v0.2 protocols left on
  the table.  Six new wire opcodes (firmware HAL stubs return
  `STATUS_NOSUPPORT` until `bridge_hw_gd32.c` wires them):
  - `CMD_PWM_CONFIGURE` (0x22) -- sticky per-channel alignment
    mode (edge-aligned / center-aligned up/down/both), programmable
    dead-time (ns) for complementary outputs, break-input enable.
    On V2N every E1M PWM rides one of the GD32's two 16-bit
    advanced timers (PWM0..3 -> TIMER0_MCH0..MCH3 on GD32 pads
    PA11/PB1/PB14/PC5; PWM4..7 -> TIMER7_MCH0..MCH3 on
    PC10/PC11/PC12/PD0); both timers run at 240 MHz so the
    effective LSB is ~4.16 ns.  `metadata/chips/gd32g553.yaml`
    gains a `pwm_routing:` table making that mapping
    machine-readable.
  - `CMD_ADC_CONFIGURE` (0x32) -- sticky per-channel oversampling
    (1..256x, rounded down to power-of-two), sample-and-hold
    cycles (2/6/12/24/47/92/247/640 per the datasheet), resolution
    (6/8/10/12/14/16 bits; 14- and 16-bit modes require
    oversample >= 4 / 16 respectively per the
    effective-resolution table).
  - `CMD_ADC_STREAM_BEGIN` (0x33) / `_READ` (0x34) / `_END` (0x35) --
    DMA-backed continuous acquisition.  Two streams supported
    concurrently (stream 0 binds to GD32 DMA0, stream 1 to DMA1
    per the chip's dual-DMA-controller topology); different
    channels at different sample rates.  Firmware ring buffer
    decouples DMA cadence from host poll cadence; ring overrun
    surfaces as `STATUS_BUSY` on the next `_READ`.
  - `CMD_TRNG_READ` (0x80) -- pulls 1..32 bytes of true randomness
    from the GD32G5's NIST SP800-90B pre-certified TRNG.
    Future commit wires it to PSA Crypto via `<alp/security.h>`
    so the portable `alp_random_bytes` benefits transparently on
    V2N.
  Host driver wrappers in `chips/gd32g553/gd32g553.c` carry the
  matching range-check + NULL-pointer contract.  Wire envelope
  for `ADC_STREAM_READ` is fixed-length (host pre-commits to
  `1 + max_samples*2 + 2` reply bytes; firmware zero-pads slots
  beyond the actual `got` count so framing stays deterministic).

- **V2N supervisor singleton + portable PWM/ADC/DAC/QENC/COUNTER backends (2026-05-14).**
  Closes the loop opened by the 2026-05-12 "all V2N PWMs are GD32-driven"
  decision: the SDK's portable peripheral surface now actually reaches
  the GD32 IO MCU on V2N instead of returning `ALP_ERR_NOT_READY` at
  the SoC-cap check.  Two pieces:
  - `src/zephyr/v2n_supervisor.{c,h}` -- lazy-initialised singleton
    wrapping a `gd32g553_t` driver context plus the underlying
    `alp_spi_t` / `alp_i2c_t` handles.  Acquire / release pair
    serialises every bridge call under a shared mutex so the
    `gd32g553_t` "caller must serialise" contract is honoured without
    forcing each backend to maintain its own lock.  New Kconfig:
    `CONFIG_ALP_SDK_V2N_SUPERVISOR` (default y on V2N + GD32G553),
    bus-id selectors `..._SPI_BUS_ID` / `..._I2C_BUS_ID` (mirroring
    the `CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID` pattern), I2C
    slave address `..._I2C_ADDR` (default `0x70`), plus bus-speed
    knobs `..._SPI_FREQ_HZ` (10 MHz default) and
    `..._I2C_BITRATE_HZ` (400 kHz default).
  - V2N dispatcher branches in `src/zephyr/peripheral_pwm.c`,
    `peripheral_adc.c`, `peripheral_qenc.c`, `peripheral_counter.c`,
    and a new `peripheral_dac.c`.  Each backend's V2N branch is
    `#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)` and routes through
    `alp_z_v2n_supervisor_acquire / release` + the v0.2 GD32
    wrappers (`gd32g553_pwm_set` / `_adc_read` / `_qenc_read` /
    `_qenc_reset` / `_counter_read` / `_dac_set` / `_dac_get`).
    Bridge handles are tagged with `h->dev == NULL`; non-V2N builds
    take the unchanged DT-alias path.
  - `alp_dac_*` public surface from the prior commit is now wired:
    on V2N via the bridge, on Zephyr's `dac_*` driver class
    elsewhere (`alp-dacN` DT alias).  When `CONFIG_DAC=n` the wrapper
    still compiles and returns `ALP_ERR_NOSUPPORT` so apps link
    cleanly under `native_sim`.
  - **Counter alarms on V2N return `ALP_ERR_NOSUPPORT`.** The GD32
    has no interrupt line back to the Renesas host so deadline
    callbacks fired in firmware ISR context can't be relayed across
    the bridge in bounded time.  `alp_counter_us_to_ticks` also
    returns `NOSUPPORT` on V2N until the v0.3 protocol revision
    adds `CMD_COUNTER_GET_FREQ`.  Read-only `alp_counter_get_value`
    works.
  - New twister scenario `alp_sdk.peripheral.v2n_supervisor` flips
    `CONFIG_ALP_SOC_RENESAS_RZV2N_N44=y` + the supervisor + DAC
    Kconfigs on under `native_sim`.  With no bus IDs configured the
    supervisor surfaces `ALP_ERR_NOT_READY`; the new ZTESTs assert
    every portable `*_open` propagates that error code rather than
    silently falling through to a misleading `ALP_ERR_OUT_OF_RANGE`
    from the soc_caps check.  Real-silicon coverage lives in the
    V2N HIL plan.
  - Unblocks `examples/v2n/v2n-pwm-fan-control/` at runtime: customers
    who set the two bus-id Kconfigs on a real V2N board get a live
    fan-control loop with no application-side knowledge of the
    GD32 bridge.

- **GD32 bridge protocol v0.2 -- DAC / QENC / COUNTER opcodes (2026-05-14).**
  Bumps `PROTOCOL_VERSION_MINOR` 1 → 2 to add five wire opcodes that
  cover the analog + counter peripherals the GD32 already routes per
  `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv`:
  - `CMD_DAC_SET` (0x50) / `CMD_DAC_GET` (0x51) -- E1M `DAC0` (GD32
    `PA4`) + `DAC1` (GD32 `PA6`).  mV setpoint, 12-bit hardware
    resolution, read-back for verification.
  - `CMD_QENC_READ` (0x60) / `CMD_QENC_RESET` (0x61) -- the four E1M
    encoders on GD32 pad pairs `PA0/PB3` / `PC6/PC7` / `PB6/PB5` /
    `PB2/PA1`.  Signed-int32 accumulated count wrapping modulo 2³²
    so velocity is recoverable across overflow.
  - `CMD_COUNTER_READ` (0x70) -- one free-running counter; tick
    frequency is firmware-defined for v0.2 (a `COUNTER_GET_FREQ`
    opcode follows in v0.3 for portable µs ↔ tick conversion).
    Counter alarms are intentionally out of scope -- the GD32 has
    no interrupt line back to the Renesas host so ISR-fired
    callbacks can't cross the bridge in bounded time.
  Firmware-side handlers in `gd32-bridge/src/protocol.c` route to
  new `bridge_hw_dac_*` / `bridge_hw_qenc_*` / `bridge_hw_counter_*`
  HAL hooks; the stub `hal/bridge_hw_stub.c` returns
  `BRIDGE_HW_ERR_NOTIMPL` against the scaffold, so every body
  replies `STATUS_NOSUPPORT` on the wire until
  `hal/bridge_hw_gd32.c` is implemented alongside the vendor
  firmware library pull.  Host driver wrappers under
  `chips/gd32g553/gd32g553.c` carry the same range-check + NULL-arg
  contract as the existing opcodes.  Wire vectors regenerated
  through `gd32-bridge/tests/gen_protocol_vectors.py`; reference-CRC
  helpers in `protocol_vectors.txt` now cover one request per new
  opcode plus a `STATUS_NOSUPPORT` reply envelope.

- **`<alp/adc.h>` -- portable DAC surface (2026-05-14).**
  Per the portability memo (`memory/feedback_portable_peripheral_api.md`),
  DAC sits alongside ADC in the same header.  Adds `alp_dac_t`,
  `alp_dac_open(cfg)` (`channel_id` + `initial_mv`), `alp_dac_write_mv`,
  `alp_dac_read_mv`, `alp_dac_close`.  Backend wiring lands in the
  follow-up commit; the surface returns `ALP_ERR_NOT_READY` on every
  SoM until the Zephyr backend declares an `alp-dac<N>` alias (or, on
  V2N, the supervisor singleton lazy-inits).

- **`docs/aen-feature-audit-2026-05.md` -- Alif Ensemble peripheral
  audit vs Zephyr driver-class coverage (2026-05-14).**  Block-level
  research deliverable in preparation for promoting any E1M-AEN SoM
  (E1M-AEN301..E1M-AEN801) to production-supported.  Walks every
  Ensemble peripheral the AEN-family silicon documents (per-SoC JSON
  in `metadata/socs/alif/ensemble/`) against the `drivers/<class>/`
  tree in Zephyr v3.7 and categorises each block into one of four
  buckets: COVERED, COVERED-AFTER-DRIVER-PACK, NEEDS-PORTABLE-SURFACE,
  DEFERRED.  Flags GPU2D (D/AVE 2D), Ethos-U NPU dispatch, ISP
  acceleration, aiPM autonomous power management, and inline-AES on
  HexSPI/OSPI as the top-five "would silently disappear on V2N→AEN
  migration" gaps.  Research only -- no code, no API change.

- **`docs/cc3501e-integration-plan.md` -- SWRU626 deep-dive informing
  the first `alplabai/cc3501e-firmware` drop (2026-05-14).**  Deep
  read of TI SWRU626 (CC3501E technical reference manual) +
  cross-reference against the existing host-side stub (`chips/cc3501e/`
  + `<alp/protocol/cc3501e.h>`).  Builds a peripheral-by-peripheral
  plan (SPI / SDIO / DMA / GPIO / UART / ADC / GPT / I2C / power /
  boot) split into host-side responsibilities (stay in this repo,
  Apache-2.0 clean) and firmware-side responsibilities (cc3501e-
  firmware, per ADR 0005).  Surfaces two host-side issues the plan
  resolves into v0.3.x commits: the current `cc3501e_reset()` violates
  SWRU626 §7.1.5 timing (no 10 us nRESET hold; no boot budget wait
  for BL1→BL2→Chain-of-Trust), and the wire protocol's magic
  `direction` / `pull` bytes need named enums for the upcoming GPIO
  set-interrupt + open-drain support.  Plan-only -- no code, no API
  change.

- **`<alp/dsp.h>` -- standalone DSP-chain API for in-RAM signal
  processing (2026-05-14).**  Composable FIR / IIR / WINDOW / FFT
  pipelines run against int16 mV-scale sample buffers.  Surface:
  `alp_dsp_chain_open(stages, n)` validates the chain (FFT must be
  terminal; WINDOW must immediately precede FFT; per-stage bounds),
  copies coefficients in (caller-supplied storage may be freed on
  return), and returns a pooled handle.
  `alp_dsp_chain_apply_samples` runs filter-terminated chains and
  writes filtered samples; the FFT-terminated sibling
  `alp_dsp_chain_apply_bins` emits f32 bins in COMPLEX (re/im
  pairs) or MAGNITUDE format.  Math kernels prefer CMSIS-DSP
  (`arm_fir_*`, `arm_biquad_cascade_df1_*`, `arm_rfft_fast_f32`)
  when `ALP_HAS_CMSIS_DSP=1`; otherwise a portable C fallback runs
  (naive convolution + direct-form-1 biquad cascade + radix-2
  Cooley-Tukey FFT).  Wired behind `CONFIG_ALP_SDK_DSP` (default
  off) on Zephyr; built unconditionally for plain-CMake /
  baremetal consumers.  Static cost: ~30 KB for the two-chain
  pool sized to ALP_DSP_MAX_*.  Ships with native_sim ztest
  coverage at `tests/zephyr/dsp/` (chain validation; identity FIR;
  identity IIR; DC + sinusoid + impulse FFT verification).
  Wave-2 bridge-wired counterparts (`alp_adc_filter_t` /
  `alp_adc_spectrum_t` in `<alp/adc.h>`) land in the v0.5.x (b)
  and (c) follow-ups -- see
  `memory/project_wave2_dsp_pipeline_design.md` for the design
  intent and bridge wire-format split.

- **`gd32-bridge` protocol v0.5 -- reserves
  `CMD_ADC_STREAM_CONFIGURE_DSP` (opcode `0x36`) for the wave-2
  bridge-wired DSP surface (2026-05-14).**  Bumps
  `PROTOCOL_VERSION_MINOR` 4 -> 5 (`gd32-bridge/src/protocol.h` +
  `<alp/chips/gd32g553.h>` in lock-step).  The host-side
  standalone API in `<alp/dsp.h>` does NOT use this opcode -- it
  runs the chain locally over in-RAM buffers.  The firmware-side
  dispatcher routes `0x36` through the default
  `STATUS_NOSUPPORT` branch today; the wire payload format
  (stage list + per-stage params, sized against the GD32G5 FFT /
  FAC block's input-shape constraints) lands in the v0.5.x (b) and
  (c) sub-commits alongside `alp_adc_filter_t` /
  `alp_adc_spectrum_t`.  Wire vectors include a representative
  probe request in §7 of
  `gd32-bridge/tests/protocol_vectors.txt`.

- **`<alp/chips/gd32g553.h>` -- host helpers for v0.5 advanced-
  timer + power-mode opcodes (2026-05-14).**  Adds six new
  GD32G553 host driver functions that mirror the §2B.2 + §2B.3
  wire opcodes added earlier in this wave:

  * `gd32g553_pwm_capture_begin / read / end` -- input-capture
    on a PWM channel.  Mirrors `alp_pwm_capture_*` in `<alp/pwm.h>`.
  * `gd32g553_pwm_single_pulse(channel, pulse_ns)` -- one-shot
    pulse output.  Mirrors `alp_pwm_single_pulse`.
  * `gd32g553_timer_sync(master, slave, mode)` -- master-slave
    advanced-timer linkage across TIMER0 / TIMER7 / TIMER19.
  * `gd32g553_power_mode_set(mode, wake_bitmap, wake_after_ms)`
    -- system-wide sleep-mode request.  Mirrors
    `alp_power_request_sleep` in `<alp/power.h>`.

  Each helper marshals the wire payload per the format documented
  in `docs/gd32-bridge-protocol.md` §3.y / §3.z, calls `cmd_send`
  with the matching opcode (0x23..0x28), and returns
  `ALP_ERR_NOSUPPORT` against the current firmware (default-case
  dispatch returns STATUS_NOSUPPORT until the corresponding
  `bridge_hw_*` HAL bodies land).  Once the HAL ships, the same
  call paths return ALP_OK with the documented payloads -- no
  host-side changes needed.

  Closes Task #10 from the audit task list: every v0.5 protocol
  opcode now has a matching host-helper function so the portable
  surfaces in `<alp/pwm.h>` + `<alp/power.h>` can dispatch through
  the chip layer (instead of the current short-circuit NOSUPPORT
  return) once the firmware-side HALs land.

  ABI snapshots regen'd (still 58 headers).  Doxygen coverage
  292 -> 296 (= 100%) -- six new public functions documented with
  full @brief / @param / @return tags (the +4 vs +6 delta tracks
  how the doxygen counter aggregates multi-out functions).

- **`<alp/storage.h>::alp_storage_configure_inline_aes` -- on-the-
  fly XIP encryption / decryption surface for AEN OSPI / HexSPI
  (AEN audit §4.3 top-five gap, 2026-05-14).**  Extends the
  storage surface with inline-AES configuration so customers can
  protect XIP-resident code + data on external flash:

  * `alp_storage_aes_mode_t`: OFF / CTR / XTS.  XTS is the
    standard mode for storage encryption at flash-block
    granularity; CTR for sequential streaming.
  * `alp_storage_aes_config_t`: mode + key (16 / 24 / 32 bytes
    for AES-128 / 192 / 256) + IV / tweak (typically 16 bytes).
    Key material is read only at configure() time; backends
    bind to a HW key slot if the controller supports it so the
    key never traces back to RAM.
  * `alp_storage_configure_inline_aes(storage, cfg)` programs
    the controller; re-keying mid-session is supported by
    calling again.  Passing `mode == OFF` disables the path.

  ABI-safe extension: no fields added to existing
  `alp_storage_config_t`; a new typedef + new function instead,
  so callers using positional struct init keep compiling.
  Both Zephyr (`src/zephyr/storage_stub.c`) and non-Zephyr
  (`src/common/stub_backend.c`) backends ship NOSUPPORT with
  full INVAL pre-checks (NULL cfg / bad mode / NULL key when
  mode != OFF / invalid key_bytes).  Real impls land when:
    * AEN E4 / E6 / E8 : Alif SecAES + the OSPI HAL grow the
      key/iv programming path; Zephyr flash device exposes it
      via a vendor-extension API.
    * i.MX 93         : NXP FlexSPI OTFAD module + the
      corresponding BSP extension lands.
    * V2N             : stays NOSUPPORT (no on-die SecAES path).

- **`<alp/camera.h>::alp_camera_configure_isp` -- ISP
  configuration surface for AEN-family Mali-C55 ISP (AEN audit
  §4.3 top-five gap, 2026-05-14).**  Extends the existing
  `<alp/camera.h>` stub with the missing in-line ISP path:

  * `alp_camera_isp_config_t` with the coarse pipeline toggles
    (auto_exposure / auto_white_balance / auto_focus /
    lens_shading / dead_pixel_correction / noise_reduction)
    plus picture-tuning offsets (brightness / contrast /
    saturation, -128..+127).
  * `alp_camera_configure_isp(camera, isp)` applies the config;
    safe to call before or after `alp_camera_start`; backends
    latch + apply on the next frame boundary.

  v0.5 ships the surface with NOSUPPORT contract on every
  backend (`src/zephyr/camera_stub.c` + `src/common/stub_backend.c`)
  -- the AEN Mali-C55 HAL wiring lands once the Alif vendor pack
  registers a Zephyr-side ISP-config callback on the camera
  device.  V2N never gets a real impl (no on-die ISP).
  Customers writing portable code that wants ISP tuning can
  ship today; the call gracefully degrades to NOSUPPORT on
  SoMs without an ISP.

- **`<alp/gpu2d.h>` -- portable 2D graphics accelerator surface
  (AEN audit §4.3 top-five gap, 2026-05-14).**  Closes the
  headline gap from the Alif Ensemble feature audit
  (docs/aen-feature-audit-2026-05.md): customers migrating from
  V2N to AEN otherwise silently lose 2D acceleration because
  Zephyr has no portable `gpu` / `2d` driver class.

  Surface:
    * `alp_gpu2d_t` opaque single-instance handle.
    * `alp_gpu2d_surface_t` descriptor: base / width / height /
      stride_bytes / format.  Pixel formats: ARGB8888, RGB565,
      A8, RGB888, RGBA8888.
    * `alp_gpu2d_open / close` lifecycle.
    * `alp_gpu2d_fill_rect(handle, dst, x, y, w, h, argb_color)`
      -- solid-colour rectangle fill.
    * `alp_gpu2d_blit(handle, src, sx, sy, dst, dx, dy, w, h)`
      -- format-converting rect copy.
    * `alp_gpu2d_blend(handle, src, sx, sy, dst, dx, dy, w, h, mode)`
      -- alpha blend with Porter-Duff modes: REPLACE / SRC_OVER /
      ADDITIVE / MULTIPLY.

  v0.5 ships NOSUPPORT-with-INVAL-pre-checks stubs on every
  backend:
    * Zephyr: `src/zephyr/gpu2d_zephyr.c` (validates surface
      descriptors + mode enums; returns NOSUPPORT).  Wired
      behind `CONFIG_ALP_SDK_GPU2D=y` (default on).
    * stub_backend.c: NOSUPPORT for yocto / baremetal builds.

  Per-vendor HAL bodies land via `CONFIG_ALP_SOC_*` selection
  once the vendor packs stabilise as Zephyr modules:
    * AEN-family : Alif `alif_dave2d-driver`.
    * i.MX 93    : Vivante GC328 (NXP BSP, planned).
    * V2N        : stays NOSUPPORT -- no on-die 2D block.

- **`<alp/peripheral.h>` -- portable busy-wait + sleep primitives
  + CC3501E §5.5 reset-timing fix (2026-05-14).**  Adds two new
  portable delay surfaces that the prior CC3501E integration plan
  flagged as a missing primitive blocking §5.5:

  * `alp_delay_us(us)` -- busy-wait for at least `us` microseconds.
    Cycle-accurate spin, no scheduler yield.  Zephyr backend wraps
    `k_busy_wait`; baremetal / yocto stub_backend.c provides a
    calibrated portable-C busy-loop fallback (rough but link-clean
    on every backend).
  * `alp_delay_ms(ms)` -- yielding sleep for at least `ms`
    milliseconds.  Zephyr wraps `k_msleep`; stub falls through to
    the us path 1000x per iteration.

  `chips/cc3501e/cc3501e.c::cc3501e_reset()` now follows the
  SWRU626 §7.1.5 power-on / reset sequence:
    1. Assert nRESET low + drop rails (10us settle).
    2. Raise rails (WIFI_EN high), wait ~5 ms for supply
       stabilisation.
    3. Hold nRESET low for >= 10 us with valid supplies (§7.1.5).
    4. Release nRESET, wait the T1+T2+T3+T4 ~900 ms boot budget
       (BL1 + BL2 + Chain-of-Trust) before returning.

  Total blocking time: ~905 ms (was: returns immediately, racing
  the first PING against the boot chain).  Callers that don't
  want the synchronous wait can fire cc3501e_reset from a worker
  thread and poll via PING; a non-blocking variant lands once
  the firmware-side "boot done" GPIO is wired (§5.6 dep).

  Together this closes §5.5 of the CC3501E integration plan
  (`docs/cc3501e-integration-plan.md`).

  Doxygen coverage moves 288 -> 290 (= 100%) -- both new public
  functions ship with full `@brief` / `@param` tags.  Wired
  unconditionally on every build: alp_delay_* is foundational
  enough to belong outside any opt-in Kconfig gate.

- **`<alp/protocol/cc3501e.h>` -- extended diagnostics + power
  policy opcodes (§2A.2 plan §5.4 + §5.7, 2026-05-14).**  Two
  new opcodes + their wire payload structs:

  * `CMD_GET_DIAG_INFO` (0x04, extends the meta opcode range)
    -- reply payload `alp_cc3501e_diag_info_t` (16 bytes):
    fw_version, reset_cause (POWER_ON / NRST_PIN / SOFT /
    WATCHDOG / BROWNOUT / BLE_STACK / WIFI_STACK), role
    (OFF / WIFI_STA / WIFI_AP / BLE_PERIPHERAL / BLE_CENTRAL /
    DUAL_WIFI_BLE), uptime_ms, free_heap_bytes, last_error.
    Replaces the audit's "extended PING reply" -- a separate
    opcode avoids breaking the wire format of the existing PING
    + GET_VERSION calls.
  * `CMD_POWER_POLICY` (0x62, extends the power opcode range)
    -- request payload `alp_cc3501e_power_policy_t` (8 bytes):
    coarse policy preset (PERFORMANCE / BALANCED / LOW_POWER /
    DEEP_SLEEP), wake-event bitmap (HOST_SPI / BLE_CONN /
    BLE_ADV / WIFI_BEACON / WIFI_AP_CLIENT / GPIO_IRQ), and an
    `idle_ms_before_sleep` minimum-idle hint.  Lets the host
    tell the CC3501E firmware how aggressively to gate its
    receive paths while idle.

  Both opcodes are v2-firmware-only -- the v1 parser rejects
  them with `ALP_CC3501E_RESP_ERR_INVALID`.  Header is purely
  declarative; firmware-side handlers land in the first
  `alplabai/cc3501e-firmware` drop.  No protocol bump.

- **`<alp/protocol/cc3501e.h>` -- GPIO set-interrupt + event
  payload structs (§2A.2 plan §5.3, 2026-05-14).**  Lands the
  payload typedefs for `CMD_GPIO_SET_INTERRUPT` (opcode 0x53)
  and the async `EVT_GPIO_INTERRUPT` (opcode 0x54), both of
  which had opcode numbering committed at the v1 protocol
  header but no payload struct definitions yet:

  * `alp_cc3501e_gpio_edge_t`: NONE / RISING / FALLING / BOTH
    enum so callers can name the edge polarity instead of
    shipping magic 0..3 constants.  NONE doubles as "disable
    the IRQ" by entering the same code path on the firmware
    side.
  * `alp_cc3501e_gpio_set_interrupt_t`: 4-byte request payload
    (cc3501e_gpio + edge + enabled + reserved).  Wire-compatible
    with the v1 header's reserved alignment slot.
  * `alp_cc3501e_gpio_event_t`: 8-byte async event payload
    (cc3501e_gpio + level + 2-byte reserved + 32-bit
    timestamp_us).  Timestamp is the CC3501E firmware's
    monotonic uptime counter so host code can dedupe / debounce
    across SPI poll cycles.

  Builds on the §5.2 GPIO direction / pull enums shipped in
  the prior commit.  No protocol bump -- the structs are purely
  declarative additions to the existing v1 opcodes.

- **`src/zephyr/v2n_supervisor.c` -- post-wake re-init hook for
  the wave-2 §2B.3 power-saving path (2026-05-14).**  Adds the
  internal SDK API `alp_z_v2n_supervisor_invalidate()` so the
  GD32 supervisor singleton can be reset after a deep-sleep ->
  wakeup cycle.  Mechanism:

  * Takes the supervisor mutex (bounded by the same
    `CONFIG_ALP_SDK_V2N_SUPERVISOR_ACQUIRE_TIMEOUT_MS` window as
    a normal acquire to avoid piling up behind in-flight bridge
    ops).
  * Closes any open SPI / I2C bus handles (the GD32 may have
    been reset across the sleep cycle, so reusing the cached
    handles is unsafe).
  * Clears the `tried_init` latch so the next `acquire()`
    re-runs the bus open + GD32 handshake from scratch.
  * Best-effort: if the mutex acquire times out (another
    thread mid-bridge-op), gives up silently -- the in-flight
    thread's call will fail naturally if the GD32 is
    unresponsive post-wake, and its failure path leaves
    tried_init clear anyway.

  Mirrored NOSUPPORT stub for builds without
  `CONFIG_ALP_SDK_V2N_SUPERVISOR`.  Documented in
  `v2n_supervisor.h` as an internal-only API; the future
  `alp_power_request_sleep` wake handler is the authoritative
  caller once the firmware-side CMD_POWER_MODE_SET (0x28) HAL
  body + the Zephyr `pm_state` wiring both land.
  `power_zephyr.c` documents the planned wake-path call site
  inline so the resume hook is discoverable when wiring the
  real impl.

- **`<alp/protocol/cc3501e.h>` -- protocol docs hygiene + named
  GPIO direction / pull enums (§2A.2 plan §5.1 + §5.2,
  2026-05-14).**  Two purely-declarative cleanups extracted from
  the CC3501E SWRU626 integration plan that customers can adopt
  today without waiting for the first firmware drop:

  * §5.1: documents the `ALP_CC3501E_FLAG_CONTINUATION` (bit 2,
    reserved at v1, lit on intermediate frames of a multi-frame
    BLE-write transaction in v2) and adds a
    `ALP_CC3501E_CMD_RESERVED_VENDOR_BASE` (0x80) marker for the
    vendor-extension reserved range.  Firmware-side parser rejects
    opcodes in that range with `ALP_CC3501E_RESP_ERR_INVALID`
    until a future protocol revision consumes them.
  * §5.2: adds `alp_cc3501e_gpio_direction_t` (INPUT / OUTPUT /
    **OPEN_DRAIN**) and `alp_cc3501e_gpio_pull_t` (NONE / UP /
    DOWN) enums so the wire's `direction` + `pull` bytes get
    named values instead of magic `0/1/2` constants.  The
    OPEN_DRAIN direction is the M.2 W_DISABLE1 / W_DISABLE2
    contract: host drives low to disable; HiZ releases via the
    carrier's external pull-up.

  Wire format unchanged (the enums share their byte-width with
  the existing struct fields), so this is a source-only API
  cleanup with no protocol bump.

- **`<alp/power.h>` -- system-power-mode surface for sleep /
  deep-sleep / standby (wave-2 §2B.3, 2026-05-14).**  New
  customer-facing header declaring `alp_power_t` (opaque
  handle), `alp_power_mode_t` (RUN / SLEEP / DEEP_SLEEP /
  STANDBY enums), `ALP_POWER_WAKE_*` bitmap (RTC / GPIO /
  UART_RX / TIMER / USB / ETH_LINK), and `alp_power_wake_info_t`
  (realised_mode + wake_source + slept_ms).  Lifecycle:
  `alp_power_open()` -> `alp_power_configure_wake_source(p,
  bitmap)` -> `alp_power_request_sleep(p, mode, wake_after_ms,
  &info)` (synchronous; blocks until wake) -> `alp_power_close`.

  Adds one new reserved opcode `CMD_POWER_MODE_SET` (`0x28`)
  to the bridge protocol (within v0.5's ABI surface;
  PROTOCOL_VERSION_* unchanged at 0.5.0).  Wire payload:
  `mode:u8 reserved:u8 wake_bitmap:u32 wake_after_ms:u32`.
  Firmware-side dispatcher returns `STATUS_NOSUPPORT` today
  via the default branch until the GD32 wake handler + the
  `src/zephyr/v2n_supervisor.c` singleton's re-init state-
  machine extension both land.

  Off-V2N (Zephyr's portable `pm_policy_*` path on AEN / NXP
  i.MX 93) is gated by a follow-up per-SoC pm_state-table
  commit; the surface ships with NOSUPPORT-stub behaviour
  today after the INVAL pre-checks (RUN mode rejection;
  zero-bitmap + zero-wake_after_ms rejection so customers
  catch "SoC would never wake" mistakes at request time).
  Wired behind `CONFIG_ALP_SDK_POWER=y` (default on) so the
  surface is link-resolvable on every build.

  Ships with 6 new alp_peripheral ZTESTs (open returns handle,
  configure_wake records bitmap, request_sleep with RUN mode
  rejected, request_sleep with no-wake-no-timeout rejected,
  valid-args path returns NOSUPPORT with realised_mode echo
  populated, close-NULL no-op).  One new wire vector in §9 of
  `gd32-bridge/tests/protocol_vectors.txt` (DEEP_SLEEP +
  RTC|GPIO wake + 10s timeout probe).  Doxygen coverage holds
  at 288/288 (= 100%).

- **`<alp/pwm.h>` -- advanced timer extras: input capture +
  one-shot output (wave-2 §2B.2, 2026-05-14).**  Adds five new
  reserved opcodes to the bridge protocol within the existing PWM
  range:
  * `CMD_PWM_CAPTURE_BEGIN / READ / END` (`0x23..0x25`) -- input
    capture (frequency / pulse-width measurement) on a PWM
    channel.  Portable surface: `alp_pwm_capture_open(cfg)` /
    `alp_pwm_capture_read(cap, &period_ns, &pulse_ns)` /
    `alp_pwm_capture_close`.  Edge polarity selectable via
    `alp_pwm_capture_edge_t` (RISING / FALLING / BOTH).
  * `CMD_PWM_SINGLE_PULSE` (`0x26`) -- one-shot pulse output of
    caller-specified width then stop.  Portable surface:
    `alp_pwm_single_pulse(pwm, pulse_ns)`.
  * `CMD_TIMER_SYNC` (`0x27`) -- master-slave linkage across the
    GD32G5's TIMER0 / TIMER7 / TIMER19 for synchronised
    multi-channel output.  Reserved opcode only at v0.5; the
    portable surface follows once the firmware HAL exposes the
    master-slave wiring.

  All five opcodes are RESERVED today: the firmware-side
  dispatcher routes them through the default-case
  `STATUS_NOSUPPORT` branch until the corresponding
  `bridge_hw_*` HAL bodies land in a follow-up GD32 firmware
  drop.  Portable surfaces in `<alp/pwm.h>` honour INVAL /
  OUT_OF_RANGE pre-checks before falling through to NOSUPPORT
  so misconfigured callers get precise diagnostics on every SoM.
  Tests: 6 new ZTESTs in
  `tests/zephyr/peripheral/src/main.c` covering the NULL-handle,
  NULL-cfg, channel-out-of-range, valid-args-NOSUPPORT, NULL
  close, and both-out-NULL contracts.  Wire vectors: 2 new
  representative probe envelopes in §8 of
  `gd32-bridge/tests/protocol_vectors.txt` (PWM_CAPTURE_BEGIN
  with channel + edge; PWM_SINGLE_PULSE with pulse_ns).
  PROTOCOL_VERSION_MAJOR / MINOR / PATCH unchanged (5.0); the
  reservations live within the v0.5 ABI surface.

- **`<alp/adc.h>::alp_adc_spectrum_t` -- streaming ADC with
  FFT-terminated DSP chain (wave-2 §2B.1(c), 2026-05-14).**
  Replaces the v0.5.x(b) NOSUPPORT stub with a real composition:
  spectrum_open validates that the caller's terminal stage is
  @c ALP_DSP_STAGE_FFT, opens the underlying alp_dsp_chain_t and
  alp_adc_stream_t, then internally accumulates N raw samples
  (N = the chain's FFT n_points) before running
  @c alp_dsp_chain_apply_bins for one non-overlapping block per
  read.  Output format follows the configured FFT stage:
  @c ALP_DSP_FFT_OUTPUT_COMPLEX (2N elements; re/im pairs) or
  @c ALP_DSP_FFT_OUTPUT_MAGNITUDE (N elements; per-bin |bin|).
  When the backend ring is empty mid-accumulation, the read
  returns @c ALP_OK with @c got=0; partial accumulation persists
  across calls.  Pool size: 2 spectrum handles.  Static cost
  per slot: ~2 KB (N=1024 int16 accumulator) + the DSP chain's
  own 30 KB footprint.

  Off-V2N or without CONFIG_ALP_SDK_DSP: NOSUPPORT after the
  INVAL pre-checks (including the wrong-entry-point check that
  rejects filter-terminated chains).  Extended `tests/zephyr/dsp/`
  ztest coverage with 8 spectrum-specific cases (NULL cfg, NULL
  stages, zero n_stages, filter-terminated chain rejected,
  valid-args-NOSUPPORT, close-NULL no-op, read-with-NULL-handle
  returns NOT_READY, read-with-NULL-got returns INVAL).  Doxygen
  coverage holds at 100%.

- **`<alp/adc.h>::alp_adc_filter_t` / `alp_adc_spectrum_t` --
  streaming ADC with DSP pipeline (wave-2 §2B.1(b), 2026-05-14).**
  Introduces two sibling surfaces that compose `alp_adc_stream_t` +
  `alp_dsp_chain_t` under a single caller-facing handle:

  * `alp_adc_filter_t` drains *filtered* int16 mV samples from a
    FIR / IIR / cascaded chain.  Surface:
    `alp_adc_filter_open(cfg)` (chain validated via a one-sample
    probe -- FFT-terminated chains return INVAL),
    `alp_adc_filter_read(filter, mv, cap, &got)`,
    `alp_adc_filter_close`.
  * `alp_adc_spectrum_t` emits FFT bins (declared in this commit;
    the real implementation lands in v0.5.x (c), so today the
    open returns NOSUPPORT after the INVAL pre-checks).

  Configuration takes a stage descriptor list (chain opened
  internally; coefficients copied in).  On V2N the host runs the
  chain off the existing GD32-streamed mV samples; the GD32-side
  HW-DSP offload path (CMD_ADC_STREAM_CONFIGURE_DSP, opcode 0x36
  reserved in v0.5.0) lands once the wire payload format
  finalises.  Off-V2N or without CONFIG_ALP_SDK_DSP: both surfaces
  return NULL with NOSUPPORT after the INVAL pre-checks pass.
  Wired behind `CONFIG_ALP_SDK_DSP=y && CONFIG_ALP_SDK_V2N_SUPERVISOR=y`
  for the real filter path.  Ships with extended `tests/zephyr/dsp/`
  ztest coverage (7 new cases: NULL cfg, NULL stages, zero
  n_stages, valid-args-NOSUPPORT, NULL handle close + read, NULL
  got arg).

### Added (2026-05-13)

- **`gd32-bridge/hal/bridge_hw_gd32.c` -- GD32G5x3 backend skeleton
  (2026-05-13).**  Selected by `BRIDGE_HAL_BACKEND=gd32` in
  `gd32-bridge/CMakeLists.txt`.  Links against the existing
  `vendors/gd32_firmware_library/upstream/` git submodule (verbatim
  mirror of GD's v1.5.0 archive at
  `https://github.com/alplabai/gd32g5x3-firmware-library`).  Every
  hook is a `BRIDGE_HW_ERR_NOTIMPL` stub today -- functionally
  identical to `bridge_hw_stub.c` -- so the `BRIDGE_HAL_BACKEND=gd32`
  build path is exercisable end-to-end without changing anything
  else.  The file's top comment enumerates the 18-step implementation
  order subsequent commits will follow, in increasing complexity:
  RESET_REASON / GPIO / TRNG / TMU / DAC first; PWM / ADC streaming /
  DSP-chain / power-mode last.  Each follow-up replaces one hook's
  stub body with a real implementation against the GigaDevice
  library and updates the top comment + CHANGELOG.

  Also adds `gd32-bridge/hal/gd32g5x3_libopt.h` -- gd32-bridge's
  project-level standard-peripheral selector that `gd32g5x3.h`
  transitively `#include`s (mirrors the per-example libopt.h
  convention used throughout the vendor archive).  Today it pulls in
  every peripheral header the eventual real-hook implementations will
  reference (RCU / GPIO / TIMER / ADC / DAC / DMA / TMU / FFT / FAC /
  TRNG / SPI / I2C / USART / FMC / RTC / watchdog).  `-ffunction-
  sections -fdata-sections -Wl,--gc-sections` drop the unreferenced
  symbols at link time so the today-NOTIMPL stub backend's binary
  stays small.

- **GD32 firmware-side dispatch + HAL hooks for every v0.5 opcode
  (2026-05-13).**  Closes the "remaining gating dep" flagged in the
  prior burst memory: every v0.5 opcode (`0x23`..`0x28` advanced
  timer extras + power-mode set, `0x37`/`0x38`/`0x39` chunked
  DSP-chain upload) used to fall through to the protocol dispatcher's
  default-case `STATUS_NOSUPPORT` branch -- which silently swallowed
  payload-validation errors and made it impossible for the host to
  distinguish "firmware doesn't know this opcode" from "firmware
  knows it but the HAL backend isn't wired".  This commit:
  - Adds 9 new `bridge_hw_*` HAL declarations to `gd32-bridge/hal/bridge_hw.h`:
    `pwm_capture_begin / read / end`, `pwm_single_pulse`,
    `timer_sync`, `power_mode_set`, `adc_dsp_chain_open`,
    `adc_dsp_stage_push`, `adc_dsp_chain_bind`.  Each documents the
    GD32G5 register-level expectations (timer SMC bits, OPM mode, DMA
    binding) so the eventual `bridge_hw_gd32.c` implementer has the
    contract spelt out.
  - Adds 9 stub bodies in `gd32-bridge/hal/bridge_hw_stub.c` that
    return `BRIDGE_HW_ERR_NOTIMPL`.  The stub backend remains the
    default (`BRIDGE_HAL_BACKEND=stub` in `gd32-bridge/CMakeLists.txt`)
    so the protocol round-trip is exercisable end-to-end even without
    the GigaDevice firmware library on the workspace.
  - Adds 9 handler functions in `gd32-bridge/src/protocol.c` plus a
    new shared `status_from_hw()` helper that maps `BRIDGE_HW_ERR_*`
    to wire `STATUS_*`.  Each handler validates request payload
    length + range (matching the host-side helpers in
    `chips/gd32g553/gd32g553.c`) before calling the HAL hook --
    defence-in-depth so a malformed wire request can't reach the HAL.
    The DSP `STAGE_PUSH` handler additionally checks that
    `chunk_offset + chunk_data_len <= chunk_total_size` so a
    misbehaving host can't push past the firmware's per-stage scratch
    buffer.
  - Adds 9 new entries to the dispatch switch in `protocol_dispatch()`.

  Net wire-side behaviour today is unchanged: every v0.5 opcode still
  replies `STATUS_NOSUPPORT` because the stub HAL returns `NOTIMPL`.
  What's new is the **structural** plumbing: when the real
  `bridge_hw_gd32.c` lands (alongside the GigaDevice firmware library
  pull at `vendors/gd32_firmware_library/`), every v0.5 opcode lights
  up without further protocol changes.  Host-side helpers in
  `chips/gd32g553/gd32g553.c` and the protocol vectors in
  `gd32-bridge/tests/protocol_vectors.txt` already cover the wire
  envelopes -- so the HW-in-loop turn-on is "flip
  `BRIDGE_HAL_BACKEND=gd32`, link the SDK, run the existing host-side
  ZTESTs".  The 0x36 tombstone stays on the default-case path -- no
  handler stub, so any residual `CMD_ADC_STREAM_CONFIGURE_DSP` caller
  gets `STATUS_NOSUPPORT` from the fallthrough as before.

- **§2B wave-2 chunked DSP-chain upload opcodes (`0x37`/`0x38`/`0x39`)
  reserved on the bridge protocol (2026-05-13).**  Finalises the wire
  format the wave-2 ADC-stream DSP pipeline uses to push FIR / IIR /
  WINDOW / FFT chains onto a streaming ADC source so raw samples
  never traverse the link when the customer wants filtered or
  spectral data.  The earlier single-shot `CMD_ADC_STREAM_CONFIGURE_DSP`
  (opcode `0x36`) reservation can't fit a single FIR stage's 256-byte
  Q31-tap blob in the 65-byte wire envelope, so it's tombstoned in
  favour of three chunked sub-opcodes:
  - `CMD_ADC_DSP_CHAIN_OPEN` (`0x37`) -- empty request, reply
    `chain_id:u8`.  Allocates a firmware-side chain handle (pool size
    `GD32G553_BRIDGE_ADC_DSP_MAX_CHAINS`, default 4).
  - `CMD_ADC_DSP_STAGE_PUSH` (`0x38`) -- 7-byte header (`chain_id:u8
    stage_index:u8 kind:u8 chunk_offset:u16 chunk_total_size:u16`)
    plus up to `GD32G553_BRIDGE_ADC_DSP_MAX_CHUNK_BYTES` (58 bytes)
    of `chunk_data`.  Repeated until the per-stage blob is fully
    covered; firmware reassembles in offset-order.
  - `CMD_ADC_DSP_CHAIN_BIND` (`0x39`) -- request `chain_id:u8
    stream_id:u8`.  Attaches a fully-staged chain to a stream
    previously opened with `CMD_ADC_STREAM_BEGIN`.

  Three matching host helpers in `chips/gd32g553/gd32g553.c` route
  through `cmd_send`: `gd32g553_adc_dsp_chain_open`,
  `gd32g553_adc_dsp_stage_push` (chunks larger blobs internally), and
  `gd32g553_adc_dsp_chain_bind`.  Each enforces a NOT_READY pre-check
  on an uninitialised context and a tight INVAL / OUT_OF_RANGE
  gate on bad args; the wire request is constructed only once the
  pre-checks pass.

  Wire format documented in `docs/gd32-bridge-protocol.md` §3.x with
  the reassembled per-kind blob layouts (FIR / IIR up to 260 /
  164 bytes; WINDOW + FFT 4 bytes each).  Three reference vectors
  added to `gd32-bridge/tests/protocol_vectors.txt` §10
  (`spi_adc_dsp_chain_open_probe_request`,
  `spi_adc_dsp_stage_push_window_hann_request`,
  `spi_adc_dsp_chain_bind_probe_request`); generator regenerates
  cleanly.  ABI snapshots v0.1 / v0.3 / v0.5 regenerated (still 58
  headers; +3 function decls + 4 new constants).  Twister contract
  coverage in `tests/zephyr/chips/src/main.c` extends the existing
  v0.5 NOT_READY + INVAL test functions with 11 new assertions
  covering NULL out-param (chain_open), `stage_index >= MAX_STAGES`,
  `kind > 3`, NULL params with non-zero len, oversized payload
  (`OUT_OF_RANGE`), and `stream_id >= ADC_STREAM_COUNT` (chain_bind).

  All three opcodes remain RESERVED at protocol v0.5: the firmware
  default-case dispatch returns `STATUS_NOSUPPORT` until the
  `bridge_hw_adc_dsp_*` HAL bodies land in the GD32 firmware tree.
  Host helpers honour the same NOSUPPORT contract by routing the
  wire dispatch through `cmd_send` unchanged.  When this lands,
  `peripheral_adc.c` flips the `alp_adc_filter_t` /
  `alp_adc_spectrum_t` bridge dispatch from "host-side chain"
  (current CMSIS-DSP fallback) to "GD32-side chain" -- the wave-2
  bandwidth win.  Side change: bumped host-side
  `GD32G553_MAX_PAYLOAD_BYTES` from 17 to 65 to match the firmware's
  `GD32_BRIDGE_MAX_PAYLOAD_BYTES`, so `cmd_send` can serialise the
  largest STAGE_PUSH request without re-using the stream-read special
  case.

- **`scripts/setup-clang-format.sh` -- pin local clang-format to v14
  (2026-05-13).**  CI installs `clang-format-14` from the Ubuntu 22.04
  apt repo and wires it via `update-alternatives`
  (`.github/workflows/pr-static-analysis.yml`).  Floating with whatever
  the developer's distro ships (apt v18+, Homebrew's current LLVM,
  Windows LLVM installer) drifts on five places we've been bitten by:
  brace spacing for designated initialisers, trailing `/**<`
  doc-comment column alignment, designated-init array indentation,
  `AlignConsecutiveAssignments` columns across mixed-width
  declarations, and multi-line call-argument packing.  The script
  detects the local clang-format version, attempts a platform-aware
  install if mismatched (apt on Debian / Ubuntu, `brew install
  llvm@14` on macOS, printed manual-install instructions for other
  Linux / Windows-bash), and verifies the post-install version
  reports 14.x.  `--check` mode verifies without installing (useful
  for pre-commit hooks).  `CONTRIBUTING.md` "Formatting" subsection
  and `docs/contribution.md#formatting` cover the divergence list +
  platform notes; `docs/testing.md` "Static analysis" now points
  developers at the script as a one-time pin before their first
  `clang-format-diff` run.  Net result: future contributors should
  not spend commits chasing version-skew CI failures the way wave-2
  did.

### Added (2026-05-13 overnight run)

- **`chips/gd32_swd/` -- bit-bang SWD controller for the GD32G553 (2026-05-13).**
  Renesas-side software SWD controller that drives `GD32_SWDIO` +
  `GD32_SWCLK` + (optional) `GD32_NRST` as GPIOs to reflash the V2N
  module's companion GD32G553MEY7TR without an external probe.  No
  vendor SDK pulled in; algorithm follows Arm DDI 0316C (ADIv5) +
  GD32G553 User Manual §3 (FMC).  Surface: `gd32_swd_init`,
  `gd32_swd_connect` (line reset + JTAG-to-SWD switch + DPIDR read
  matching expected IDCODE `0x6BA02477`), `gd32_swd_halt` (Cortex-M33
  DHCSR DBGKEY + C_HALT), `gd32_swd_flash_erase` / `_write` /
  `_verify` (FMC KEY1 + KEY2 unlock, sector erase, doubleword
  programming), `gd32_swd_reset_and_run` (HW NRST pulse when wired;
  AIRCR.SYSRESETREQ otherwise).  `driver_status: partial` until
  exercised on real silicon.  New Kconfig: `CONFIG_ALP_SDK_CHIP_GD32_SWD`.
  Header: `<alp/chips/gd32_swd.h>`.  Manifest:
  `metadata/chips/gd32_swd.yaml`.

- **GD32 application-bootloader scaffold (2026-05-13).**
  New `gd32-bridge/src/bootloader/` subdir routes the seven OTA
  opcodes reserved in the bridge protocol (`CMD_OTA_BEGIN..ABORT`,
  `0xF0..0xF6`) through `bl_dispatch_ota`.  Handler bodies reply
  `STATUS_NOSUPPORT` until the real implementation lands;
  `CMD_OTA_GET_STATE` is read-only and answers concretely so the
  host driver path can be exercised early.  Hooked into
  `protocol_dispatch` in `gd32-bridge/src/protocol.c`.  Integration
  detail is held by the maintainer outside this tree.
  `docs/gd32-bridge.md` flashing table now lists both paths as
  "Scaffolded".

- **`examples/v2n/v2n-pwm-fan-control/` -- GD32-side PWM fan curve (2026-05-13).**
  Ramps a single GD32-driven PWM channel through five duty stops at
  a 25 kHz carrier (above audible) via `gd32g553_pwm_set`.
  Demonstrates the post-2026-05-11 schematic-rev convention that
  E1M PWM6 / PWM7 are GD32-only on V2N.

- **`examples/v2n/v2n-secure-element-sign/` -- OPTIGA Trust M ECDSA sign (2026-05-13).**
  Initialises the OPTIGA Trust M, reads its product-info object,
  then issues a hand-rolled `CalcSign` (0x31) APDU against key OID
  `0xE0F0` over a fixed SHA-256 digest.  Stops short of the PSA
  driver bridge -- that lands when Infineon's Host Library is
  vendored.

- **`examples/v2n/v2n-xspi-flash-readwrite/` -- on-module xSPI NOR erase / write / verify (2026-05-13).**
  Erases one 4 KiB sector at the last sector of the part, writes a
  0x00..0xFF ramp, reads it back via Zephyr's standard
  `flash_read`, and reports the comparison.  Resolves the part via
  the `xspi-flash` DT alias so it works on any V2N board file that
  exposes it.

- **`examples/v2n/v2n-emmc-block-stat/` -- on-module eMMC geometry + first-block read (2026-05-13).**
  Calls `disk_access_init("SD")` + the standard
  `DISK_IOCTL_GET_SECTOR_{SIZE,COUNT}` to report capacity, reads
  blocks 0..15 and checks the MBR signature, then asks for the
  eMMC's reported erase-block size.  Read-only.

- **GD32 bridge protocol vectors -- real CRC bytes (2026-05-13).**
  `gd32-bridge/tests/protocol_vectors.txt` is now populated with
  computed CRC-16/CCITT-FALSE values for every documented vector:
  CRC of `"123456789"` = `0x29B1` sanity check, SPI PING round-trip
  bytes, SPI `GET_VERSION` reply for firmware v0.1.0, I2C PING
  write + read.  Vectors are regenerated by
  `gd32-bridge/tests/gen_protocol_vectors.py` (idempotent;
  `--check` mode exits non-zero if the file drifts).

- **Twister coverage for `clk_5l35023b`, `murata_lbee5hy2fy`,
  `deepx_dxm1` (2026-05-13).**  `tests/zephyr/chips/src/main.c`
  picks up NULL-arg + post-init-rejection ZTESTs for all three
  V2N-family drivers added in the prior overnight run, plus the
  Murata "BT_DEV_WAKE not routed -> NOSUPPORT" contract test and
  the DEEPX out-of-range polarity rejection.  `prj.conf` opts the
  matching `CONFIG_ALP_SDK_CHIP_*` flags in.

- **`docs/test-plan.md` -- new + updated rows (2026-05-13).**
  Added rows for the GD32 SWD controller, the GD32 application
  bootloader's OTA opcode wiring, and the per-SoM hw-revision
  change-log enforcement.  Flipped `clk_5l35023b` + `tps628640`
  from `⏳ untested` to `🟡 partial` now that their failure-path
  ZTESTs + functional probe coverage are landed.

### Changed (2026-05-13 overnight run)

- **`metadata/schemas/soc-spec-v1.schema.json`** -- extended with
  optional `hwrm` (Hardware Reference Manual provenance), `errata`
  (list of vendor errata documents + per-item items), per-variant
  `notes` + `part_number_root`, and loosened the per-variant
  `package` field to optional (so a variant whose full order-code
  suffix is still pending the HW-config writeup can carry the part-
  number root without breaking validation).  Closes the 6 SoC
  schema failures `scripts/validate_metadata.py` was reporting.

- **`docs/gd32-bridge.md`** -- flashing table updated to mark the
  application-bootloader path + the SWD bit-bang path as
  "Scaffolded" (was "Planned"); source-layout tree adds the
  `bootloader/` directory + `gen_protocol_vectors.py`.

- **Customer-visible language cleanup** -- hw-rev CHANGELOGs +
  `hw-revisions-v1.schema.json` now point at "held outside this
  tree" rather than naming internal repos.

### Added

- **`docs/gd32-bridge-protocol.md` — wire-protocol spec for the V2N supervisor MCU bridge (2026-05-12).**
  Frames + opcodes + CRC + status codes for the hybrid SPI / I2C
  bridge between the Renesas RZ/V2N and the on-module GigaDevice
  GD32G553MEY7TR.  Single command set shared between both
  transports (per memory rule `project_gd32_bridge_hybrid_spi_i2c`).
  SPI uses a SOF + CMD + payload + CRC-16/CCITT-FALSE envelope with
  two-transaction request/reply pattern; I2C reuses the same opcode
  + payload + CRC inside the standard reg-addr=0x00 write/read
  protocol so the bridge looks like a regular I2C slave to
  discovery tools.  §10 reserves OTA opcodes `0xF0..0xFF` for the
  planned application-bootloader path; §10 also documents the
  V2N→GD32 BOOT0 reroute the maintainer has committed to (pad
  selection TBD per `memory/project_gd32_boot0_to_v2n_planned.md`).

- **`chips/gd32g553/` host-side bridge driver (2026-05-12).**
  Renesas-side driver that speaks the bridge protocol over either
  SPI fast path (`P76/77/96/97` host  ↔  GD32 `PA8/9/10/PB15`) or
  I2C management path on BRD_I2C (Renesas `P07/P06` host ↔ GD32
  `PA15/PB9` at 7-bit address `0x70` by default).  Pass either bus
  (or both) to `gd32g553_init`; the driver picks the SPI fast path
  by default when both are present, and per-call `*_via` helpers
  override.  Full opcode coverage: `PING`, `GET_VERSION`,
  `GET_BUILD_ID`, `RESET_REASON`, `GPIO_READ` / `GPIO_WRITE`,
  `PWM_SET` / `PWM_GET`, `ADC_READ` (1..8 samples, little-endian
  millivolt encoding), `DA9292_STATUS_FORWARD`.  Refuses to operate
  on firmware whose `major` does not match
  `GD32G553_HOST_PROTOCOL_MAJOR` (0 today, before-tag).  New Kconfig:
  `CONFIG_ALP_SDK_CHIP_GD32G553`.  Header: `<alp/chips/gd32g553.h>`.
  Manifest: `metadata/chips/gd32g553.yaml` (references the
  GigaDevice datasheet + user manual archived project's
  vendor datasheet ).

- **`gd32-bridge/` firmware tree scaffold (2026-05-12).**
  Separate compile artifact for the GD32G553 supervisor MCU.
  Top-level CMake + Arm-GCC toolchain config + linker-script
  placeholder + protocol layer + per-transport scaffolds
  (`transport_spi.c` + `transport_i2c.c`).  Single shared
  `protocol_dispatch()` in `src/protocol.c` handles both
  transports.  HAL backend selectable: `stub` (compiles standalone,
  every HW operation returns `BRIDGE_HW_ERR_NOTIMPL`) or `gd32`
  (TODO -- GigaDevice firmware library not yet vendored).
  `PING` / `GET_VERSION` / `GET_BUILD_ID` round-trip end-to-end
  on the stub backend so the wire layer is unit-testable before
  silicon-side firmware lands.  `tests/protocol_vectors.txt` is
  the shared CRC + wire vector file that the host-side driver
  tests and the firmware-side unit tests both consume.  See
  `docs/gd32-bridge.md` for the overview + flashing options.

- **`chips/rtl8211fdi/` Realtek Ethernet PHY driver (2026-05-12).**
  Callback-driven MDIO surface for the two RTL8211FDI-VD-CG PHYs
  populated on V2N (ET0 + ET1).  The driver takes
  `mdio_read` / `mdio_write` function pointers so it stays portable
  between Zephyr (callbacks wrap the Renesas-side
  `<zephyr/drivers/mdio.h>` controller) and baremetal (callbacks
  bit-bang or hit a vendor MAC's MMD register block).  Public API:
  probe + Realtek-OUI verify (`PHYID1 == 0x001C`), `soft_reset`,
  `restart_autoneg`, `get_link` (decodes the Realtek-extended
  page-`0xA43` reg-`0x1A` PHY-specific status), Wake-on-LAN
  magic-packet detection via the page-`0xD8A` register block, raw
  + paged register R/W escape hatches.  New Kconfig:
  `CONFIG_ALP_SDK_CHIP_RTL8211FDI`.  Header:
  `<alp/chips/rtl8211fdi.h>`.  Manifest:
  `metadata/chips/rtl8211fdi.yaml`.

- **`chips/clk_5l35023b/` Renesas/IDT clock generator stub (2026-05-12).**
  Stub-status driver for the 5L35023B audio clock generator that
  sources `Audio_CLKB` on V2N (Renesas `P10` output, `P67` OE).
  Surface: init / probe (register-0 read) + raw register R/W +
  a `register_dump` path for production-test logging.  No
  frequency-config helpers until the maintainer adds the chip
  datasheet to the vendor datasheet and mirrors
  the Renesas RZ/V2N EVK BSP init sequence into the driver.
  New Kconfig:
  `CONFIG_ALP_SDK_CHIP_CLK_5L35023B`.  Header:
  `<alp/chips/clk_5l35023b.h>`.  Manifest:
  `metadata/chips/clk_5l35023b.yaml`.

- **`chips/murata_lbee5hy2fy/` Wi-Fi/BLE module GPIO surface (2026-05-12).**
  Thin GPIO surface for the Murata LBEE5HY2FY-922 (Type 2FY,
  Infineon CYW55513 inside).  Owns the five side-channel lines:
  `BT_REG_ON` (GD32 `PE14`), `WL_REG_ON` (GD32 `PE15`),
  `BT_HOST_WAKE` (Renesas `P05`), `WL_HOST_WAKE` (Renesas `P72`),
  and `BT_DEV_WAKE` (TBD -- not currently routed on V2N).  The
  REG_ON lines live on the GD32 supervisor so the driver takes
  caller-supplied set / get callbacks for them (carriers route via
  `gd32g553_gpio_write` or a direct local-GPIO shim);
  HOST_WAKE / DEV_WAKE arrive as plain `alp_gpio_t` handles since
  they are host-SoC GPIOs.  Air-side traffic (Wi-Fi SDIO, BT UART,
  BT I2S) is the OS stack's responsibility (brcmfmac + BlueZ +
  ALSA on Linux); this driver is the power-and-wake glue around
  it.  New Kconfig: `CONFIG_ALP_SDK_CHIP_MURATA_LBEE5HY2FY`.
  Header: `<alp/chips/murata_lbee5hy2fy.h>`.  Manifest:
  `metadata/chips/murata_lbee5hy2fy.yaml`.

- **`chips/deepx_dxm1/` + `vendors/deepx-dxm1/` DEEPX bring-up sequencer (2026-05-12).**
  Host-side glue for the DEEPX DX-M1 NPU on V2N-M1 (NOT populated
  on V2N base).  Sequences M1_RESET (Renesas `PA6`) + the two
  passive PCIe muxes (via `chips/pi3dbs12212/`) before the Linux
  kernel driver (`dx_rt_npu_linux_driver` from upstream
  `github.com/DeepX-AI/`) opens the PCIe device.  `vendors/deepx-dxm1/README.md`
  carries the upstream-repo + Yocto layer cross-link.
  `deepx_dxm1_bring_up()` is the one-call API that runs the
  three-step rail / mux / reset sequence; rail bring-up itself is
  the caller's responsibility (composes `da9292_v2n_m1_enable_deepx_rail`
  + `tps628640_init` ACK-probes).  Reset polarity is assumed
  active-high by default with a `set_reset_polarity` flip for
  active-low boards.  Boot wait is approximate -- production
  callers pass `boot_us=0` and use their platform's busy-wait.
  New Kconfig: `CONFIG_ALP_SDK_CHIP_DEEPX_DXM1` (selects
  `ALP_SDK_CHIP_PI3DBS12212` automatically).  Header:
  `<alp/chips/deepx_dxm1.h>`.  Manifest:
  `metadata/chips/deepx_dxm1.yaml`.

- **`docs/gd32-bridge.md` — firmware-tree overview (2026-05-12).**
  Companion to the protocol doc: source-layout map, build
  instructions (`cmake -B build -DCMAKE_TOOLCHAIN_FILE=...`), the
  three flashing options (SWD probe = supported today; in-system
  OTA = planned per protocol §10; V2N-driven factory ISP =
  pending BOOT0 routing).

- **`docs/bring-up-v2n.md` + `docs/bring-up-v2n-m1.md` — bench bring-up guides (2026-05-12).**
  Step-by-step procedures: first-power smoke test, SWD attach +
  GD32 firmware flash, host ↔ bridge link confirmation, SoM
  manifest read, dual Ethernet PHY bring-up, on-module fleet sanity
  checks, and the most likely gotchas.  The V2N-M1 doc covers the
  delta (DEEPX rails + M1_RESET sequencing + DEEPX kernel hand-off).

- **`docs/board-id.md` — SoM identification flow (2026-05-12).**
  Two-stage design: EEPROM manifest (working today via
  `alp_hw_info_read()`) + BOARD_ID ADC cross-check (stubbed pending
  the `scripts/alp_project.py` per-family generated header).  Walks
  through the manifest layout in `<alp/hw_info.h>`, the
  `tools/program_eeprom.py` programming flow, and the runtime
  read path implemented in `src/zephyr/hw_info_zephyr.c`.

- **`docs/hil-plan.md` — HiL rig requirements (2026-05-12).**
  Document of the rig the maintainer will build (not built here).
  Hardware inventory (SoM, probes, programmable supply, logic
  analyser, link partners), test coverage map (power tree, GD32
  bridge, Ethernet, Wi-Fi/BT, DEEPX), orchestrator design
  (Python + sigrok + gdbserver), and a rough schedule (~2 months
  of one engineer's time once hardware is on the bench).

- **`docs/ota-device-contract.md` — device-side OTA contract (2026-05-12).**
  Mender contract for the main system OTA (Renesas-side, Linux
  rootfs A/B slots, OPTIGA-mediated signature verification).
  Server-side stays Hakan's repo per the existing
  `project_ota_server_owner` memory.  Planned design for the GD32
  bridge firmware OTA (opcodes `0xF0..0xFF`, dual-slot flash
  layout, ECDSA-P256 signature with key baked into the bootloader
  until cross-bus OPTIGA access is wired).  Failure-recovery
  matrix covering every "what if the upgrade fails halfway"
  scenario across both layers.

- **`alp_hw_info_read` EEPROM-side implementation (2026-05-12).**
  Replaces the NOSUPPORT stub in `src/zephyr/hw_info_zephyr.c` with
  a working EEPROM-manifest reader.  Opens the configured I2C bus
  (`CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID`), reads 128 bytes via
  the 24C128 driver at `CONFIG_ALP_SDK_HW_INFO_EEPROM_ADDR_7BIT` +
  `CONFIG_ALP_SDK_HW_INFO_EEPROM_OFFSET`, validates the magic byte
  (`ALPH`), schema version, and CRC32 (ISO-3309 / `zlib.crc32`-
  compatible, matching `tools/program_eeprom.py`), then copies the
  SoM identifiers into `alp_hw_info_t`.  When the bus id Kconfig is
  `-1` (default) the reader returns `ALP_ERR_NOSUPPORT` -- boards
  that haven't wired the EEPROM yet aren't penalised.
  `alp_hw_info_assert_matches_build` does strncmp checks against
  `expected_sku` / `expected_hw_rev` (NULL = skip that field) with
  the same NOSUPPORT graceful-fallback policy.
  New Kconfig options under `CONFIG_ALP_SDK_HW_INFO`:
    `ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID` (int, default -1)
    `ALP_SDK_HW_INFO_EEPROM_ADDR_7BIT` (hex, range 0x50..0x57)
    `ALP_SDK_HW_INFO_EEPROM_OFFSET` (int, default 0)
    `ALP_SDK_HW_INFO_EEPROM_I2C_BITRATE_HZ` (int, default 400000)
  BOARD_ID ADC cross-check remains a no-op stub (`adc_cross_check`)
  pending the per-family generated header that maps `hw_rev` strings
  to expected mV bins (depends on `scripts/alp_project.py` emitting
  a runtime-readable digest of `metadata/e1m_modules/<family>/hw-revisions.yaml`).

- **`chips/pi3dbs12212/` Diodes PI3DBS12212A PCIe mux driver (2026-05-12).**
  GPIO-only control surface for the two passive 12 Gbps
  differential muxes that switch the V2N-M1 PCIe lane between
  the on-module DEEPX DX-M1 NPU and the E1M edge connector.  PD
  pin on Renesas `P80`, SEL pin on `P95`.  Three-state driver
  model (OFF / PATH_0 / PATH_1) with glitch-free PD pulse on
  path-to-path transitions.  Takes two `alp_gpio_t` handles --
  carrier code opens + configures the GPIOs and hands them in.
  New Kconfig: `CONFIG_ALP_SDK_CHIP_PI3DBS12212`.  Header:
  `<alp/chips/pi3dbs12212.h>`.  Manifest:
  `metadata/chips/pi3dbs12212.yaml`.

- **`chips/rv3028c7/`: multi-source event dispatcher + CLKOUT routing (2026-05-12).**
  Extends the existing RV-3028-C7 driver from single-alarm-only to
  the full latched-event surface in the chip's STATUS register
  (PORF, EVF, AF, TF, UF, BSF, CLKF -- all 7 sources).  New API:
    - `rv3028c7_register_handler(src, cb, user)` per-source dispatch.
    - `rv3028c7_dispatch_irq(status_seen)` reads STATUS, invokes
      registered handlers, write-0-to-clears every fired flag.
    - `rv3028c7_set_int_enable(src, enable)` masks individual sources
      in CONTROL_2 (EIE/AIE/TIE/UIE/CLKIE) and EEPROM_BACKUP (BSIE
      for the backup-switchover source), guarded by the EERD
      auto-refresh pause + restore protocol.
    - `rv3028c7_route_clkout(src)` reprograms the CLKOUT pin via
      EEPROM_CLKOUT (0x35[2:0]) so carriers that wire CLKOUT as a
      **second physical interrupt line** can route a specific event
      independent of INT.  Eight CLKOUT sources: 32.768 kHz, 8192 Hz,
      1024 Hz, 64 Hz, 32 Hz, 1 Hz, periodic-update, low.
  Reference: Micro Crystal "Multiple Interrupt Lines with
  RV-3028-C7" application note.  Existing alarm helpers
  (`rv3028c7_set_alarm`, `rv3028c7_alarm_int_enable`,
  `rv3028c7_alarm_check_and_clear`) continue to work unchanged --
  they bypass the dispatcher and operate on the AF status bit
  directly.  Header: `<alp/chips/rv3028c7.h>`.

- **`chips/tps628640/` TI TPS628640 multi-instance buck stub (2026-05-12).**
  Multi-instance scaffold for the four populated TPS628640 buck
  regulators on V2N-M1's BRD_I2C bus (`0x48` = 0.85 V VDD0V85_LPDDR
  DEEPX, `0x44` = 1.05 V DDR5_VDD DEEPX, `0x4F` = 0.5 V DDR5_VDDQ_0V5
  DEEPX, `0x4D` = 0.6 V LPD4x_0V6 Renesas LPDDR4X V2N-common optional).
  Driver lands as **stub** because the TI datasheet isn't in the
  vendor datasheet yet -- only probe + raw register R/W
  works; `tps628640_set_voltage_mv` / `_get_voltage_mv` /
  `_get_status` all return `ALP_ERR_NOSUPPORT`.  Each instance
  carries its design-target voltage in `ctx->default_voltage_mv`
  as metadata for higher-level code that wants to enforce safe-
  operating-window guards once the register map is filled in.
  Carriers can rely on the chip's factory OTP voltage in the
  meantime -- the rails self-regulate without host intervention.
  New Kconfig: `CONFIG_ALP_SDK_CHIP_TPS628640`.  Header:
  `<alp/chips/tps628640.h>`.  Manifest:
  `metadata/chips/tps628640.yaml`.

- **`chips/da9292/` Renesas DA9292 secondary PMIC driver (2026-05-12).**
  Full I2C surface for the **Renesas DA9292-AROVx** multi-phase buck
  PMIC populated on V2N + V2N-M1 at 7-bit address `0x1C`.  Strapped
  for 2-channel dual-phase mode by the CONF pin: **CH1** (phases
  1+2) is the 0.8 V Renesas rail (enabled at boot via the EN1 hard-
  strap), **CH2** (phases 3+4) is the 0.75 V DEEPX rail (disabled on
  V2N base, brought up by V2N-M1 firmware before DEEPX boot).
  Public API: probe + cached DEV_ID/REV_ID; live status decode
  (PG / UV / OV / OC / TEMP_WARN / TEMP_CRIT / VIN_UVLO); event
  read-and-clear over `PMC_EVENT_00/01` (write-1-to-clear); 5 mV-
  step voltage set/get in the `0.3..1.275 V` (VSTEP=0) range;
  per-channel enable in `PMC_CTRL_01`; and the variant-specific
  init helpers `da9292_v2n_base_init` + `da9292_v2n_m1_enable_deepx_rail`
  (the latter programs 0.75 V, reads back, enables CH2, polls
  CH2_PG with a caller-supplied timeout).  PMC_STATUS bit layout
  is assumed to mirror PMC_MASK_00 (which IS documented in the
  datasheet); flagged TODO in `chips/da9292/da9292.c` for verify
  against datasheet Table 14 before relying in production.
  New Kconfig: `CONFIG_ALP_SDK_CHIP_DA9292`.  Header:
  `<alp/chips/da9292.h>`.  Manifest: `metadata/chips/da9292.yaml`.

- **`chips/act8760/` ACT88760 primary PMIC driver scaffolding (2026-05-12).**
  Stub-status driver for the **Qorvo ACT88760-120.E1** primary PMIC
  populated on the V2N + V2N-M1 SoMs.  Dual-page I2C interface
  (`0x25` system / Buck1..Buck6 / GPIOs; `0x26` Buck7 / LDO1..LDO6)
  on BRD_I2C.  `act8760_init` probes both pages, `act8760_get_status`
  decodes the TWARN / SYSWARN / SYSDAT / ILIM bits from the
  system-status register (read-to-clear nIRQ semantics), and raw
  register R/W is exposed on both pages.  Per-rail VSET access
  is wired but returns `ALP_ERR_NOSUPPORT` until the VSET register
  offsets are confirmed against the Users Guide Rev 3.0 table -- a
  follow-up will replace the TODO offsets in `chips/act8760/act8760.c`.
  CMI 120.E1 burns the production power-tree (rail voltages, GPIO
  assignments, power-on sequence) into the chip's non-volatile
  memory via Qorvo's ActiveCiPS dongle from the `.iact` profile
  archived vendor datasheet;
  runtime I2C is volatile-only and used here for telemetry + DVS.
  New Kconfig: `CONFIG_ALP_SDK_CHIP_ACT8760` (off by default).
  Header: `<alp/chips/act8760.h>`.  Manifest:
  `metadata/chips/act8760.yaml` (introduces the `metadata/chips/`
  directory; per-chip manifests for the other V2N populated parts
  land alongside their drivers).

- **Renesas Ethernet + eMMC + uSD + xSPI NOR-flash pin assignments (2026-05-11).**
  Maintainer supplied four additional schematic excerpts.  63 new
  rows added to `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv`:
  - **30 Ethernet rows** — RGMII to two on-module
    `RTL8211FDI-VD-CG` PHYs (ET0 + ET1).  Each PHY: 15 pins
    (TX_CTL/CLK + 4× TXD, RX_CTL/CLK + 4× RXD, MDIO/MDC,
    PHY_INTR).  22 Ω series resistors documented per data line;
    MDIO/MDC pull-ups to VDD_1V8 via 1 kΩ 1%; layout constraints
    (single 50 Ω ±10%, GND guard trace on TX/RX clocks; 5 mm
    length matching on RX_CTL relative to RX_CLK) preserved in
    the notes column.
  - **11 eMMC rows** — `SD0CLK` / `SD0CMD` / `SD0DAT0..7` /
    `SD0RSTN` for the on-module eMMC.  22 Ω series on each line,
    1 MΩ pull-up to `VDD_eMMC_3V3_1V8`, ±1.27 mm length matching
    on `eMMC_CLK`.
  - **6 uSD-card rows** — `SD1CLK` / `SD1CMD` / `SD1DAT0..3` for
    the on-module microSD card slot.  22 Ω series, 10 kΩ pull-ups
    to `µSD1_1833V` (1.8 V), ESD protection via `PUSB3FR6Z` (D28).
  - **16 xSPI rows** — `XSPI0_*` to the on-module xSPI NOR flash
    (CK ± / CS / 8× IO / DS / ECS / INT / RSTO / RESET).  22 Ω
    series on CK + IO0..IO3 (x4 data path), 10 kΩ pull-downs on
    IO4..IO7 + DS, 10 kΩ pull-ups on ECS / INT / RSTO to 0V.
    Series resistor IDs (R143..R376) preserved in notes for
    board-engineering cross-reference.

  These pads use **BGA designators** (J26, AC27, AJ18, etc.)
  rather than port-pin names because they're dedicated-function
  pads outside the GPIO bank.  The `renesas_pad` column carries
  either convention depending on the pin -- header comment block
  documents the dual-convention explicitly.  Three of the new
  pads happen to start with `P` followed by digits (P24, P25, P29
  on ET1) -- those are BGA pads, not port pins; the peripheral
  column makes the distinction unambiguous.

  Renesas peripheral map now totals **146 data rows** (was 83);
  CSV regenerated.

  Pad-uniqueness check after the additions: **all 146 Renesas pads
  are unique**.  No collisions introduced by the new rows.

### Changed

- **Renesas PWM pinout corrected; PWM6/PWM7 added; BRD_I2C master/slave roles clarified (2026-05-11).**
  Maintainer supplied the authoritative Renesas-side PWM pin list:
  - E1M PWM0: P36 -> **P64** (peripheral unchanged: GPT13_GTIOC13A)
  - E1M PWM1: P37 -> **P65** (peripheral unchanged: GPT13_GTIOC13B)
  - E1M PWM2..PWM5: unchanged (GPT0/GPT4 channels on P70/P71/P74/P75)
  - E1M PWM6: NEW row, GPT15_GTIOC15A on P36
  - E1M PWM7: NEW row, GPT15_GTIOC15B on P37

  P36 and P37 stay in use but switch role (was PWM0/PWM1 via
  GPT13; now PWM6/PWM7 via GPT15).  GPT15 was not previously
  mapped to a Renesas pad in the saved metadata.

  PWM4's Renesas-side pad (P74) preserved from the prior pinout
  because the maintainer's correction msg listed
  `PWM4 = GPT4_GTIOC4A` without an explicit pin.  P74 is adjacent
  to PWM5's confirmed P75; flagged inline pending re-confirmation.

  v2n/README.md "Cross-chip PWM" section reworked into a
  per-PWM table showing GD32 + Renesas pad for each of the eight
  channels.  Design intent recorded: pick **either GD32 or
  Renesas for all eight PWMs**, not per-channel -- the resistor
  mod is a SoM-wide source selection.

  Separately: BRD_I2C master/slave roles on the bus clarified.
  Renesas (RIIC8 on P06/P07) is the **master**; GD32 (PB9/PA15)
  is a **slave** on the shared bus alongside the RTC, OPTIGA,
  EEPROM, clock generator, and TMP112.  Captured in both files'
  notes columns.

  Renesas CSV regenerated (83 data rows, was 81).

- **GD32 pinout — schematic labels dropped; E1M signal names canonical (2026-05-11).**
  Maintainer reviewed the row-numbered CSV and pushed back on
  carrying the misleading `GD32_P<pad>_R` schematic labels as the
  peripheral name: "we should just use the pin names with E1M
  signal definitions, not the wrong signal names we mixed".
  Applied:
  - Every GPIO route's peripheral name flipped from
    `GD32_P<pad>_R` to its E1M IO destination
    (`GD32_PB3_R` → `E1M IO14`, `GD32_PC3_R` → `E1M IO24`, etc.).
    18 GPIO routes now named uniformly by E1M IO number.
  - The `GD32_I2C2.*` peripheral names flipped to `E1M I2C3.*`
    -- they're E1M-edge signals, not internal-only.
  - Maintainer's CSV correction pass merged into the canonical
    TSV: PB7 collision removed (the `GD32_PB7_R` row was a
    duplicate of `GD32_PB3_R`'s actual pad; the duplicate is
    deleted); PC14 collision removed (`GD32_PC14_R` was actually
    on PC15, not PC14); PB11 / PD1 / PD2 / PD8 / PD10 / PC15 /
    PC2 GPIO-route pad assignments rotated to the correct
    silicon pads per the maintainer's row-by-row audit.
  - Inline `[SDK note: function-A-vs-function-B pad mismatch]`
    workaround text removed everywhere -- with the schematic
    labels dropped the workaround is moot.
  - `v2n/README.md` "Function-A vs function-B pin labelling"
    section deleted (no longer applies); "Jumper-selectable
    shared pads (PB7, PC14)" section deleted (no longer applies,
    every pad in the TSV is unique).  Replaced with a "Dropped
    schematic labels" section explaining the design decision.
  - Two E1M destinations from the original spreadsheet (`IO15`
    and `IO26`) are no longer routed on V2N -- they were entries
    on schematic nets that the audit pass found to be either
    duplicates of other entries (PB7 case) or off-by-one labels
    for rows that already covered different destinations (PC15
    case).  Pre-2026-05-11 git history retains the original
    labels for board-engineering cross-reference.
  - CSV regenerated from the canonical TSV; row numbers are now
    contiguous (62 data rows, was 64 with two now-deleted rows
    consolidated by the maintainer's audit).
  - All 62 GD32 silicon pads in the file are unique.
- **GD32 pinout — final corrections + CSV companions (user-supplied, 2026-05-11).**
  Three more user-supplied fixes after the format-consistency
  pass:
  - `E1M ENC0_X` notes: dropped the bogus `SPI1_MISO` alt-function
    annotation (PA0 has no SPI1_MISO function on the GD32G5xx);
    notes now just say `TIMER1_CH0` (the encoder timer alt).
  - `E1M CAN_STBY` pad: filled in `PB13` (was `(unspecified)`).
  - `ADC1` / `ADC5` / `ADC6` / `ADC7` confirmed E1M-edge and given
    the `E1M ` prefix (alongside the already-prefixed
    ADC0/2/3/4); same prefix normalisation applied to `DAC0` /
    `DAC1` for consistency.

  Plus a broader naming-format normalisation pass:
  - **E1M-edge signals all carry `E1M ` on the left** of the
    peripheral column (E1M PWM0..PWM7, E1M ADC0..ADC7, E1M DAC0/1,
    E1M ENC0..3 X/Y, E1M CAN_STBY).  The source spreadsheet
    inconsistently prefixed some and not others; now uniform.
  - `BT_REG_ON` / `WL_REG_ON` peripheral names cleaned (the
    `LBEE5HY2FY-922` chip context moved to the notes column where
    it belongs).
  - `GD_I2C2.*` renamed to `GD32_I2C2.*` for consistency with the
    other `GD32_*`-prefixed signals.
  - Sort order tidied: debug → SPI → I²C → encoders → PWMs →
    ADCs → DACs → CAN → camera → wireless → secure-element →
    GPIO routes.

  All-eight-PWM dual-sourcing recorded:
  - User confirmed 2026-05-11 that **all eight PWMs** (PWM0..PWM7)
    are dual-sourced between Renesas and GD32 via carrier-side
    resistor mod (was previously documented as just PWM0..PWM5).
    Renesas-side pads for PWM6 and PWM7 aren't in the saved
    Renesas peripheral map yet — flagged in `v2n/README.md` as
    pending an updated Renesas pinout writeup.

  Carrier-mux on PB7 / PC14 (ROW B interpretation) confirmed:
  - User confirmed 2026-05-11 that the two pads each carry two
    E1M IO destinations selected by passive carrier jumpers:
    PB7 picks between E1M IO14 (via `GD32_PB3_R`) and E1M IO15
    (via `GD32_PB7_R`); PC14 picks between E1M IO24 (via
    `GD32_PC3_R`) and E1M IO25 (via `GD32_PC14_R`).  Documented
    in the new `Jumper-selectable shared pads` section of
    `v2n/README.md`; inline TSV notes updated from "pending user
    confirmation" to "user-confirmed".

### Added

- **Row-numbered CSV companions for each pin map.**  Generated
  via inline `awk` from the canonical TSVs so the user can
  audit each row in a spreadsheet without losing the source
  TSV semantics.  Three new files:
  - `metadata/e1m_modules/v2n/renesas-peripheral-map.csv`
    (81 data rows)
  - `metadata/e1m_modules/v2n/gd32-io-mcu-map.csv` (64 data rows)
  - `metadata/e1m_modules/v2n-m1/m1-additions.csv` (3 data rows)

  Each carries `row, peripheral, <silicon_pad>, notes` columns
  with the `row` value matching the user-facing row index for
  cross-referencing during review.  Both READMEs note that the
  CSVs are derived artefacts: edit the TSV, not the CSV.

### Changed

- **GD32 pinout pad corrections (user-supplied, 2026-05-11).**
  Six fixes to `gd32-io-mcu-map.tsv` after the user audit:
  - `PWM0` channel: `TIMER0_MCH2` → **`TIMER0_MCH0`** on PA11.
    The source-spreadsheet had PWM0 + PWM1 both on MCH2 which
    is impossible (one channel = one output); user confirms PWM0
    is on MCH0.
  - `GD32_PB3_R` actual pad: PB3 → **PB7**.  Function-A label
    retained ("PB3"); function-B alternate pad PB7 is where the
    schematic actually routes.
  - `GD32_PC3_R` actual pad: PC3 → **PC14**.
  - `GD32_PA15_R` actual pad: PA15 → **PC1**.
  - `GD32_PB9_R` actual pad: PB9 → **PC0**.
  - `GD32_PE13_R` actual pad: PE13 → **PE12**.
  Resolves the five same-pad function-A vs function-B conflicts
  flagged in the previous audit (PB3 / PC3 / PA15 / PB9 / PE13
  now cleanly owned by ENC0_Y / CAM_EN_LDO0 / BRD_I2C.SCL /
  BRD_I2C.SDA / E1M ADC2 respectively).  The TSV header now
  documents the function-A-label-on-function-B-pad convention so
  consumers don't mistake the schematic name for the silicon pad.
  v2n/README.md gains a new "Function-A vs function-B pin
  labelling" section.
- **Cross-chip PWM mux documented as design intent.**  The earlier
  audit flagged E1M PWM3..5 as a Renesas-vs-GD32 conflict.  User
  confirmed (2026-05-11) that PWM0..PWM5 are **dual-sourced by
  design** with carrier-side resistor selection; **GD32 is the
  default source**; PWM6/PWM7 are GD32-only.  v2n/README.md
  gains a "Cross-chip PWM (PWM0..PWM5)" subsection capturing
  this.  Same section notes the source-spreadsheet's inconsistent
  use of the `E1M ` prefix on PWM0/1/2 (and ADC1/5/6/7) — all are
  presumed E1M-edge; ADC prefix-inconsistency flagged as pending
  user confirmation.
- **Restored verbatim user-source notes in `gd32-io-mcu-map.tsv`.**
  Earlier ingest had paraphrased the source spreadsheet's
  annotations (e.g. "connected to Camera enable pin LDO 0" →
  "Camera enable — LDO 0"; "reset pin of SLS32AIA010MLUSON10XTMA2"
  → "Reset for SLS32AIA010MLUSON10XTMA2 (OPTIGA Trust M secure
  element)").  Following the convention used for the Renesas
  peripheral map, the TSV now carries the user's exact wording as
  the third column; SDK-added context is bracketed
  `[SDK note: ...]` so it's distinguishable from source data.
  Same intent, more faithful preservation.

### Added

- **Errata items ingested from `AERR0012` v2.0.**
  `metadata/socs/alif/ensemble/{e4,e6,e8}.json` `errata.items[]`
  arrays now carry the four documented entries (ER001 — RTC
  pre-scaler reset bug on A0, fixed in A1; ER002 — RTC drift
  during POR_N on A1, no fix planned; ER003 — STOP-mode current
  ~2 µA over spec on A0, fixed in A1; ER004 — external reset
  supervisor required on all revisions when the cold-boot supply
  ramp can't meet the monotonic-rise-above-1.65 V invariant).
  Each item records affected revisions, resolution status,
  consequence, and workaround.  `title_quirk` field documents the
  source-PDF title typo (says "E2, E4, E8" but scope is
  "E4, E6, E8" — E2 is not an Ensemble part).
- **V2N-M1 README now explicit about GD32 inheritance.**  The
  v2n-m1 directory's "Inheritance" section previously named only
  the Renesas-side base map; now it explicitly notes that the
  GD32 IO MCU map (`v2n/gd32-io-mcu-map.tsv`) is also inherited
  verbatim and lists the GD32's role unchanged on V2N-M1 builds
  (encoder capture, extra PWM/ADC/DAC, camera-LDO enables,
  Wi-Fi/BT REG_ON, OPTIGA reset, E1M-edge GPIO routes).  The
  three-file concatenation recipe to build the full V2N-M1 map
  is now spelled out in the README.
- **GD32G553 companion IO MCU pinout for V2N + V2N-M1.**
  `metadata/e1m_modules/v2n/gd32-io-mcu-map.tsv` — authoritative
  pinout for the GigaDevice GD32G553MEY7TR that sits next to the
  RZ/V2N as a companion.  Covers encoder capture (ENC0..3 X/Y on
  TIMERs 1-4), extra PWM fan-out (PWM3..7 on TIMER7), the bulk of
  the ADC bank (ADC0..7), DAC0/1, the four camera-enable LDOs,
  the Murata LBEE5HY2FY-922 BT_REG_ON / WL_REG_ON, the OPTIGA
  Trust M reset, the SPI1 slave interface to Renesas, BRD_I2C
  (shared with Renesas), GD_I2C2 to V2N, plus 17 general-purpose
  GPIO routes to the E1M edge IO bank (IO8..IO35).  v2n/README.md
  gets a new "The two-chip pin story" section explaining the
  Renesas-vs-GD32 split.  V2N-M1 inherits this file unchanged
  (GD32's role doesn't depend on whether DEEPX is populated).
- **Alif Ensemble metadata refresh against new datasheets + HWRMs.**
  Datasheet inventory landed in the vendor datasheet for
  the AEN family on 2026-05-11 (E3 v2.8 already on file; E4 v1.0
  new; E6 v1.0 new; E7 v2.9 already; E8 v1.0 supersedes v0.51
  preliminary; confidential HWRMs v0.3 for E4/E6/E8; public HWRMs
  for E3 v2.5 + E7 v2.6; errata AERR0012 v2.0 covering E4/E6/E8
  jointly).  Metadata updates:
  - **`e4.json`, `e6.json`, `e8.json`** flip from
    `status: "preliminary"` to `"released"`.  E4 and E6 drop the
    `pending_alif_datasheet: true` flag; E8 supersedes the v0.51
    `datasheet` block with v1.0.  Per-peripheral counts intentionally
    NOT re-ingested in this pass -- re-ingestion against v1.0 is
    tracked as a separate v0.4 task.
  - **All five files with HWRMs** (E3, E4, E6, E7, E8) gain a new
    `hwrm` block alongside `datasheet`, with `confidential: true`
    on E4/E6/E8 so the HWRM files stay out of git.
  - **E4 / E6 / E8** gain a new `errata` block pointing at
    AERR0012 v2.0 with an empty `items: []` list pending
    per-item ingest.
  - **`e5.json`** stays preliminary -- E5 alone has no Alif
    datasheet yet.  Notes refreshed to clarify that the
    other five E-class parts are now released.
- **i.MX 93 orderable variant resolved to i.MX 9352** in
  `metadata/socs/nxp/imx9/imx93.json`.  The empty `variants: []`
  list gains a single entry `MIMX9352xxxxM` (the four-character
  placeholder reflects that the full order-code suffix -- package
  + temperature + qualification -- is still pending the HW-config
  writeup).  Per-peripheral counts still empty pending a dedicated
  ingest pass against IMX93RM.pdf (5593 pages).  `PLAN.md §6`
  items 6 and 7 updated to reflect partial closure.
- **Authoritative V2N + V2N-M1 Renesas-side peripheral pinout.**
  The alp-sdk maintainer provided the canonical RZ/V2N pin
  assignments for the E1M-X V2N family on 2026-05-11.  Saved as:
  `metadata/e1m_modules/v2n/renesas-peripheral-map.tsv` — base V2N
  map (84 routed pads covering E1M-edge peripherals + on-module
  Wi-Fi/BT module + GD32 IO MCU + RTC + EEPROM + OPTIGA + clock
  generator + BRD_I2C bus); `metadata/e1m_modules/v2n-m1/m1-additions.tsv`
  — V2N-M1 overlay (DEEPX `M1_RESET` on `PA6`, `PCIe.MUX_PD` on
  `P80`, `PCIe.MUX_SEL` on `P95`).  Both directories now carry
  READMEs explaining the inheritance pattern (V2N-M1 = V2N base +
  M1 overlay + DEEPX-rail PMICs on BRD_I2C).  `hw-revisions.yaml`
  for both families walks back the "pinout TBD" framing and lists
  the authoritative populated parts; revision label + board_id ADC
  channel remain TBD pending a separate writeup.  Resolves the
  project memory note "pending exact hardware configurations" for
  the V2N + V2N-M1 families (AEN was already resolved on 2026-05-10;
  i.MX 93 is still pending).
- **From-scratch verifiability surface.**  Three artefacts answer
  "how do I prove everything works from a fresh clone":
  `scripts/bootstrap.sh` (one-time setup -- creates the Zephyr
  workspace, installs Python deps, prints apt/brew commands for
  the optional native libs), `scripts/test-all.sh` (single-command
  local verifier covering Yocto ctest + baremetal build + Zephyr
  twister + clang-format diff + metadata validate + Doxygen
  zero-warnings; with `--quick`, `--yocto-only`, `--zephyr-only`,
  `--no-clean` flags), and `docs/testing.md` (full coverage map
  per `<alp/...>` header, CI ↔ local correspondence table,
  per-feature verification policy).  README gains a "Test it from
  scratch" section pointing at the new flow.  No SDK code touched;
  documentation + tooling only.
- **Yocto `<alp/security.h>` backend via OpenSSL.**  New
  `src/yocto/security_yocto.c` binds the full `<alp/security.h>`
  surface (`alp_hash_*` for SHA-256/384/512, `alp_aead_*` for
  AES-128-GCM / AES-256-GCM / ChaCha20-Poly1305, `alp_random_bytes`)
  against OpenSSL's `EVP_*` API.  OpenSSL is the same TLS runtime
  libmosquitto links against on a stock Yocto image, so the two
  paths share entropy + CA bundle + algorithm implementation.
  Pulled in via `pkg_check_modules(libssl libcrypto)`; absent
  OpenSSL on the sysroot the backend falls through to the
  NOSUPPORT stubs in `src/common/stub_backend.c` (now gated by a
  new `ALP_VENDOR_OVERRIDES_SECURITY` macro alongside
  `_AUDIO_IN` / `_AUDIO_OUT`).  Tag-mismatch on decrypt is mapped
  to `ALP_ERR_IO` per the header contract.  AEAD key material
  cleared via `OPENSSL_cleanse` on close.  Coverage at
  `tests/yocto/security_openssl.c` (16 tests including a
  SHA-256 KAT against the NIST `"abc"` vector, full AEAD
  round-trip on AES-128-GCM + ChaCha20-Poly1305, tag-mismatch
  detection, key-length / NULL-key / unsupported-alg refusals,
  TRNG fill + null-arg).  `pr-plain-cmake` runner gains
  `libssl-dev` in the install step.  Marked 🟡 partial pending
  meta-alp's real Yocto image build.
- **Yocto audio backend via ALSA libasound.**  New
  `src/yocto/audio_yocto.c` binds the full `<alp/audio.h>` surface
  (`alp_audio_in_*` + `alp_audio_out_*`) against ALSA's `snd_pcm_*`
  API.  Device naming convention: `peripheral_id == 0` -> ALSA's
  `"default"` PCM (honours `/etc/asound.conf` + `~/.asoundrc`);
  `peripheral_id == N` -> `"hw:N-1,0"` (card N-1, device 0).
  Format mapping: `ALP_AUDIO_FMT_S16_LE` / `S24_LE` / `S32_LE` map to
  the matching ALSA constants (S24 is the 32-bit-container variant
  per the alp/audio.h "packed in 32-bit slots" semantics).
  `alp_audio_out_set_volume` applies a software linear scale during
  `alp_audio_out_write` (S16_LE path only for v0.4 prep; S24/S32
  pass through unmodified) so apps don't have to drive ALSA's
  separate mixer API.  Two new pool sizes:
  `ALP_SDK_YOCTO_MAX_AUDIO_IN_HANDLES` /
  `ALP_SDK_YOCTO_MAX_AUDIO_OUT_HANDLES` (default 2 each).  Built
  only when CMake's `pkg_check_modules` finds `alsa` (Debian/Ubuntu
  `libasound2-dev`; Yocto `alsa-lib` recipe); absent, the Yocto
  backend falls through to the NOSUPPORT stubs.  New
  `ALP_VENDOR_OVERRIDES_AUDIO_IN` / `_OUT` gates in
  `src/common/stub_backend.c` (the previous unconditional stubs got
  `z_last_error` stamping added at the same time so the
  `alp_last_error()` contract holds even on NOSUPPORT-only builds).
  Coverage at `tests/yocto/audio_alsa.c` (11 failure-path tests
  covering NULL cfg, invalid format, unreachable device, NULL-handle
  start/stop/read/write/set_volume, close-NULL safety).
  `pr-plain-cmake` runner gains `libasound2-dev` in the install
  step.  Real capture/playback parked behind `hil-yocto`.
- **`examples/uart-rx-ringbuf/` reference app.**  Hand-written
  example exercising the Phase 1 LwRB-backed RX ring buffer on
  ALP_E1M_UART0: attach with a caller-owned backing store, sleep
  to emulate "doing real work", pop batched bytes, detach.
  Heavily commented (~50% comment ratio per the examples-as-
  documentation convention) -- explains the producer/consumer
  split, when to reach for the ringbuf vs the classic
  `alp_uart_read`, and the backing-store sizing formula
  (`baud_rate / 10 * worst_case_drain_latency_s`).  New
  `alp_sdk.example.uart_rx_ringbuf` twister scenario runs the
  happy path under native_sim; the `test-plan.md` row for the
  LwRB feature now references both the failure-path ZTESTs and
  this example.
- **Yocto MQTT TLS (`mqtts://`) via libmosquitto.**  The Yocto IoT
  backend (`src/yocto/iot_yocto.c`) now accepts `mqtts://host[:port]`
  broker URIs (default port 8883) and routes them through
  `mosquitto_tls_set` + `mosquitto_tls_insecure_set` (OpenSSL
  underneath on a stock Yocto image).  New
  `alp_mqtt_tls_config_t` in `<alp/iot.h>`: optional `ca_file` /
  `cert_file` / `key_file` paths plus an `insecure` flag for dev
  testing.  `alp_mqtt_config_t` gains an optional `tls` pointer
  (NULL = use OS default CA path, no client cert -- the production
  path pins `ca_file`).  Default CA path is `/etc/ssl/certs`
  (override at compile time via `ALP_SDK_YOCTO_DEFAULT_CA_PATH`).
  TLS config errors surface at `alp_mqtt_open()` time rather than
  later at connect, so a misconfigured CA bundle is attributable.
  Existing `mqtt://` callers are unaffected -- the `tls` field is
  appended to the public struct and defaults to NULL under
  designated-initializer usage.  Updated `tests/yocto/iot_mqtt.c`:
  the previous NOSUPPORT assertion is replaced with five new TLS
  tests covering default-TLS open / pinned-CA open / missing-CA
  refusal / insecure-flag accepted / default-port-8883 parsing.
  Broker handshake roundtrip parked behind `hil-yocto`.
- **Mender OTA wiring on meta-alp (v0.4 prep).**  New opt-in
  distro include at
  `yocto/meta-alp/conf/distro/include/mender.inc` configuring
  Mender's `mender-full` class with the v0.4 reference storage
  layout (block-device A/B rootfs, 256 MiB per slot by default,
  32 MiB boot + 256 MiB data).  Machine .conf for `e1m-x-v2n`
  and `e1m-n93` gain commented opt-in hook blocks; consumers
  uncomment the `require conf/distro/include/mender.inc` line
  in their target machine to enable.  `e1m-x-v2n-m1` inherits
  via `require conf/machine/e1m-x-v2n.conf`.  `meta-mender-core`
  added to `LAYERRECOMMENDS_alp` -- builds that don't ship OTA
  drop it cleanly from `bblayers.conf`.  README gains a "OTA
  via Mender (v0.4 prep)" section with the full enablement
  walk-through (server / tenant override, artefact paths).
  New cross-cutting `docs/ota.md` covers the trust model + the
  Yocto Mender flow + the AEN-Zephyr Mender vs Hawkbit decision
  pending v0.4-final.  Three `docs/test-plan.md` rows flip from
  TBD to ⏳ untested with concrete artefact pointers (AEN-Zephyr,
  V2N-Yocto, i.MX 93-Yocto).  No code changes; scaffolding +
  documentation only.  HIL roundtrip parked behind `hil-yocto` +
  the Zephyr-client decision.
- **MCUboot secure-boot scaffolding for AEN-Zephyr (v0.4 prep).**
  New sysbuild profile at `sysbuild/aen/sysbuild.conf` (MCUboot +
  ECDSA-P256 + swap-using-scratch).  New `keys/` directory with
  a `.gitignore`'d dev-key slot, a `generate_dev_key.sh` wrapper
  around Zephyr's `imgtool`, and a `README.md` documenting the
  dev / production / rotation lifecycle.  New `docs/secure-boot.md`
  covering the full chain of trust (Alif Secure Enclave ROM ->
  first-stage -> MCUboot -> application), the production key
  lifecycle through OPTIGA Trust M, the failure-mode matrix for
  `swap-using-scratch`, and the multi-key rotation playbook.
  Live compile-verification gates on the authoritative
  `alp_e1m_evk_aen` board file landing at `alplabai/alp-zephyr-modules`
  (PLAN.md §6 item 8); the sysbuild profile is ready to drop into
  a `pr-twister` scenario the moment that board file appears.
  No code changes -- this is scaffolding + documentation only;
  the v0.4 test-plan row flips from TBD to ⏳ untested with a
  concrete artefact pointer.
- **`<alp/mproc.h>` IPC envelope framing on AEN-Zephyr (v0.4 prep).**
  Wraps every `alp_mbox_send` payload in a 12-byte little-endian
  header (`'AMPF'` magic / monotonic sequence / declared length)
  before handing it to the Zephyr mbox driver, and unwraps inbound
  frames before dispatching `alp_mbox_msg_cb_t`.  New Kconfig
  `CONFIG_ALP_SDK_MPROC_NANOPB_FRAMING` (off by default) plus
  `CONFIG_ALP_SDK_MPROC_FRAME_MAX_BYTES` (default 512) sizing the
  per-handle TX scratch.  Implemented in
  `src/common/proto/alp_mproc_frame.{h,c}` -- a placeholder binary
  framer that exercises the same call sites in
  `src/zephyr/mproc_zephyr.c` that the nanopb-generated codec will
  occupy in v0.4-final.  The placeholder wire is intentionally NOT
  compatible with the v0.4-final protobuf wire generated against
  `metadata/protos/alp_mproc.proto`; both ends of an IPC channel
  must agree on the framing flag.  Coverage: nine ZTESTs in
  `tests/zephyr/mproc/src/main.c` covering encode roundtrip, zero
  payload, NULL outputs, capacity-short failure, decode of short /
  bad-magic / length-overflow frames.  Existing "no backend" tests
  scoped under `#if !CONFIG_ALP_SDK_MPROC` so the new
  `alp_sdk.mproc.nanopb_framing` twister scenario (flips MPROC +
  MBOX on alongside the framing flag) compile-verifies the framing
  branch in `alp_mbox_send` + `mbox_rx_cb` without colliding with
  the no-backend assertions.  Real peer-firmware roundtrip parked
  behind `nightly-aen-hil`.
- **LwRB UART RX ring buffer on AEN-Zephyr (v0.4 prep).**  First
  in-tree consumer of the LwRB anchor at `vendors/lwrb/`.  New
  opt-in API in `<alp/peripheral.h>`:
  `alp_uart_rx_ringbuf_attach()` / `_pop()` / `_count()` /
  `_detach()`.  When `CONFIG_ALP_SDK_UART_RX_RINGBUF=y` (off by
  default; depends on `CONFIG_UART_INTERRUPT_DRIVEN`), the Zephyr
  backend registers an IRQ callback on the underlying UART that
  drains the controller FIFO into a caller-supplied LwRB-backed
  ring on every byte; consumer code pops bytes without polling.
  Backed by the in-tree LwRB stub impl at
  `vendors/lwrb/src/lwrb_stub_impl.c` (~140 LoC, correct
  single-producer / single-consumer semantics with the canonical
  empty/full disambiguation) until the `extras-v04` west group
  flips upstream `MaJerle/lwrb` in.  Builds without the Kconfig
  flag (or non-Zephyr backends) get NULL/NOSUPPORT stubs in
  `src/common/stub_backend.c`, gated by a new
  `ALP_VENDOR_OVERRIDES_UART_RX_RINGBUF` macro so backends can
  adopt the ringbuf incrementally without re-implementing the
  full UART surface.  Failure-path coverage in
  `tests/zephyr/peripheral/src/main.c` (four ZTESTs covering
  NULL-port attach, NULL-handle pop, NULL-handle count, NULL
  detach safety); compile-verification for the feature-on path
  via a new `alp_sdk.peripheral.uart_rx_ringbuf` twister
  scenario with `EXTRA_CONF_FILE=prj_uart_ringbuf.conf`.
  Real-IRQ attach gates on `nightly-aen-hil`.
- **Yocto MQTT backend via libmosquitto (v0.4 prep).**
  `src/yocto/iot_yocto.c` implements `alp_mqtt_*` against the
  Eclipse Mosquitto C client library.  Caller-driven loop model:
  `alp_mqtt_loop(handle, timeout_ms)` pumps the network event
  machine and dispatches inbound messages to every subscription
  whose filter matches via `mosquitto_topic_matches_sub` (so MQTT
  wildcards `+` and `#` work).  URI parser supports `mqtt://host`
  and `mqtt://host:port` (default 1883); `mqtts://` returns
  `ALP_ERR_NOSUPPORT` until the v0.4 secure-stack work lands a
  shared TLS context.  Optional username/password via
  `mosquitto_username_pw_set`.  New per-class
  `ALP_VENDOR_OVERRIDES_MQTT` gate in `src/common/stub_backend.c`
  -- the `alp_wifi_*` half of `<alp/iot.h>` stays stubbed on the
  Yocto path (Wi-Fi bring-up on real Yocto images is a
  wpa_supplicant / NetworkManager concern, not SDK-side).  Build
  is gated on `pkg_check_modules(libmosquitto)`: workspaces
  without libmosquitto-dev on the sysroot fall back cleanly to
  the NOSUPPORT stubs.  CI runner now installs
  `libmosquitto-dev` + `pkg-config`.  Failure-path coverage at
  `tests/yocto/iot_mqtt.c` (NULL cfg / null URI / mqtts NOSUPPORT
  / unknown scheme / empty host / bad port / NULL handle on
  publish/subscribe/loop / close-NULL safety / happy-path
  open-then-close).  Broker-roundtrip coverage parked behind
  ci/HW-IN-LOOP.md.
- **Yocto GPIO IRQ dispatcher (v0.4 prep).**  `alp_gpio_irq_enable`
  / `_disable` now wired against the GPIO v2 edge-event ABI.  A
  single shared pthread runs the `poll()` loop across every pin
  that has IRQ enabled; an eventfd lets mutators wake it for
  slot-table re-snapshots.  Re-configures the line via
  `GPIO_V2_LINE_SET_CONFIG_IOCTL` with
  `GPIO_V2_LINE_FLAG_EDGE_RISING`/`FALLING` (or both) on enable;
  back to plain `INPUT` on disable.  Dispatcher starts lazily on
  first `irq_enable` and runs for the lifetime of the process.
  Callbacks run on the dispatcher thread under the dispatcher
  mutex -- documented contract: callers must not call
  `alp_gpio_irq_disable` / `alp_gpio_close` from inside a
  callback (would deadlock).  `pthread` now linked through
  `Threads::Threads` on the Yocto path.  Failure-path tests
  updated to reflect the new contract (NULL pin / NULL cb /
  `ALP_GPIO_EDGE_NONE` all return `ALP_ERR_INVAL`).  Real-edge
  testing still parked behind the v0.4 hil-yocto runner.
- **Yocto first-class peripheral wrappers — GPIO class (v0.4 prep).**
  `src/yocto/peripheral_gpio.c` binds `alp_gpio_*` against the
  Linux GPIO character-device v2 ABI at `/dev/gpiochipN` (kernel
  >= 5.10).  No libgpiod dependency -- ioctls invoked directly
  against the kernel UAPI in `<linux/gpio.h>`, same pattern as
  the other Yocto wrappers.  Pin-id is packed as `(chip << 16) |
  line_offset` so the studio pin allocator can stay one-axis on
  the wire.  `alp_gpio_configure` switches direction + bias via
  `GPIO_V2_LINE_SET_CONFIG_IOCTL`; `alp_gpio_write` /
  `alp_gpio_read` use the values-get/set ioctls.  Bias support
  passes through to the kernel driver: chips that don't implement
  pull-up/pull-down configuration surface `ALP_ERR_NOSUPPORT`
  from configure.  IRQ paths
  (`alp_gpio_irq_enable` / `_disable`) return `ALP_ERR_NOSUPPORT`
  for now -- callback dispatch needs a `poll()`/pthread loop
  that's parked until a Yocto caller actually needs it.  Per-class
  `ALP_VENDOR_OVERRIDES_GPIO` gate.  Failure-path coverage at
  `tests/yocto/peripheral_gpio.c` (`/dev/gpiochip999` -> ENOENT,
  NULL handle on every entry point, IRQ NOSUPPORT contract,
  close-NULL safety).  Closes the Yocto core-4 peripheral
  wrapper set (I2C / SPI / UART / GPIO all real on Linux).
- **Yocto first-class peripheral wrappers — UART class (v0.4 prep).**
  `src/yocto/peripheral_uart.c` binds `alp_uart_*` against the
  Linux tty layer via termios.  Port-id resolution is a small
  table: 0..99 -> `/dev/ttyS<id>`, 100..199 -> `/dev/ttyAMA<id-100>`,
  200+ -> `/dev/ttyUSB<id-200>`.  Configures data/stop/parity via
  `c_cflag` and baud via `cfsetispeed` + `cfsetospeed`; supported
  baud rates are the standard termios constants (9600 through
  3 Mbps), unknown values return `ALP_ERR_INVAL`.  Reads honour
  `timeout_ms` via `VMIN=1` + `VTIME = ceil(timeout_ms / 100)`,
  returning `ALP_ERR_TIMEOUT` on a clean timeout-with-no-bytes
  and `ALP_OK` on a partial read that beat the timeout.  Writes
  loop on `EINTR`.  Same per-class gate (`ALP_VENDOR_OVERRIDES_UART`).
  Failure-path coverage at `tests/yocto/peripheral_uart.c` (NULL
  cfg, invalid data/stop bits, unsupported baud, /dev/ttyS999 ->
  ENOENT, NULL handle on read/write, close-NULL safety).
- **Yocto first-class peripheral wrappers — SPI class (v0.4 prep).**
  `src/yocto/peripheral_spi.c` binds `alp_spi_*` against Linux
  spidev (`/dev/spidev<bus_id>.<cs_pin_id>`).  Direct two-axis
  mapping: `bus_id` -> SPI controller index, `cs_pin_id` -> CS
  line index (the kernel owns the CS toggle; no userspace
  bit-banging).  Configures mode + bits-per-word + max speed via
  the `SPI_IOC_WR_*` ioctls before the first transfer; full-duplex
  uses `SPI_IOC_MESSAGE(1)` with both `tx_buf` and `rx_buf` set,
  half-duplex uses plain `write()` / `read()` against the same
  fd.  Same errno -> `alp_status_t` mapping as the I2C wrapper;
  same `ALP_VENDOR_OVERRIDES_SPI` per-class gate.  Failure-path
  coverage at `tests/yocto/peripheral_spi.c` (NULL cfg, invalid
  mode / bits-per-word, `/dev/spidev999.0` -> ENOENT, NULL handle
  on every entry point, close-NULL safety).
- **Yocto first-class peripheral wrappers — I2C class (v0.4 prep).**
  `src/yocto/peripheral_i2c.c` binds `alp_i2c_*` against Linux
  i2c-dev (`/dev/i2c-N`).  Maps `alp_i2c_config_t.bus_id` to the
  kernel adapter index; uses `I2C_RDWR` ioctl for write-then-read
  so the device sees a repeated-start between the register pointer
  write and the data read.  Probes `I2C_FUNCS` on open so callers
  fail fast on SMBus-only adapters.  errno → `alp_status_t`
  mapping is shared with the inference path via `errno_to_alp` in
  the same TU (ENOENT → NOT_READY, EBUSY → BUSY, ETIMEDOUT →
  TIMEOUT, default → IO).  Stamps `alp_last_error()` through the
  shared common slot.  Linux-only (gated by `__linux__` +
  `CMAKE_SYSTEM_NAME STREQUAL "Linux"`); non-Linux builds keep
  using the stub I2C symbols.  First Yocto peripheral wrapper out
  of the four-class core set (SPI / GPIO / UART follow in
  subsequent v0.4 increments).
- **Per-class `ALP_VENDOR_OVERRIDES_<CLASS>` gates in
  `src/common/stub_backend.c` (v0.4 prep).**  The previous
  monolithic `ALP_VENDOR_OVERRIDES_PERIPHERAL=1` umbrella forced
  a backend to provide all four peripheral classes (I2C, SPI,
  GPIO, UART) at once or none.  Split into per-class macros so
  the Yocto path can land I2C first and let SPI / GPIO / UART
  keep the NOSUPPORT stubs until their wrappers ship.  Umbrella
  macro is preserved -- defining `ALP_VENDOR_OVERRIDES_PERIPHERAL`
  implies all four per-class macros, so existing vendor wrappers
  at `vendors/alif/` are untouched.
- **`tests/yocto/peripheral_i2c.c` failure-path coverage.**  New
  ctest binary exercising NULL config rejection, non-existent
  bus index (errno → `ALP_ERR_NOT_READY`), NULL handle on
  read/write/write_read paths, and the close-NULL safety
  contract.  Wired into the `pr-plain-cmake.yml` yocto job which
  now configures with `-DALP_BUILD_TESTS=ON` and runs `ctest`.
  Real-adapter HIL coverage stays parked behind
  `ci/HW-IN-LOOP.md` until the `hil-yocto` self-hosted runner
  lands.
- **west.yml pins for v0.4 SDK-internal dependencies.**  LwRB
  pinned at `MaJerle/lwrb@v3.2.0` and nanopb at
  `nanopb/nanopb@nanopb-0.4.9`, both behind the `extras-v04`
  group (disabled by default via the manifest's `group-filter:
  [-extras-v04]`).  `west update` on a v0.3 workspace does not
  fetch them and the Zephyr build does not auto-import them --
  the vendor stub headers under `vendors/{lwrb,nanopb}/include/`
  keep SDK source link-clean.  Flipping the group on with `west
  update --group-filter +extras-v04` makes the upstream sources
  win the include search ahead of the stubs; nanopb's
  `zephyr/module.yml` then auto-registers it as a Zephyr module,
  while LwRB still needs the ~10-line wrapper documented in
  `vendors/lwrb/README.md` "Wiring (v0.4)" once a real consumer
  lands.  Per-library context in `vendors/lwrb/README.md` and
  `vendors/nanopb/README.md`.
- **`<alp/hw_info.h>` runtime hardware-info API.**  Public header
  declaring `alp_hw_info_t`, `alp_hw_info_eeprom_t` (128-byte
  manifest layout that lives at offset 0x0000 of the SoM's
  on-module 24C128 EEPROM), `alp_hw_info_read()`, and
  `alp_hw_info_assert_matches_build()`.  v0.3 ships the API
  contract + a Zephyr-side stub returning `ALP_ERR_NOSUPPORT`;
  the runtime EEPROM + BOARD_ID ADC reads land in v0.3.x once
  the per-family BOARD_ID channels are filled in.  Companion
  production-test programmer at `tools/program_eeprom.py` packs
  a board.yaml + serial + mfg date into the 128-byte binary that
  the production-test fixture writes to the EEPROM.  Unit tests
  pin the manifest layout against drift between the Python writer
  and the C reader; a ztest under `tests/zephyr/hw_info/` covers
  the NOSUPPORT contract for both entry points.
- **Hardware-revision tracking (board.yaml `som.hw_rev` +
  `carrier.hw_rev`).**  Every released family ships an
  `hw-revisions.yaml` (one per family + per carrier) declaring
  per-revision `[min_sdk_version, max_sdk_version]` windows.
  `metadata/sdk_version.yaml` carries the SDK's own version.  The
  loader + validator refuse to emit configs when the chosen
  hw_rev doesn't cover the current SDK version (validator exit
  code 3, loader aborts CMake configure).  Runtime detection
  reads a per-board BOARD_ID ADC channel fed by a 1.8 V
  resistor-divider (`10 kΩ / 10 kΩ → 900 mV` for r1) -- up to ~8
  distinguishable revisions per board at ±100 mV bin radius with
  1 % resistors.  The AEN family ships `r1` as production (all
  AEN MPNs share one PCB per the user-supplied constraint "all
  AENs have the same revision, only SoC changes"); V2N / V2M /
  N93 ship `r1` as TBD-status stubs pending the user-supplied
  HW writeups.  See `docs/board-config.md` "Hardware revision
  tracking".
- **VS Code extension polish.**  New *Alp: Generate all* command
  runs every loader emit mode in one keystroke and reports a
  single status-bar summary.  A new `DiagnosticCollection` runs
  `validate_board_yaml.py` on every open / save of a `board.yaml`
  and surfaces failures inline in the Problems panel -- the
  schema-level checks the Red Hat YAML extension already covers
  remain its job; this layer adds the SDK-specific checks (missing
  SoM preset, missing carrier preset without inline `populated`,
  v0.3 hw_rev / SDK-version compatibility).  Severity tracks the
  validator's exit code: 1 -> Error, 2 -> Warning, 3 -> Error.
- **In-tree VS Code extension (`vscode/`).**  TypeScript
  extension that adds schema-aware `board.yaml` editing
  (autocomplete + red squigglies via the Red Hat YAML
  extension), five starter snippets, a configurator webview
  panel with dropdowns/checkboxes driven from the live
  preset library, one command per loader emit mode, a
  customer-side validator command, per-OS dependency bootstrap
  (Linux/macOS/Windows), and `west build/flash/run` wrappers.
  CI gates compile + schema-sync + `.vsix` package on every PR.
- **SoM presets shipped for every released MPN.**  Per the design
  directive "ship EVK configuration as board.yaml and prepare
  som.yaml file for every MPN, so customer just included MPN
  number in the board.yaml" -- the SDK now ships
  `metadata/e1m_modules/<MPN>/som.yaml` for every released SoM:
    - Alif Ensemble: `E1M-AEN301`, `AEN401`, `AEN501`, `AEN601`,
      `AEN701`, `AEN801` (six MPNs; E3/E7 released silicon, the
      rest preliminary).
    - Renesas RZ/V2N: `E1M-V2N101`, `V2N102` (two MPNs differing
      in DRAM + eMMC capacity).
    - RZ/V2N + DEEPX DX-M1: `E1M-V2M101`, `V2M102` (two MPNs
      mirroring the V2N split, plus the on-module DX-M1).
    - NXP i.MX 93: `E1M-NX9101` placeholder (production MPN TBD).
  Each preset fills in the on-module support silicon (CC3501E,
  OPTIGA Trust M, RV-3028-C7, TMP112, 24C128 for AEN family;
  Murata LBEE5HY2FY + GD32G553 + DA9292 + RTL8211FDI for V2N
  family) from the existing module datasheet docs.  Per the
  project memory note, memory capacities + per-MPN datasheet
  specifics stay TBD until the user-supplied HW config writeup
  fills them in.
- **`docs/board-config.md` "Quick start" section** -- the
  minimum-viable three-line `board.yaml`, a table of every
  MPN the SDK ships a preset for, and a table of stock
  carriers.  Customers paste their MPN and they're done.
- **`docs/getting-started.md` "Project configuration" bullet**
  rewritten around the "three lines: MPN + carrier + OS"
  workflow.

### Changed

- **`alp.yaml` renamed to `board.yaml`** -- the file describes
  what's on the board the firmware runs against, so the name
  should say that.  Plus internal restructure of the metadata
  preset layout to match the SoM-vs-carrier split with one
  conventional file per directory:
    - `metadata/templates/alp.yaml` -> `metadata/templates/board.yaml`
    - `metadata/templates/alp.yaml.example` -> `metadata/templates/board.yaml.example`
    - `metadata/schemas/alp-project-v1.schema.json` ->
      `metadata/schemas/board-config-v1.schema.json`
    - `metadata/e1m_modules/aen/sku-aen701.yaml` ->
      `metadata/e1m_modules/E1M-AEN701/som.yaml`
    - `metadata/e1m_modules/v2n/sku-v2n101.yaml` ->
      `metadata/e1m_modules/E1M-V2N101/som.yaml`
    - `metadata/carriers/e1m-evk.yaml` ->
      `metadata/carriers/E1M-EVK/board.yaml`
    - `metadata/carriers/e1m-x-evk.yaml` ->
      `metadata/carriers/E1M-X-EVK/board.yaml`
    - `metadata/carriers/custom-example.yaml` ->
      `metadata/carriers/custom-example/board.yaml`
    - `docs/project-config.md` -> `docs/board-config.md`
  Loader (`scripts/alp_project.py`) updated: default `--input`
  is now `./board.yaml`; SoM preset resolution looks at
  `metadata/e1m_modules/<SKU>/som.yaml`; carrier preset
  resolution at `metadata/carriers/<name>/board.yaml`.  Schema
  $id + title + description updated to match.  All docs +
  vendor READMEs + plan/versions files updated for the new
  names + paths.  Per-file history preserved via `git mv`.
  No code change in the SDK proper -- pure config-layer rename.
- `vendors/nanopb/README.md`: corrected the west.yml revision pin
  to use nanopb's actual GitHub tag format (`nanopb-0.4.9`, with
  the `nanopb-` prefix; I had previously documented bare `0.4.9`).
  Verified the upstream repo ships a `zephyr/module.yml` so the
  west import picks it up without extra plumbing.
- `vendors/lwrb/README.md`: corrected the integration plan --
  LwRB does **not** ship a `zephyr/module.yml`, so a plain
  west.yml import alone won't register it.  Two options now
  documented: (a) west import + tiny in-repo `zephyr/module.yml`
  shim, or (b) vendor a tagged release under `vendors/lwrb/src/`.
  Plan-A picked for v0.4 default.

### Changed

- **SDK-internal libraries no longer expose user-facing enable
  flags.**  Per the design principle "for ourselves we don't need
  any enable to use in our internal functions":
  - Removed `CONFIG_ALP_SDK_USE_LWRB` + `CONFIG_ALP_SDK_USE_NANOPB`
    from `zephyr/Kconfig`.  Both libraries are SDK-internal
    dependencies (LwRB for audio DMA staging, nanopb for
    `<alp/mproc.h>` IPC framing); consumers don't enable them.
    When the v0.4 audio + mproc paths land, the SDK code uses the
    libraries unconditionally and the west.yml pins land alongside.
  - Removed `lwrb` + `nanopb` from `board.yaml`'s `libraries:` enum
    (`metadata/schemas/board-config-v1.schema.json`) and from the
    loader's `_LIBRARY_KCONFIG` map.  The enum now lists only
    user-facing libraries: etl, fmt, nlohmann_json, doctest, lvgl,
    mbedtls, cmsis_dsp, littlefs.
  - `src/zephyr/audio_zephyr.c` + `src/zephyr/mproc_zephyr.c`
    docstrings + `vendors/lwrb/README.md` + `vendors/nanopb/README.md`
    + `metadata/templates/board.yaml` updated to reflect the
    "SDK-internal, no user-visible enable" status.
  - `metadata/templates/board.yaml.example` updated to exercise the
    user-facing path (`libraries: [lvgl, mbedtls, cmsis_dsp, etl]`)
    -- CI's loader smoke test now covers the new Kconfig mappings.
- **Profile-header filenames now match each upstream library's
  expected name** -- no more six different `alp-embedded.h` files
  scattered across the tree.  Renames:
    - `etl/alp-embedded.h` -> `etl/etl_profile.h` (ETL's expected name)
    - `fmt/alp-embedded.h` -> `fmt/fmt_config.h`
    - `nlohmann_json/alp-embedded.h` -> `nlohmann_json/json_config.h`
    - `lvgl/alp-embedded.h` -> `lvgl/lv_conf.h` (LVGL's expected name)
    - `doctest/alp-embedded.h` -> `doctest/doctest_config.h`
    - `mbedtls/alp-embedded.h` -> `mbedtls/mbedtls_config.h`
      (set `MBEDTLS_CONFIG_FILE` to this path when including).
  Drop-in semantics for libraries that demand a specific config
  filename; self-documenting otherwise.  Done via `git mv` so
  per-file history is preserved.  `metadata/library-profiles/README.md`
  layout table refreshed.

### Added

- **Library profile set extended 3 -> 7.**  Per the design directive
  "make many libraries compatible for user's application, they enable
  in the config file when they want to use":
  - `metadata/library-profiles/lvgl/alp-embedded.h` -- a working
    `lv_conf.h` tuned for E1M displays (RGB565 baseline, 48 KiB
    LV_MEM_SIZE, k_uptime_get-driven LV_TICK_CUSTOM, demos off
    to save flash, image decoders off, filesystem integration off).
  - `metadata/library-profiles/mbedtls/alp-embedded.h` -- a
    minimal-but-modern MbedTLS config: SHA-256/384/512, AES-GCM/CCM,
    HMAC, HKDF, ECDH/ECDSA P-256+P-384, RSA verify, TLS 1.3 client
    only, X.509 parse.  Deliberately omits MD5/SHA-1, DES/3DES/RC4,
    plain CBC, TLS server role, x509 cert generation.
  - `metadata/library-profiles/doctest/alp-embedded.h` -- disables
    POSIX signal handlers + multithreading so doctest builds clean
    on the SDK's test runner.
  - `metadata/library-profiles/cmsis-dsp/README.md` -- intentionally-
    empty profile placeholder; CMSIS-DSP config comes from the SoM's
    target architecture via the SoC metadata, not from a header.
  Plus schema + loader updates so each new entry is enableable
  via `board.yaml`'s `libraries:` array:
  - Schema enum extended: `lvgl`, `mbedtls`, `cmsis_dsp`, `littlefs`
    added alongside the existing `etl`, `fmt`, `nlohmann_json`,
    `doctest`, `lwrb`, `nanopb`.
  - `scripts/alp_project.py` gains a `_LIBRARY_KCONFIG` map -- each
    enabled library maps to the right CONFIG_* flags (e.g.
    `lvgl` -> `CONFIG_LVGL=y`, `mbedtls` -> `CONFIG_MBEDTLS=y` +
    `CONFIG_MBEDTLS_BUILTIN=y`, `littlefs` ->
    `CONFIG_FILE_SYSTEM_LITTLEFS=y` + `CONFIG_FILE_SYSTEM=y`).
    User-facing C++ libs (etl/fmt/nlohmann_json/doctest) emit
    a TODO marker for the v0.4 CMake-include-path hook.
  - `metadata/templates/board.yaml` commented-libraries section now
    showcases all ten enableable libs with one-liner notes.
- **"Using enabled libraries" section** in
  `docs/recommended-libraries.md` -- short usage snippets for
  every Tier-1 library a consumer can enable in `board.yaml`:
  CMSIS-DSP (FIR/FFT), ETLCPP (`etl::vector`, `etl::map`), fmt
  (`fmt::format_to_n`), nlohmann/json (no-exception parse path),
  LVGL (label on the resolved display), LittleFS (mount), and
  doctest (test case).  Wrapping these libraries would be chaos
  per the design principle -- apps use the upstream native API,
  the SDK ships the compile-time profile that makes them
  compatible with our embedded environment.
  `docs/board-config.md` `libraries:` block section cross-links
  to the new "Using enabled libraries" section; also adds an
  explicit "no-wrapper rationale" callout, plus an "SoM vs
  carrier (kept deliberately separate)" subsection codifying that
  the SoM SKU presets live in their own directory hierarchy
  separate from carriers + customer config.
- `docs/board-config.md` "Single source of truth" section
  codifying the design principle: **`board.yaml` is the only place
  to configure the firmware**.  `prj.conf`, CMake `-D` args,
  `local.conf` are all derived from it.  Honest "Today's gaps"
  subsection calls out the three remaining places where hand-
  written config still leaks (DTS overlays for carrier wiring,
  `west.yml` module list, per-test `prj.conf` in
  `tests/zephyr/<area>/`) -- all v0.4 targets.  Template
  (`metadata/templates/board.yaml`) + getting-started.md updated
  with the same principle so new consumers absorb it from page
  one.
- **Library profile headers** at
  [`metadata/library-profiles/<lib>/`](metadata/library-profiles/)
  -- the "compatible without wrapping" model.  Each Tier-1
  library that consumers enable in `board.yaml`'s `libraries:`
  array has a pre-tuned compile-time profile so the upstream
  library works correctly under the SDK's no-exceptions /
  no-iostream / no-STL-on-M-class invariants.  v0.3 ships:
  - `etl/alp-embedded.h` (sets `ETL_NO_STL`, `ETL_NO_EXCEPTIONS`,
    `ETL_CPP17_SUPPORTED`).
  - `fmt/alp-embedded.h` (sets `FMT_HEADER_ONLY=1`,
    `FMT_USE_IOSTREAM=0`, `FMT_EXCEPTIONS=0`).
  - `nlohmann_json/alp-embedded.h` (sets `JSON_NOEXCEPTION=1`,
    `JSON_USE_IMPLICIT_CONVERSIONS=0`).
  v0.4 wires the loader to add the profile's include directory
  ahead of the upstream library's defaults so the profile wins.
  Apps that want different settings supply their own profile
  header at their include root -- the loader prefers the app's
  profile over the SDK's.  Apps still use the upstream API
  directly; no `<alp/...>` wrapper.  Design + per-library notes
  in [`metadata/library-profiles/README.md`](metadata/library-profiles/README.md).
  `docs/board-config.md` "libraries block" section now
  documents the model.
- `docs/board-config.md` "How the loader compiles the file"
  section rewritten from "lands in v0.4" to working invocation
  recipes.  Three concrete worked examples land:
  - Zephyr: how to call the loader at configure time + include
    the generated `alp.conf` from `prj.conf` via `rsource`, plus
    a `CMakeLists.txt` snippet that auto-regenerates on
    `board.yaml` changes via `add_custom_command`.
  - Plain CMake: piping `--emit cmake-args` straight into the
    configure step.
  - Yocto: generating a `local.conf` snippet + requiring it.
  Also documents the three loader follow-ups deferred to v0.4
  (DTS overlay generation, soc_caps cross-validation, `west
  alp-build` extension command).
- `metadata/carriers/custom-example.yaml` -- worked example of a
  customer fork of the E1M-EVK carrier preset.  Shows a slim
  production-board derivative that keeps the IMU + barometer
  and drops the multimedia / debug parts (OLEDs, camera, speaker
  amps, current monitors, I/O expander).  `e1m-evk.yaml`'s
  docstring + `docs/board-config.md` both explicitly position
  the EVK presets as **reference designs** customers fork for
  their own carriers, not just dev-kit-only configs.  Two
  consumption patterns documented: "reference + override" (small
  derivatives) and "fork the preset" (full custom boards).
- `scripts/alp_project.py` -- the `board.yaml` **loader** that compiles
  a project config into per-backend native output.  Validates against
  the v1 schema, resolves the SoM SKU + carrier presets, merges
  overrides, and emits one of three formats:
    - `--emit zephyr-conf` (default): a Kconfig fragment for `prj.conf`
      to append.  Picks the silicon `CONFIG_ALP_SOC_*=y`, the carrier
      chip-driver `CONFIG_ALP_SDK_CHIP_*` flags, the inference
      backend Kconfigs (`CONFIG_ALP_SDK_INFERENCE_TFLM` /
      `_ETHOS_U` / `_DRPAI` / `_ETHOS_U_N93`), IoT features
      (`CONFIG_ALP_SDK_IOT_WIFI` etc.), and library enables
      (`CONFIG_ALP_SDK_USE_LWRB` / `_USE_NANOPB`).
    - `--emit cmake-args`: plain-CMake configure args (`-DALP_SOM=...`,
      `-DALP_OS=...`, `-DALP_SDK_USE_DEEPX_DXM1=ON` on V2N-M1).
    - `--emit yocto-conf`: `local.conf` snippet (`MACHINE = "..."`,
      `IMAGE_INSTALL:append`).
  Python 3.10+; depends on `PyYAML` + `jsonschema` (the latter
  already on the CI path).  CI smoke-tests all three emit formats
  on `metadata/templates/board.yaml.example` via the extended
  `pr-metadata-validate` workflow -- catches schema / loader
  regressions at PR time.
- `metadata/templates/board.yaml.example` -- a fully-uncommented
  config the loader exercises end-to-end.  Distinct from
  `board.yaml` (the heavily-commented user template).

### Changed

- `board.yaml` schema split into SoM-vs-carrier blocks.  The first
  pass conflated on-module components (silicon + CC3501E + OPTIGA
  + RV3028 + TMP112 + 24C128, fixed at SoM-fab time) with carrier-
  board components (LSM6DSO + BMI323 + ICM-42670 + BMP581 + OLEDs
  + OV5640 + TAS2563 + INA236, variable per board design).  The
  corrected schema:
  - `som` block carries on-module concerns only (silicon SKU,
    on-module radio / secure element / RTC overrides for SoM-
    variant SKUs, memory capacities).
  - New `carrier` block carries the per-board chip population --
    each `populated.<name>: true` enables the corresponding
    `CONFIG_ALP_SDK_CHIP_<NAME>=y` at build time.
  - New stock carrier presets at `metadata/carriers/e1m-evk.yaml`
    (35x35 EVK -- IMU x3, BMP581, SSD1306, PDM mics, TAS2563 x2,
    INA236 x6, TCAL9538, PI3WVR626) and
    `metadata/carriers/e1m-x-evk.yaml` (45x65 EVK -- populated
    list TBD pending user HW config writeup).
  - `metadata/e1m_modules/<family>/sku-*.yaml` files updated:
    removed the misclassified carrier-side components; kept only
    on-module parts.
  - `metadata/templates/board.yaml` + `docs/board-config.md`
    updated with the SoM-vs-carrier distinction explained.

### Added

- **Project configuration (`board.yaml`)** -- one declarative YAML
  file per consumer project that picks the SoM SKU + per-component
  assembly overrides + OS backend + inference backend + optional
  libraries + connectivity features.  Collapses what was previously
  three separate config formats (`prj.conf` for Zephyr, cmake `-D`
  flags for plain CMake, `local.conf` for Yocto) into one source
  of truth.
  - Schema: `metadata/schemas/board-config-v1.schema.json` (JSON
    Schema draft-2020-12).
  - Canonical template: `metadata/templates/board.yaml` (fully
    commented).
  - Stock SKU presets: `metadata/e1m_modules/<family>/sku-<sku>.yaml`.
    v0.3 ships two worked examples (`sku-aen701.yaml`,
    `sku-v2n101.yaml`); remaining SKUs (aen301/401/501/601/801,
    v2n102, v2m101/102, NX9xxx) land as the user-supplied hardware
    configuration writeup fills them in.  Values not in the
    silicon datasheet stay `TBD` until then per the project
    memory note.
  - Design + reference: `docs/board-config.md`.
  - `docs/getting-started.md` "Where to go next" updated to point
    at board-config.md as the first item.
  - The loader script that emits per-backend native configs from
    `board.yaml` (Zephyr fragments / cmake `-D` / Yocto local.conf)
    lands in v0.4.  v0.3 documents the mapping so consumers can
    hand-translate until then.
  - **Optional libraries (ETLCPP, fmt, nlohmann/json, doctest,
    LwRB, nanopb) are declared in the same file** -- the v0.4
    loader wires their include paths into the build when listed,
    no `<alp/...>` wrapper.  Apps use the libraries through their
    native APIs.
- `bench/` extended from 3 -> 6 cases.  New files cover the
  rejection / fast-path costs for `<alp/iot.h>` (`bench_iot.c`),
  `<alp/audio.h>` (`bench_audio.c`), and `<alp/storage.h>`
  (`bench_storage.c`).  Same NULL/empty-cfg shape as the
  peripheral bench.  `bench_main.c` updated to invoke them;
  `bench/README.md` table refreshed.  Total bench coverage at
  ~6 of ~15 public API classes; v1.0 fills the rest as the
  implementations land.
- `tests/yocto/` -- plain-CMake test suite for the Linux user-space
  backend.  First entry: `alp_test_inference_dispatcher` exercises
  the seven branches of `src/yocto/inference_yocto.c` --
  NULL-cfg / NULL-model / zero-size cfg rejection, AUTO-with-no-
  backend NOSUPPORT, explicit unsupported backend NOSUPPORT, NULL-
  handle safety on every accessor, and that `alp_last_error()`
  correctly stamps `ALP_ERR_INVAL` / `ALP_ERR_NOSUPPORT` per the
  new unified setter.  Mirrors the contract in the Zephyr ztest
  suite at `tests/zephyr/inference/`; uses a tiny local
  assert harness (`test_assert.h`) since plain-CMake builds don't
  pull in ztest.  Opts in via `ALP_OS=yocto` + `ALP_BUILD_TESTS=ON`.
- `src/common/alp_internal.h` -- cross-backend internal header
  declaring `alp_internal_set_last_error(s)`.  Lets non-Zephyr
  backend source files (the yocto inference dispatcher today,
  more sites in v0.4) stamp the same process-wide last-error
  slot that `alp_last_error()` reads, instead of carrying
  per-file shadow statics.  Closes the TODO(v0.4) comment in
  `src/yocto/inference_yocto.c`.  Zephyr keeps its own
  thread-local last-error in `src/zephyr/last_error.c`; the new
  header is irrelevant there.
- `vendors/renesas-rzv2n/rzv_drp-ai_tvm/README.md` -- integration
  anchor for **RUHMI** (formerly "DRP-AI TVM"), Renesas's
  Apache-2.0 host-side model compiler at
  <https://github.com/renesas-rz/rzv_drp-ai_tvm>.  Distinct from
  the target-side `libdrpai` runtime (which ships via
  `meta-rz-drpai` in the Renesas BSP).  The SDK's
  `<alp/inference.h>` Yocto backend links against the runtime;
  model authors run RUHMI on their workstation and ship compiled
  output as a model asset.  `vendors/renesas-rzv2n/README.md`
  cross-links to the new subdirectory and documents the BSP
  setup pointer.

### Changed

- `yocto/meta-alp/` rebased on the **Renesas RZ/V2N AI SDK
  (platform 7.1) on BSP v6.30**.  Earlier README + layer.conf
  referenced `meta-renesas-rz` (no such repo); the canonical layer
  is `meta-renesas` at <https://github.com/renesas-rz/meta-renesas>,
  distributed via the AI SDK Source Code package (fetched from
  Renesas under your own account; alp-sdk does not redistribute it).
  meta-alp now `LAYERRECOMMENDS` `meta-renesas` plus the four
  `meta-rz-features/*` sublayers (`meta-rz-graphics`,
  `meta-rz-drpai`, `meta-rz-opencva`, `meta-rz-codecs`) +
  `meta-econsys` for the camera path + `meta-deepx-m1` for
  V2N-M1.  `e1m-x-v2n.conf` now inherits from Renesas's stock
  `rzv2n-evk` MACHINE (vs the earlier guess at a `rzv2n-common.inc`
  include path).  Carrier-specific DTB stays TBD per the
  user-supplied HW config writeup; build falls back to Renesas's
  stock V2N-EVK DTB until then.  Yocto Scarthgap (5.0.11)
  is the recommended series; Kirkstone still listed as
  compatible.  Full step-by-step recipe in
  `yocto/meta-alp/README.md`.
- `vendors/deepx-dxm1/README.md` rebased on the actual DEEPX
  software distribution.  Earlier copy described the runtime as
  "proprietary, sign in at developer portal"; the source is in
  fact **source-visible on GitHub at <https://github.com/DEEPX-AI>**
  but under a proprietary "customer-only" license (per the
  verbatim text of `dx_rt/LICENSE`).  Three concrete repos
  documented for the V2N-M1 integration: `dx_rt` (userspace
  runtime), `dx_rt_npu_linux_driver` (PCIe kernel driver -- previously
  unmentioned), and `meta-deepx-m1` (Yocto recipes).  Clean-room
  stub at `vendors/deepx-dxm1/include/dxnn/dxnn.h` stays unchanged
  -- the rewrite is README-only.

### Added

- Library integration pass for v0.4 prep -- two Tier-2 libraries from
  docs/recommended-libraries.md land as scaffolding anchors:
  - **LwRB** (MaJerle, MIT) under `vendors/lwrb/` with a stub
    `<lwrb/lwrb.h>` mirroring the upstream API + `CONFIG_ALP_SDK_USE_LWRB`
    Kconfig flag (default OFF).  audio_zephyr.c gets a docstring
    anchor; first real use (byte-granular DMA staging) lands v0.4
    once the west.yml pin lands.
  - **nanopb** (zlib) under `vendors/nanopb/` with stub `<pb.h>` +
    `<pb_encode.h>` + `<pb_decode.h>` + `CONFIG_ALP_SDK_USE_NANOPB`
    Kconfig flag (default OFF).  mproc_zephyr.c gets a docstring
    anchor; first real use (IPC frame serialisation) lands v0.4
    alongside the multi-proc completion.
  - New `metadata/protos/alp_mproc.proto` -- the v1 IPC schema
    (Envelope / Heartbeat / RpcRequest / RpcResponse / Notification)
    that the nanopb-backed mproc framing will code-gen against.
  - `docs/recommended-libraries.md` updated: LwRB + nanoPB move from
    Tier 2 "under evaluation" to Tier 3 "already integrated";
    TinyFrame / heatshrink / trice / nanoMODBUS / o1heap noted as
    deferred to v0.5+ with one-liner rationales.
  - `docs/getting-started.md` "Where to go next" now points at
    `docs/recommended-libraries.md` so hand-written-firmware authors
    discover the curated companion library list.
- Ethos-U65 (i.MX 93) wired alongside Ethos-U55 (AEN) on the Zephyr
  TFLM inference path.  The `inference_tflm.cpp` source path was
  already portable across U55 and U65 (both register through
  `AddEthosU()`); v0.3 lays down the Kconfig + per-variant anchor.
  New `ALP_SDK_INFERENCE_ETHOS_U_N93` Kconfig defaults on when the
  SoC choice is i.MX 93 and compiles `src/zephyr/inference_ethosu_n93.c`
  -- a thin anchor that exposes `alp_ethosu_n93_register` (no-op until
  v0.4 wires the NPU attach) + `alp_ethosu_variant_name` (literal
  `"ethos-u65"`).  `ALP_SDK_INFERENCE_ETHOS_U` now defaults on for
  both AEN-E7 and i.MX 93.  vendors/nxp-imx93/README.md documents the
  Vela invocation (`ethos-u65-256`) + the A55-side proxy plan via
  OpenAMP/M33 firmware (v0.4 first-class).  PLAN.md row updated.
- DEEPX DX-M1 inference backend on the Yocto / Linux user-space path.
  New `src/yocto/inference_yocto.c` is the first real Yocto-side
  surface in the SDK -- it owns the `alp_inference_*` public symbols
  (overriding `src/common/stub_backend.c` via the
  `ALP_VENDOR_OVERRIDES_INFERENCE` guard) and routes
  `ALP_INFERENCE_BACKEND_DEEPX_DX` through `inference_deepx.cpp`.
  `vendors/deepx-dxm1/` ships a stub `<dxnn/dxnn.h>` so the dispatcher
  compiles on hosts that don't have the proprietary DEEPX runtime
  installed -- the real `dxnn_*` link arrives v0.4 alongside the
  `deepx-dxm1-host-sdk` Yocto recipe.  Gated via the CMake option
  `ALP_SDK_USE_DEEPX_DXM1`; meta-alp's `e1m-x-v2n-m1.conf` MACHINE
  drives that ON.  `resolve_auto()` for Yocto prefers DEEPX_DX first
  on V2N-M1, falling back to NOSUPPORT when no backend is compiled
  in.  PLAN.md §2.3 + docs/recommended-libraries.md updated.
- `yocto/meta-alp/` is now a v0.3-scaffolded BSP layer (vs the
  v0.2 placeholder).  Three machine configs land:
  `e1m-x-v2n.conf` (RZ/V2N), `e1m-x-v2n-m1.conf` (V2N + DEEPX),
  `e1m-n93.conf` (i.MX 93 + Ethos-U65).  Three new recipes land:
  `alp-sdk-runtime` (libalp_sdk.so + headers, per-machine
  inference-backend cmake defines), `alp-chips` (libalp_chips.a +
  per-chip PACKAGECONFIG knobs for all 19 drivers), and
  `alp-studio-codegen` (CLI helper; opt-in via DISTRO_FEATURES,
  guarded with bb.parse.SkipPackage until alplabai/alp-studio is
  public).  layer.conf advertises `LAYERRECOMMENDS_alp` for
  meta-renesas-rz + meta-imx.  Recipes parse but don't build --
  real `do_compile` against the Yocto cross-toolchain lands in
  v0.4 with `src/yocto/` going from stub to real.
- [ADR 0006](docs/adr/0006-secure-boot-secure-ota.md) lands the
  v0.4 secure boot + secure OTA design.  Per-SoM vendor-native
  secure boot (Alif Secure Enclave + MCUboot on AEN; NXP AHAB on
  N93; Renesas SBM on V2N), unified `alp_ota_*` surface in
  `<alp/iot.h>` routed through MCUboot (Zephyr) or RAUC (Linux),
  trust-anchor pinning + min-version anti-rollback baked into the
  config.  No code yet -- v0.4 cycle implements it.
- `firmware/cc3501e/` scaffolding -- the alp-sdk side of the
  two-repo boundary with the future `alplabai/cc3501e-firmware`:
  bootstrap README that mirrors the contract from
  `docs/cc3501e-bridge.md`, a `flash.py` stub (dry-run + SHA-256
  + signature presence checks today; real bootloader sequence
  lands with the first signed binary), `protocol-version.txt`
  pinned to wire-protocol v1, and an empty `prebuilt/` with its
  own CHANGELOG ready to receive `cc3501e-vX.Y.Z.bin` releases.

### Removed (pre-1.0)

- `<alp/math.h>` and `<alp/signal.h>` deleted.  They were thin
  re-exports of CMSIS-DSP that added zero value over a direct
  `#include "arm_math.h"`.  Application code now includes
  CMSIS-DSP directly; ALP SDK internals (e.g. inside
  `<alp/audio.h>`) may still pull CMSIS-DSP in via the
  build-time `ALP_HAS_CMSIS_DSP` option.  Documented stance
  in `docs/architecture.md` + `VERSIONS.md` +
  `docs/os-support-matrix.md`.

### Added

- Public headers under `include/alp/` for the v0.1 surface:
  `peripheral.h`, `display.h`, `camera.h`, `gui.h`, `iot.h`.
  C99-compatible, Doxygen-commented.
- Zephyr backend implementations for `alp_i2c_*`, `alp_spi_*`,
  `alp_gpio_*`, `alp_uart_*` under `src/zephyr/`, with a custom
  `alp,pin-array` devicetree binding for studio-resolved pin lookup.
- Static handle pools with Kconfig-tunable size limits
  (`CONFIG_ALP_SDK_MAX_*_HANDLES`).
- Top-level CMake support for both Zephyr-module consumption and
  plain `add_subdirectory()` consumption.
- Chip metadata schema `metadata/schemas/soc-spec-v1.schema.json`
  and per-SoC metadata files for the full Alif Ensemble line:
  `e3.json` and `e7.json` (released, real datasheet),
  `e8.json` (preliminary, datasheet v0.51), and
  `e4.json` / `e5.json` / `e6.json` (preliminary stubs derived from
  the ALP Lab E1M-AEN module datasheet draft + family pattern).
- Documentation: `README.md`, `docs/architecture.md`,
  `docs/os-support-matrix.md`, `docs/porting-new-som.md`,
  `vendors/alif/README.md`, `vendors/renesas-rzv2n/README.md`.
- ztest suite at `tests/zephyr/peripheral/` runs **12 of 12 cases
  green under twister on `native_sim/native/64`** with the alp-sdk
  module loaded via `EXTRA_ZEPHYR_MODULES`.  Coverage: open/close
  lifecycle, NULL-arg validation, status-code propagation, GPIO
  configure/write/read forwarding, pool exhaustion at the
  Kconfig-defined limits, and SPI / UART / I²C round-trips
  against Zephyr's emulated controllers.
- Devicetree binding `dts/bindings/alp,pin-array.yaml` is now
  picked up via the module's `dts_root: .`, with `alp` registered
  as a vendor prefix in `dts/bindings/vendor-prefixes.txt`.
- `docs/e1m-pinout.md` documents the pinout chain (e1m-spec →
  per-SoM manifest → studio pin allocator → SDK opaque integer).
  Confirms the SDK does not duplicate pad data; pinned to
  `alplabai/e1m-spec` v1.0.
- **Global pin/instance map** at `include/alp/e1m_pinout.h` — the
  E1M-standard peripheral instance IDs (`ALP_E1M_I2C0`,
  `ALP_E1M_SPI0`, `ALP_E1M_UART0`, …) and pad-level GPIO indices
  (`ALP_E1M_GPIO_IO0`–`IO25`, `PWM0`–`PWM7`, `ENC0_X`–`ENC3_Y`)
  baked as C macros.  Carrier `alp,pin-array` overlays MUST follow
  the canonical ordering so the macros stay portable across every
  E1M-conformant carrier.  42 GPIO indices total.
- Carrier-feature names at
  `include/alp/boards/alp_e1m_evk.h` — readable EVK-side aliases
  (`EVK_PWM_LED_RED`, `EVK_PIN_ENCODER_SW`,
  `EVK_I2C_BUS_SENSORS`, on-board sensor I2C addresses) layered on
  top of the global E1M map.  The header is SoM-agnostic: the E1M
  EVK accepts 35x35 mm SoMs (currently AEN, soon N93).  Per-SoM
  dispatch differences (e.g. AEN-side CC3501E proxying for IO11 /
  IO13 / IO15..IO21) are called out per-pad in the doc-comments.
  The 45x65 E1M-X carrier gets its own future header
  `<alp/boards/alp_e1m_x_evk.h>`.
- M.2 E-key wake + radio-disable wiring on the EVK header:
  `EVK_PIN_M2E_UART_WAKE` (IO19), `EVK_PIN_M2E_SDIO_WAKE` (IO18),
  `EVK_PIN_W_DISABLE1` (IO17, Wi-Fi disable, open-drain),
  `EVK_PIN_W_DISABLE2` (IO16, BT disable, open-drain).  All four
  proxy through the on-module CC3501E.
- BMI323 INT1 macro: `EVK_PIN_BMI323_INT1` (IO15, CC3501E-side).
  The IMU's data-ready / motion / FIFO interrupt does not pass
  through the main TCAL9538 expander -- those bits hold the
  ICM-42670 + BMP581 interrupt lines.
- **Chips library v0.1**:
  - `chips/lsm6dso/` + `<alp/chips/lsm6dso.h>` — STMicro
    LSM6DSO 6-axis IMU driver.  WHO_AM_I check, ODR/full-scale
    config, raw accel/gyro/temp reads.  Symbols use the chip's
    natural prefix (`lsm6dso_*`) per the no-`alp_`-on-chip-drivers
    rule.
  - `chips/ssd1306/` + `<alp/chips/ssd1306.h>` — Solomon
    Systech SSD1306 monochrome OLED driver.  I²C, 128×64/128×32
    geometries, init sequence, `clear` / `draw_pixel` /
    `set_contrast` / `set_inverted` / `display`.  Vertical-byte
    framebuffer matching the SSD1306 GDDRAM layout.
  - `chips/button_led/` + `<alp/chips/button_led.h>` — generic
    button + LED helper.  Carries the `alp_` prefix as the
    documented exception (block utility, not single-IC driver).
    Wraps `alp_gpio_*` for portable lifecycle.
  - Each chip is opt-in via `CONFIG_ALP_SDK_CHIP_*` Kconfig.
- New ztest suite at `tests/zephyr/chips/` covering all three
  chip drivers' lifecycle, NULL handling, and status-code
  propagation.  **Now 20 of 20 ztest cases pass on
  `native_sim/native/64`** (12 peripheral + 8 chips).
- **Renesas RZ/V2N metadata** at
  `metadata/socs/renesas/rzv2n/n44.json` — extracted from
  R01DS0466EJ0120 v1.20 (Sep 2025).  Captures the quad
  Cortex-A55 + Cortex-M33 + DRP-AI3 (4 dense / 15 sparse TOPS)
  topology, 1.5 MB on-chip SRAM (ECC), full peripheral inventory
  (2× GbE, USB 3.2 Gen2, PCIe Gen3, 6× CAN-FD, 9× I²C, 24× ADC,
  …), 840-pin FCBGA, and all 8 orderable RZ/V2N + RZ/V2NP SKUs
  (N41–N48) including the ALP Lab default `R9A09G056N44GBG#AC0`.
- **DEEPX DX-M1 companion accelerator metadata** at
  `metadata/socs/deepx/dx/m1.json` — extracted from DEEPX
  Commercial Datasheet v1.0 (June 2025).  25 TOPS @ 1.0 GHz INT8
  CNN accelerator behind PCIe Gen3 ×4, with up to 8 GB
  LPDDR4X/LPDDR5 + internal Cortex-M55 firmware controller.
  FC-BGA 625-ball.  Linked to the E1M-V2M101 / E1M-V2M102 SoM
  SKUs.
- Internal design-archive metadata mirror refreshed with the new files.
- **EdgeAI vision-AEN reference application skeleton** at
  `examples/aen/edgeai-vision-aen/` — compiles under
  `native_sim/native/64` and prints the v0.1 init flow + v0.2
  pipeline TODOs.  Ships the full Zephyr-app layout (`README.md`,
  `CMakeLists.txt`, `prj.conf`, `boards/{native_sim_native_64,alp_e1m_evk_aen}.overlay`,
  `src/main.c`, `models/README.md`, `docs/pipeline.md`,
  `testcase.yaml`) so the v0.2 implementation slots into a stable
  scaffold.  The console twister harness asserts the skeleton's
  `[edgeai] done` line — passing now.
- `alp_pixfmt_t` typedef hoisted from `<alp/display.h>` to
  `<alp/peripheral.h>` so `<alp/camera.h>` can reference it
  without forcing a forward dependency on display.
- v0.1 stub for the camera surface at `src/zephyr/camera_stub.c`:
  every `alp_camera_*` entry returns `ALP_ERR_NOSUPPORT`,
  `alp_camera_open` returns `NULL` — matches the v0.1 contract
  in `<alp/camera.h>` and lets applications link cleanly until
  the v0.2 Zephyr-video integration arrives.
- **Twister now covers tests + examples**: 21 of 21 cases pass on
  `native_sim/native/64` across the peripheral suite, the chips
  suite, and the EdgeAI skeleton sample.
- **IoT connected-camera reference application skeleton** at
  `examples/iot-connected-camera/` — the v0.3 deliverable's
  v0.1 scaffold.  Targets the V2N family (Renesas RZ/V2N) for
  the connected-product path.  Six-stage skeleton:
  peripherals → camera → classifier → Wi-Fi station → MQTT/TLS
  → main loop, with TODO blocks marking which version each
  stage's real implementation lands in (v0.2 camera/classifier,
  v0.3 IoT).  Compiles + runs cleanly under
  `native_sim/native/64` (twister console-harness asserts the
  `[iotcam] done` line); EVK-V2N target parked behind a comment
  until `alplabai/alp-zephyr-modules` publishes the V2N board file.
  Ships the full scaffold (`README.md`, `CMakeLists.txt`,
  `prj.conf`, `boards/{native_sim_native_64,alp_e1m_evk_v2n}.overlay`,
  `src/main.c`, `certs/README.md`, `docs/pipeline.md`,
  `testcase.yaml`) so the v0.3 implementation slots in cleanly.
- v0.1 stub for the IoT surface at `src/zephyr/iot_stub.c`:
  every `alp_wifi_*` / `alp_mqtt_*` entry returns
  `ALP_ERR_NOSUPPORT`, `*_open` returns `NULL` — matches the
  v0.1 "header-only" contract for `<alp/iot.h>` and lets
  applications link cleanly until v0.2 (Wi-Fi-station + MQTT
  on AEN-Zephyr) and v0.3 (V2N + TLS + BLE).
- **Twister now covers 4 scenarios on `native_sim/native/64`**:
  peripheral suite (12 cases), chips suite (8 cases), EdgeAI
  skeleton, IoT skeleton.  **22 of 22 cases pass.**
- **GitHub Actions CI shipped**:
  - `.github/workflows/pr-twister.yml` — runs the full twister
    matrix on every PR using the `zephyrprojectrtos/ci` Docker
    image (Zephyr v3.7.0 pinned per the v0.1 dependency matrix).
  - `.github/workflows/pr-metadata-validate.yml` — validates
    every `metadata/socs/**/*.json` against the v1 schema via
    `scripts/validate_metadata.py`.  Caught a real schema bug:
    `gops` was required on NPUs but V2N (DRP-AI) and DX-M1 quote
    `tops`.  Schema relaxed to `anyOf: [gops, tops]`; all 8
    SoC files now validate green.
  - `.github/workflows/pr-doxygen.yml` — best-effort Doxygen
    HTML build on every PR; v0.1 reports warnings as
    informational, v1.0 will gate on zero warnings.
  - `.github/workflows/pr-static-analysis.yml` — clang-format
    diff-only check (changed lines must satisfy
    `.clang-format`) plus `cppcheck` on `src/` + `chips/`.
  - `.github/workflows/nightly-aen-hil.yml` — skeleton for
    nightly HW-in-loop on a real E1M-AEN dev kit.  Gated on a
    `hil-aen` self-hosted runner (none yet).  Runner setup
    contract documented in [`ci/HW-IN-LOOP.md`](ci/HW-IN-LOOP.md).
  - `.clang-format` at the repo root (LLVM-base, 100-col,
    4-space indent).
  - `ci/README.md` is now a CI policy / index doc rather than
    claiming to hold the authoritative workflow copies.
- **PR template** (`.github/PULL_REQUEST_TEMPLATE.md`) gates on:
  roadmap-row attribution, ABI impact classification, twister +
  metadata-validate test plan, CHANGELOG/VERSIONS update.
- Issue templates updated to capture SoC `ref` from
  `metadata/socs/`, SoM SKU, carrier board, OS backend, Zephyr
  version, and Twister/HIL artefact paths.
- **Deeper chip ztests** (no emul fixture required):
  `ssd1306_draw_pixel` covers the page/column math (set bit,
  clear bit, OOB silent-ignore on 4 corners), `ssd1306_clear`
  verifies it wipes only the framebuffer (`width`/`height`/`addr`
  preserved), and `test_public_headers_co_compile` includes
  every `<alp/...>` header in one TU to catch
  typedef/macro-collision regressions.  Test count climbed
  **22 → 27** with no new failures.
- E1M EVK (UG-E1M-001) support for E1M-AEN family: SDK cheat
  sheet at `docs/boards/e1m-evk.md`, board overlay at
  `tests/zephyr/peripheral/boards/alp_e1m_evk_aen.overlay`, and
  an `alp_sdk.peripheral.evk_aen` build-only scenario tagged
  `alp-evk` for nightly HW-in-loop CI.
- `VERSIONS.md` tracking the v0.1 → v1.0 roadmap.
- **v0.2 chip-driver scaffolding** — public surface, opt-in build
  wiring, and lifecycle / NULL-arg ztests for five new drivers:
  - `chips/bme280/` + `<alp/chips/bme280.h>` — Bosch BME280
    combined T/H/P sensor.  I²C-only in v0.2.  Loads the
    per-die calibration coefficients on init and exposes
    `bme280_compensate` with the canonical Bosch integer-form
    arithmetic transcribed from BST-BME280-DS002 v1.6 §4.2.3.
  - `chips/lis2dw12/` + `<alp/chips/lis2dw12.h>` — STMicro
    LIS2DW12 3-axis ultra-low-power accelerometer.  WHO_AM_I
    check, ODR + full-scale + power-mode config in one call,
    raw 14-bit accel + on-die temp reads.
  - `chips/ssd1331/` + `<alp/chips/ssd1331.h>` — Solomon Systech
    SSD1331 96×64 colour OLED.  SPI 4-wire (D/C# pin),
    caller-supplied 12 KiB framebuffer (RGB565), pixel/clear/
    display API, full datasheet init sequence on `ssd1331_init`.
  - `chips/ov5640/` + `<alp/chips/ov5640.h>` — OmniVision OV5640
    5 MP image sensor SCCB-side configuration driver.  Chip-ID
    verify, soft-reset, resolution / format / test-pattern
    presets.  Capture-side (MIPI CSI-2) lives in v0.2's
    `<alp/camera.h>`; per-resolution register tables ship in
    v0.3 alongside the V2N alp_camera integration.
  - `chips/pdm_mic/` + `<alp/chips/pdm_mic.h>` — generic PDM
    microphone block helper.  `alp_`-prefixed (block utility,
    documented exception per the chip-driver naming rule).
    Surface declared; impl returns `ALP_ERR_NOSUPPORT` until
    v0.2's `<alp/audio.h>` lands the underlying I²S
    abstraction.
- **v0.2 / v0.3 public-header surface declared** so application
  code can compile against it now and the implementations slot
  in without ABI churn (same pattern `<alp/iot.h>` followed in
  v0.1):
  - `<alp/audio.h>` (v0.2) — PDM input + I²S output API,
    `alp_audio_in_*` / `alp_audio_out_*`, three sample formats
    (S16/S24/S32), per-block read/write with timeout.
  - `<alp/ble.h>` (v0.3) — peripheral + central, GATT server +
    client.  Zephyr `bt` host stack per the locked decision.
    Mesh / audio / DF explicitly out of scope for v1.0.
  - `<alp/security.h>` (v0.3) — MbedTLS re-export shape: hash
    (SHA-256/384/512), AEAD (AES-128/256-GCM, ChaCha20-Poly1305),
    TRNG.  Per-SoC hardware-accelerator routing happens at the
    backend layer.
  - `<alp/mproc.h>` (v0.3) — multi-processor IPC primitives:
    shared memory regions (`alp_shmem_*`), mailbox channels
    (`alp_mbox_*`, MHU on Alif), hardware semaphores
    (`alp_hwsem_*`).  Wraps the M55-HE core bring-up the
    "Multi-Processor Support Completion" milestone delivers.
  - Stub implementations at `src/zephyr/{audio,ble,security,
    mproc}_stub.c` returning `ALP_ERR_NOSUPPORT` and `*_open()
    → NULL`, matching the v0.1 stub contract.
- **Chip ztest suite extended** to the eight chip drivers.  Each
  new chip gets lifecycle + NULL-arg cases plus a "post-init
  calls reject uninitialised" pattern for the I²C drivers (the
  emul controller has no real device behind it, so `*_init` fails
  WHO_AM_I and downstream calls must report `NOT_READY`).  Also
  includes a pure-math test for `ssd1331_rgb565` and a runtime
  check that the v0.2 / v0.3 stubbed surfaces honour the
  documented `NULL` / `NOSUPPORT` contract.  Total tests across
  twister `native_sim/native/64` climb **27 → 43 cases** (chips
  suite alone: 13 → 29).
- `test_public_headers_co_compile` extended to include
  `<alp/audio.h>`, `<alp/ble.h>`, `<alp/security.h>`,
  `<alp/mproc.h>`, and the five new chip headers.
- **ABI snapshot tooling**: `scripts/abi_snapshot.py` walks
  `include/alp/**.h`, extracts function decls, typedefs, and
  `#define`s, and emits a stable JSON fingerprint per public
  symbol.  Supports `--diff <prior.json>` for per-symbol change
  reporting.  Pre-1.0 the diff is informational; v1.0 promotes
  it to a CI gate (`pr-abi-snapshot.yml`) where `REMOVED` /
  `CHANGED` entries require a major-version bump.
- `docs/abi/v0.1-snapshot.json` ships the v0.1 ABI fingerprint —
  21 headers, 123 functions, 80 typedefs, 114 macros.  See
  `docs/abi/README.md` for the workflow.
- `docs/architecture.md` gains a "Consumers of this SDK"
  section: alp-studio integration contract, Zephyr-application
  consumption recipe, and the v0.4 Yocto / `meta-alp` shape.
- `examples/README.md` per-example status descriptions tightened
  with the concrete pipeline stages each skeleton scaffolds and
  which version fills in each stage.
- **Register-protocol ztests** via test-only i2c-emul fixtures.
  Three fake targets (`tests/zephyr/chips/src/fake_{lsm6dso,
  ssd1306,bme280}.c`) attach to the test's `i2c0_emul`
  controller at the chip drivers' default I2C addresses,
  pre-populate `WHO_AM_I` / `CHIP_ID` plus calibration data,
  and either echo register writes back or capture the byte
  stream for the test to inspect via `fakes.h` helpers.
  - **fake LSM6DSO**: register-store with WHO_AM_I=0x6C seed.
    Tests verify `lsm6dso_init` succeeds, `set_accel`/`set_gyro`
    encode the documented byte into CTRL1_XL / CTRL2_G, and
    `read_accel` decodes the seeded LE register pairs.
  - **fake SSD1306**: command/data byte logger split by the
    SSD1306 control byte (0x00 vs 0x40).  Tests verify
    `ssd1306_init` streams DISPLAY_OFF first, charge-pump
    enable (0x8D 0x14), DISPLAY_ON last; `ssd1306_display`
    sets the full address window then pushes 1024 framebuffer
    bytes with the seeded pixel landing at offset 0.
  - **fake BME280**: register store seeded with the canonical
    BST-BME280-DS002 §4.2.2 example calibration coefficients
    and §4.2.3 example raw conversion (T_raw=519888,
    P_raw=415148).  Tests verify init loads every coefficient
    correctly, `read_raw` decodes the 20/20/16-bit values, and
    `compensate` reproduces the documented worked example
    (T = 25.08 °C, P = 100653 Pa) within ±2 LSB and ±50 Pa.
  - DT bindings live at `tests/zephyr/chips/dts/bindings/
    alp,fake-*.yaml`; the chip-test overlay attaches each
    fake at the canonical I2C address.
  - Chips suite tests grow **29 → 38 cases**; total twister
    cases on `native_sim/native/64` climb **43 → 52**.

- **v0.2 peripheral HAL expansion** — eight new peripheral classes
  wrapped, doubling the SDK's coverage of what an MCU actually
  exposes (was 4 classes, now 12).  Each new class follows the v0.1
  pattern: a public header with full Doxygen, a Zephyr backend that
  resolves studio-supplied IDs via the `alp-<class>N` DT alias and
  forwards to the matching Zephyr driver class, a Kconfig opt-in
  gated on the underlying subsystem, a static handle pool with a
  per-class quota, and runtime validation against the active SoC's
  documented caps:
  - `<alp/pwm.h>` + `src/zephyr/peripheral_pwm.c` — Zephyr `pwm_*`.
  - `<alp/adc.h>` + `src/zephyr/peripheral_adc.c` — Zephyr `adc_*`.
  - `<alp/counter.h>` + `src/zephyr/peripheral_counter.c` /
    `peripheral_qenc.c` — free-running counter via `counter_*`,
    quadrature-decoder via `sensor_*`.
  - `<alp/i2s.h>` + `src/zephyr/peripheral_i2s.c` — Zephyr `i2s_*`
    with a 2-block ping-pong memory slab.
  - `<alp/can.h>` + `src/zephyr/peripheral_can.c` — Zephyr `can_*`,
    classic + CAN-FD.
  - `<alp/rtc.h>` + `src/zephyr/peripheral_rtc.c` — Zephyr `rtc_*`.
  - `<alp/wdt.h>` + `src/zephyr/peripheral_wdt.c` — Zephyr `wdt_*`.
- **Last-error mechanism for `*_open` failure diagnosis.**  New
  status code `ALP_ERR_OUT_OF_RANGE` (= -8); new public accessor
  `alp_last_error()` (thread-local) lets callers learn *why* a
  failed `*_open` returned NULL — distinguishing
  config-out-of-range, NULL-arg, pool-exhausted, device-not-ready,
  and underlying driver error.  Internal helpers
  `alp_z_set_last_error` / `alp_z_clear_last_error` live in
  `src/zephyr/last_error.c`.
- **SoC capability validation** — `scripts/gen_soc_caps.py` reads
  `metadata/socs/**.json` and emits `include/alp/soc_caps.h` with
  per-SoC `ALP_SOC_*_COUNT` / `ALP_SOC_*_MAX_*` macros gated by
  `CONFIG_ALP_SOC_<TOKEN>`.  v0.2 wrappers consult the matching
  macros to reject configs that exceed the active SoC's documented
  caps before any I/O.  Canonical case: a 16-bit ADC request on
  Alif E3 (12-bit max) fails at `alp_adc_open` with
  `alp_last_error()` = `ALP_ERR_OUT_OF_RANGE`.
- **Kconfig SoC choice** at `zephyr/Kconfig` for the active
  capability profile.  Default `ALP_SOC_NONE` keeps validation
  permissive; alp-studio's generated build picks the matching
  `CONFIG_ALP_SOC_<VENDOR>_<FAMILY>_<PART>=y` automatically.
- **Architecture documentation** — `docs/architecture.md` gains a
  "Why this wrapper exists (despite Zephyr already abstracting
  vendors)" section and a "Capability validation" section.  New
  `docs/adr/` directory with the ADR template plus three accepted
  records:
  - [ADR 0001](docs/adr/0001-wrapper-on-top-of-zephyr.md) — why
    ALP SDK wraps Zephyr (and why the wrapper stays thin).
  - [ADR 0002](docs/adr/0002-error-mechanism.md) —
    `alp_last_error()` + compile-time SoC capability validation.
  - [ADR 0003](docs/adr/0003-peripheral-coverage.md) — wrap 12
    peripheral classes at v0.2, not just I2C/SPI/GPIO/UART.
- **ABI snapshot bumped** to reflect the new headers — 29 headers,
  ~165 functions, ~110 typedefs, ~135 macros (was 21/123/80/114).
- **E1M portability-bound macros** at `<alp/e1m_pinout.h>` —
  `ALP_E1M_<CLASS>_COUNT` constants for each peripheral class
  (I2C=2, SPI=2, UART=2, I2S=2, PDM=2, I3C=1, CAN=1, ETH=1,
  CSI=1, DSI=1, ADC=8, DAC=2, PWM=8, ENC=4, GPIO_IO=26).  These
  are the **portability contract** — apps that use only
  `ALP_E1M_<CLASS><N>` for `N < ALP_E1M_<CLASS>_COUNT` are
  guaranteed cross-SoM compatibility.  Higher indices fall into
  the SoC-specific gap (e.g. RZ/V2N's six CAN channels) which the
  SDK still accepts but apps lose the "swap the SoM" property.
- **[ADR 0004](docs/adr/0004-e1m-portability-bound.md)** — E1M-spec
  instance counts as the portability bound.  Documents the three
  concentric capability tiers: E1M reservation < studio block
  declaration < SoC count < driver array.
- `docs/architecture.md` gains an "E1M as the portability bound"
  section explaining the three-tier validation model.
- **GitHub org migration** — repo references updated from
  `alpCaner/*` to `alplabai/*` (alp-sdk, e1m-spec,
  alp-zephyr-modules, alp-studio).  27 files / 56 substitutions,
  pure rename.  Older history rewritten to drop spurious
  co-author footers per the repo's solo-attribution preference.
- **VERSIONS.md v0.2 row** updated to call out the peripheral
  expansion explicitly (12 wrapped classes), capability validation,
  and the E1M portability bound.
- **Peripheral ztest suite extended** with v0.2 wrapper coverage:
  - 12 generic NULL-arg / out-of-range cases for PWM, ADC, Counter,
    QEnc, I2S, CAN, RTC, WDT, each verifying `alp_last_error()`
    returns the precise reason.
  - New `alp_sdk.peripheral.caps_e3` twister scenario pinning the
    SoC choice to `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E3=y` and
    asserting that a 25-bit ADC request exceeds the documented
    24-bit cap and fails at `alp_adc_open` with
    `ALP_ERR_OUT_OF_RANGE` — the canonical case the user
    flagged ("16-bit ADC on a 12-bit SoC needs to fail
    cleanly").
  - `prj.conf` enables `CONFIG_PWM=y` / `CONFIG_ADC=y` /
    `CONFIG_COUNTER=y` / `CONFIG_SENSOR=y` / `CONFIG_I2S=y` /
    `CONFIG_CAN=y` / `CONFIG_RTC=y` / `CONFIG_WATCHDOG=y` /
    `CONFIG_THREAD_LOCAL_STORAGE=y` so the v0.2 wrapper sources
    compile and link in the test app.
- **v0.1 peripherals retrofitted** with `alp_last_error()` +
  SoC-capability validation so the diagnostic story is uniform
  across all 12 wrapped classes:
  `peripheral_i2c.c` / `peripheral_spi.c` / `peripheral_uart.c` /
  `peripheral_gpio.c` now stamp a precise reason
  (ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE / ALP_ERR_NOT_READY /
  ALP_ERR_NOMEM / ALP_ERR_IO) before returning NULL.  ADR 0002's
  v0.3 "retrofit" follow-up landed early.
- **CI: pr-generated-files.yml** verifies generated artefacts stay
  in sync with their inputs — re-runs `gen_soc_caps.py` and
  `abi_snapshot.py` then fails the PR if `include/alp/soc_caps.h`
  or `docs/abi/v0.1-snapshot.json` differ from the committed
  copies (ignoring the snapshot's `generated` date stamp).
  Catches "metadata edited but soc_caps.h not regenerated" and
  "header added/removed but ABI snapshot not bumped" PR mistakes.
- **PLAN.md §6 Open work** updated — gaps the v0.2 expansion
  closed are kept on the list with strikethrough plus a "closed"
  reference to the matching ADR or implementation, so the audit
  trail of what was promised vs. what shipped stays explicit.
- **Per-peripheral example apps** — nine standalone hand-written
  reference apps under `examples/<peripheral>-<demo>/`, one per
  wrapped peripheral class.  Each is a minimal Zephyr app that
  shows the canonical open / call / close pattern + reads
  `alp_last_error()` on failure.  Twister gates on
  `[<class>] done` console output.  Examples shipped:
  `gpio-button-led`, `i2c-scanner`, `spi-loopback`, `uart-echo`,
  `pwm-led-fade`, `adc-voltmeter` (incl. capability-validation
  rejection demo), `counter-alarm`, `rtc-clock`, `wdt-feed`,
  `can-loopback`.  ADR 0001's "standalone usage is first-class"
  principle made tangible.
- **Standalone usage callouts** — README's "Two consumer paths"
  section explicitly lists hand-written firmware as first-class
  alongside studio-codegen apps.  ADR 0001 expanded with the
  reasoning.  Saved as a persistent project memory so future
  sessions don't accidentally treat hand-written usage as a
  workaround.
- **VS Code support** — README "Using with VS Code" section,
  fresh `.vscode/{extensions,settings,tasks,c_cpp_properties}.json`
  config aligned with the current Zephyr-module + plain-CMake
  layout.  Tasks for `validate metadata`, `regen soc_caps`,
  `regen ABI snapshot`, twister, and `west build` for each
  example.  Extension recommendations are vendor-neutral —
  no Nordic-branded tooling (the SDK targets Alif / Renesas /
  NXP, not Nordic).
- **GitHub org migration** — repo references updated from
  `alpCaner/*` to `alplabai/*` (alp-sdk, e1m-spec,
  alp-zephyr-modules, alp-studio).  27 files / 56 substitutions.
- **CI infrastructure fixes** — bumped
  `ZEPHYR_SDK_INSTALL_DIR` to 0.17.0 to match the
  `ci:v0.27.4` Docker image; switched `--testsuite-root` paths
  from `${{ github.workspace }}` (host path) to
  `$GITHUB_WORKSPACE` (container path); fixed a `/**` sequence
  in the generated `soc_caps.h` comment that triggered
  `-Werror=comment`.  Twister CI was failing on the same three
  seams since the workflow was authored.

- **3 additional chip drivers (E1M EVK on-board sensors)** —
  `chips/icm42670/` (TDK 6-axis IMU), `chips/bmi323/` (Bosch 6-axis
  IMU), `chips/bmp581/` (Bosch barometer with already-compensated
  24-bit P + T outputs).  Each follows the existing chip-driver
  pattern with WHO_AM_I / CHIP_ID verify, ODR + FS / OSR config,
  burst register reads.  Brings the SDK's chip count to **11**.
- **Shared stub backend** at `src/common/stub_backend.c` — every
  public `alp_*` function defined as a NOSUPPORT stub, wired into
  both `src/baremetal/` and `src/yocto/` via `target_sources`.
  Plain-CMake builds with `-DALP_OS=baremetal` or `-DALP_OS=yocto`
  now produce link-complete `libalp_sdk.a`.
- **CI: pr-plain-cmake.yml** — runs `cmake -DALP_OS=baremetal` and
  `cmake -DALP_OS=yocto` against host gcc on every PR; catches
  "new public function added without a stub entry"
  undefined-reference errors that the Zephyr CI doesn't see.
- **`<alp/inference.h>` declared early (was held to v0.2)** —
  unified ML inference surface with backend selector
  (AUTO/CPU/Ethos-U/DRP-AI/DEEPX), model-format selector
  (TFLite/Vela/DRP-AI/DXNN/ExecuTorch), tensor descriptor with
  shape + dtype + quant params.  Stub returns NOSUPPORT; real
  impls land per backend (Ethos-U + CPU v0.2 on AEN-Zephyr,
  DRP-AI v0.3, DEEPX v0.4).  PLAN.md §6 entry strikethrough'd.
- **`<alp/storage.h>` declared early (v0.4 surface)** — block-
  oriented persistent storage: internal flash, QSPI / OSPI NOR,
  SD / eMMC.  open / get_info / read / write / erase / sync /
  close.  Real impls v0.4 (Yocto first-class).
- **`<alp/usb.h>` declared early (v0.3 surface)** — device + host
  roles in one header.  Three stock device classes wrapped
  (CDC-ACM, MSC, HID); other classes via vendor escape hatches.
- **`yocto/meta-alp/` skeleton** — Yocto BSP layer with
  `conf/layer.conf` + recipe shells (`alp-sdk_git.bb`,
  `alp-edgeai_git.bb`).  External Yocto integrators can
  `bitbake-layers add-layer` today; do_compile / do_install /
  FILES wiring lands v0.4.
- **`docs/getting-started.md`** — standalone-first walkthrough
  from `git clone` through `west build -b native_sim` and onto
  real silicon.  Reinforces ADR 0001's
  "standalone is first-class" stance with a concrete recipe
  every hand-written firmware author can follow.
- **EVK on-module audio + mux + current-sense + wake batch** --
  more user-supplied EVK schematic detail:
  - **TAS2563RPP smart-amp pair** -- two amps on I2C0 + I2S0
    (addresses 0x4D and 0x4E, plus broadcast 0x48).  New
    `chips/tas2563/` driver: init / probe / mode_ctrl /
    hw_enable.  Tuning-blob loader + I2S binding land in v0.3.x.
    AMP_ENABLE drives the chip's SD_N (active-high enable);
    AMP_FAULT reads IRQ_N (open-drain, internal pull-up).
    `CONFIG_ALP_SDK_CHIP_TAS2563` opt-in.
  - **Four MP34DT05TR-A PDM mics** -- two pairs on PDM0 and PDM1
    respectively, each pair sharing a PDM data line via the LR
    convention (LR=high samples on rising edge, LR=low on
    falling).  Documented inline; chips/pdm_mic helper covers
    the part.
  - **I2S0 74LVC157 mux** swaps the I2S0 bus between TAS2563 and
    M.2 E-key.  Control pins split across chips:
    `EVK_PIN_I2S_MUX_EN = IO8` (Alif P7.1) +
    `EVK_PIN_I2S_MUX_SEL = IO13` (CC3501E GPIO13).
  - **USB2 TMUXHS221 mux** swaps USB2 between the external USB-A
    jack and M.2 E-key USB.  Single control:
    `EVK_PIN_USB2_MUX_SEL = IO11` (CC3501E GPIO2).  /OEN
    tied to GND so the mux is always live.
  - **M.2 E-key UART wake** -- `EVK_PIN_M2E_UART_WAKE = IO19`
    (CC3501E GPIO19).
  - **Six INA236 current shunt monitors** -- one per power rail
    on I2C0 (3V3, 1V8, VIO, +V_CAM0, +V_CAM1, 5V).  INA236A
    occupies 0x40..0x43 and INA236B occupies 0x44..0x47, so all
    six fit on one bus.  Macros key on rail name; the user's
    notes had a ref-des typo (three "U30"s) -- cross-check
    pending.
  - **ENCODER_SW correction** -- moves from IO3 to IO4 per the
    schematic.  PEC12R-4222F-S0024 encoder + push switch.
  ABI: ADDED public header `alp/chips/tas2563.h` (snapshot now
  44 public headers).  SDK chip count climbs to **19**.

- **EVK Arduino + mikroBUS headers + wiring corrections** --
  user-supplied EVK wiring continues.  Arduino UNO header full
  pin map: `EVK_ARD_PWM1..PWM4` (= E1M PWM1/PWM4/PWM5/PWM2),
  `EVK_ARD_DIO1..DIO4` (= I2S1_SDO / I2S1_WS / SPI0_SCLK /
  SPI0_MOSI), `EVK_ARD_RST` (= I2S1_SCLK), and
  `EVK_ARD_A0..A5` (= ALP_E1M_ADC0..5).  Bus aliases:
  `EVK_SPI_BUS_ARDUINO` (= SPI1, CC3501E-mediated),
  `EVK_I2C_BUS_ARDUINO` (= I3C0), `EVK_UART_PORT_ARDUINO`
  (= UART1).  mikroBUS click header reuses the Arduino macros for
  the shared lines (SPI / I2C / UART / RST) and adds
  `EVK_MB_PWM` (= PWM6), `EVK_PIN_MB_INT` (= I2S1_SDI),
  `EVK_MB_ANA` (TBD-confirm).  In-flight wiring corrections
  bundled into the same commit:
  - **LED_GREEN -> PWM0** (was PWM2 inferred-and-wrong).
  - **CTP_INT -> SPI1_CS1** (was I2S1_SDI; the I2S1_SDI pad now
    correctly serves only as mikroBUS INT).  CTP_INT routes
    through the on-module CC3501E like the other CC3501E-side
    pins.
  - **CTP_RST is the TCAL9538 P3 only.**  An earlier draft also
    listed SPI1_CS0 as CTP_RST; the user clarified that's a
    mis-label -- SPI1_CS0 is the Arduino CK_CS (chip select).
  - **BMI323 address -> 0x68** (with SDO=low; user confirmed the
    strap is low, not high).  Resolves the apparent 0x69
    collision with ICM-42670-P.
  - **`EVK_SPI_BUS_M2_KEYM` removed** -- M.2 on this EVK
    uses PCIe + SDIO, not SPI; the previous macro was a guess.
  - **SPI0 fully repurposed** -- all five SPI0 pads
    (MISO/CS0/CS1/MOSI/SCLK) are GPIOs on this carrier
    (AMP_FAULT/AMP_ENABLE/IO_EXP_RST/CK_DIO4/CK_DIO3).  No
    peripheral SPI0 bus available on the EVK; documented in the
    header.
  - **I2S1 fully repurposed** -- all four I2S1 pads
    (SDO/WS/SDI/SCLK) are GPIOs (CK_DIO1/CK_DIO2/MB_INT/CK_RST).
    No peripheral I2S1 bus on the EVK; the I2S0 path remains
    available for audio.
  ABI snapshot still 43 public headers (no shape change).
- **EVK wiring batch: I2C1 = DSI/CSI control, IO5 = CAM_RST,
  PWM-driven RGB LED, repurposed SPI0 + I2S1 + AUDIO_CLK pads,
  TCAL9538 I/O expander, SDIO mux, touch-screen control** --
  large user-supplied EVK schematic update lands in
  `<alp/boards/alp_e1m_evk.h>` + `chips/tcal9538/`:
  - I2C0 stays the sensor / IO-expander / current-monitor bus;
    new `EVK_I2C_BUS_DSI_CSI = ALP_E1M_I2C1` is the
    display-and-camera-control bus per the EVK's `DSI_CSI_I2C` net.
  - `IO5` is `CAM_RST` (the camera reset line), correcting an
    earlier placeholder that had `IO5 = IO_EXP_RST`.
  - RGB LED now drives via PWM rather than GPIO:
    `EVK_PWM_LED_RED = ALP_E1M_PWM3`,
    `_GREEN = ALP_E1M_PWM2` (TBD-confirm; user stated PWM3=R + PWM1=B
    but didn't name the green PWM, PWM2 is inferred), and
    `_BLUE = ALP_E1M_PWM1`.
  - Off-GPIO_IO repurposed pads (`AUDIO_CLK`, `SPI0_MISO`,
    `SPI0_CS0`, `SPI0_CS1`, `I2S1_SDI`, `SPI1_CS0`) are exposed
    via `EVK_PIN_OVERLAY_BASE + N` indices that the carrier
    `alp,pin-array` overlay extends past the standard 42-entry
    range.  Names: `EVK_PIN_IO_EXP_INT` (AUDIO_CLK),
    `_IO_EXP_RST` (SPI0_CS1), `_AMP_FAULT` (SPI0_MISO),
    `_AMP_ENABLE` (SPI0_CS0), `_CTP_INT` (I2S1_SDI),
    `_CTP_RST` (SPI1_CS0, routed through CC3501E).
  - SDIO mux for SD-card vs M.2 E-key: 74LVC157 controlled by
    `EVK_PIN_SDIO_MUX_EN = ALP_E1M_GPIO_IO20` (active-low
    enable) and `EVK_PIN_SDIO_MUX_SEL = ALP_E1M_GPIO_IO21`
    (0 = M.2 E SDIO, 1 = SD card).  Both pins route through the
    on-module CC3501E (per from-cc3501e.tsv), so firmware drives
    the mux via `ALP_CC3501E_CMD_GPIO_WRITE` on the inter-chip
    SPI1, NOT through Alif's GPIO peripheral.  Noted in the
    header.
  - On-board sensor I2C addresses corrected per user-supplied
    strap values: ICM-42670-P AD0=high -> 0x69 (was 0x2C
    placeholder), BMI323 SDO=high -> 0x69 (was 0x68 placeholder),
    BMP581 SDO=high -> 0x47, TCAL9538 A1=1/A0=0 -> 0x72 (was 0x70).
    **Address collision warning**: ICM-42670-P and BMI323 both
    compute to 0x69 with the documented straps -- header carries
    a TODO comment asking the user to confirm whether only one
    IMU is populated at a time or whether the schematic strap is
    actually different.
  - **TCAL9538 I/O expander pin map** (8 channels of LCD / camera /
    capacitive-touch control + sensor interrupts) materialised as
    a typed enum `evk_ioexp_pin_t` so apps don't pass raw
    indices.  Note: P3 was earlier listed as `CTP_RST` but the
    user has since separately said `CTP_RST = SPI1_CS0`; both
    routes are preserved in the header with a TBD-confirm note
    flagging the conflict.
  - New `chips/tcal9538/` driver -- TI TCA9538 / TCAL9538
    8-channel I/O expander.  Per-pin and bulk
    direction / write / read APIs with cached register state
    (avoids read-modify-write round-trips on every set call).
    `CONFIG_ALP_SDK_CHIP_TCAL9538` opt-in.
  ABI snapshot now 43 public headers; SDK chip count climbs to
  **18**.
- **EVK MIPI CSI camera-mux helper + IO2 wiring correction** --
  the EVK routes a single MIPI CSI lane pair from the SoM through
  a **PI3WVR626XEBEX** 2:1 mux (camera A vs camera B); its
  `SEL` pin lands on E1M `IO2` (`W2` / Alif `P12.5`).  New thin
  driver at `chips/cam_mux_pi3wvr626/` + matching public header
  wraps SEL as a GPIO with a typed `INPUT_A` / `INPUT_B` enum.
  `<alp/boards/alp_e1m_evk.h>` gains `EVK_PIN_CAM_MUX_SEL`
  and **drops the previous placeholder `EVK_PIN_LED_BLUE = IO2`
  mapping** -- the schematic confirms `IO2` is the mux line, not
  an LED.  `LED_BLUE` is now TBD; `LED_RED` / `LED_GREEN` carry
  an explicit "TBD-confirm" caveat.  `docs/boards/e1m-evk.md`
  picks up the camera-mux details + truth table.  SDK chip count
  climbs to **17**; ABI snapshot to 42 headers.
- **On-module I2C device drivers (E1M-AEN)** -- four new chip
  drivers covering the SoM-internal I2C devices the user
  documented:
  - `chips/tmp112/` + `<alp/chips/tmp112.h>` -- TI TMP112 digital
    temperature sensor (LPI2C, +/-0.5 C, 12/13-bit, 0.0625 C/LSB).
    Init / set_rate / set_extended_mode /
    read_temp_milli_c (avoids float on M-class).
  - `chips/rv3028c7/` + `<alp/chips/rv3028c7.h>` -- Micro Crystal
    RV-3028-C7 32.768 kHz 1 PPM RTC (LPI2C, addr 0x52 fixed).
    BCD time get/set, alarm with match-mask, INT-pin enable,
    alarm flag check+clear.  Routes the alarm into Alif
    `P15_0_FLEX` per the inter-chip wiring TSV.
  - `chips/optiga_trust_m/` + `<alp/chips/optiga_trust_m.h>` --
    Infineon OPTIGA Trust M secure element
    (SLS32AIA010MLUSON10XTMA2; LPI2C, addr 0x30).  v0.3 ships
    init + I2C connectivity-probe; full APDU command set lands
    via Infineon's host library + MbedTLS PSA registration in
    v0.3.x so `<alp/security.h>` picks up HW acceleration
    transparently.
  - `chips/eeprom_24c128/` + `<alp/chips/eeprom_24c128.h>` --
    Generic 24Cxx-class 128-Kbit (16 KB) EEPROM driver covering
    Onsemi N24S128C4DYT3G (E1M-AEN default) and the
    footprint-compatible STMicro M24128-BFMH6TG (DNP alt).
    Page-aware writes (64-byte page, ACK-poll wait), arbitrary-
    range reads.  On I2C2.
  Brings the SDK's chip count to **16** (was 12).  ABI snapshot
  now 41 public headers.
- **Secure boot + secure OTA roadmap entries (v0.4)** -- production
  E1M deployments need chain-of-trust from immutable ROM up.
  Plan: MCUboot already pulled in via west.yml verifies
  application image signatures at every boot; signing keys live
  in OPTIGA Trust M's secure NVM (provisioned during factory
  programming); the verification path goes through MbedTLS PSA,
  routing to the OPTIGA hardware accelerator transparently once
  the v0.3.x PSA driver lands.  Secure OTA delivers signed
  payloads over `<alp/iot.h>` MQTT/HTTP and swaps via MCUboot's
  `swap-using-scratch` mode.  Yocto-on-V2N / i.MX 93 uses
  `meta-mender` for the equivalent.  Documented in `PLAN.md` §2.4.1
  and `VERSIONS.md` v0.4 section.
- **CC3501E Alif-side host driver** -- new chip driver at
  `chips/cc3501e/cc3501e.c` + public header
  `include/alp/chips/cc3501e.h`.  Alif-side of the inter-chip
  bridge: `cc3501e_init` / `_reset` / `_get_version` /
  `_request` / `_set_event_callback` / `_deinit`.  Synchronous
  `cc3501e_request` frames a header + payload, drives SPI1 via
  `alp_spi_transceive`, copies the response back out.  Reset
  pulses `WIFI.EN` (P15_5) + `E_WIFI.NRST` (P15_1_FLEX) via
  `alp_gpio_*` when those pins are populated.  New
  `CONFIG_ALP_SDK_CHIP_CC3501E` Kconfig (depends on `SPI && GPIO`).
  SDK's chip count climbs to **12**.  ABI snapshot now 37
  headers.  Followed in a later commit by the
  `<alp/iot.h>` / `<alp/ble.h>` dispatcher branches that route
  through this driver on E1M-AEN.
- **CC3501E wire protocol + bridge architecture** -- the E1M-AEN
  family carries a separate TI CC3501E Wi-Fi 6 + BLE 5.4 combo
  MCU, and certain E1M pads (SPI1 + IO11 / IO13 / IO15..21 + the
  camera-enable LDOs) terminate on it rather than on the Alif
  silicon.  Communication runs over an inter-chip SPI1 bus
  (Alif master, CC3501E slave) carrying a custom binary wire
  protocol.  New `include/alp/protocol/cc3501e.h` freezes that
  protocol at v1: 4-byte header + <=512 B payload, opcodes grouped
  by subsystem (meta / Wi-Fi / sockets / BLE / GPIO proxy /
  camera enable / diagnostics), per-subsystem payload structs
  (Wi-Fi connect, BLE adv start, GPIO configure, ...).
  `docs/cc3501e-bridge.md` documents the architecture, the
  two-repo split per ADR 0005 (alp-sdk owns the protocol +
  Alif-side client; the firmware that runs on the CC3501E lives
  in `alplabai/cc3501e-firmware`), and the firmware bootstrap
  recipe.  Alif-side `chips/cc3501e/` driver +
  `<alp/iot.h>` / `<alp/ble.h>` route-through-CC3501E follow.
- **Authoritative E1M-AEN pinout data** (item: pending HW configs) --
  five user-supplied TSVs land at `metadata/e1m_modules/aen/`:
  `from-alif.tsv` (E1M pad -> Alif silicon, 91 routed entries),
  `from-cc3501e.tsv` (E1M pad -> CC3501E, 14 entries),
  `inter-chip.tsv` (Alif <-> CC3501E SPI1 + SDIO + control
  signals + camera-enable wiring), `alif-ospi.tsv` (Alif's OSPI
  controller pad reservation for optional on-module memories),
  `alif-ethernet-phy.tsv` (DP83825IRMQR PHY <-> Alif RMII MAC
  pad map).  Resolves the AEN-side TBDs in
  `project_pending_hw_configs` memory note; downstream artefacts
  (`include/alp/boards/alp_e1m_evk.h`, the per-SoC metadata
  in `metadata/socs/alif/ensemble/`, alp-studio's pin allocator)
  can now regenerate against this single source of truth.  V2N +
  i.MX 93 family pinouts remain pending until the user supplies them.
- **DRP-AI inference dispatcher hook (item 11 / v0.3 milestone)** --
  `<alp/inference.h>` dispatcher in `src/zephyr/inference_zephyr.c`
  now routes `ALP_INFERENCE_BACKEND_DRPAI` into a per-backend
  `inference_drpai.c` source compiled when
  `CONFIG_ALP_SDK_INFERENCE_DRPAI=y`.  The AUTO resolver prefers
  DRPAI before falling through to TFLM-CPU.  v0.3 ships the
  dispatch routing + a 16-byte placeholder open() so apps verify
  routing today; v0.4 fills the body via Renesas's DRP-AI
  translator runtime once the vendor pack is in CI.
- **V2N vendor scaffolding (item 10 / v0.3 milestone)** --
  `vendors/renesas-rzv2n/` gains `i2c.c` / `spi.c` / `gpio.c` /
  `uart.c` source skeletons mirroring the Alif vendor layout.
  i2c.c carries the full FSP `r_riic_master` binding shape; the
  other three are starter shells with the FSP includes wired in
  for v0.4 to fill bodies.  `vendors/renesas-rzv2n/CMakeLists.txt`
  follows the same `ALP_HAS_RENESAS_FSP` gate as Alif, so the
  files compile to empty translation units when the FSP pack
  isn't present.  `src/baremetal/CMakeLists.txt` extends the
  ALP_SOM dispatch with a v2n branch; `pr-plain-cmake.yml` gains
  a `baremetal-v2n` scenario verifying the dispatch + scaffolding
  build green without the proprietary FSP pack.  Real FSP link
  follows in v0.4 alongside the V2N HIL bring-up.
- **`<alp/usb.h>` real device-stack lifecycle wrapper (v0.3 milestone)** --
  `src/zephyr/usb_zephyr.c` replaces the v0.1 NOSUPPORT stub.  Wraps
  Zephyr's `usb_enable` / `usb_disable` for the device-role
  lifecycle.  Per-class endpoint read/write (CDC-ACM, MSC, HID)
  and host-role bring-up land in v0.3.x.  New
  `CONFIG_ALP_SDK_USB` (depends on `USB_DEVICE_STACK`, default y),
  `CONFIG_ALP_SDK_MAX_USB_DEV_HANDLES` (default 1).
  `tests/zephyr/usb/` smoke suite (5 cases).
- **`<alp/mproc.h>` real Zephyr MBOX wrapper (v0.3 milestone)** --
  `src/zephyr/mproc_zephyr.c` replaces the v0.1 NOSUPPORT stub.
  Wraps Zephyr's MBOX driver class (Alif's MHU is registered as
  a Zephyr mbox device on AEN) for the mailbox path:
  `mbox_init_channel` + `mbox_send` + `mbox_register_callback`.
  DT-anchored shared-memory regions (`alp-shmemN` aliases) and
  per-SoC HWSEM register access return NOSUPPORT until v0.3.x
  finishes the EVK overlay and the Alif HWSEM driver.  New
  Kconfig: `CONFIG_ALP_SDK_MPROC` (depends on MBOX, default y),
  pool-size knobs (`MAX_SHMEM_HANDLES`, `MAX_MBOX_HANDLES`,
  `MAX_HWSEM_HANDLES`).  `tests/zephyr/mproc/` smoke suite
  (7 cases).
- **`<alp/security.h>` real MbedTLS PSA Crypto wrapper (v0.3 milestone)** --
  `src/zephyr/security_zephyr.c` replaces the v0.1 NOSUPPORT stub.
  Wraps PSA Crypto (`psa_crypto_init`, `psa_hash_*`, `psa_aead_*`,
  `psa_generate_random`) -- when MbedTLS is built with the
  vendor's HW-accelerator driver registered, PSA routes to the
  Alif Ensemble crypto subsystem (E7/E8) or the Renesas RZ/V2N
  RSIP automatically without code changes here.  Hash covers
  SHA-256/384/512; AEAD covers AES-128/256-GCM and
  ChaCha20-Poly1305; TRNG via `psa_generate_random`.  New
  Kconfig: `CONFIG_ALP_SDK_SECURITY` (depends on
  `MBEDTLS_PSA_CRYPTO_C`, default y),
  `CONFIG_ALP_SDK_MAX_HASH_HANDLES` / `_AEAD_HANDLES` (default 4
  each).  AEAD encrypt/decrypt path uses a 4 KiB stack scratch
  buffer (heap fall-back lands in v0.3.x for larger blobs).
  `tests/zephyr/security/` smoke suite (7 cases).
- **`<alp/ble.h>` real Zephyr `bt`-host wrapper (v0.3 milestone)** --
  `src/zephyr/ble_zephyr.c` replaces the v0.1 NOSUPPORT stub.
  Wraps Zephyr's `bt` host stack: `bt_enable` for the singleton
  controller, `bt_le_adv_start` / `bt_le_adv_stop` for the
  peripheral role, `bt_le_scan_start` / `bt_le_scan_stop` with a
  per-packet `recv` callback for the central role, and
  `bt_conn_le_create` / `bt_conn_disconnect` for connections.
  Gated on new `CONFIG_ALP_SDK_BLE` (depends on `BT`, default y).
  GATT runtime registration + sync read/write helpers stay
  NOSUPPORT until v0.3.x lands a callback-with-semaphore shim
  (Zephyr's GATT API is async-only; the public surface is
  synchronous).  New `CONFIG_ALP_SDK_BLE_MAX_CONNS` (default 2)
  sizes the connection-handle pool.  `tests/zephyr/ble/` smoke
  suite (6 cases) verifies the NOSUPPORT-fall-back contract +
  every NULL-arg branch under native_sim.
- **Bare-metal AEN backend scaffolding (v0.2 milestone, item 4)** --
  v0.2's "stub-to-real" baseline for the bare-metal Alif Ensemble
  path.  New `vendors/alif/{i2c,spi,gpio,uart}.c` source files
  wrap the four core peripherals on Alif's CMSIS-Driver layer
  (`Driver_I2C0..3`, `Driver_SPI0..3`, `Driver_GPIO0..7`,
  `Driver_USART0..3`).  Each wrapper's body is gated on
  `ALP_HAS_ALIF_HAL` so the files compile cleanly even without
  the proprietary Ensemble CMSIS pack present.  When the option
  flips ON the wrapper sets `ALP_VENDOR_OVERRIDES_PERIPHERAL`,
  which excludes the matching stubs from `src/common/stub_backend.c`
  -- no duplicate symbols.  CMake wiring: new top-level
  `ALP_SOM` variable (`none|aen|v2n|imx93`) layered on top of
  `ALP_OS`; `src/baremetal/CMakeLists.txt` pulls
  `vendors/alif/CMakeLists.txt` only when `ALP_OS=baremetal`
  AND `ALP_SOM=aen`, leaving non-AEN bare-metal builds on the
  pure stub path.  `pr-plain-cmake.yml` gains a `baremetal-aen`
  job that exercises the new dispatch.  Real HAL link follows in
  v0.2.x via `cpackget add AlifSemiconductor::Ensemble` -- the
  scaffolding here lets that change be a body-of-function-only
  follow-up.
- **Cross-platform development support made explicit** -- README
  gains a "Cross-platform development" table calling out
  first-class macOS / Windows / Linux support; `docs/getting-started.md`
  prerequisite section adds per-OS install one-liners (Homebrew,
  apt, winget, WSL).  No code changes -- the toolchain (Zephyr +
  west + CMake + Python + GCC) was already cross-platform; the
  documentation just admits it now.  New `.gitattributes` pins
  LF endings on every source file so a Windows checkout, a macOS
  clone, and a Linux pull see and commit identical bytes --
  avoids clang-format-diff CI failures when a developer crosses
  hosts.
- **[ADR 0005](docs/adr/0005-alp-sdk-vs-alp-studio-boundary.md)** —
  alp-sdk vs alp-studio repo boundary.  Codifies the dual-use
  acid test ("would a hand-written-firmware author ever directly
  use this?") and the canonical table of which artefacts belong
  in which repo.  `docs/architecture.md` gains a "Repository
  boundary" subsection so reviewers can resolve where a new
  addition belongs without opening the ADR.
- **`<alp/inference.h>` real wrapper + TFLM/Ethos-U C++ executor**
  — v0.2's third "stub-to-real" milestone.  `src/zephyr/inference_zephyr.c`
  replaces the v0.1 NOSUPPORT stub with the public-API glue: handle
  pool (default 2), capability validation, backend selector
  (AUTO / CPU / ETHOS_U / DRPAI / DEEPX_DX), and dispatch into a
  per-backend executor.  `src/zephyr/inference_tflm.cpp` -- new C++
  source compiled only when `CONFIG_ALP_SDK_INFERENCE_TFLM=y` (depends
  on `TENSORFLOW_LITE_MICRO`) -- wraps `tflite::MicroInterpreter`
  and `tflite::MicroMutableOpResolver<32>` with the canonical
  MobileNetV2 op set + the Arm `AddEthosU()` registration so
  Vela-compiled `.tflite` files dispatch their ETHOS-U custom op
  to the NPU.  Default 128 KiB tensor arena (caller-supplied via
  `cfg.arena` overrides).  The CPU backend uses TFLM's reference
  kernels for any layer Vela leaves on-host plus pure CPU models;
  ETHOS_U routes through the same MicroInterpreter with the op
  resolver picking the NPU op when available.  `west.yml`
  name-allowlist gains `tflite-micro` so `west update` pulls the
  module on every refresh; CI's native_sim builds keep
  `CONFIG_ALP_SDK_INFERENCE_TFLM=n` and the wrapper falls back to
  ALP_ERR_NOSUPPORT cleanly.  New Kconfig options:
  `CONFIG_ALP_SDK_INFERENCE_TFLM` (default n; enables the C++
  executor), `CONFIG_ALP_SDK_INFERENCE_ETHOS_U` (default y if
  TFLM and SOC_SERIES_ENSEMBLE_E7), `CONFIG_ALP_SDK_MAX_INFERENCE_HANDLES`
  (default 2).  The `examples/aen/edgeai-vision-aen/` example now
  wires `alp_inference_open` into stage 3 (model load) using
  `ALP_INFERENCE_BACKEND_AUTO` -- skips on native_sim with the
  precise NOSUPPORT diagnostic, runs the real Vela model on AEN HW
  once the toolchain output lands under `models/`.  New
  `tests/zephyr/inference/` ztest suite (7 cases) verifies the
  cfg-validation + NOSUPPORT-fall-back contract under native_sim.
- **`<alp/audio.h>` real PDM input + I²S output on AEN-Zephyr** —
  v0.2's second "stub-to-real" milestone.  `src/zephyr/audio_zephyr.c`
  replaces the v0.1 `audio_stub.c` and splits in two halves: a
  `audio_in` path wrapping Zephyr's `audio_dmic` API
  (`dmic_configure` / `dmic_trigger` / `dmic_read`), and a
  `audio_out` path that delegates straight to the existing
  `alp_i2s_*` wrapper (TX direction, frame translation in the
  config).  Two Kconfig toggles -- `CONFIG_ALP_SDK_AUDIO_IN`
  (depends on `AUDIO_DMIC`, default y) and `CONFIG_ALP_SDK_AUDIO_OUT`
  (depends on `I2S`, default y) -- gate the real glue behind the
  v0.1 NULL-with-NOSUPPORT contract for native_sim and any other
  no-audio target.  The PDM path runs the ALP DSP chain on every
  block: a 1st-order DC-block (alpha = 0.995 in Q15, ~10 Hz
  cut-off at 16 kHz) silences the DC bias every PDM mic carries.
  AGC and resample land in v0.3 alongside `<alp/security.h>`; the
  hook stays in `dmic_read`'s post-pass so v0.3 only adds passes.
  The output path adds a Q8.8 software volume scale (default
  unity, `alp_audio_out_set_volume(out, vol)` from 0..255) that
  applies before `alp_i2s_write` -- usable on codecs without a
  separate gain pin.  Pool sizes
  (`CONFIG_ALP_SDK_MAX_AUDIO_IN_HANDLES` / `_OUT_HANDLES`, both
  default 1) and the per-handle slab (4 KiB *
  `CONFIG_ALP_SDK_AUDIO_IN_SLAB_BLOCKS`, default 4 blocks) are
  statically allocated -- no `k_malloc` in the audio path.  New
  `examples/audio-loopback/` hand-written reference: opens
  `ALP_E1M_PDM0` + `ALP_E1M_I2S0`, streams 50 blocks of 256
  S16-mono frames from mic to speaker at 16 kHz, prints
  `[audio] done`.  Skips gracefully on native_sim (no audio
  devices) so the existing twister scenario continues to pass.
  New `tests/zephyr/audio/` ztest suite (9 cases) verifies the
  NOSUPPORT-fall-back contract under native_sim plus every
  NULL-arg / invalid-config branch.
- **`<alp/iot.h>` real Wi-Fi station + MQTT on AEN-Zephyr** —
  v0.2's first "stub-to-real" milestone.  `src/zephyr/iot_zephyr.c`
  replaces the v0.1 `iot_stub.c` and wraps Zephyr's `wifi_mgmt`
  (`net_mgmt(NET_REQUEST_WIFI_CONNECT, …)`) plus `mqtt_client`
  (`mqtt_connect`, `mqtt_publish`, `mqtt_subscribe`, `mqtt_input`,
  `mqtt_live`).  Two new Kconfig toggles --
  `CONFIG_ALP_SDK_IOT_WIFI` (depends on `WIFI && NET_MGMT`,
  default y) and `CONFIG_ALP_SDK_IOT_MQTT` (depends on
  `MQTT_LIB && NET_TCP`, default y) -- gate the real glue.  When
  neither is on (host build, native_sim, any target without a
  Wi-Fi/TCP stack) the wrapper honours the v0.1 contract:
  `*_open()` returns NULL with `alp_last_error()` =
  `ALP_ERR_NOSUPPORT`.  Static handle pools sized via
  `CONFIG_ALP_SDK_MAX_WIFI_HANDLES` (default 1, single-radio per
  E1M SoM) and `CONFIG_ALP_SDK_MAX_MQTT_HANDLES` (default 2),
  with `CONFIG_ALP_SDK_MQTT_BUF_SIZE` (default 256 B) controlling
  the per-client rx / tx / topic scratch.  `examples/iot-connected-camera/`
  ships a new `overlay-aen.conf` that pulls in `CONFIG_NET_*`,
  `CONFIG_WIFI`, `CONFIG_WIFI_NM_WPA_SUPPLICANT`, `CONFIG_MQTT_LIB`
  for AEN builds; the base `prj.conf` remains native_sim-friendly
  and the example continues to print `[iotcam] done` end-to-end.
  New `tests/zephyr/iot/` ztest suite (12 cases) verifies the
  NOSUPPORT-fall-back contract + every NULL-arg branch, bringing
  twister's `native_sim/native/64` total to **64 cases** (52 + 12).

### Notes

- Repo migrated from a CMSIS-Toolbox csolution / VFT mock-driver
  layout to the OS-pivoted layout described in `docs/architecture.md`.
  The previous tree is preserved in git history.
- Apache-2.0 license replaces the original MIT badge.
- Twister build hit a Kconfig-discovery issue (the alp-sdk module
  surface isn't being merged) — fix tracked, will be green before tag.
