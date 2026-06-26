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
`E1M-NX9101` as the canonical, released MPN.

## Consumed by

* `scripts/alp_project.py` -- reads `<SKU>.yaml` and
  `<family>/hw-revisions.yaml` to emit per-backend config from
  the customer's `board.yaml`.
* `alp-studio`'s pin allocator (same files).
* Documentation generators that translate the per-SKU SoM preset into
  per-SKU reference sheets.
