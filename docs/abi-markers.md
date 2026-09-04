# ABI stability markers

Every public header in `include/alp/` that declares an ABI symbol carries an
`@par ABI status:` tag classifying it as **`[ABI-STABLE]`** or
**`[ABI-EXPERIMENTAL]`**. Two top-level headers are symbol-less facades and
carry no tag at all -- `board.h` (`#include`s the active board's generated
routes header; declares no symbols of its own) and `console.h` (the portable
shell command group self-registers; no exported symbols) -- see their rows
in the table below. This doc explains what each marker means + lists the
current classification per header.

Paired with [`docs/release-policy.md`](release-policy.md) (the SemVer
contract) and `.github/workflows/pr-abi-snapshot.yml` (the post-1.0
enforcement gate).

## What the markers mean

### `[ABI-STABLE]`

The header's public symbols are part of the v1.0 frozen surface
contract:

- Removing or renaming a symbol → **major bump required**.
- Changing a function signature (return type, parameter list,
  calling convention) → **major bump required**.
- Adding new symbols (functions, enums, struct fields appended) →
  minor bump.
- Adding new behaviour to an existing function (a previously-
  ignored input now triggers an error path; a previously-impossible
  return value becomes possible) → **major bump if it breaks
  documented contracts; minor otherwise**.

Reviewers + the post-1.0 `pr-abi-snapshot.yml` gate enforce this.

### `[ABI-EXPERIMENTAL]`

The header's surface is provisional.  It may change in any minor
release before being promoted to stable:

- Removing or renaming a symbol → minor bump (no major required).
- Adding a function-level `[ABI-EXPERIMENTAL]` marker overrides
  the file-level marker for that symbol -- this is how a stable
  file gradually accumulates experimental additions.
- Promotion to `[ABI-STABLE]` happens via a deliberate PR that
  updates this doc + the header marker.

The `pr-abi-snapshot.yml` gate posts the diff but does not fail
the PR for experimental symbols.

## Current classification (v0.5 -> v1.0 prep)

### Top-level headers (`include/alp/*.h`)

| Header                | Marker             | Rationale                                                         |
|-----------------------|--------------------|-------------------------------------------------------------------|
| `peripheral.h` (I²C/SPI/UART/GPIO) | `[ABI-STABLE]` | v0.1 surface; locked across every since-then release.  v0.9 adds the I²C/SPI target (slave) mode surfaces (`alp_i2c_target_*` / `alp_spi_target_*`) and the `alp_init` / `alp_deinit` SDK-lifecycle entry points, all marked `[ABI-EXPERIMENTAL]` at function granularity (the file-level marker stays STABLE — the mixed-tier mechanism in "What the markers mean" above). |
| `pwm.h`               | `[ABI-STABLE]`     | v0.2 surface; locked.                                             |
| `adc.h`               | `[ABI-STABLE]`     | v0.2 + v0.5 additive (filter/spectrum handle types).  Base surface stable; new `alp_adc_filter_t` / `alp_adc_spectrum_t` may evolve `[ABI-EXPERIMENTAL]` at function granularity.  v0.8.0: the DAC half (`alp_dac_*`) split out to `dac.h` (same signatures; a source-include move, not a symbol change).  v0.9.0: `alp_adc_stream_read` / `alp_adc_filter_read` renamed to `alp_adc_stream_read_mv` / `alp_adc_filter_read_mv` so every read entry point carries its unit suffix (pre-1.0 rename; parameter lists unchanged). |
| `dac.h`               | `[ABI-STABLE]`     | v0.1 surface (`alp_dac_open` / `write_mv` / `read_mv` / `close`); split out of `adc.h` into its own header in v0.8.0 when DAC moved to the registry/dispatcher pattern.  Signatures unchanged.  v0.9.0: additive `alp_dac_capabilities`, aligning DAC with every other opened-handle class. |
| `counter.h`           | `[ABI-STABLE]`     | v0.2.                                                              |
| `i2s.h`               | `[ABI-STABLE]`     | v0.2.                                                              |
| `can.h`               | `[ABI-STABLE]`     | v0.2.                                                              |
| `rtc.h`               | `[ABI-STABLE]`     | v0.2.                                                              |
| `wdt.h`               | `[ABI-STABLE]`     | v0.2.  v0.9.0: `wdt_id` moved into `alp_wdt_config_t` so `alp_wdt_open(const alp_wdt_config_t *)` matches every other config-taking open (pre-1.0 signature change).  v0.17.0 (#1637): `alp_wdt_config_t` additively gains `on_expire` + `user` so `ALP_WDT_INTERRUPT_ONLY` can notify the app instead of silently doing nothing; `alp_wdt_open` gains a new `ALP_ERR_INVAL` case (INTERRUPT_ONLY with no `on_expire`) and the Yocto backend now returns `ALP_ERR_NOSUPPORT` for INTERRUPT_ONLY instead of silently accepting it.  `alp_wdt_close` no longer implicitly disables the watchdog on the Zephyr backend -- that used to disarm the whole device, not just the closing handle's channel. Pre-1.0, additive/behavioural, no signature break. |
| `audio.h`             | `[ABI-STABLE]`     | v0.2 decl + v0.3 impl; PDM-in / I²S-out shape stable.             |
| `iot.h`               | `[ABI-STABLE]`     | v0.2-v0.4; Wi-Fi station + MQTT (TLS) signatures stable.          |
| `security.h`          | `[ABI-STABLE]`     | v0.3 MbedTLS PSA Crypto wrapper.                                  |
| `ble.h`               | `[ABI-STABLE]`     | v0.2 decl + v0.3 impl; advertise + connect + GATT-read shape stable. |
| `inference.h`         | `[ABI-STABLE]`     | v0.3 dispatcher (auto/cpu/ethos_u/drpai/deepx_dxm1); v0.5 adds `alp_inference_open_alpmodel()` + the `.alpmodel` loader/selection engine.  v0.16 adds `alp_inference_last_invoke_latency_us()` (the model-perf-capture latency accessor), marked `[ABI-EXPERIMENTAL]` at function granularity -- the file-level marker stays STABLE. |
| `mproc.h`             | `[ABI-STABLE]`     | v0.3 mailbox + shmem + hwsem.  v0.9 adds `alp_mproc_boot_core` (peer-core release), marked `[ABI-EXPERIMENTAL]` at function granularity. |
| `hw_info.h`           | `[ABI-STABLE]`     | v0.3 EEPROM manifest (sole SoM-rev source); `som_board_id_mv` removed pre-1.0 (no-legacy-compat).  v0.9 adds the SoC-identity block (`alp_soc_info_read` / `alp_soc_secure_fw_ping`), marked `[ABI-EXPERIMENTAL]` at function granularity. |
| `e1m_pinout.h`        | `[ABI-STABLE]`     | v0.1 portable instance IDs (`ALP_E1M_I2C0`, etc.); pinned by e1m-spec. |
| `version.h`           | `[ABI-STABLE]`     | v0.9 new -- compile-time SDK version macros (`ALP_VERSION*`, `ALP_VERSION_AT_LEAST`), the per-class `ALP_ABI_STATUS_*` tier macros mirroring this table, and the runtime `alp_version_string()` getter.  Pure constants + one read-only getter; the values change every release by design, the symbol set is stable. |
| `soc_caps.h`          | `[ABI-STABLE]`     | v0.1 generated; capability constants.                              |
| `gui.h`               | `[ABI-STABLE]`     | v0.2 LVGL re-export shim.                                          |
| `camera.h`            | `[ABI-EXPERIMENTAL]` | v0.5 added `alp_camera_configure_isp` (ISP-Pico toggles) — surface tentative pending real hardware feedback.  Base capture path stable; ISP block experimental. |
| `jpeg.h`              | `[ABI-EXPERIMENTAL]` | v0.13 new -- portable JPEG-encoder surface (`alp_jpeg_open/encode/capabilities/close`).  A portable software baseline-JPEG backend (4:2:0 + mono, priority 50) encodes on every SoM without JPEG hardware; the Alif E8 Hantro VC9000E hardware backend (priority 100, E1M-AEN801) is silicon-proven -- `JPEG_SWREG0` reads back `JPEG_HW_ID` `0x90001000` and a real HW encode round-tripped through libjpeg to a correct image. |
| `storage.h`           | `[ABI-EXPERIMENTAL]` | v0.5 added `alp_storage_configure_inline_aes` (SecAES on OSPI / HexSPI) -- surface tentative, ALP_ERR_NOSUPPORT until a vendor pack implements it.  Base read/write/erase are real on Zephyr (zephyr_flash / zephyr_littlefs). |
| `display.h`           | `[ABI-EXPERIMENTAL]` | v0.3 placeholder; v0.9 adds the real Zephyr display backend (ADR-0017 Tier 1, `src/backends/display/zephyr_drv.c`), native_sim-verified against the upstream dummy controller -- no silicon run yet, so the surface stays experimental pending hardware. |
| `usb.h`               | `[ABI-EXPERIMENTAL]` | v0.3 placeholder; surface skeleton.  Device enable/disable is real on Zephyr; host lifecycle routes to `usbh_*`/`uhc_xhci_alif` but is BENCH-UNVERIFIED (UHC driver is a TODO(aen401-bench) skeleton); endpoint I/O not yet wired. |
| `dsp.h`               | `[ABI-EXPERIMENTAL]` | v0.5 new -- standalone DSP-chain API (FIR/IIR/WINDOW/FFT).  Composes ADC-pipeline filter/spectrum types; both sides may co-evolve. |
| `gpu2d.h`             | `[ABI-EXPERIMENTAL]` | v0.5 new -- AEN audit headline gap.  Surface designed for portability but only one silicon family populates it today. |
| `power.h`             | `[ABI-EXPERIMENTAL]` | v0.5 new -- system-power-mode surface (sleep / deep-sleep / standby + wake-source bitmaps).  v0.9 adds the operating-point-profile surface (`alp_power_profile_get` / `alp_power_profile_set`).  v0.17 settles the shape per #1813: adds `ALP_POWER_MODE_STOP`, `alp_power_configure_retention()` + `alp_power_retain_t` (a named RAM-retention footprint, not a raw vendor bitmask), `alp_power_wake_capabilities()` (a dedicated accessor -- NOT an overload of `alp_power_capabilities()`'s shared `alp_capabilities_t::flags`, whose bit values collide numerically with `ALP_POWER_WAKE_*`), `ALP_POWER_WAKE_COMPARATOR` / `ALP_POWER_WAKE_BROWNOUT`, and replaces the old "unsupported wake bits are silently ignored" contract with reported-capability (`alp_power_wake_capabilities()`) + `ALP_ERR_NOSUPPORT` from `alp_power_configure_wake_source()`, enforced centrally in the dispatcher against every registered backend. |
| `tmu.h`               | `[ABI-EXPERIMENTAL]` | Wave-1 GD32 CORDIC TMU helpers; surface limited and may be folded into `<alp/dsp.h>` for v1.0. |
| `update_log.h`        | `[ABI-EXPERIMENTAL]` | v0.7 new; experimental until the hardware-enforced backend is silicon-proven. |
| `i2c_regfile.h`       | `[ABI-EXPERIMENTAL]` | v0.9 new -- register-file target (slave) helper layered over the `[ABI-EXPERIMENTAL]` `alp_i2c_target_*` surface in `peripheral.h`; ships the "register-pointer + auto-increment file" state machine once.  Tracks the wrapped surface's tier exactly. |
| `i3c.h`               | `[ABI-EXPERIMENTAL]` | v0.14 new -- portable MIPI I3C Basic controller surface (`alp_i3c_open/write/read/write_read/close/capabilities`), mirroring `alp_i2c_*`.  Zephyr backend over upstream `i3c_dw.c` on the Alif Ensemble E8 (lpi3c0).  Controller init BENCH-PROVEN on E1M-AEN801 (Flow C RAM-run, 2026-07-25): lpi3c0 binds, `alp_i3c_open()` returns a handle, so the `ALIF_LPI3C_CLK` clock-id + P7_6/P7_7 fn3 pinctrl are confirmed.  Live transfer still unproven (no I3C target populated this batch).  Promotion gate: a live transfer against a populated target. |
| `ahrs.h`              | `[ABI-EXPERIMENTAL]` | v0.10 new -- caller-owned Madgwick AHRS filter (`alp_ahrs_init`/`update_imu`/`euler`/`reset`) fusing gyro+accel into a drift-corrected orientation quaternion.  Struct layout is stack-allocatable but may gain fields before v1.0 -- treat as opaque. |
| `backend.h`           | `[ABI-EXPERIMENTAL]` | v0.7 new -- backend registration + selection API (`ALP_BACKEND_REGISTER`; per-class dispatch by `silicon_ref` + priority).  Promoted to `[ABI-STABLE]` after three vendor families exercise the registry. |
| `board.h`             | (facade, no ABI symbols) | Board-agnostic compile-time facade: `#include`s the active board's generated routes header selected by the `ALP_BOARD_<SLUG>` define (from `board.yaml`); declares no symbols of its own. |
| `cap.h`               | `[ABI-EXPERIMENTAL]` | Auto-generated umbrella capability header (`scripts/gen_soc_caps.py`) aggregating `soc_caps.h` + `cap_instance.h`.  Upgraded to `[ABI-STABLE]` once both underlying pieces stabilise. |
| `cap_instance.h`      | `[ABI-EXPERIMENTAL]` | v0.7 new -- per-opened-instance capability flags (distinct from the SoC-level macros in `soc_caps.h`); promoted to `[ABI-STABLE]` once at least three vendor families exercise it. |
| `e1m_x_pinout.h`      | `[ABI-STABLE]`     | v0.6 + 2026-05-18 E1M-X (V2N/V2M) portable instance-ID namespace, pinned by `e1m-spec` x-v1.0.  2026-05-24 additive sync (I2C2/3, SPI2, CAN1, CSI2/3, DSI1, USB1, PCIE1, LCD0) kept every pre-existing instance ID's value. |
| `model.h`             | `[ABI-EXPERIMENTAL]` | Read-side parser for the `.alpmodel` package (header + CBOR manifest); no-malloc bounded view decoded once into a caller-provided `alp_model_t`. |
| `pid.h`               | `[ABI-EXPERIMENTAL]` | v0.10 new -- caller-owned PID controller (`alp_pid_init`/`alp_pid_step`/`alp_pid_reset`) with output clamp + anti-windup + derivative-on-measurement.  Struct layout transparent but treat as opaque before v1.0. |
| `rpc.h`               | `[ABI-STABLE]`     | v0.6 framed RPC surface over OpenAMP/RPMsg (`alp_rpc_open`/`subscribe`/...).  Adding optional `alp_rpc_config_t` fields is permitted; reshaping the callback signatures is not.  v0.17 additively adds link-liveness reporting (issue #1643): `alp_rpc_link_state_t`, `alp_rpc_link_cb_t`, `alp_rpc_set_link_callback`, `alp_rpc_link_state` (minor bump); `alp_rpc_send`'s documented `ALP_ERR_NOT_READY` return now also fires once the link is observed `ALP_RPC_LINK_LOST`, not just on a NULL/closed channel -- widening an already-documented return, not adding a new one. |
| `console.h`           | (portable, no ABI symbols) | v0.9 chip-neutral console header; the portable `alp` shell command group self-registers. Companion binding moved to the chip-specific `ext/cc3501e/console.h`. |
| `ext/cc3501e/console.h` | `[ABI-EXPERIMENTAL]` | v0.9 new -- app-facing companion-console binder (`alp_console_companion_set`) for the `alp companion` CLI verb.  No-op where the companion is a singleton (V2N auto-binds the GD32 supervisor); Alif apps register their CC3501E handle.  Experimental with the CC3501E companion surface it depends on. |
| `ext/alif/storage.h`  | `[ABI-EXPERIMENTAL]` | v0.5 header+stub.  Issue #224: `alp_alif_storage_secaes_key_provision()` gained a real body over the hal_alif SE-service transport (`se_service_send_request`, `SERVICE_APPLICATION_OSPI_WRITE_KEY_ID`), gated on `CONFIG_ALP_SDK_STORAGE_ALIF_SECAES` -- **default OFF** (unlike `CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL_SEND_SEAM`, which defaults ON because that path bench-PASSED) because the round-trip is UNVERIFIED ON SILICON (no OSPI SecAES-relevant part on any bench unit reachable at implementation time; see `src/backends/ext/alif/storage.c`).  The verdict path reads both `header.hdr_error_code` (transport-layer NACK) and `resp_error_code` (pre-seeded to a non-success sentinel before send) so an SE that never implements service 105 cannot read back as a false `ALP_OK`.  `key_bytes` narrowed to 16-only (AES-128 is the only width the SE service accepts; the header previously and incorrectly documented 16/24/32).  The call brackets the ~35 s SE round-trip with the `alp_handle_op_enter`/`_leave` guard (issue #629) so a concurrent `alp_storage_close` cannot free the handle mid-transaction.  `alp_alif_storage_secaes_get_status()` still unconditionally `ALP_ERR_NOSUPPORT` -- no vendor-published SE service reads the engine's status back. |

### Chip-driver headers (`include/alp/chips/*.h`)

Chip drivers are **opt-in via `board.yaml`** and not part of the
core `<alp/*.h>` ABI contract.  Each driver carries its own
stability statement in the file's top doxygen block.  Defaults:

- A chip driver that ships with a verified ✅ HIL row in
  `docs/test-plan.md` → `[ABI-STABLE]`.
- All others → `[ABI-EXPERIMENTAL]` until HIL flips them.

Per-chip status lives in
[`docs/test-plan.md`](test-plan.md)'s per-row "Status" column.

### Internal headers (`include/alp/internal/*.h`, `src/**/*.h`)

Not part of the public ABI.  No marker required.  Renames /
removals are unrestricted at any release boundary; callers
relying on internal headers do so at their own risk.

## How `pr-abi-snapshot.yml` reads markers

Post-1.0 the workflow:

1. Runs `scripts/abi_snapshot.py --diff <latest>` to get the raw
   diff.
2. For each `CHANGED` / `REMOVED` line, reads the source header
   to determine if the symbol's enclosing comment block carries
   `[ABI-STABLE]` or `[ABI-EXPERIMENTAL]`.
3. Function-level marker overrides file-level marker.
4. Posts the diff to the PR with each entry annotated.
5. Fails the PR if any `[ABI-STABLE]` symbol is `CHANGED` /
   `REMOVED` and `metadata/sdk_version.yaml` doesn't bump the
   major component vs `main`.

Before v1.0 this same workflow runs in informational mode
(`continue-on-error: true`); the diff comment is for reviewer
awareness but doesn't gate merge.

## Promoting EXPERIMENTAL -> STABLE

When a surface stabilises after real-customer use:

1. Verify the matching `docs/test-plan.md` row is `✅ verified`.
2. PR updates:
   - This doc's classification table.
   - The header's `@par ABI status` line.
   - Any `[ABI-EXPERIMENTAL]` function-level markers in the
     header.
   - CHANGELOG `### Changed` entry: "Promoted `<alp/X.h>` from
     EXPERIMENTAL to STABLE."
3. Reviewer confirms no public-customer use was relying on the
   experimental status (i.e. no one was monkey-patching around
   it).
4. Merge → next minor release.

Demoting STABLE → EXPERIMENTAL is **forbidden post-1.0**.  If a
stable surface needs to change, it deprecates per
[`docs/release-policy.md`](release-policy.md) and the
deprecation marker (not the EXPERIMENTAL marker) signals
intent-to-remove.

## See also

- [`docs/release-policy.md`](release-policy.md) -- SemVer
  contract + deprecation procedure.
- `.github/workflows/pr-abi-snapshot.yml` -- the gate.
- `docs/abi/v*-snapshot.json` -- per-release ABI fingerprints.
