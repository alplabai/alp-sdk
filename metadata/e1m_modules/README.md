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

## Consumed by

* `scripts/alp_project.py` -- reads `<SKU>.yaml` and
  `<family>/hw-revisions.yaml` to emit per-backend config from
  the customer's `board.yaml`.
* `alp-studio`'s pin allocator (same files).
* Documentation generators that translate the per-SKU SoM preset into
  per-SKU reference sheets.
