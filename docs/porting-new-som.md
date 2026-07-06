# Porting a new E1M SoM to the Alp SDK

> **30-minute target.**  This guide walks through every step required
> to add a hypothetical 7th AEN-family SKU — **E1M-AEN901**, a
> next-generation Alif Ensemble part — from a clean working tree to a
> green `validate_metadata.py` run and a portable example that picks
> up the new module via a single `som.sku:` edit.  Total file count
> touched: **4** (one new SoC JSON, one new SoM YAML, one schema-
> pattern edit, one hw-revisions append).

## 1. Introduction

> **Scaffold first:** `alp new-som` generates both metadata skeletons
> this guide walks through (the SoM YAML + the SoC JSON) with
> schema-legal `TBD` placeholders, and prints the porting checklist --
> run it, then use the sections below to fill in the facts.  See the
> verb reference in [cli.md](cli.md).
>
> The scaffold is **mergeable as-is**: it passes the full
> `pr-metadata-validate` command set (`validate_metadata.py` and
> `check_inference_backend_parity.py`) the moment it is committed.
> The `inference.preferred_backend: tbd` placeholder rides on the
> preset's `status.preliminary: true` marker -- the parity gate
> accepts `tbd` *only* on preliminary presets, so the port cannot
> graduate (clear `status.preliminary`) until the real silicon
> backend replaces the placeholder.  Preview a scaffold without
> writing anything via `alp new-som ... --dry-run`.

"Porting a new SoM" in the Alp SDK means *adding one row of
machine-readable metadata*.  It is **not**:

- *adding a new chip driver* — that lands under `chips/<part>/` and
  is orthogonal to SoM identity (a chip driver is shared across every
  SoM that populates it);
- *adding a new board* — that lands under `metadata/boards/<name>/`
  and only describes the off-module PCB the SoM plugs into;
- *adding a new SoC family* — same shape as below but starts one
  layer deeper (a brand-new vendor / family directory under
  `metadata/socs/<vendor>/<family>/`).

The SDK's [[simplification-unification-principle]] holds: every
hardware fact has **one** machine-readable source under
`metadata/`.  Downstream artifacts — Zephyr DTS overlays, Kconfig
fragments, Yocto layers, generated C headers, the `<alp/...>`
public surface — are all derived.  Add the YAML/JSON row, run the
validator, and the rest of the codebase comes along for free.

Within a single form factor (E1M *or* E1M-X — see
[[e1m-vs-e1m-x-separate-product-lines]]), the result is
**intra-family portability**: swap `som.sku:` in a customer's
`board.yaml`, rerun `west build`, get a working binary on the new
SoM.  No source change required.

## 2. Prerequisites

- **Familiarity** with reading and writing JSON + YAML, and with
  JSON Schema diagnostics (the validator surfaces failures by JSON
  Pointer + message).
- **CPython 3.10+** with `pyyaml` and `jsonschema` installed
  (`pip install pyyaml jsonschema`).
- **Baseline check** — before you touch anything, run

  ```bash
  python scripts/validate_metadata.py
  ```

  and confirm **0 failures**.  If the baseline is red, fix that
  first; you cannot tell which failures came from your port.

- **Optional**: a copy of the silicon's datasheet (or the vendor's
  reference module datasheet for the on-module BOM).  Per
  [[pending-hw-configs]], it is fine to leave fields as `TBD` when
  authoritative values are not yet available — **never invent
  numbers**.

## 3. Worked example: E1M-AEN901

For the rest of this guide we hand-craft a fictional next-gen Alif
Ensemble SoM:

| Field             | Value                                                 |
|-------------------|-------------------------------------------------------|
| SKU               | `E1M-AEN901`                                          |
| Form factor       | E1M (same as E1M-AEN701)                              |
| Silicon ref       | `alif:ensemble:e9`                                    |
| Silicon variant   | `AE921F80F55D5LS` (fictional order code)              |
| Family            | `alif-ensemble`                                       |
| On-module BOM     | Identical to AEN701 + one new chip (`cc3511e`, an     |
|                   | imagined CC3501E successor; replace with the real     |
|                   | part when AEN9 silicon lands)                         |
| Default board   | `E1M-EVK`                                             |

We treat the E1M-AEN701 preset as our template — it is the closest
existing SKU shape.  See
[`metadata/e1m_modules/E1M-AEN701.yaml`](../metadata/e1m_modules/E1M-AEN701.yaml)
and [`metadata/socs/alif/ensemble/e7.json`](../metadata/socs/alif/ensemble/e7.json)
for the canonical structure.

> The E1M-AEN901 SKU and the `alif:ensemble:e9` silicon ref are
> **purely illustrative** for this guide; they are NOT created in the
> repo.  Per [[pending-hw-configs]], we cannot invent technical
> details for a part Alif has not announced.

---

## 4. Step 1 — Add the SoC variant

**New file:** `metadata/socs/alif/ensemble/e9.json`

The SDK looks up `silicon: alif:ensemble:e9` by splitting the triple-
colon ref into `<vendor>/<family>/<part>` and reading
`metadata/socs/<vendor>/<family>/<part>.json` (see
`_resolve_silicon_variant()` in `scripts/alp_project.py`).  One JSON
per part — never bundle multiple Ensemble parts into one file; the
descriptive-filename rule ([[descriptive-filenames]]) wants one
artifact per silicon SKU.

### Template

```json
{
  "soc_spec_version": 1,
  "ref": "alif:ensemble:e9",
  "vendor": "Alif Semiconductor",
  "family": "Ensemble",
  "part": "E9",
  "status": "preliminary",
  "pending_alif_datasheet": true,
  "_pending_reason": "E9 datasheet not yet published; counts below are placeholders pending the vendor release.",
  "cores": [
    {
      "id": "a32_cluster",
      "type": "cortex-a32",
      "count": 2,
      "freq_mhz": 1000,
      "isa": "Armv8-A",
      "trustzone": true,
      "vector_extension": "Neon",
      "fpu": "double-precision",
      "l1_kb": 32,
      "l2_kb": 512,
      "shared_l2": true,
      "mmu": true
    },
    {
      "id": "m55_hp",
      "type": "cortex-m55",
      "subtype": "high-perf",
      "count": 1,
      "freq_mhz": 480,
      "tcm_kb": 1280,
      "isa": "Armv8.1-M",
      "trustzone": true,
      "vector_extension": "Helium",
      "fpu": "double-precision"
    },
    {
      "id": "m55_he",
      "type": "cortex-m55",
      "subtype": "high-efficiency",
      "count": 1,
      "freq_mhz": 200,
      "tcm_kb": 512,
      "isa": "Armv8.1-M",
      "trustzone": true,
      "vector_extension": "Helium",
      "fpu": "double-precision"
    }
  ],
  "npus": [
    { "type": "ethos-u85", "subtype": "high-perf",        "mac_per_cycle": 512, "freq_mhz": 480, "gops": 491 },
    { "type": "ethos-u55", "subtype": "high-efficiency",  "mac_per_cycle": 128, "freq_mhz": 200, "gops":  51 }
  ],
  "soc_ram_kb": 13824,
  "soc_flash_mb": 5.5,
  "always_on_sram_kb": 4,
  "peripherals": {},
  "capabilities": {
    "ethos_u85_count": 1,
    "ethos_u55_count": 1,
    "helium_mve":      true,
    "neon":            true,
    "gpu2d":           true,
    "dave2d":          true,
    "cryptocell":      true,
    "inline_aes":      true,
    "cau":             false,
    "xspi_dma":        true,
    "dma2d":           true
  },
  "variants": [
    {
      "order_code": "AE921F80F55D5LS",
      "package": "FBGA194",
      "temperature_grade": "standard",
      "mram_mb": 5.5,
      "sram_kb": 13824,
      "sram_banks_kb": {
        "SRAM0": 4096, "SRAM1": 2560,
        "SRAM2_M55_HP_ITCM": 256, "SRAM3_M55_HP_DTCM": 1024,
        "SRAM4_M55_HE_ITCM": 256, "SRAM5_M55_HE_DTCM": 256,
        "SRAM6": 2048, "SRAM7": 512, "SRAM8": 2048, "SRAM9": 768
      },
      "gpio_18v": 120,
      "alp_module_skus": ["E1M-AEN901"]
    }
  ],
  "notes": [
    "Hypothetical E9 part used to demonstrate the porting workflow.",
    "Replace `pending_alif_datasheet` and the placeholder fields with primary-source values once the silicon ships."
  ]
}
```

### Field-by-field notes

| Field                           | Purpose                                                                                                                                                |
|---------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------|
| `soc_spec_version: 1`           | Schema version; required.                                                                                                                              |
| `ref`                           | The `<vendor>:<family>:<part>` triple-colon ref the SoM preset uses to find this file.  **Must** match the file path: `socs/alif/ensemble/e9.json`. |
| `cores[].id`                    | Canonical core id; **must match** the keys you use in the SoM preset's `topology:` block.  Regex: `^[a-z][a-z0-9_]+$`.                                  |
| `cores[].type`                  | Used by codegen + by `<alp/system_ipc.h>` ARM core checks.                                                                                              |
| `capabilities`                  | Drives `include/alp/soc_caps.h` boolean macros (`ALP_SOC_HELIUM_MVE`, `ALP_SOC_NEON`, etc.) via `scripts/gen_soc_caps.py`.                              |
| `peripherals`                   | Counts per peripheral kind; drives `ALP_SOC_*_COUNT` ceilings in the same generated header.  `{}` is legal but trips the `pending_*` warning.          |
| `variants[].order_code`         | Vendor order code; **must match** the SoM preset's `silicon_variant:` field for the loader to resolve memory layout from this entry's `sram_banks_kb`. |
| `variants[].alp_module_skus`    | Reverse-lookup hint; lets the validator catch a SoM SKU that references this variant by silicon ref alone (no `silicon_variant:` declared).            |
| `pending_alif_datasheet: true`  | Surfaces a non-fatal `WARN` line from `validate_metadata.py` so reviewers know the values are preliminary.                                              |

**Rules of thumb:**

- *Silicon-determined* fields (cores, NPUs, peripherals, capabilities,
  memory) belong **only** in this file — never duplicated in the SoM
  YAML.  See [[silicon-determined-fields-not-customer-facing]].
- Anything you do not yet know goes in as the literal string `"TBD"`
  on string-valued fields, or simply omitted from optional integer
  fields.  **Do not guess.**

---

## 5. Step 2 — Create the SoM YAML

**New file:** `metadata/e1m_modules/E1M-AEN901.yaml`

This is the single source of truth for the SoM PCB: which silicon
variant it carries, which chips are populated on-module, how the
E1M pads route to the silicon (or through an on-module mediator
like CC3501E/CC3511E), and which per-core default applications run
where.

### Template

```yaml
# Stock preset for E1M-AEN901 (the SoM, NOT the EVK board).
# See E1M-AEN701.yaml for the file's role + the project memory note
# governing what stays TBD.

schema_version: 1

sku: E1M-AEN901
family: alif-ensemble
silicon: alif:ensemble:e9
silicon_variant: AE921F80F55D5LS         # see metadata/socs/alif/ensemble/e9.json variants[]

display_name: "E1M-AEN901 (Alif Ensemble E9 -- preliminary)"

# On-module components only.  Board-side parts (IMUs, OLEDs, ...)
# live in metadata/boards/<board>/board.yaml.
on_module:
  silicon:              alif:ensemble:e9
  wifi_ble:             cc3511e               # next-gen TI Wi-Fi 6E + BLE coprocessor
  secure_element:       optiga_trust_m
  rtc_external:         rv3028c7
  temperature_sensor:   tmp112
  eeprom:               eeprom_24c128
  ospi_memories:
    ospi0:
      chip:           TBD
      assembled:      optional
      capacity_mbit:  TBD
      role:           app_storage
    ospi1:
      chip:           TBD
      assembled:      optional
      capacity_mbit:  TBD
      role:           data_log

# Inference defaults -- silicon-determined.  See E1M-AEN801 for the
# E8 (U85 + 2x U55) shape; tune to whatever the E9 silicon actually
# carries once the datasheet lands.
inference:
  preferred_backend:    ethos_u
  ethos_u_variant:      u85
  npu_population:
    - { variant: u85, role: NPU-HP, paired_with: M55-HP }
    - { variant: u55, role: NPU-HE, paired_with: M55-HE }

# SoM-side extensions to silicon capabilities.  The loader merges
# this on top of soc_spec.capabilities at codegen time (see
# resolve_capabilities() in scripts/alp_project.py).  Only declare
# capabilities the SoM ADDS on top of the silicon; a capability the
# silicon offers but this SKU does NOT populate goes in
# `silicon_capabilities.unpopulated` below, never as `false` here.
capabilities:
  optiga_trust_m:       true               # on-module SE; not in silicon caps.

# OPTIONAL -- per-SKU RESTRICTION of the silicon capability set.  Only
# declare when this SKU leaves a silicon capability unpopulated (e.g.
# an accelerator fused off / not bonded out on this order code); every
# listed name must be truthy in the SoC JSON's `capabilities:` block
# (validate_metadata.py cross-check).  Omit entirely when the SKU
# carries the silicon's full set -- true for every current SKU.
# silicon_capabilities:
#   unpopulated: [gpu2d, dave2d]           # HYPOTHETICAL -- transcribe from the datasheet.

topology:
  a32_cluster:
    app: alp-image-edge
    machine: e1m-aen901-a32                # Yocto MACHINE
    toolchain: poky-glibc
  m55_hp:
    app: alp-stock-shim
    board: alp_e1m_aen901_m55_hp           # Zephyr board target
    toolchain: arm-zephyr-eabi
  m55_he:
    app: alp-stock-shim
    board: alp_e1m_aen901_m55_he           # Zephyr board target
    toolchain: arm-zephyr-eabi

# Memory layout (SRAM banks + MRAM) is derived from the SoC variant
# (resolved via silicon_variant:) -- see
# metadata/socs/alif/ensemble/e9.json variants[].sram_banks_kb +
# mram_mb. Declare a memory_map: block here ONLY for non-stock
# partitioning.

mailbox:
  controller: TBD                          # Alif IPC controller name pending HW config.
  channels:
    - { id: 0, reserved_for: alp_default_rpmsg }
    - { id: 1, reserved_for: app }
    - { id: 2, reserved_for: app }
    - { id: 3, reserved_for: power_mgmt }

# E1M pads / instances that route through the on-module CC3511E
# coprocessor instead of through the Alif Ensemble silicon.  Mirror
# the AEN701 shape; revise pin numbers once the AEN9 HW design is
# frozen.
pad_routes:
  - { e1m: E1M_SPI1,      dispatch: cc3511e,
      doc: "Inter-chip SPI1 fast path; CC3511E acts as SPI peripheral." }
  - { e1m: E1M_GPIO_IO11, dispatch: cc3511e, dispatch_pin: TBD,
      doc: "CC3511E GPIO (pin number pending HW config)." }
  # ... add remaining IO13/IO15..IO21 routes mirroring AEN701 once
  # the AEN9 schematic is frozen.

helper_firmware:
  - name:          cc3511e_otp
    chip:          cc3511e
    firmware_path: TBD
    flash_method:  TBD
    flash_args:    TBD

default_hw_rev:         r1
default_board:          E1M-EVK

status:
  preliminary:          true               # E9 silicon not yet released
  partial_hw_config:    true               # memory / mailbox / pad routes still TBD
```

### Field rules

| Block                  | Silicon-determined?              | TBD-acceptable?           | Notes                                                                                                                  |
|------------------------|----------------------------------|---------------------------|------------------------------------------------------------------------------------------------------------------------|
| `silicon`              | Yes — must match an `e9.json`    | No                        | Loader fails fast if the triple-colon ref does not resolve to a SoC JSON.                                              |
| `silicon_variant`      | Yes — must match a `variants[]`  | Yes (`"TBD"`)             | When `TBD`, the loader falls back to `alp_module_skus[]` reverse lookup.                                                |
| `on_module:*`          | No — SoM extension                | Yes per field             | The set of keys is open; chip names match `chips/<part>/` driver dirs (driver-naming convention applies).               |
| `inference`            | Mixed                             | Yes (omit when unsure)    | `preferred_backend` is silicon-determined; the customer cannot override it from `board.yaml` (per the v0.6 cleanup).    |
| `capabilities`         | SoM extension only                | Yes                       | Only list keys the SoM **adds** to silicon caps (e.g., on-module CAU on V2N, `optiga_trust_m` on AEN/V2N).               |
| `silicon_capabilities` | Silicon-determined (restriction)  | Omit when unrestricted    | Optional `unpopulated:` list of SoC `capabilities:` keys this SKU does **not** populate; can only remove what the silicon offers (`validate_metadata.py` cross-check). |
| `topology`             | Silicon-determined (core ids)     | No                        | Keys must match `soc.cores[].id`; `app:` / `board:` / `machine:` / `toolchain:` are SoM-extension.                       |
| `memory_map`           | Silicon-determined (derived)      | Omit entirely              | Only declare for non-stock partitioning; otherwise the loader derives from SoC `sram_banks_kb`.                          |
| `mailbox.controller`   | Mixed                             | Yes (`"TBD"`)             | Required when any topology entry runs Zephyr or baremetal; controller name comes from the hand-written HW config.        |
| `pad_routes[]`         | SoM extension                     | Yes (`dispatch: TBD`)     | One row per E1M pad that routes through an on-module mediator; pads NOT listed are implicit `dispatch: direct`.          |
| `helper_firmware[]`    | SoM extension                     | Yes (`TBD` per field)     | One entry per on-module helper MCU image (CC3511E firmware, GD32 bridge firmware, …).                                    |
| `default_hw_rev`       | SoM extension                     | No                        | Must match a key in `metadata/e1m_modules/<family>/hw-revisions.yaml`.                                                  |
| `status.*`             | SoM extension                     | n/a                       | Flags for tooling (e.g. preliminary, partial HW config).                                                                |

---

## 6. Step 3 — Update the schema `sku` pattern

**Edit:** `metadata/schemas/som-preset-v1.schema.json`

The current pattern accepts `E1M-AEN[3-8]01` — i.e. SKUs for the
released parts E3..E8 only.  Adding **E1M-AEN901** widens the
allowed range; without this edit, `validate_metadata.py` will reject
the new YAML with:

```
FAIL metadata/e1m_modules/E1M-AEN901.yaml
  · sku: 'E1M-AEN901' does not match '^E1M-(AEN[3-8]01|V2N10[12]|V2M10[12]|NX9[0-9]{3})$'
```

Edit the pattern to accept `AEN[3-9]01`:

```jsonc
"sku": {
  "type": "string",
  "pattern": "^E1M-(AEN[3-9]01|V2N10[12]|V2M10[12]|NX9[0-9]{3})$"
}
```

> **Note:** This is the *only* file outside `metadata/e1m_modules/`
> and `metadata/socs/` you should need to touch for a new SoM
> *within an existing family*.  If you are also extending an
> existing family's SKU shape (e.g. adding a `-2P` suffix), expand
> the pattern accordingly; for a brand-new family, add a fresh
> alternation branch.  The pattern is the only schema-side
> constraint specific to SKU strings; everything else is structural.

---

## 7. Step 4 — Add the family `hw-revisions` row

**Edit:** `metadata/e1m_modules/aen/hw-revisions.yaml`

The AEN family invariant (already stated in the file's `summary:`)
is that **all AEN SKUs share the same module PCB**; per-SoC-tier
differences live in the per-SKU `som.yaml`, not as hw-revisions of
the module.  That means you do not actually add a new revision row
for E1M-AEN901 — the existing `r1` row applies — but **you do**
need to ensure the SoM YAML's `default_hw_rev: r1` resolves
against this family file.

Run:

```bash
python scripts/validate_metadata.py
```

and confirm the hw-revisions row resolves cleanly (the family file
is named `metadata/e1m_modules/aen/hw-revisions.yaml`; the SoM
preset's `family: alif-ensemble` is the human-readable family
slug, NOT the directory name — see `scripts/alp_project.py`
`_sku_family()` for the SKU-prefix → directory map: `AEN` → `aen`,
`V2N` → `v2n`, `V2M` → `v2n-m1`, `NX9` → `imx93`).

**Future revision rows.**  If the AEN9 generation introduces a true
PCB respin (different stack-up, different connector position, …)
then a new `r2:` row would be added — but only then, and only after
the family invariant is broken.  Most ports do not touch this file
at all.

For a family that **does** need a new revision row, the row shape is
(from `metadata/schemas/hw-revisions-v1.schema.json`):

```yaml
r2:
  min_sdk_version: "0.7.0"   # earliest SDK release that recognises r2
  max_sdk_version: ~          # ~ = open-ended
  status: production          # or `preliminary`, `reserved`, `eol`
  summary: |
    One-paragraph rationale for the respin.
  changes:
    - "Connector J3 rotated 180° relative to r1."
    - "On-module crystal moved from Y1 to Y2."
```

---

## 8. Step 5 — Validate

```bash
python scripts/validate_metadata.py
```

Expected output (excerpt — full output lists every file):

```
OK   metadata/socs/alif/ensemble/e7.json  (ref=alif:ensemble:e7)
OK   metadata/socs/alif/ensemble/e8.json  (ref=alif:ensemble:e8)
OK   metadata/socs/alif/ensemble/e9.json  (ref=alif:ensemble:e9)
WARN metadata/socs/alif/ensemble/e9.json: pending_reference_manual_ingestion ...
...
OK   metadata/e1m_modules/E1M-AEN801.yaml  (sku=E1M-AEN801)
OK   metadata/e1m_modules/E1M-AEN901.yaml  (sku=E1M-AEN901)
...
N SoC file(s) + M SoM preset(s) + K hw-revisions file(s) checked, 0 failure(s)
```

The `N` and `M` should each be one larger than baseline.  The `WARN`
line on `e9.json` is non-fatal — it exists to flag preliminary
silicon whose peripheral counts are not yet locked in.

If a failure appears:

| Failure                                           | Most likely cause                                                                                |
|---------------------------------------------------|--------------------------------------------------------------------------------------------------|
| `sku: 'E1M-AEN901' does not match ...`            | Skipped Step 3 (schema pattern).                                                                  |
| `silicon: ... unknown ref`                        | The triple-colon `silicon:` in the SoM YAML doesn't resolve to a JSON file under `metadata/socs/`. |
| `topology/{key}: additional properties not allowed` | A `topology.{core_id}` key doesn't match any `cores[].id` in the SoC JSON (e.g. typo: `m55hp`).    |
| `mailbox: required when any core runs Zephyr/baremetal` | Forgot the `mailbox:` block, or it lacks a `controller:` / `channels:`.                        |

---

## 9. Step 6 — Swap-test verification

The portability promise: *the same example source, the same
`board.yaml` shape, builds against the new SoM with one field
edited.*  Pick the simplest portable example as the swap-test
victim — `examples/peripheral-io/i2c-scanner/` works because it touches only
`<alp/peripheral.h>` and has no SoM-specific code paths.

### 1. Copy the example

Create `examples/peripheral-io/i2c-scanner-aen901/` as a sibling, copying the
content verbatim.  (For a full smoke-test you can keep the
existing directory and just patch its `board.yaml` in-place; the
sibling form is easier to revert.)

### 2. Edit `board.yaml`

The fragment below is shown using `E1M-AEN801` (a real, schema-known
SKU) so the doc-YAML linter accepts it.  In your real port, swap the
single `som.sku:` line to your new SKU — `E1M-AEN901` for this
walkthrough — and the schema pattern in
`metadata/schemas/board.schema.json` must be widened to
accept it (one regex tweak; see the **Common pitfalls** section
later in this document).

```yaml
som:
  sku: E1M-AEN801          # in your port: E1M-AEN901 (the new SKU)

preset: e1m-evk            # AEN901 ships on the same EVK as AEN701

cores:
  m55_hp:
    app: ./src
    peripherals:
      - i2c

diagnostics:
  log_level: info
```

### 3. Run the loader (without building)

```bash
python scripts/alp_project.py \
    --input examples/peripheral-io/i2c-scanner-aen901/board.yaml \
    --core m55_hp \
    --emit zephyr-conf
```

Expected stdout (excerpt):

```
# Generated by alp_project.py
CONFIG_ALP_OS_ZEPHYR=y
CONFIG_ALP_SOC_ALIF_ENSEMBLE_E9=y
CONFIG_ALP_SDK_CHIP_CC3511E=y
CONFIG_ALP_SDK_CHIP_OPTIGA_TRUST_M=y
CONFIG_ALP_SDK_CHIP_RV3028C7=y
CONFIG_ALP_SDK_CHIP_TMP112=y
CONFIG_ALP_SDK_CHIP_EEPROM_24C128=y
CONFIG_ALP_PERIPHERAL_I2C=y
...
```

The `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E9=y` line is the headline
result — the loader successfully resolved the new SoM through the
new SoC JSON.  The Kconfig symbol is **computed** from the ref
(`ALP_SOC_ + ref.upper().replace(':','_')`), so the swap-test
goes green once the new ref is added to the allowlist in
`metadata/registries/silicon-kconfig.json`.  (The existing Alif
rows run `ALP_SOC_ALIF_ENSEMBLE_E3` through `E8`; a brand-new SoC
adds its own ref to that list.)

> **Caveat (current state).**  Adding a new SoC ref means one row in
> `metadata/registries/silicon-kconfig.json` (the versioned allowlist
> consumed by `silicon_to_kconfig()` in `scripts/alp_project.py`) AND
> a matching `config ALP_SOC_<NEW_REF>` stanza in `zephyr/Kconfig`.
> `scripts/validate_metadata.py` gates that every allowlisted ref
> resolves to an existing `metadata/socs/` spec.  Until the registry
> row + the Kconfig stanza land, the loader emits a blank Kconfig
> fragment for the new SoC; this is expected, and is the only
> data-touching step in the otherwise metadata-only port.

### 4. Confirm capabilities resolve

```bash
python scripts/alp_project.py \
    --input examples/peripheral-io/i2c-scanner-aen901/board.yaml \
    --core m55_hp \
    --emit cmake-args
```

Expected `-D` args for the merged capabilities — e.g.
`-DALP_CAP_HELIUM_MVE=1`, `-DALP_CAP_NEON=1`, `-DALP_CAP_OPTIGA_TRUST_M=1`.
The merge happens in `resolve_capabilities()`:

1. SoC JSON `capabilities:` provides the silicon defaults.
2. SoM YAML `capabilities:` overlays SoM additions
   (`optiga_trust_m: true` on AEN901).
3. SoM YAML `silicon_capabilities.unpopulated` (when present) forces
   the listed silicon capabilities to `false`/`0` — the per-SKU
   restriction layer.
4. Customer's `board.yaml` capabilities (rare) wins last.

---

## 10. Step 7 — Build a real binary (optional)

Generating Zephyr board files (`<board>.dts`, `<board>.yaml`,
`<board>_defconfig`, `<board>.cmake`, `Kconfig.board`,
`Kconfig.defconfig`) is **planned to be a one-shot transcription**:

```bash
python scripts/alp_project.py \
    --input examples/peripheral-io/i2c-scanner-aen901/board.yaml \
    --core m55_hp \
    --emit zephyr-board \
    --output build/boards/alp_e1m_aen901_m55_hp/
```

Then:

```bash
west build \
    -b alp_e1m_aen901_m55_hp \
    --board-root build/boards \
    examples/peripheral-io/i2c-scanner-aen901
```

> **Future work flag.**  The `--emit zephyr-board` emitter is on the
> roadmap (see [[zephyr-board-from-yaml]]) but not yet wired in
> `scripts/alp_project.py` as of 2026-05-18 — the script's `--emit`
> choices currently end at `composed-route-table`.  Until the
> emitter lands, customers either hand-author a minimal Zephyr
> board file under `boards/arm/alp_e1m_aen901_m55_hp/` (mirroring
> the existing AEN701 board files), or run the example under
> `native_sim` for a non-silicon smoke build.  When the emitter
> lands this section becomes a literal copy-paste.

Either way, the resulting `.elf` is a real binary that exercises
the new SoM's BSP path.  Run it on hardware once silicon arrives,
or under `native_sim` for portable CI today.

---

## 11. Conformance gate — prove the portable contract

Metadata validation (Step 5) proves the *description* of the new
SoM; the conformance suite proves its *backends*.
`tests/zephyr/conformance/` is one data-driven ztest suite that
every backend must pass — it mechanically exercises the uniform
lifecycle contract of every portable peripheral class (GPIO / I²C /
SPI / UART / ADC / DAC / PWM / CAN / RTC / WDT / counter / qenc /
I²S, plus the I²C/SPI target modes) and the non-class v0.9
surfaces (`alp_init`/`alp_deinit` idempotency, the
`alp_uart_rx_ringbuf_*` contract, I²C-target config validation):

- `alp_<class>_open(NULL cfg)` → NULL + `alp_last_error()` ==
  `ALP_ERR_INVAL`
- `alp_<class>_open(<bad instance>)` → NULL + a documented
  `alp_status_t` code (never a raw negative errno)
- `alp_<class>_close(NULL)` and double-close are safe no-ops
- `alp_<class>_capabilities(NULL)` → NULL; non-NULL for an open
  handle
- NULL-handle data-path calls refuse with an in-enum status

**Expectations come from the capability layer.**  On a real-SoM
build (any `CONFIG_ALP_SOC_<X>`), whether `open(instance 0)` must
succeed is derived from `alp_has()` (`<alp/cap.h>`), so the gate
doubles as a caps↔backend parity check: cap **true** but open
failed = the port is broken (FAIL); cap **false** but open handed
out a handle = the capability table is broken (FAIL); cap false and
open refused the documented way = asserted degrade path, logged as
`silicon lacks it`.  On `native_sim` (`CONFIG_ALP_SOC_NONE`, the
permissive profile) the suite falls back to what the test overlay
wires: gpio / i2c / spi / uart / adc / counter run the positive
path against emulated controllers; the rest assert the failure
contract.  The target-mode rows never promote to must-open —
their headers document `ALP_ERR_NOSUPPORT` as a legitimate
per-driver refusal.

Run it on `native_sim` (the same invocation CI uses, minus the
module paths):

```bash
twister --testsuite-root tests/zephyr/conformance \
    -p native_sim/native/64 -O twister-out-conformance
```

The qualified in-repo boards (`zephyr/boards/alp/`) are in the
scenario's `platform_allow` too, so the same gate builds — and, on
the bench or the nightly HIL, runs — with the real backend
selected.  Per-board `boards/<board>.conf` fragments pin the
matching `CONFIG_ALP_SOC_*` capability profile.  E.g. a build-only
smoke for the AEN801 HE core:

```bash
twister --testsuite-root tests/zephyr/conformance \
    -p alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    --build-only -O twister-out-conformance-arm
```

**Watchdog safety.**  The WDT row's positive open arms a real
`RESET_SOC` watchdog, and many M-class watchdogs are
write-once-armed (`<alp/wdt.h>`) — close() cannot stop them.  The
arming path therefore runs only under
`CONFIG_TEST_ALP_CONFORMANCE_WDT_ARM` (default **y** on
`native_sim` where no watchdog device is wired, default **n** on
hardware).  A bench operator opting in should run the suite
dead-last and expect a possible end-of-run reset.

A new SoM port satisfies the gate when the
`alp_sdk.conformance.portable_api` scenario passes with the new
backend selected — add a `boards/<board>.conf` in the suite pinning
the new `CONFIG_ALP_SOC_<X>` and the board name to
`platform_allow` in `testcase.yaml`.  Nothing skips silently: the
suite asserts the failure contract and logs every degrade path
(`conformance[<class>]: degrade path -- ...` in the handler log).
A new portable class is enrolled by adding **one row** to the
`conf_classes[]` table in `tests/zephyr/conformance/src/main.c` —
the generic runners pick it up automatically; see the test-matrix
comment at the top of that file.

---

## 12. Common pitfalls

- **Inventing pin values.**  Per [[pending-hw-configs]], unknown
  routes / addresses / bases must be `"TBD"`, never plausible
  guesses.  The validator accepts `"TBD"` on every field where it
  was sanctioned; the codegen layer surfaces TBDs as warnings or
  `/* TBD */` comments in the generated artifacts.

- **Cross-form-factor confusion.**  E1M and E1M-X are separate
  product lines ([[e1m-vs-e1m-x-separate-product-lines]]).  Adding
  an E1M SoM uses the `E1M_*` pad namespace (`E1M_SPI1`,
  `E1M_GPIO_IO15`); adding an E1M-X SoM would use the
  `E1M_X_*` namespace and a different family directory.  Do not
  mix.  AEN901 is firmly E1M, so this pitfall does not apply to
  this walkthrough — but it is the #1 footgun when porting a SoM
  in a new family.

- **Skipping the `hw-revisions` resolution check.**  Even when the
  family invariant means no new row is needed, the SoM YAML's
  `default_hw_rev:` must resolve.  Running
  `validate_metadata.py` after Step 4 catches the cross-file
  reference.

- **Hand-editing generated artifacts.**  The Zephyr board files
  emitted by `--emit zephyr-board` carry a *"GENERATED BY
  alp_project.py — DO NOT HAND-EDIT"* header.  Per
  [[zephyr-board-from-yaml]], if you need a change, edit the YAML
  upstream and regenerate; never fix the generator's output
  in-place.

- **Conflating silicon facts with SoM extensions.**  Memory
  layout, peripheral counts, and core topology all come from the
  SoC JSON ([[silicon-determined-fields-not-customer-facing]]).
  On-module BOM, mailbox controller, pad routes, and helper-MCU
  firmware come from the SoM YAML.  If a fact is identical across
  every SoM that uses this silicon, it belongs in the JSON.

---

## 13. Where to next

- **Customer cookbook** — [`docs/portability.md`](portability.md)
  walks through the intra-family swap from the customer's side
  (one-line `som.sku:` edit, what the build does, when it
  fails).  *In progress — Phase D.1.*
- **Architectural rationale** —
  [`docs/adr/0011-intra-family-portability.md`](adr/0011-intra-family-portability.md)
  records the "swap SKU, no source change" guarantee as an ADR
  with explicit non-goals (cross-family portability is **not** a
  goal).  *In progress — Phase D.6.*
- **Broader SDK layout** — [`docs/architecture.md`](architecture.md)
  is the high-level map: OS targets, repository layout, public
  surface (`<alp/...>`), the SDK ↔ studio boundary.
- **Pinout chain** — [`docs/e1m-pinout.md`](e1m-pinout.md) explains
  how E1M pads link the E1M open-standard spec, the per-SoM
  `pad_routes:` block, and the board's `e1m_routes:` block.
- **Reference SoM presets** —
  [`metadata/e1m_modules/E1M-AEN701.yaml`](../metadata/e1m_modules/E1M-AEN701.yaml)
  (AEN reference) and
  [`metadata/e1m_modules/E1M-V2N101.yaml`](../metadata/e1m_modules/E1M-V2N101.yaml)
  (V2N reference with on-module GD32 bridge).

If anything in this guide didn't match what `validate_metadata.py`
or `alp_project.py` actually do, the YAML/JSON source is canonical
and this doc is wrong — please open an issue (or send a patch).
