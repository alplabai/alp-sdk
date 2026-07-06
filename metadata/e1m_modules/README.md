# `metadata/e1m_modules/`

Per-SoM-family metadata for the E1M-X module catalogue.

Each family directory carries the pin-to-function mapping for the
silicon under the module's lid plus the per-revision SDK-version
compatibility table.  Each `E1M-<SKU>.yaml file` directory carries the
per-SKU manifest (silicon ref, populated on-module chips, I2C
device addresses, memory specs).

## Families

| Family | SKUs                          | Silicon                           |
|--------|-------------------------------|-----------------------------------|
| AEN    | `E1M-AEN301..801`             | Alif Ensemble E3..E8              |
| imx93  | `E1M-NX9101` [^imx93-tbd]     | NXP i.MX 93 (i.MX 9352 variant)   |
| v2n    | `E1M-V2N101`, `E1M-V2N102`    | Renesas RZ/V2N                    |
| v2n-m1 | `E1M-V2M101`, `E1M-V2M102`    | Renesas RZ/V2N + DEEPX DX-M1      |

[^imx93-tbd]: `E1M-NX9101` is a **placeholder MPN** — the production SKU is
TBD pending the hand-written HW config (see the header of
`E1M-NX9101.yaml`).  The `som.sku` regex accepts `E1M-NX9xxx` for any
4-digit tail, so the real SKU drops in as a sibling preset; do not treat
`E1M-NX9101` as the canonical, released MPN and **never hardcode the
string `E1M-NX9101`** in tooling, docs, or examples as if it were a
shipping part.  The machine-visible marker is the preset's
`status.preliminary: true` (paired with `status.partial_hw_config:
true`) — tools that filter for released SoMs must key off that flag,
not off the SKU string.  When the real SKU lands, its preset flips
`preliminary` to `false` and this placeholder is deleted (no
legacy-compat alias).

## Schema + validation

Every `E1M-<SKU>.yaml` preset validates against
`metadata/schemas/som-preset-v1.schema.json`.  Since the 2026-07
tightening the schema sets `additionalProperties: false` and pins
**one canonical shape** per fact family — `memory:` (module DRAM /
flash capacities), `on_module:` (incl. `pmic_main` and the
`i2c_devices` address map), and `inference:` (`preferred_backend`
always; `ethos_u_variant` / `npu_population` where applicable) — so
a preset can no longer carry a misspelled or family-idiosyncratic
key silently.  Unknown hardware facts stay explicit `TBD`s (values
are never invented); `alp new-som` scaffolds a schema-valid preset
with exactly this shape.

Per-family pinmux capability tables live beside the presets at
`metadata/pinmux/<family>.yaml` (`aen.yaml`, `v2n.yaml`), generated
by `scripts/gen_pinmux_capability.py` and drift-gated by
`pr-generated-files.yml`.  `scripts/validate_metadata.py` sweeps the
SoM presets and `metadata/boards/` in one gate (CI:
`pr-metadata-validate.yml`).

### Per-SKU capability restriction (`silicon_capabilities:`)

Capability flags default to the **silicon's** set: the SoC JSON's
`capabilities:` block is the base layer and the preset's `capabilities:`
block only **adds** SoM-side features (on-module chips, bridge
accelerators).  When two SKUs of one family differ in silicon
*population* — an accelerator fused off / not bonded out on one order
code — the narrower SKU declares the delta as a **restriction**:

```yaml
# HYPOTHETICAL example only — no released SKU restricts its silicon
# capability set today.  Do not copy population facts from here.
silicon_capabilities:
  unpopulated: [gpu2d, dave2d]     # silicon offers these; this SKU does not populate them
```

Rules (enforced by `scripts/validate_metadata.py`):

* every listed name must resolve to a **truthy** entry in the referenced
  SoC JSON's `capabilities:` block — a SKU can only remove what its
  silicon offers, never add;
* a name must not also appear in the preset's additive `capabilities:`
  block (a capability is either SoM-added or silicon-unpopulated, never
  both);
* omitting the field means the SKU inherits the full silicon capability
  set — the default for every current SKU.

Downstream, `resolve_capabilities()` (scripts/alp_project.py) forces each
listed capability to `false`/`0` for that SKU, and
`scripts/gen_soc_caps.py` appends an `ALP_SOM_<SKU>`-gated override block
to `include/alp/soc_caps.h` so `ALP_HAS(...)` drops the matching
`ALP_CAP_*` flags; the build emitters pass `-DALP_SOM_<SKU>` only for
restricted SKUs.

## Known cross-SKU TBDs

**AEN `helper_firmware` / `cc3501e_otp`** — all six AEN presets
(`E1M-AEN301..801`) deliberately carry `TBD` for the CC3501E helper
firmware's `firmware_path` / `flash_method` / `flash_args`.  That is
correct per the project's never-invent policy, and the six SKUs must
stay in lockstep (same TBD set, same annotations).  What unblocks
them: the first bench-built, **signed** binary from the embedded
`firmware/cc3501e/` tree (ADR 0015) landing in
`firmware/cc3501e/prebuilt/` — at that point `firmware_path` points at
the prebuilt, `flash_method` resolves to `cc3501e_usb_bootloader` or
`alif_spi_relay` (see `firmware/cc3501e/flash.py`), and `flash_args`
follows.  See also `docs/v0.6-tbd-and-assumptions.md`.  (Issue #44.)

## Consumed by

* `scripts/alp_project.py` -- reads `<SKU>.yaml` and
  `<family>/hw-revisions.yaml` to emit per-backend config from
  the customer's `board.yaml`.
* `alp-studio`'s pin allocator (same files).
* Documentation generators that translate the per-SKU SoM preset into
  per-SKU reference sheets.
