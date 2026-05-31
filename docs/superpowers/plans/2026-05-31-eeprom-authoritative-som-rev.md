# EEPROM-authoritative SoM hardware revision — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the on-module EEPROM manifest's `hw_rev` the single authoritative source of the SoM hardware revision; drop the never-implemented SoM-side ADC cross-check; distinguish a blank (unprovisioned) module from a corrupt one.

**Architecture:** The SoM-side ADC revision path in `src/zephyr/hw_info_zephyr.c` is a no-op stub (`adc_cross_check()`); it is deleted along with the `som_board_id_mv` field of `alp_hw_info_t`. Manifest validation (magic → schema → CRC32 → field population) is split into a pure, non-static `alp_hw_info_classify_manifest()` declared in a new internal header `src/common/hw_info_manifest.h`, so native_sim ztests can feed crafted 128-byte buffers and exercise the new provisioned/corrupt/blank decision without an EEPROM device. A new `ALP_ERR_NOT_PROVISIONED` status distinguishes a blank EEPROM (no `ALPH` magic) from real corruption (`ALP_ERR_IO`). The carrier-side `board_id` path and `hw-revisions.yaml` are untouched.

**Tech Stack:** C11 (Zephyr module), Zephyr ztest under `native_sim/native/64`, twister, clang-format-14, Python doc-drift/doxygen gates.

**Spec:** `docs/superpowers/specs/2026-05-31-eeprom-authoritative-som-rev-design.md`

**Branch:** `feat/eeprom-authoritative-som-rev` (already created off `dev`; the spec doc is already committed there).

---

## Test runner (scratch, do NOT commit)

Several tasks below run only the `hw_info` twister suite. The WSL inline-variable-empty bug means you must use a `.sh` script with literal paths, not inline `VAR=… wsl bash -lc`. Create this scratch file once at the repo root (it is untracked; never `git add` it):

`_twister_hwinfo.sh`:
```bash
#!/usr/bin/env bash
export ZEPHYR_TOOLCHAIN_VARIANT=host
export ZEPHYR_BASE=/mnt/c/Users/caner/Documents/GitHub/zephyrproject/zephyr
export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk
cd "$ZEPHYR_BASE" || { echo "NO_ZEPHYR_BASE"; exit 2; }
python3 scripts/twister \
  --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/tests/zephyr/hw_info \
  -p native_sim/native/64 -O /tmp/tw-hwinfo --clobber-output
rc=$?
echo "HWINFO_TWISTER rc=$rc"
exit $rc
```

Run it (from the Bash tool, after stripping CRs):
```bash
sed -i 's/\r$//' /mnt/c/Users/caner/Documents/GitHub/alp-sdk/_twister_hwinfo.sh
wsl bash /mnt/c/Users/caner/Documents/GitHub/alp-sdk/_twister_hwinfo.sh
```
Expected on success: `HWINFO_TWISTER rc=0` and twister reporting the `alp_sdk.hw_info` scenario passed.

---

## Task 1: Add `ALP_ERR_NOT_PROVISIONED` to the status enum

**Files:**
- Modify: `include/alp/peripheral.h:66-67`

- [ ] **Step 1: Add the enumerator**

In `include/alp/peripheral.h`, the enum currently ends:

```c
    ALP_ERR_NOT_FOUND = -14 /**< An explicitly-requested backend is absent from the package. */
} alp_status_t;
```

Change it to (add the comma after `-14`, then the new entry):

```c
    ALP_ERR_NOT_FOUND = -14, /**< An explicitly-requested backend is absent from the package. */
    ALP_ERR_NOT_PROVISIONED =
        -15 /**< Hardware identity store (e.g. the on-module EEPROM manifest) is blank / unprogrammed -- the module has not been provisioned by the factory tool yet. */
} alp_status_t;
```

- [ ] **Step 2: Format**

Run: `wsl bash -lc 'cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk && clang-format-14 -i include/alp/peripheral.h'`
(If `clang-format-14` is not on PATH, use the repo's pinned binary the same way `running-local-ci` invokes it.)
Expected: the new entry is reflowed in-style; inspect `git diff include/alp/peripheral.h` and confirm only the two intended lines changed (plus any whitespace clang-format applies to the new entry). If `AlignConsecutiveAssignments` reflows neighbouring entries, that is acceptable — accept clang-format's output.

- [ ] **Step 3: Verify the header still compiles**

Run the hw_info suite (it includes `peripheral.h`):
```bash
sed -i 's/\r$//' /mnt/c/Users/caner/Documents/GitHub/alp-sdk/_twister_hwinfo.sh
wsl bash /mnt/c/Users/caner/Documents/GitHub/alp-sdk/_twister_hwinfo.sh
```
Expected: `HWINFO_TWISTER rc=0` (behaviour unchanged; this only proves the enum addition compiles).

- [ ] **Step 4: Commit**

```bash
git add include/alp/peripheral.h
git commit -q -m "feat(hw_info): add ALP_ERR_NOT_PROVISIONED status code"
```

---

## Task 2: Split out a testable manifest classifier with the new error semantics

This is the core change. TDD: write the classifier's failing tests first, then implement it, rewire the reader, and delete the ADC stub.

**Files:**
- Create: `src/common/hw_info_manifest.h`
- Test: `tests/zephyr/hw_info/src/main.c` (append new ztests)
- Modify: `src/zephyr/hw_info_zephyr.c`

- [ ] **Step 1: Create the internal header**

Create `src/common/hw_info_manifest.h` (this directory is already on the Zephyr include path via `zephyr_include_directories(.../src/common)`, so the test reaches it with `#include "hw_info_manifest.h"` and no CMake change):

```c
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal (non-public) declarations for the hw_info EEPROM-manifest
 * reader.  Implemented in src/zephyr/hw_info_zephyr.c and split out
 * from the I2C read path so native_sim tests can classify crafted
 * 128-byte manifest buffers without a real EEPROM device.
 * NOT part of the public <alp/*> API.
 */
#ifndef ALP_SDK_HW_INFO_MANIFEST_H
#define ALP_SDK_HW_INFO_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "alp/hw_info.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** CRC-32 (ISO-3309 / zlib polynomial 0xEDB88320) over @p len bytes of @p buf. */
uint32_t alp_hw_info_crc32(const uint8_t *buf, size_t len);

/**
 * @brief Validate a 128-byte EEPROM manifest and populate the SoM half of @p out.
 *
 * @param[in]  manifest  A 128-byte manifest buffer as read from the EEPROM.
 * @param[out] out       Populated on success; left as-is on failure (the
 *                       caller pre-zeroes it; this function does NOT zero it).
 *
 * @return ALP_OK on a valid manifest;
 *         ALP_ERR_NOT_PROVISIONED when no ALPH magic is present (blank/erased EEPROM);
 *         ALP_ERR_IO when magic is present but schema_version or CRC32 is invalid (corruption);
 *         ALP_ERR_INVAL when either pointer is NULL.
 */
alp_status_t alp_hw_info_classify_manifest(const alp_hw_info_eeprom_t *manifest,
                                           alp_hw_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ALP_SDK_HW_INFO_MANIFEST_H */
```

- [ ] **Step 2: Write the failing tests**

In `tests/zephyr/hw_info/src/main.c`, add the include near the existing includes (after `#include "alp/peripheral.h"`):

```c
#include "hw_info_manifest.h"
```

Then append these to the file (after the existing `test_eeprom_manifest_is_exactly_128_bytes` ztest, before EOF):

```c
/* ---- alp_hw_info_classify_manifest: the EEPROM-authoritative path ---- */

static void make_valid_manifest(alp_hw_info_eeprom_t *m)
{
    memset(m, 0, sizeof(*m));
    m->magic          = ALP_HW_INFO_MAGIC;
    m->schema_version = ALP_HW_INFO_SCHEMA_VERSION;
    strcpy(m->family, "v2n");
    strcpy(m->sku, "E1M-V2N101");
    strcpy(m->hw_rev, "r1");
    strcpy(m->serial, "ALP-V2N101-26W19-00042");
    m->mfg_year  = 2026;
    m->mfg_month = 5;
    m->mfg_day   = 9;
    m->crc32 = alp_hw_info_crc32((const uint8_t *)m, sizeof(*m) - sizeof(m->crc32));
}

ZTEST(alp_hw_info, test_classify_valid_returns_ok)
{
    alp_hw_info_eeprom_t m;
    make_valid_manifest(&m);
    alp_hw_info_t info;
    memset(&info, 0, sizeof(info));
    zassert_equal(alp_hw_info_classify_manifest(&m, &info), ALP_OK);
    zassert_str_equal(info.som_family, "v2n");
    zassert_str_equal(info.som_sku, "E1M-V2N101");
    zassert_str_equal(info.som_hw_rev, "r1");
    zassert_equal(info.som_mfg_year, 2026);
    zassert_equal(info.som_mfg_month, 5);
    zassert_equal(info.som_mfg_day, 9);
}

ZTEST(alp_hw_info, test_classify_blank_eeprom_returns_not_provisioned)
{
    alp_hw_info_eeprom_t m;
    memset(&m, 0xFF, sizeof(m));   /* erased flash/EEPROM pattern */
    alp_hw_info_t info;
    memset(&info, 0, sizeof(info));
    zassert_equal(alp_hw_info_classify_manifest(&m, &info), ALP_ERR_NOT_PROVISIONED);
    zassert_equal(info.som_hw_rev[0], 0); /* out untouched on failure */
}

ZTEST(alp_hw_info, test_classify_zeroed_eeprom_returns_not_provisioned)
{
    alp_hw_info_eeprom_t m;
    memset(&m, 0, sizeof(m));      /* freshly zeroed, no magic */
    alp_hw_info_t info;
    memset(&info, 0, sizeof(info));
    zassert_equal(alp_hw_info_classify_manifest(&m, &info), ALP_ERR_NOT_PROVISIONED);
}

ZTEST(alp_hw_info, test_classify_bad_schema_returns_io)
{
    alp_hw_info_eeprom_t m;
    make_valid_manifest(&m);
    m.schema_version = 99u;        /* magic OK, body wrong */
    m.crc32 = alp_hw_info_crc32((const uint8_t *)&m, sizeof(m) - sizeof(m.crc32));
    alp_hw_info_t info;
    memset(&info, 0, sizeof(info));
    zassert_equal(alp_hw_info_classify_manifest(&m, &info), ALP_ERR_IO);
}

ZTEST(alp_hw_info, test_classify_bad_crc_returns_io)
{
    alp_hw_info_eeprom_t m;
    make_valid_manifest(&m);
    m.crc32 ^= 0xFFFFFFFFu;         /* corrupt the checksum */
    alp_hw_info_t info;
    memset(&info, 0, sizeof(info));
    zassert_equal(alp_hw_info_classify_manifest(&m, &info), ALP_ERR_IO);
}

ZTEST(alp_hw_info, test_classify_null_args_return_inval)
{
    alp_hw_info_eeprom_t m;
    make_valid_manifest(&m);
    alp_hw_info_t info;
    zassert_equal(alp_hw_info_classify_manifest(NULL, &info), ALP_ERR_INVAL);
    zassert_equal(alp_hw_info_classify_manifest(&m, NULL), ALP_ERR_INVAL);
}
```

- [ ] **Step 3: Run the tests to confirm they fail (link error)**

```bash
sed -i 's/\r$//' /mnt/c/Users/caner/Documents/GitHub/alp-sdk/_twister_hwinfo.sh
wsl bash /mnt/c/Users/caner/Documents/GitHub/alp-sdk/_twister_hwinfo.sh
```
Expected: BUILD FAILURE — `undefined reference to alp_hw_info_classify_manifest` / `alp_hw_info_crc32` (the symbols are declared but not yet defined).

- [ ] **Step 4: Implement the classifier + expose the CRC**

In `src/zephyr/hw_info_zephyr.c`:

(a) Add the internal header to the includes (after `#include "alp/peripheral.h"`):
```c
#include "hw_info_manifest.h"
```

(b) Change the CRC helper from `static`/`unused` to the exposed internal symbol. Replace:
```c
__attribute__((unused)) static uint32_t crc32_iso3309(const uint8_t *buf, size_t len)
```
with:
```c
uint32_t alp_hw_info_crc32(const uint8_t *buf, size_t len)
```

(c) `copy_field` is now used by the always-compiled classifier, so drop its `unused` marker. Replace:
```c
__attribute__((unused)) static void copy_field(char *dst, size_t dst_len, const char *src,
                                               size_t src_len)
```
with:
```c
static void copy_field(char *dst, size_t dst_len, const char *src, size_t src_len)
```

(d) Delete the entire `adc_cross_check` block (the `TODO(hw_info ADC cross-check)` comment through the closing brace of the function — currently lines 107-119):
```c
/* TODO(hw_info ADC cross-check): once scripts/alp_project.py emits
 * ...
__attribute__((unused)) static alp_status_t adc_cross_check(...)
{
    (void)manifest;
    (void)out;
    return ALP_OK; /* No-op until the generated header lands. */
}
```
In its place, add the classifier (always compiled — it must sit OUTSIDE the `#if ALP_HW_INFO_EEPROM_ENABLED` guard, e.g. right here after the guard's `#endif`):
```c
/* ----------------------------------------------------------------
 * Manifest classification (pure; no I2C).  The EEPROM manifest is
 * the single authoritative source of the SoM hardware revision --
 * there is no ADC cross-check.  Split out from the I2C read path so
 * native_sim tests can feed crafted buffers; declared in
 * src/common/hw_info_manifest.h.
 * ---------------------------------------------------------------- */
alp_status_t alp_hw_info_classify_manifest(const alp_hw_info_eeprom_t *manifest,
                                           alp_hw_info_t *out)
{
    if (manifest == NULL || out == NULL) return ALP_ERR_INVAL;

    /* No valid header: a blank/erased EEPROM (0xFF or zeroed) carries
     * no ALPH magic.  This is the expected state of a module the
     * factory programmer has not run on yet -- distinct from a read
     * fault, so report NOT_PROVISIONED rather than IO. */
    if (manifest->magic != ALP_HW_INFO_MAGIC) return ALP_ERR_NOT_PROVISIONED;

    /* Header claims our format but the body disagrees: corruption. */
    if (manifest->schema_version != ALP_HW_INFO_SCHEMA_VERSION) return ALP_ERR_IO;

    const size_t crc_covered_len = sizeof(*manifest) - sizeof(manifest->crc32);
    uint32_t     calc_crc        = alp_hw_info_crc32((const uint8_t *)manifest, crc_covered_len);
    if (calc_crc != manifest->crc32) return ALP_ERR_IO;

    copy_field(out->som_family, sizeof(out->som_family),
               manifest->family, sizeof(manifest->family));
    copy_field(out->som_sku, sizeof(out->som_sku),
               manifest->sku, sizeof(manifest->sku));
    copy_field(out->som_hw_rev, sizeof(out->som_hw_rev),
               manifest->hw_rev, sizeof(manifest->hw_rev));
    copy_field(out->som_serial, sizeof(out->som_serial),
               manifest->serial, sizeof(manifest->serial));
    out->som_mfg_year  = manifest->mfg_year;
    out->som_mfg_month = manifest->mfg_month;
    out->som_mfg_day   = manifest->mfg_day;
    return ALP_OK;
}
```

(e) Rewire `alp_hw_info_read`. Replace the body of the `#else` branch (currently lines 133-172: the inline `manifest.magic`/`schema_version`/CRC checks, the `copy_field` block, the `adc_cross_check` call, and the board-side TODO) with:
```c
    alp_hw_info_eeprom_t manifest;
    memset(&manifest, 0, sizeof(manifest));

    alp_status_t s = read_manifest(&manifest);
    if (s != ALP_OK) return s == ALP_ERR_NOSUPPORT ? ALP_ERR_NOT_READY : s;

    /* Validate + populate.  Board-side BOARD_ID decode is a future
     * addition (board.yaml -> generated header). */
    return alp_hw_info_classify_manifest(&manifest, out);
```
Leave the `#if !ALP_HW_INFO_EEPROM_ENABLED ... return ALP_ERR_NOSUPPORT;` arm and the `#endif` exactly as they are.

(f) Update the file's top banner comment (lines 11-16) that describes the ADC cross-check as TODO. Replace:
```c
 * BOARD_ID ADC cross-check is TODO pending the per-family generated
 * header that maps `hw_rev` strings to expected mV bins -- those
 * tables live in metadata/e1m_modules/<family>/hw-revisions.yaml
 * today and need a `scripts/alp_project.py` pass to emit the
 * runtime-readable form.  Once available, plug into the
 * adc_cross_check() helper below.
```
with:
```c
 * The EEPROM manifest is the single authoritative source of the SoM
 * hardware revision; validation (magic + schema_version + CRC32) and
 * field population live in alp_hw_info_classify_manifest(), split out
 * so native_sim tests can exercise it without a real EEPROM.
```

- [ ] **Step 5: Run the tests to confirm they pass**

```bash
wsl bash /mnt/c/Users/caner/Documents/GitHub/alp-sdk/_twister_hwinfo.sh
```
Expected: `HWINFO_TWISTER rc=0`; all `alp_hw_info` ztests pass, including the six new `test_classify_*` cases.

- [ ] **Step 6: Format**

Run: `wsl bash -lc 'cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk && clang-format-14 -i src/zephyr/hw_info_zephyr.c src/common/hw_info_manifest.h tests/zephyr/hw_info/src/main.c'`
Expected: minor or no changes; review `git diff` for sanity.

- [ ] **Step 7: Commit**

```bash
git add src/common/hw_info_manifest.h src/zephyr/hw_info_zephyr.c tests/zephyr/hw_info/src/main.c
git commit -q -m "feat(hw_info): EEPROM manifest is the sole SoM-rev source

Split manifest validation into a pure, testable
alp_hw_info_classify_manifest(); a blank EEPROM (no ALPH magic) now
returns ALP_ERR_NOT_PROVISIONED, a present-but-bad manifest returns
ALP_ERR_IO. Delete the no-op SoM-side ADC cross-check stub."
```

---

## Task 3: Remove `som_board_id_mv` and rewrite the public header docs

**Files:**
- Modify: `include/alp/hw_info.h`
- Modify: `tests/zephyr/hw_info/src/main.c:39`

- [ ] **Step 1: Update the existing NOSUPPORT test (drop the removed field)**

In `tests/zephyr/hw_info/src/main.c`, in `test_read_returns_nosupport_v03`, delete this line:
```c
    zassert_equal(info.som_board_id_mv, 0);
```
Leave the surrounding `som_family` / `som_sku` / `som_hw_rev` / `board_hw_rev` / `board_id_mv` assertions intact.

- [ ] **Step 2: Remove the field from the struct**

In `include/alp/hw_info.h`, delete the SoM BOARD_ID field + its comment from `alp_hw_info_t`:
```c
    /** Measured SoM BOARD_ID ADC reading, mV.  0 when not read. */
    uint32_t som_board_id_mv;
```
Keep all other SoM fields and the entire board-side block (`board_name` / `board_hw_rev` / `board_id_mv`).

- [ ] **Step 3: Rewrite the header's design narrative (EEPROM-authoritative)**

In `include/alp/hw_info.h`, replace the two-surface description (lines 12-35, the block that starts "The SDK identifies the assembled hardware via two independent surfaces" and ends with the "read fails loudly" paragraph) with:

```c
 * The SoM hardware revision is identified by ONE authoritative
 * surface:
 *
 *   **On-module EEPROM manifest** -- a fixed-layout 128-byte block
 *   at offset 0x0000 of the SoM's on-module 24C128.  Carries the
 *   exact MPN string, hw_rev, factory serial number, and
 *   manufacturing date.  Programmed at production-test time by
 *   `scripts/program_eeprom.py`; read + integrity-checked (magic +
 *   schema_version + CRC32) by the SDK at boot.  The EEPROM travels
 *   with the SoM, so it IS the module's identity -- there is no
 *   ADC resistor-divider cross-check on the SoM side.
 *
 * Carrier boards may still encode their own revision on a board-side
 * BOARD_ID resistor divider (see `board_hw_rev` / `board_id_mv`
 * below); that is a separate, board-side path and is independent of
 * the SoM revision.
```

- [ ] **Step 4: Update the `alp_hw_info_read` Doxygen `@return` + "Reads, in order" list**

In `include/alp/hw_info.h`, replace the "Reads, in order:" list (the `-#` items describing EEPROM, SoM BOARD_ID ADC cross-check, board-side BOARD_ID) with:
```c
 * Reads + integrity-checks the on-module EEPROM manifest (magic +
 * schema_version + CRC32) and copies the SoM identifiers out.  The
 * manifest is the sole source of the SoM hardware revision.  (A
 * board-side BOARD_ID decode is a future addition.)
```
Replace the `@return` block with:
```c
 * @return  @ref ALP_OK on a valid manifest read.
 *          @ref ALP_ERR_INVAL when @p out is NULL.
 *          @ref ALP_ERR_NOT_PROVISIONED when the EEPROM reads back
 *                                       blank/unprogrammed (no ALPH magic).
 *          @ref ALP_ERR_IO when the manifest is corrupt (magic present
 *                          but bad schema_version / CRC) OR the EEPROM
 *                          bus read faults (NAK / line error).
 *          @ref ALP_ERR_NOT_READY when the EEPROM/I2C layer reports the
 *                                 device is unavailable.
 *          @ref ALP_ERR_NOSUPPORT when no EEPROM bus is configured
 *                                 (CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID
 *                                 unset / < 0).
```

- [ ] **Step 5: Fix the struct-level doc comment for `alp_hw_info_t`**

Replace its comment (the "Populated from BOTH the EEPROM manifest … and the BOARD_ID ADC readings (cross-check on hw_rev)" paragraph) with:
```c
/**
 * @brief Combined runtime board info as returned by @ref alp_hw_info_read.
 *
 * The SoM fields come from the on-module EEPROM manifest (the
 * authoritative SoM identity).  The board-side fields are decoded
 * from the board preset's BOARD_ID path and stay zero/empty when no
 * board is declared in `board.yaml` or no board-side BOARD_ID ADC
 * channel is wired.
 */
```

- [ ] **Step 6: Update the `@code` usage comment + the stale v0.3 NOSUPPORT note**

In the top `@code` example, change the comment:
```c
 *     // EEPROM unprogrammed, corrupted, or ADC mismatch.
```
to:
```c
 *     // EEPROM unprogrammed (NOT_PROVISIONED), corrupted (IO),
 *     // unreachable (NOT_READY), or no bus configured (NOSUPPORT).
```
Then update the "v0.3 ships the API contract only … both entry points return @ref ALP_ERR_NOSUPPORT" paragraph to reflect that the EEPROM read path is implemented and NOSUPPORT now specifically means "no EEPROM bus configured":
```c
 * The runtime EEPROM read path is implemented.  On a build with no
 * EEPROM bus configured (CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID
 * unset / < 0) both entry points return @ref ALP_ERR_NOSUPPORT and
 * the out-struct is zero-filled.
```

- [ ] **Step 7: Build + test**

```bash
wsl bash /mnt/c/Users/caner/Documents/GitHub/alp-sdk/_twister_hwinfo.sh
```
Expected: `HWINFO_TWISTER rc=0`; the suite still passes with `som_board_id_mv` gone.

- [ ] **Step 8: Format + commit**

```bash
wsl bash -lc 'cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk && clang-format-14 -i include/alp/hw_info.h tests/zephyr/hw_info/src/main.c'
git add include/alp/hw_info.h tests/zephyr/hw_info/src/main.c
git commit -q -m "refactor(hw_info)!: drop som_board_id_mv; EEPROM-authoritative docs

Remove the SoM BOARD_ID ADC field from alp_hw_info_t and rewrite the
public header to describe the EEPROM manifest as the single SoM-rev
source. No deprecation shim (no active customers)."
```

---

## Task 4: Update the `v2n-board-id-readout` example

**Files:**
- Modify: `examples/v2n/v2n-board-id-readout/src/main.c`

- [ ] **Step 1: Add the NOT_PROVISIONED case and reword the IO case**

In the `switch (s)` block, replace the existing `ALP_ERR_IO` case:
```c
    case ALP_ERR_IO:
        printf("[board-id] manifest CRC mismatch or magic byte wrong\n");
        printf("[board-id]   factory programming has not run on this\n");
        printf("[board-id]   module; flag for production-test follow-up.\n");
        break;
```
with these TWO cases (IO now means strictly corruption; NOT_PROVISIONED is the blank module):
```c
    case ALP_ERR_NOT_PROVISIONED:
        printf("[board-id] EEPROM is blank -- module not yet provisioned\n");
        printf("[board-id]   run scripts/program_eeprom.py at production\n");
        printf("[board-id]   test to write the SoM manifest.\n");
        break;
    case ALP_ERR_IO:
        printf("[board-id] manifest is corrupt (bad schema or CRC)\n");
        printf("[board-id]   magic is present but the body failed its\n");
        printf("[board-id]   integrity check; flag for rework.\n");
        break;
```

- [ ] **Step 2: Build the example**

Temporarily point the scratch runner at the example (or run the full examples root). Quick check — run the examples suite filtered to this app:
```bash
wsl bash -lc 'export ZEPHYR_TOOLCHAIN_VARIANT=host; export ZEPHYR_BASE=/mnt/c/Users/caner/Documents/GitHub/zephyrproject/zephyr; export EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk; cd "$ZEPHYR_BASE" && python3 scripts/twister --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/examples/v2n/v2n-board-id-readout -p native_sim/native/64 -O /tmp/tw-ex --clobber-output'
```
Expected: the example builds (it may be statically filtered/skipped on native_sim if it requires real hardware — a `skipped` result is acceptable, a BUILD FAILURE is not). If it is skipped by a platform filter, confirm a clean compile instead by running the full examples scope in Task 6.

- [ ] **Step 3: Commit**

```bash
git add examples/v2n/v2n-board-id-readout/src/main.c
git commit -q -m "docs(example): v2n-board-id-readout handles ALP_ERR_NOT_PROVISIONED"
```

---

## Task 5: Update docs — `board-id.md` and `soms/v2n.md`

**Files:**
- Modify: `docs/board-id.md`
- Modify: `docs/soms/v2n.md`

- [ ] **Step 1: Retitle and rewrite the intro of `docs/board-id.md`**

Replace the title + the two-stage intro (lines 1-27, from `# Board identification …` through the "Wrong firmware build." bullet block) with:

```markdown
# Board identification — SoM EEPROM manifest

The SoM hardware revision is identified by a single authoritative
surface: a **128-byte EEPROM manifest** programmed once into the
SoM's on-module 24C128. It carries family, SKU, hardware revision,
serial number, and manufacturing date. Implemented in
[`src/zephyr/hw_info_zephyr.c`](../src/zephyr/hw_info_zephyr.c) as the
EEPROM-side reader behind `alp_hw_info_read()`.

The EEPROM travels with the SoM, so it *is* the module's identity.
There is no SoM-side ADC resistor-divider cross-check; the manifest's
own integrity protection (magic + `schema_version` + CRC32) is what
guards against an unprogrammed or corrupt module.

This guards against:

* **Wrong firmware build.** Application links against the wrong
  metadata-driven config. `alp_hw_info_assert_matches_build()` is the
  explicit check that catches a build pointed at the wrong SoM SKU /
  hw_rev.
* **Unprogrammed or corrupt module.** A blank EEPROM returns
  `ALP_ERR_NOT_PROVISIONED`; a present-but-corrupt manifest returns
  `ALP_ERR_IO`. Boot code can branch on which.

> **Carrier note.** A carrier/EVK may still encode its own revision on
> a board-side BOARD_ID resistor divider, surfaced as `board_hw_rev` /
> `board_id_mv`. That is a separate, board-side path independent of the
> SoM revision and is not covered here.
```

- [ ] **Step 2: Update the runtime read-flow diagram**

In `docs/board-id.md`, in the "Runtime read flow" code block, replace the line:
```
   ├── adc_cross_check(manifest, out)   ← currently no-op TODO
```
with:
```
   ├── classify: magic (else NOT_PROVISIONED) → schema/CRC (else IO)
```
And below that block, replace the paragraph that says "Mismatch returns `ALP_ERR_IO`; application code can choose to log and continue, or halt boot" with:
```markdown
A blank EEPROM (no `ALPH` magic) returns `ALP_ERR_NOT_PROVISIONED`; a
manifest whose magic is present but whose `schema_version` or CRC32 is
wrong returns `ALP_ERR_IO`. `alp_hw_info_assert_matches_build()`
returns `ALP_ERR_IO` on a SKU/hw_rev disagreement. Application code
can log and continue, or halt boot, depending on safety requirements.
```

- [ ] **Step 3: Delete the BOARD_ID ADC cross-check section**

In `docs/board-id.md`, delete the entire `## BOARD_ID ADC cross-check (TODO)` section (heading through the end of its content, including the `adc_cross_check` C snippet, the `v2n_hwrev_bins[]` sample, the divider-math paragraph, and the `scripts/check-hw-rev-bins.py` reference) — i.e. everything from `## BOARD_ID ADC cross-check (TODO)` up to (but not including) the next `## V2N-specific specifics` heading.

- [ ] **Step 4: Fix the V2N / V2N-M1 specifics + See-also**

In `docs/board-id.md`, in `## V2N-specific specifics`, delete the `**BOARD_ID ADC**: …` bullet entirely (keep the `**EEPROM**` and `**Kconfig**` bullets). In `## V2N-M1 specifics`, replace "Same EEPROM and BOARD_ID ADC; the manifest's `family` field reads `v2n-m1`…" with "Same EEPROM manifest; the manifest's `family` field reads `v2n-m1`…". In `## See also`, delete the `hw-revisions.yaml … divider definition` bullet's "divider definition" framing — change it to:
```markdown
* [`metadata/e1m_modules/v2n/hw-revisions.yaml`](../metadata/e1m_modules/v2n/hw-revisions.yaml) --
  V2N hw-rev registry (revision ids + SDK-version gating).
```

- [ ] **Step 5: Update `docs/soms/v2n.md` identification section**

Replace the "Two-stage SoM-ID flow:" block (the numbered list with EEPROM manifest + BOARD_ID ADC) with:
```markdown
SoM identification is EEPROM-authoritative:

**EEPROM manifest** -- 128-byte block at offset 0 of the on-module
24C128 carrying family / SKU / hw_rev / serial / mfg date, integrity-
checked (magic + schema + CRC32). Read via `alp_hw_info_read()`. A
blank module returns `ALP_ERR_NOT_PROVISIONED`; a corrupt one returns
`ALP_ERR_IO`. The EEPROM is the sole source of the SoM revision (no
ADC cross-check).
```

- [ ] **Step 6: Run the doc gates**

```bash
wsl bash -lc 'cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk && py -3.14 scripts/check_doc_drift.py && py -3.14 scripts/check_doxygen_coverage.py'
```
(If `py -3.14` is Windows-only, run via PowerShell instead: `py -3.14 scripts/check_doc_drift.py; py -3.14 scripts/check_doxygen_coverage.py`.)
Expected: both pass. If `check_doc_drift.py` complains that `ALP_ERR_NOT_PROVISIONED` is undocumented, confirm it now appears in `docs/board-id.md` (Steps 1-2 add it). If it complains about a now-dangling reference to `check-hw-rev-bins.py` elsewhere, grep for and fix the stray reference.

- [ ] **Step 7: Commit**

```bash
git add docs/board-id.md docs/soms/v2n.md
git commit -q -m "docs: EEPROM-authoritative SoM revision; retire ADC cross-check prose"
```

---

## Task 6: Full local CI + final verification

**Files:** none (verification only).

- [ ] **Step 1: Confirm no stray references remain**

```bash
cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk 2>/dev/null || cd "C:/Users/caner/Documents/GitHub/alp-sdk"
git grep -n 'som_board_id_mv\|adc_cross_check\|check-hw-rev-bins' -- ':!docs/superpowers/specs/*' ':!docs/superpowers/plans/*'
```
Expected: NO matches (the spec/plan docs under `docs/superpowers/` legitimately still mention them as history).

- [ ] **Step 2: Full native_sim twister scope**

Use the load-bearing local gate (full tests + examples roots), per the `reference_local_twister_invocation` convention:
```bash
sed -i 's/\r$//' /mnt/c/Users/caner/Documents/GitHub/alp-sdk/_twister_gap.sh
wsl bash /mnt/c/Users/caner/Documents/GitHub/alp-sdk/_twister_gap.sh
```
Expected: `TWISTER_DONE rc=0`, 0 failed / 0 errored (skips are fine). Do NOT switch git branches while this runs.

- [ ] **Step 3: clang-format diff gate**

```bash
wsl bash -lc 'cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk && git fetch origin dev -q && git diff -U0 origin/dev...HEAD -- "*.c" "*.h" | clang-format-14 --files=- 2>/dev/null; echo "format-check-done"'
```
Or run the repo's documented diff-only clang-format gate the same way `running-local-ci` does. Expected: no diffs reported on changed lines.

- [ ] **Step 4: Push the branch (only after the user confirms)**

Per the `dev`-integration + ask-before-push conventions, do NOT push automatically. Report the green local CI to the user and ask whether to push `feat/eeprom-authoritative-som-rev` and open a PR into `dev`.

---

## Task 7: Sibling-repo follow-up (alp-sdk-internal) — separate commit

**Files (different repo):**
- Modify: `alp-sdk-internal/EEPROM-MANIFEST-SPEC.md`

This file lives in the private `alp-sdk-internal` repo, outside this repo's CI. Do it as a separate commit in that repo (not on this branch).

- [ ] **Step 1: Mark `hw_rev` authoritative + document the read contract**

In `alp-sdk-internal/EEPROM-MANIFEST-SPEC.md`, add/lift a note that the manifest's `hw_rev` field is the **single authoritative source of the SoM hardware revision** (the SoM-side ADC divider path is retired), and document the runtime read contract: blank EEPROM (no `ALPH` magic) → `ALP_ERR_NOT_PROVISIONED`; magic present but `schema_version`/CRC32 invalid → `ALP_ERR_IO`. Record the provisioning safeguard: the factory flow should do **write → read-back → CRC-verify** before passing a board, since the EEPROM is now the single point of truth.

- [ ] **Step 2: Commit in the sibling repo**

```bash
cd "C:/Users/caner/Documents/GitHub/alp-sdk-internal"
git add EEPROM-MANIFEST-SPEC.md
git commit -q -m "docs(eeprom): hw_rev is the authoritative SoM revision; document read contract"
```

---

## Notes / explicitly out of scope

- `metadata/e1m_modules/v2n/hw-revisions.yaml` is **intentionally untouched** — it is the revision registry (`r1`–`r8`) + SDK-version gating, not ADC bin data (it never carried bin voltages).
- The carrier-side `board_id` path (`board_hw_rev` / `board_id_mv`) is **kept**.
- No `alp_hw_info_write()` public API is added; provisioning stays a factory-tool/QC concern (`scripts/program_eeprom.py`).
- `ADC2_CH7` being freed on the SoM is a hardware/maintainer follow-up, not an SDK change.
