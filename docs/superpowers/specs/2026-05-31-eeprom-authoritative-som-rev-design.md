# EEPROM-authoritative SoM hardware revision

**Date:** 2026-05-31
**Status:** Design approved, pending spec review
**Branch:** `feat/eeprom-authoritative-som-rev` (off `dev`)

## Problem

The SoM hardware revision is currently meant to be resolved two ways:

1. The on-module EEPROM manifest carries `hw_rev` (alongside family, SKU,
   serial, mfg date).
2. A per-revision resistor divider on a SoM-internal ADC channel
   (`ADC2_CH7` on V2N) is meant to be sampled at boot and cross-checked
   against the manifest's `hw_rev`.

The ADC cross-check has never been implemented — it is a no-op stub
(`adc_cross_check()` in `src/zephyr/hw_info_zephyr.c`) waiting on a
generated bin-threshold header that was never produced. The supporting
artifacts (the bin-table prose in `docs/board-id.md`, a planned
`scripts/check-hw-rev-bins.py` CI gate, the illustrative bin millivolts)
describe a path the SDK does not walk.

This duplicates the source of truth for one fact (the SoM revision) across
two mechanisms, violating the SDK's single-source-of-truth principle. The
EEPROM manifest already carries `hw_rev`, the EEPROM physically travels
with the SoM, and its contents are integrity-protected (magic + schema +
CRC32). It is the natural sole authority.

## Decision

Drop the SoM-side ADC revision path entirely. The validated EEPROM manifest
becomes the single authoritative source of the SoM hardware revision. Add an
error code that distinguishes an un-provisioned module (expected before
factory programming) from a corrupt one.

**In scope:** SoM-side ADC revision sensing only.
**Out of scope:** the carrier-side `board_id` path (`board_hw_rev` /
`board_id_mv`), which remains its own mechanism — a carrier with no EEPROM
can still encode its revision on a resistor.

## Approach

Surgical removal plus a provisioning note. Rejected alternatives:

- **Keep a coarse SoM cross-check** — defeats the simplification. The EEPROM
  travels with the SoM, so it *is* the module's identity; a second sensor to
  "confirm" it adds a divergence failure mode without adding truth.
- **Full `hw_info` module split** (separate SoM-identity vs carrier-board_id
  units) — unwarranted refactor. `hw_info_zephyr.c` is one focused file; the
  removal leaves it cleaner, not larger.

## Design

### 1. Removed (SoM-side ADC)

- `adc_cross_check()` and its call site in `src/zephyr/hw_info_zephyr.c`.
- The `som_board_id_mv` field of `alp_hw_info_t` in `include/alp/hw_info.h`
  (clean removal — no ABI shim, per the no-legacy-compat convention; there
  are no active customers).
- All framing that describes the SoM revision as "cross-checked against
  `ADC2_CH7`". `ADC2_CH7` is freed on the SoM as a hardware/maintainer
  follow-up (the channel can be repurposed); the SDK simply stops reading it.

### 2. EEPROM authoritative (read path)

`alp_hw_info_read()` populates `som_hw_rev` solely from the validated
manifest. The happy path is unchanged except that the `adc_cross_check()`
call is gone. Validation order stays: read 128 bytes → magic → schema →
CRC32 → copy fields.

### 3. Error semantics (new)

Add to the `alp_status_t` enum in `include/alp/peripheral.h`:

```c
ALP_ERR_NOT_PROVISIONED = -15, /**< EEPROM manifest absent (module not yet provisioned). */
```

In `hw_info_zephyr.c`, after a **successful** 128-byte I2C read:

| Condition                                            | Result                       |
|------------------------------------------------------|------------------------------|
| I2C read itself fails (no device ACK / bus error)    | `ALP_ERR_IO` (unchanged)     |
| read OK, magic absent / ≠ `ALPH` (erased `0xFF` / zeroed) | `ALP_ERR_NOT_PROVISIONED` |
| read OK, magic = `ALPH`, but schema or CRC32 invalid | `ALP_ERR_IO` (corruption)    |
| read OK, valid manifest                              | `ALP_OK`                     |

`ALP_ERR_NOT_PROVISIONED` is reserved for the case where the EEPROM is
physically present and readable but carries no valid manifest header — a
blank module awaiting factory programming. A failure to reach the EEPROM at
all (missing pull-ups, wrong bus, no device) stays an `ALP_ERR_IO` bus error,
since it is a wiring/bring-up fault, not an un-provisioned-but-healthy module.

On any non-`OK` result, `som_hw_rev` (and the rest of the SoM half of
`alp_hw_info_t`) is left empty/zeroed. `ALP_ERR_NOT_PROVISIONED` is an
expected, non-fatal state on a freshly assembled board before the factory
programmer runs; application/bring-up code can detect it specifically rather
than treating a blank module as a hard I/O fault.

### 4. Explicitly kept

- **Carrier-side `board_hw_rev` / `board_id_mv`** in `alp_hw_info_t` — the
  board-side identification path (still a TODO in the reader) is a separate
  concern and survives untouched. A carrier without an EEPROM can encode its
  revision on a divider; that is legitimate and not what this change removes.
- **`metadata/e1m_modules/v2n/hw-revisions.yaml`** — this is the revision
  registry (`r1`–`r8`) with `min_sdk_version` / `max_sdk_version` / `status`
  / `changes`. It is SDK-version gating metadata, not ADC bin data (it never
  carried bin voltages). It stays as-is.

### 5. Documentation and metadata

- **`docs/board-id.md`** — rewrite the SoM section to "EEPROM-authoritative".
  Delete: the BOARD_ID ADC cross-check section, the illustrative bin table,
  the `hwrev_bin_t` / `v2n_hwrev_bins[]` sample, the divider math, and the
  reference to the planned `scripts/check-hw-rev-bins.py` gate (which does not
  exist). Retitle the page to "Board identification — SoM EEPROM manifest".
  Keep the carrier board-side mention as its own (still-TODO) concept. Update
  the "Wrong EEPROM swap" protection bullet: the cross-check no longer guards
  this, so reframe protection around `alp_hw_info_assert_matches_build()` plus
  the manifest's own `family`/`sku`/`hw_rev` integrity.
- **`docs/soms/v2n.md`** — the "Boot + identification" section drops the
  BOARD_ID ADC bullet; SoM rev = EEPROM manifest. Remove the ADC2_CH7
  reference from the "V2N-specific" / identification prose.
- **`alp-sdk-internal/EEPROM-MANIFEST-SPEC.md`** — mark `hw_rev` the
  authoritative SoM revision field; document the `NOT_PROVISIONED`-on-blank vs
  `IO`-on-corrupt read contract; record the provisioning safeguard (§6).

### 6. Provisioning safeguard (note, not new SDK code)

With the divider gone, the EEPROM is the single point of truth — and of
failure. The read-side validation (magic + schema + CRC32) already rejects a
bad write at runtime (→ `ALP_ERR_IO`). The factory provisioning flow
(`scripts/program_eeprom.py`, run during assembly QC) should additionally do
**write → read-back → CRC-verify** before passing a board. This is a process
note for the provisioning tool/QC, not a new public `<alp/*>` API. No
`alp_hw_info_write()` is added unless separately requested.

### 7. Propagation

- New error code → enum entry in `peripheral.h`; check for and update any
  error-to-string table and any docs that enumerate `ALP_ERR_*` codes.
- Removing `som_board_id_mv` → update every consumer in lockstep:
  `examples/v2n/v2n-board-id-readout/` (and any test or host-side reader that
  prints/asserts the field). No deprecation period.

## Testing

- `tests/zephyr` `hw_info` coverage (native_sim): valid manifest → `ALP_OK`
  with the right `som_hw_rev`; blank/erased EEPROM (no magic) →
  `ALP_ERR_NOT_PROVISIONED`; magic present + corrupted CRC → `ALP_ERR_IO`.
- Build the `v2n-board-id-readout` example after the field removal to confirm
  it compiles and reads cleanly.
- Full local native_sim twister scope + clang-format diff gate before push
  (local-first CI).

## Risk

Low. The removed ADC path is a no-op stub — no behavior is lost. The only
behavioral change for existing callers is that a blank EEPROM now returns
`ALP_ERR_NOT_PROVISIONED` instead of `ALP_ERR_IO`, and `som_board_id_mv`
disappears from the public struct (compile-time break for any consumer, fixed
in the same change). The single-source-of-truth posture improves.
