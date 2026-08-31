# #1365 split A — declare the MRAM aperture, author write authority, gate both

Date: 2026-08-31
Status: Draft — blocked on two decisions (see "Before this starts")
Issue: [#1365](https://github.com/alplabai/alp-sdk/issues/1365)
Scope owner: alpCaner

## What this is, and what it deliberately is not

#1365 asks for three things in dependency order: derive a region's flash/RAM
class, require its owner, and emit a resolved memory view into
`system-manifest-v1`. Reviewing it against the tree produced a three-way split.
**This plan covers split A only.**

| Split | Contents | Status |
|---|---|---|
| **A** — this plan | SoC aperture field, authored write authority, three producer-side checks | Ready once two decisions land |
| **B** | Derived `kind` in the allocator, `carveout` demoted, the two eligibility predicates | Deferred behind ADR-0026 amendment section G step 2 |
| **C** | `system-manifest-v1` `memory[]` | With B, in whichever repo owns manifest bytes per ADR-0026 section D |

A is all hardware truth plus producer-side checks. ADR-0026 clause 2 keeps
`metadata/` in alp-sdk under every migration outcome, so none of it is work that
moves or dies. Its consumer surface is zero: no emitted artefact changes, so
tan's byte-parity fixtures are untouched.

**Explicit non-goals of A.** Each of these is B or C, and doing any of them here
turns a zero-consumer-surface change into a cross-repo port:

- No allocator behaviour change. `scripts/alp_orchestrate/carveout.py` and
  `scripts/alp_orchestrate/partition.py` are not edited.
- **`mram_main`'s `base: "TBD"` is not filled.** See "The ordering hazard".
- No `carveout` demotion, deprecation or removal.
- No `system-manifest-v1` change, so no emitted bytes change.
- `kind` is computed **inside the checks only**, never consumed by the build.

## Why A is worth landing on its own

The hazard is recorded on silicon in `metadata/e1m_modules/E1M-AEN801.yaml`:

```text
#   5552 B   bench-observed 2026-08-08 -- ATOC magic `ckBS` (0x53426B63)
#            read at 0x8057EA50, intact, while an app erased 0x80560000
#            inside what was then the SAME `storage` partition
```

`storage` (`0x80560000`, 96 KiB, customer-mountable) and `atoc` (`0x80578000`,
32 KiB, written by the Secure Enclave) are **indistinguishable in the data**:
same shape, same `carveout: false`. The distinction survives only in a prose
comment. ATOC corruption can leave the part unbootable.

What protects that band today is three coincidences — hand-authored
`carveout: false` on six regions, `mram_main`'s TBD base, and #1331's
reserved-span seeding. A does not change the allocator, so it removes none of
them. What it does is make the **authored data unable to rot**: after A, a new
SoM whose author forgets a flag fails a check instead of shipping.

## The ordering hazard — read before writing any code

`scripts/alp_orchestrate/carveout.py:88-91` states the allocator will "emit a
`status: blocked` entry when the matching region has a TBD base / size (the SoM
isn't HW-mapped yet)". So `mram_main.base: "TBD"` is **currently one of the
things blocking IPC carve-out allocation into the MRAM window**.

Filling that base from the new aperture field before derived-kind ineligibility
is enforced in the allocator would open the hole the TBD is plugging. The order
is: **derived-kind refusal first (split B), then base fill.** Never the reverse,
and never inside A.

## Before this starts — two decisions

Neither is mine to make, and A cannot be written without them.

1. **The write-authority vocabulary.** #1365 proposes `customer` / `vendor` /
   `secure`. Test it against four regions before adopting: `he_slot0` (customer
   image, flashed, never runtime-writable), `mcuboot` (vendor image, flashed),
   `storage` (customer, runtime-writable), `atoc` (SE-written at provisioning).
   If two collapse onto one value the vocabulary is under-specified for the next
   hazard.

   The repo's own precedent is `$defs/helper_firmware_entry/properties/flash_policy`
   in the same schema: `enum: ["customer", "factory", "recovery_only"]`,
   described as "WHO may invoke `flash_method`, and WHEN.  REQUIRED on every
   helper entry -- there is no absent-means-`customer` default". A WHO+WHEN
   vocabulary shaped like that keeps all four regions distinct.

2. **Field name and required-ness.** `owner` reads as ownership; the distinction
   the hazard needs is write *authority*. And required-ness cuts both ways: the
   `flash_policy` precedent is already `required` in som-preset **v1**, while
   `docs/porting-new-som.md` is a public customer porting guide and `tan new-som`
   scaffolds against this schema, so a v1 `required` breaks a customer-ported
   preset on upgrade. This plan assumes **authored now, semantically enforced by
   a check, schema-`required` deferred to som-preset v2**; if the maintainer
   prefers schema-`required` in v1, step 2 changes and step 4 gains a
   derived-path defaulting rule.

Placeholders below: `AUTH` is the field name, `auth-values` the enum.

## Measured facts this plan is sized against

- **Six** SoM presets author a `memory_map:`, not one:
  `E1M-AEN301`, `E1M-AEN401`, `E1M-AEN501`, `E1M-AEN601`, `E1M-AEN701`,
  `E1M-AEN801` — **7 rows each, 42 rows total**. (#1365 and its review both
  implied AEN801 alone; #1447 added the other five.) The remaining five presets
  (`E1M-V2N101/102`, `E1M-V2M101/102`, `E1M-NX9101`) author none.
- Each AEN preset names its silicon: `E1M-AEN301` to `alif:ensemble:e3`,
  `AEN401` to `e4`, `AEN501` to `e5`, `AEN601` to `e6`, `AEN701` to `e7`,
  `AEN801` to `e8`, each with a `silicon_variant:` order code
  (`AE302F80F55D5LE`, `AE402FA0E5597LE0`, `AE512F80F55D5LS`,
  `AE612FA0E5597LS0`, `AE722F80F55D5LS`, `AE822FA0E5597LS0`).
- `metadata/schemas/soc-spec-v1.schema.json` `$defs/variant`:
  `required: ['order_code']`, `additionalProperties: false`, and it already
  carries `mram_mb`, `sram_kb`, `sram_banks_kb`. The aperture base belongs here,
  beside `mram_mb`, which supplies the aperture's length.
- `metadata/schemas/som-preset-v1.schema.json` `$defs/memory_region`:
  `required: ['name', 'base', 'accessible_from']`, `additionalProperties: false`,
  properties `access_windows, accessible_from, base, cacheable, carveout,
  dt_label, name, size_kib, size_mib`.
- On AEN801 the six flagged regions tile `[0x80000000, 0x80580000)` exactly:
  64 + 2688 + 2688 + 64 + 96 + 32 = 5632 KiB, which is both `mram_main`'s
  `size_kib` and `variants[].mram_mb: 5.5`.
- `scripts/validate_metadata.py` is **already a gate** and already carries a
  `memory_map:` cross-field check (`_check_som_slot0_address_resolved`). It runs
  in CI from `.github/workflows/pr-metadata-validate.yml:206`,
  `.github/workflows/cross-platform-zephyr.yml:322` and
  `.github/workflows/pr-tier-a-libraries.yml:336`, and it is **not** registered
  in `metadata/quality-tasks-v1.json`.

That last fact sizes the gate work: A needs **no new `check_*.py`, no new
workflow, and no `quality-tasks-v1.json` entry**. Three checks are added to an
existing gate that already runs everywhere.

## Steps

Each step is independently reviewable and leaves the tree green.

### 1. Declare the MRAM aperture on the SoC variant

- `metadata/schemas/soc-spec-v1.schema.json` `$defs/variant` gains an
  **optional** aperture base property beside `mram_mb`.
- `metadata/socs/alif/ensemble/e3.json`, `e4.json`, `e5.json`, `e6.json`,
  `e7.json`, `e8.json` — set it on the variant each AEN preset names.
- The aperture's length comes from that variant's existing `mram_mb`; do not add
  a second length field.

**Optional per SoC, deliberately.** On E1M-V2N101 flash never enters
`memory_map` at all — eMMC and NOR are routing annotations and
`metadata/socs/renesas/rzv2n/n44.json`'s regions are all RAM. Making the field
or the tiling check mandatory would block every non-Alif family for no benefit.

**Aperture is per device window, not per controller.** E1M-AEN801 has the NOR
`MX25UM25645GXDI00` (Macronix OctaFlash, xSPI NOR) on `chip_select: 0` and the
HyperRAM `W958D8NBYA5I` (Winbond OctalRAM, HyperBus) on `chip_select: 1` —
byte-addressable RAM and flash behind the **same OSPI0 controller**. A
controller-scoped or XIP-window-scoped aperture would classify that RAM as
flash. Scope it to the MRAM window, `[0x80000000, 0x80580000)` on E8.

*Exit condition:* `python3 scripts/validate_metadata.py` green; each AEN
variant's declared aperture length equals its `mram_mb`.

### 2. Add the write-authority field to `memory_region`

- `metadata/schemas/som-preset-v1.schema.json` `$defs/memory_region` gains
  `AUTH` with `enum: auth-values` and a description stating WHO may write and
  WHEN, modelled on `flash_policy`'s wording.
- **Not** added to `required` (see decision 2). `additionalProperties: false`
  means the property must be declared before any preset can carry it, so this
  step lands before step 3.

*Exit condition:* schema validates; no preset changed yet; gates green.

### 3. Author the field on all 42 rows

- `metadata/e1m_modules/E1M-AEN301.yaml`, `E1M-AEN401.yaml`, `E1M-AEN501.yaml`,
  `E1M-AEN601.yaml`, `E1M-AEN701.yaml`, `E1M-AEN801.yaml` — 7 rows each.
- The values are a hardware and product fact per region, not a mechanical fill.
  `atoc` and `storage` having different values is the entire point of #1365; a
  review that cannot tell them apart from the diff means the vocabulary from
  decision 1 is wrong.

*Exit condition:* every authored `memory_map:` row carries the field;
`validate_metadata.py` green.

### 4. Three checks in `scripts/validate_metadata.py`

Follow `_check_som_slot0_address_resolved`'s shape exactly: a
`_check_som_*(som_files) -> list` returning a failure list shaped like
`_check_files()`, wired into `main()` beside the existing calls, skipping
presets with no `memory_map:`.

- **4a. Authority present.** Every authored `memory_map:` row carries the field.
  This is what decision 2 defers from the schema; a preset with no `memory_map:`
  is skipped, so the five non-AEN presets are unaffected.
- **4b. Aperture tiling.** Where the SoC variant declares an aperture, the
  authored regions contained in it tile it with no gaps and no overlaps. A gap is
  an undeclared region, which is the hazard. **Anchor on the SoC aperture, never
  on `mram_main`**, whose `base` is the string `"TBD"` while its children are
  concrete. Skip entirely where no aperture is declared.
- **4c. Class disagreement.** Compute the flash/RAM class by containment against
  the declared aperture and compare it with the authored `carveout`.
  Disagreement is an error. This is the check that makes the six hand-authored
  flags unable to rot.

  **Miss semantics are normative, per ADR-0033 clause 4:** a region whose base
  does not resolve — the `"TBD"` string, or a derived region with no base at all
  (`scripts/alp_project_loader.py`, "silicon-default bases stay unset") — is
  **unresolved**, and unresolved is never silently "not flash". Skip it and say
  so; do not guess.

*Exit condition:* each check fails on a hand-made bad fixture and passes on the
tree.

### 5. Tests

One file per check, matching the existing naming
(`tests/scripts/test_validate_metadata_slot0_address.py`):

- `tests/scripts/test_validate_metadata_memory_authority.py` — 4a
- `tests/scripts/test_validate_metadata_aperture_tiling.py` — 4b
- `tests/scripts/test_validate_metadata_class_disagreement.py` — 4c

Each asserts both directions: the real tree passes, and a synthetic preset with
the defect fails with a message naming the region. 4c additionally asserts that
an unresolved base is **skipped**, not classified — that assertion is what stops
a later refactor from turning "unknown" into "not flash".

### 6. Regenerate and document

- `python3 scripts/gen_catalog.py`, commit `metadata/catalog.json` if it moves.
- `changelog.d/1365.md` — a fragment naming the ATOC and `storage` hazard and
  stating plainly that A changes no allocator behaviour.
- `docs/board-config-hardware.md` and `docs/porting-new-som.md` — a porting
  author now has a field to fill; say what it means and how to choose a value.

## Verification

```sh
python3 scripts/validate_metadata.py
python3 -m pytest tests/scripts/test_validate_metadata_memory_authority.py \
                  tests/scripts/test_validate_metadata_aperture_tiling.py \
                  tests/scripts/test_validate_metadata_class_disagreement.py -q
python3 scripts/gen_catalog.py && git diff --exit-code metadata/catalog.json
bash scripts/test-all.sh --target dev
```

`test-all.sh --target dev` is the gate set a PR against `dev` is graded on and is
the one that must be green before opening. Note `python-smoke` also runs on
macOS and Windows in CI and cannot be reproduced on a Linux host; a
macOS-only or Windows-only red on a change that touches only Python and YAML is
a real platform bug, not a base-baseline flake.

## Risks

- **Authoring 42 values is a judgement task, not a fill.** Getting `atoc` wrong
  in either direction is the failure this issue exists to prevent. Review the
  data diff on its own, separately from the schema and check diffs.
- **The tiling check can be too strict.** If any AEN preset's six regions do not
  tile its aperture exactly, that is either a real undeclared gap or a wrong
  aperture value. Investigate before widening the check — a widened tiling check
  is a check that no longer detects the hazard.
- **A leaves the allocator hazard live** until split B. That is the accepted
  trade, and it is only defensible because A makes the authored data
  un-rottable. If ADR-0026 section G step 2 turns out to be more than roughly a
  quarter away, revisit: land B in both repos with an atomic port and pay the
  parity tax, because the bench record outranks the duplication cost.
- **Six SoC files, one aperture each.** A wrong aperture base silently
  reclassifies every region in that SKU. Cross-check each against its variant's
  `mram_mb` and against the authored regions' own arithmetic, the way AEN801's
  64 + 2688 + 2688 + 64 + 96 + 32 = 5632 KiB checks out.

## Relationship to the ADRs

- **ADR-0026 clause 2** keeps `metadata/` and `metadata/schemas/` in alp-sdk
  under every migration outcome, which is what makes A safe to land during the
  migration. Its amendment section G step 2 is what B waits on.
- **ADR-0033** supplies two rules A follows: deriving the class and shipping the
  outcome is its prong (b), and clause 4's normative miss semantics are why step
  4c must skip an unresolved base instead of guessing. The authority field is
  **not** ADR-0033 declared policy; who writes the ATOC changes when the silicon
  changes, so it is hardware truth.
