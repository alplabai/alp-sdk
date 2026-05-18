# Plan: intra-family portability proof + remaining cleanups + doc push

Authoritative plan for the next session. Self-contained — designed
to brief a fresh agent (or human) with no conversation history.

## Context (1-minute orientation)

Working dir: `C:\Users\caner\Documents\GitHub\alp-sdk` (branch:
`main`).  6 commits on 2026-05-18 landed slice 3a + 3b metadata
unification + 22 tracked audit tasks + the SoM-intrinsic chip
auto-enable wiring.  Current test state: **357 passed / 5 skipped /
0 failed**; `validate_metadata.py` clean.

Run `git log --oneline -10` to see the recent commits.  Run
`cat docs/superpowers/plans/2026-05-18-intra-family-portability-and-doc-push.md`
(this file) for the plan.

**Auto-loaded memory** (you'll see these at session start; if not,
read `~/.claude/projects/C--Users-caner-Documents-GitHub-alp-sdk/memory/MEMORY.md`):

Load-bearing principles for this work:
- `[[som-swappable-without-board-changes]]` — intra-family portability
  is the SDK's central promise
- `[[e1m-vs-e1m-x-separate-product-lines]]` — NO cross-form-factor
  portability attempt; dual `E1M_*` / `E1M_X_*` namespaces are
  intentional
- `[[silicon-determined-fields-not-customer-facing]]` — silicon caps
  live in SoC JSONs, SoM YAML carries only extensions
- `[[simplification-unification-principle]]` — every hardware fact
  has ONE machine-readable source; downstream artefacts are generated
- `[[portable-peripheral-api]]` — `<alp/*>` is the customer surface;
  chip drivers (`gd32g553_*`, `alif_*`) stay SDK-internal
- `[[examples-are-documentation]]` — example main.c files target ~50%
  comment density
- `[[no-empty-reexport-headers]]`, `[[no-claude-footer]]`,
  `[[pending-hw-configs]]`, `[[no-local-paths-or-som-design-leaks]]`,
  `[[descriptive-filenames]]`, `[[chip-driver-naming]]`,
  `[[doxygen-function-comments]]`, `[[memory-map-derived-from-soc-variant]]`,
  `[[som-pad-map-lives-in-alp-sdk]]`, `[[alp-sdk-standalone-studio-is-consumer]]`,
  `[[ubuntu-deferred-to-v2]]`, `[[roadmap-cherry-pick-backlog]]`,
  `[[zephyr-board-from-yaml]]`, `[[no-nordic-branded-tooling]]`

## The mission

**Intra-family portability is the SDK's load-bearing promise.**
The 2026-05-18 audit + slice-3b work landed the loader-side wiring
(commit `6563d4e` auto-enables SoM-intrinsic chips from
`on_module:`).  Now we need to:

1. **PROVE** the promise works with concrete swap tests across every
   SKU in each family
2. **IMPROVE** wherever the proof surfaces gaps
3. **DOCUMENT** the contract so customers can rely on it
4. **CLEAN UP** the remaining tail-end items deferred from the audit

Cross-form-factor portability is NOT a goal — see
`[[e1m-vs-e1m-x-separate-product-lines]]`.

## Phase A — Prove intra-family portability (highest priority)

**Two families, two swap-test matrices.**

### Phase A.1 — E1M family

SKUs: `E1M-AEN301`, `E1M-AEN401`, `E1M-AEN501`, `E1M-AEN601`,
`E1M-AEN701`, `E1M-AEN801`, `E1M-NX9101` (NX9101 is paper-correct
only; treat as best-effort).

Pick a canonical portable example as the swap-test subject.  Good
candidates: `examples/i2c-scanner`, `examples/gpio-button-led`,
`examples/pwm-led-fade`.  Pick one that exercises a portable
peripheral surface only (no NPU, no DEEPX, no GD32-extension features).

For each SKU:

1. Edit a copy of the example's `board.yaml` to set `som.sku:` and
   the appropriate per-core `cores:` block (`m55_hp` for AEN3..801,
   `m33` for NX9101).
2. Run `python scripts/alp_project.py --core <core_id> --emit zephyr-conf <board.yaml>`
   and capture the generated `alp.conf`.
3. Build with `west build -b native_sim/native/64 <example>` (or
   `west alp-build` if it accepts the per-SKU input).
4. Record green / yellow / red.

Diff the generated `alp.conf` across SKUs.  The DIFFs you EXPECT:
- `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E3..E8` vs `CONFIG_ALP_SOC_NXP_IMX9_IMX93`
- `CONFIG_ALP_TFLM_ETHOS_U85=y` only on E4/E6/E8 (U85 SKUs)
- `CONFIG_ALP_TFLM_ETHOS_U65=y` only on NX9101
- `CONFIG_ALP_TFLM_NEON=y` only on E5..E8 + NX9101 (A-cluster SKUs)
- SoM-intrinsic chip enables differ: CC3501E on all AEN, no
  CC3501E on NX9101; the on-module Wi-Fi differs; etc.

What you MUST NOT see (would be a portability gap):
- Different `<alp/*>`-using libraries pulled in
- Different peripheral CONFIG (the example uses portable
  peripherals — those should be identical)
- Different opt-in chips beyond what the on_module:/carrier.populated:
  data justifies

### Phase A.2 — E1M-X family

SKUs: `E1M-V2N101`, `E1M-V2N102`, `E1M-V2M101`, `E1M-V2M102`.

All four share `silicon: renesas:rzv2n:n44`.  Swap-test subject
should exercise GD32-bridged peripherals (PWM / ADC / DAC routed
through the on-module IO MCU).  Good candidates:
`examples/adc-voltmeter`, `examples/pwm-led-fade`,
`examples/v2n/v2n-pwm-fan-control`.

For each SKU:
1. Edit `board.yaml` `som.sku:` + core block (typically
   `m33_sm` for the Zephyr slice).
2. Same generate + build + diff as Phase A.1.

Expected DIFFs across V2N101 / V2N102 / V2M101 / V2M102:
- Same `CONFIG_ALP_SOC_RENESAS_RZV2N_N44=y` (same silicon)
- Same GD32 bridge enable (all four have it)
- DEEPX DX-M1 chip enable only on V2M
- Different on-module DRAM / eMMC density (V2N101=32 Gbit,
  V2N102/V2M101=32 Gbit, V2M102=64 Gbit) — visible only at the
  metadata level, not in app-facing CONFIG

### Phase A.3 — Report

Produce `docs/portability-matrix.md` with two tables (one per
family) showing for each SKU × example pair: build status,
generated config diff summary, runtime gaps if any.

If you find gaps, file them as tasks for Phase B.  Do not commit
until Phase B's gap fixes have landed and the matrix is clean.

## Phase B — Fix what Phase A surfaces

For each portability gap, apply the fix in the right layer:

| Gap symptom | Likely fix layer |
|---|---|
| A SoM-intrinsic chip doesn't auto-enable | `scripts/alp_orchestrate.py::_slugs_from_on_module` — extend the walk |
| A capability is missing from the merged result | `metadata/socs/<vendor>/.../<part>.json` `capabilities:` or `metadata/e1m_modules/<SKU>.yaml` `capabilities:` |
| A peripheral count is wrong | `metadata/socs/.../<part>.json` `peripherals:` |
| A pad routing is wrong | `metadata/e1m_modules/<SKU>.yaml` `pad_routes:` |
| The example uses a SoM-extension feature unintentionally | Fix the example to use portable surface |
| A header symbol is missing on E1M-X | `include/alp/e1m_x_pinout.h` (or revert the symbol to the example) |

DO NOT push fixes to app code as a last resort — the SDK should
absorb the difference.  If you find yourself adding `#ifdef
CONFIG_ALP_SOC_*` branches in example code, stop and ask whether
the example is testing the wrong thing.

## Phase C — Remaining cleanups (deferred audit items)

These were flagged in the 2026-05-18 audit but explicitly deferred.
Pick them up now:

### C.1 — `button_led` / `pdm_mic` directory-vs-naming boundary

Both files use `alp_button_led_*` / `alp_pdm_mic_*` prefix while
living under `include/alp/chips/` and `chips/`.  The naming convention
(`[[chip-driver-naming]]`) says chip drivers use the chip's natural
name (e.g., `lsm6dso_init`), not `alp_*`.  Two options:

- **(a) Relocate**: move to `include/alp/blocks/` and `blocks/`
  (new directories).  The `alp_*` prefix is then aligned with the
  "SDK abstraction" namespace.
- **(b) Accept location, document intent**: add a prominent comment
  in each file explaining `alp_*` is intentional because these are
  SDK-level block utilities, not raw IC drivers.

This is a design call — **surface to the maintainer via
AskUserQuestion** with both options presented; do not pick
unilaterally.

### C.2 — Yocto ENOSYS stubs

`src/zephyr/peripheral_*.c` implements 14 peripheral classes
(GPIO, I2C, SPI, UART, PWM, ADC, DAC, CAN, I2S, RTC, WDT, Counter,
QEnc, TMU).  `src/yocto/peripheral_*.c` implements only 4
(GPIO, I2C, SPI, UART).  Of the 10 missing, 6 are GD32-routed (no
direct Linux mapping); 4 are native-Linux-mappable: **CAN, I2S, RTC,
WDT**.

Add Yocto-side stub files for those 4: `src/yocto/peripheral_can.c`,
`peripheral_i2s.c`, `peripheral_rtc.c`, `peripheral_wdt.c`.  Each
should declare the same `alp_*_open` / `_close` / etc. symbols as
the Zephyr backend, but return `ALP_ERR_NOSUPPORT` until a real impl
lands.  This ensures Yocto builds compile against the full `alp_*`
surface (no link errors) and lets apps detect-and-fallback at
runtime via `alp_last_error()`.

### C.3 — `_Static_assert` for enum cast

`src/zephyr/peripheral_pwm.c:271` casts between `alp_pwm_align_t`
and `gd32g553_pwm_align_t` with a prose comment "share the same
wire encoding".  Replace the prose with compile-time enforcement:

```c
_Static_assert((int)ALP_PWM_ALIGN_EDGE   == (int)GD32G553_PWM_ALIGN_EDGE,
               "alp_pwm_align_t and gd32g553_pwm_align_t must share the same wire encoding");
_Static_assert((int)ALP_PWM_ALIGN_CENTER == (int)GD32G553_PWM_ALIGN_CENTER, "...");
// ... for every enum variant
```

If the GD32 type's enum order ever drifts, the SDK fails to compile
loudly rather than dispatching the wrong align mode at runtime.

### C.4 — Comment-density boost on thin examples

- `examples/rpmsg-imx93/m33/src/main.c` is at ~14% comments (target
  ~50% per `[[examples-are-documentation]]`)
- `examples/drone-autopilot/src/main.c` is at ~20% (especially the
  main() body, lines 75–184)

Boost both to ~50% by adding pedagogical prose around each call.
The `examples/rpmsg-v2n/m33_sm/src/main.c` is the gold standard —
mirror its density + style.

### C.5 — Chip-driver "stub register table" header notes

`chips/act8760/act8760.c`, `chips/da9292/da9292.c`,
`chips/ov5640/ov5640.c` have `/* TODO: confirm */` register offsets
behind `ALP_ERR_NOSUPPORT` gates.  The header file for each chip
should make this status visible to API consumers.  Add (or extend)
a `@par Verification status:` block with:

```c
/**
 * @par Verification status: [PAPER-CORRECT-STUB]
 *      Register table is provisional.  open() + status reads
 *      succeed; specific operations return ALP_ERR_NOSUPPORT
 *      until silicon HW-in-loop validation lands.  See the .c
 *      file's TODO markers for the exact unverified offsets.
 */
```

### C.6 — Active reminder: `imx93.json` peripheral counts gap

`metadata/socs/nxp/imx9/imx93.json` has `"peripherals": {}` (empty).
The `gen_soc_caps.py` script generates zero counts for i.MX 93,
which means runtime `ALP_SOC_*_COUNT` checks pass through
permissively (UINT16_MAX default when no SoC selected — but for
NX9101 the SoC IS selected, so the zeros become hard zero ceilings).

Fix path: ingest peripheral counts from the i.MX 93 reference
manual into `imx93.json` `peripherals:`.  The current note says
"ingestion against IMX93RM.pdf has not yet run".  If the RM is
available in the workspace, do the ingest now (use `extract_pdf.py`
or read the PDF directly if you have access).  Otherwise add
`"pending_reference_manual_ingestion": true` flag so
`validate_metadata.py` could surface this as a TODO.

## Phase D — Documentation push

The audit caught 18 doc fixes (already applied).  Now actively
push documentation forward:

### D.1 — New: `docs/portability.md` (cookbook)

Customer-facing portability cookbook.  Sections:

1. **What "swap-and-run" means.**  The intra-family promise +
   the form-factor split.  Cite `[[e1m-vs-e1m-x-separate-product-lines]]`.
2. **The swap-test recipe.**  Given an existing app's board.yaml,
   walk through changing `som.sku:` + verifying the build + flashing.
3. **Dual namespace.**  Why `E1M_*` vs `E1M_X_*`; when to use which;
   what NOT to do (don't try to share the same source between
   form factors).
4. **When NOT to expect portability.**  NPU model artefacts, form
   factor differences, heterogeneous-OS topology choices.  This is
   the "intentional gaps" section so customers aren't surprised.
5. **Capability validation.**  How `<alp/soc_caps.h>` +
   `alp_last_error()` + `ALP_ERR_NOSUPPORT` work together.
6. **The per-family portability matrix** (from Phase A.3's report).

Length: 500-1000 lines of substantive prose + code examples.

### D.2 — Refresh `docs/porting-new-som.md`

Make it a true "30-minute guide to add a hypothetical 7th AEN SKU
(E1M-AEN901)".  Walk through every step in order with exact file
paths + content templates:

1. Add the SoC variant to `metadata/socs/alif/ensemble/eN.json`
   (`variants[]` entry).
2. Create `metadata/e1m_modules/E1M-AEN901.yaml` (template).
3. Update the schema's `sku` pattern if the regex doesn't already
   accept the new SKU.
4. Add the family's hw-revisions row.
5. Run `validate_metadata.py`; expect clean.
6. Run the swap-test recipe from `docs/portability.md` against
   `i2c-scanner` to verify the new SKU is portable from day one.

### D.3 — Update `docs/architecture.md` Repository layout

The post-slice-3a state isn't fully captured.  Add or update:
- Per-core fan-out + slice emission section
- Sparse capabilities flow (SoC defaults → SoM extensions merge)
- `on_module:` auto-enable for SoM-intrinsic chips
- Generators inventory: `gen_carrier_header.py` + `gen_soc_caps.py`
  + the `--emit composed-route-table` demonstrator

### D.4 — Update `docs/v1.0-readiness.md`

Add a Pillar 3 item:
> **Intra-family portability proven** — every SKU in the E1M family
> and every SKU in the E1M-X family builds the canonical portable
> examples (`i2c-scanner`, `gpio-button-led`, `pwm-led-fade`,
> `adc-voltmeter`) under `native_sim/native/64` from the same
> source by changing only `som.sku:` in `board.yaml`.

Mark `[x]` when Phase A's matrix is fully green.

### D.5 — Cross-check 16 tutorials

Walk `docs/tutorials/*.md` (16 files).  For each, verify it aligns
with the post-slice-3b state:

- **tutorial-04-cross-family-portability**: now has a real story
  to tell — this is the perfect place to teach the swap-test recipe.
  Probably needs a partial rewrite.
- **tutorial-09-board-yaml-deep-dive**: confirm v2 schema +
  per-core `cores:` block + sparse capabilities are all correctly
  described.
- **tutorial-16-inference-mobilenet**: model artefact swap per NPU
  is the "what you can't abstract" story — make sure that's clear.

Others: light cross-check, no need for major rewrites.

### D.6 — ADR 0011: intra-family portability decision

Write `docs/adr/0011-intra-family-portability.md` capturing:
- Context: customers expect "swap SoM, no source changes" but
  cross-form-factor doesn't actually make sense (different power,
  different SoCs, different products).
- Decision: load-bearing portability = INTRA-family; cross-family
  = intentional product-line distinction with separate
  `<alp/*_pinout.h>` headers.
- Alternatives: single namespace (rejected — false equivalence),
  fully merged "lowest common denominator" API (rejected — would
  hide useful features), per-SoM custom APIs (rejected — defeats
  the SDK's purpose).
- Consequences: customers pick the right form factor up front;
  the matrix in `docs/portability.md` is the customer-facing
  guarantee.

Update `docs/adr/README.md` to add the index row.  Memory pointer:
`[[e1m-vs-e1m-x-separate-product-lines]]`.

## Phase E — Optional enrichments (if scope allows)

### E.1 — `<alp/board_info.h>` runtime API

Today apps that need to branch on SoM identity use Kconfig
(`#if defined(CONFIG_ALP_SOC_RENESAS_RZV2N_N44)`) — works at
compile time but creates per-target builds.  A runtime API would
let one binary handle multiple SoMs in the family:

```c
const alp_board_info_t *alp_board_info(void);

typedef struct {
    const char *som_sku;          // "E1M-V2N101"
    const char *silicon_ref;      // "renesas:rzv2n:n44"
    const char *silicon_variant;  // "R9A09G056N44GBG"
    uint32_t    capabilities;     // bitfield of ALP_BOARD_CAP_*
    // ... etc.
} alp_board_info_t;
```

Backed by EEPROM manifest (already in `<alp/hw_info.h>`) +
build-time SoC selection.

### E.2 — CI swap-test matrix

`.github/workflows/twister.yml` (or wherever) gets an N×M matrix
build: every canonical portable example × every SKU in its family.
Failure of any cell blocks PR merge.

### E.3 — Programmatic portability matrix generation

Script: `scripts/gen_portability_matrix.py` runs the swap-test
build matrix, captures pass/fail, emits `docs/portability-matrix.md`.
Hook into the same CI gate as E.2.

## Dispatch strategy

This is multi-track work.  Recommended fan-out:

**Wave 1 (Phase A — parallel):**
- Agent: E1M family swap-test (Phase A.1)
- Agent: E1M-X family swap-test (Phase A.2)
- Wait for both before Phase B.

**Wave 2 (Phase B — sequential, gap-specific):**
- For each gap found in Wave 1, dispatch a focused fix agent.

**Wave 3 (Phase C — parallel after Wave 2):**
- Agent: block-helper relocation decision (Phase C.1) + Yocto
  ENOSYS stubs (Phase C.2)
- Agent: `_Static_assert` (Phase C.3) + comment density boost
  (Phase C.4) + chip-stub header notes (Phase C.5)
- Phase C.6 (imx93 peripheral counts) likely needs the maintainer's
  data input — surface as a question.

**Wave 4 (Phase D — parallel):**
- Agent: write `docs/portability.md` (cookbook) + ADR 0011
- Agent: refresh `docs/porting-new-som.md` + update
  `docs/architecture.md` Repository layout + tutorial-04 rewrite
- Light cross-check of other tutorials (tutorial-09, tutorial-16) in
  main thread.

**Wave 5 (Phase E — optional, if bandwidth):**
- Agent: prototype `<alp/board_info.h>` + companion test
- Agent: CI swap-test matrix scaffolding
- Agent: `gen_portability_matrix.py`

## Verification gates

After each phase:
- `python scripts/validate_metadata.py` → 0 failures
- `python -m pytest tests/scripts/ -q` → no NEW failures
  (baseline: 357 passed / 5 skipped / 0 failed)
- For docs: `grep -rn "memory_map:\|alplab.ai\|alp-lab\|alp-studio is the parent" docs/`
  returns only neutral mentions

After Phase A (portability proof):
- `docs/portability-matrix.md` exists and shows green across both
  families.

After Phase C:
- The 4 missing Yocto stubs exist (`src/yocto/peripheral_can.c`,
  `_i2s.c`, `_rtc.c`, `_wdt.c`).
- `_Static_assert` lines exist in `src/zephyr/peripheral_pwm.c`.
- Comment density on `rpmsg-imx93/m33/main.c` + `drone-autopilot/main.c`
  ≥ 40%.

After Phase D:
- `docs/portability.md` exists, ≥ 500 lines.
- `docs/adr/0011-intra-family-portability.md` exists with Status:
  Accepted.
- `docs/adr/README.md` index has the new row.

## Constraints (apply throughout)

- **No commits without explicit user approval.**  Surface
  commit-ready batches; let the maintainer review + commit.
- Memory pointers auto-load — respect them all.
- Use TaskCreate to track phases + sub-tasks.
- For ambiguity, surface to maintainer via AskUserQuestion before
  implementing.
- Per `[[no-claude-footer]]`: when the maintainer asks for a commit
  message, attribute solely to alpCaner (no Claude footer).
- Per `[[pending-hw-configs]]`: mark TBD where values are unknown;
  never invent.
- Per `[[no-local-paths-or-som-design-leaks]]`: scrub
  `C:\Users\...`, `OneDrive\...`, internal hostnames before
  surfacing anything to the repo.

## End-of-session

Write a single summary at the top level + leave the working tree
in a state ready for human review.  Each phase commits as its own
commit for clean history.

The success criterion: a customer can pick up the SDK, target any
SKU in either family, change `som.sku:` in `board.yaml`, and
build + run the canonical portable examples without source changes
— and the `docs/portability.md` cookbook tells them how.
