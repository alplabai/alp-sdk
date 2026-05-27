# ABI stability markers

Every public header in `include/alp/` carries an `@par ABI status:`
tag classifying it as **`[ABI-STABLE]`** or **`[ABI-EXPERIMENTAL]`**.
This doc explains what each marker means + lists the current
classification per header.

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
| `peripheral.h` (I²C/SPI/UART/GPIO) | `[ABI-STABLE]` | v0.1 surface; locked across every since-then release. |
| `pwm.h`               | `[ABI-STABLE]`     | v0.2 surface; locked.                                             |
| `adc.h`               | `[ABI-STABLE]`     | v0.2 + v0.5 additive (filter/spectrum handle types).  Base surface stable; new `alp_adc_filter_t` / `alp_adc_spectrum_t` may evolve `[ABI-EXPERIMENTAL]` at function granularity. |
| `counter.h`           | `[ABI-STABLE]`     | v0.2.                                                              |
| `i2s.h`               | `[ABI-STABLE]`     | v0.2.                                                              |
| `can.h`               | `[ABI-STABLE]`     | v0.2.                                                              |
| `rtc.h`               | `[ABI-STABLE]`     | v0.2.                                                              |
| `wdt.h`               | `[ABI-STABLE]`     | v0.2.                                                              |
| `audio.h`             | `[ABI-STABLE]`     | v0.2 decl + v0.3 impl; PDM-in / I²S-out shape stable.             |
| `iot.h`               | `[ABI-STABLE]`     | v0.2-v0.4; Wi-Fi station + MQTT (TLS) signatures stable.          |
| `security.h`          | `[ABI-STABLE]`     | v0.3 MbedTLS PSA Crypto wrapper.                                  |
| `ble.h`               | `[ABI-STABLE]`     | v0.2 decl + v0.3 impl; advertise + connect + GATT-read shape stable. |
| `inference.h`         | `[ABI-STABLE]`     | v0.3 dispatcher (auto/cpu/ethos_u/drpai/deepx_dxm1); v0.5 adds `alp_inference_open_alpmodel()` + the `.alpmodel` loader/selection engine. |
| `mproc.h`             | `[ABI-STABLE]`     | v0.3 mailbox + shmem + hwsem.                                     |
| `hw_info.h`           | `[ABI-STABLE]`     | v0.3 EEPROM manifest + BOARD_ID ADC.                              |
| `e1m_pinout.h`        | `[ABI-STABLE]`     | v0.1 portable instance IDs (`E1M_I2C0`, etc.); pinned by e1m-spec. |
| `soc_caps.h`          | `[ABI-STABLE]`     | v0.1 generated; capability constants.                              |
| `gui.h`               | `[ABI-STABLE]`     | v0.2 LVGL re-export shim.                                          |
| `camera.h`            | `[ABI-EXPERIMENTAL]` | v0.5 added `alp_camera_configure_isp` (Mali-C55 toggles) — surface tentative pending real hardware feedback.  Base capture path stable; ISP block experimental. |
| `storage.h`           | `[ABI-EXPERIMENTAL]` | v0.5 added `alp_storage_configure_inline_aes` (SecAES on OSPI / HexSPI) -- surface tentative.  Base storage placeholders (v0.4 work) are still stubs. |
| `display.h`           | `[ABI-EXPERIMENTAL]` | v0.3 placeholder; no real backend impl yet.                       |
| `usb.h`               | `[ABI-EXPERIMENTAL]` | v0.3 placeholder; surface skeleton only.                          |
| `dsp.h`               | `[ABI-EXPERIMENTAL]` | v0.5 new -- standalone DSP-chain API (FIR/IIR/WINDOW/FFT).  Composes ADC-pipeline filter/spectrum types; both sides may co-evolve. |
| `gpu2d.h`             | `[ABI-EXPERIMENTAL]` | v0.5 new -- AEN audit headline gap.  Surface designed for portability but only one silicon family populates it today. |
| `power.h`             | `[ABI-EXPERIMENTAL]` | v0.5 new -- system-power-mode surface (sleep / deep-sleep / standby + wake-source bitmaps). |
| `tmu.h`               | `[ABI-EXPERIMENTAL]` | Wave-1 GD32 CORDIC TMU helpers; surface limited and may be folded into `<alp/dsp.h>` for v1.0. |

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
