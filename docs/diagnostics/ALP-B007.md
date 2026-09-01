# ALP-B007: board preset does not host the SoM's family

`preset:` names a real board preset (it passed [ALP-B006](ALP-B006.md)) and
`som.sku` resolves to a real module (it passed [ALP-B005](ALP-B005.md)), but
the preset's `hosts_som_families:` list in `metadata/boards/<preset>.yaml`
doesn't contain the SoM's own `family:` from
`metadata/e1m_modules/<SKU>.yaml`. This check only runs once both of those
resolve cleanly -- an unresolvable preset or SKU is ALP-B006/ALP-B005's job
to report, not this one's.

## Cause

- Picking an EVK preset by habit or copy/paste instead of by family --
  `metadata/boards/e1m-evk.yaml` hosts `alif-ensemble` and `nxp-imx9`
  (E1M-AEN3xx..AEN8xx, E1M-NX9101); `metadata/boards/e1m-x-evk.yaml` hosts
  `renesas-rzv2n` and `renesas-rzv2n-deepx` (E1M-V2N1xx, E1M-V2M1xx). Per
  [ADR-0011](../adr/0011-intra-family-portability.md), the two families
  are not interchangeable -- there is no single preset that hosts both.
- Swapping `som.sku` to a different family's module (e.g. moving a project
  from an AEN SoM to a V2N SoM) without also swapping `preset:`.
- A custom preset's `hosts_som_families:` list that was never updated after
  the preset started being used with a new family.

## Diagnose

Read-only; validates the file without touching the build:

```sh
python3 scripts/validate_board_yaml.py --input board.yaml
# or, with a separate `tan` install:
tan validate --board-yaml board.yaml
```

The diagnostic names both sides of the mismatch and, when a hosting preset
exists, suggests one:

```
error[ALP-B007]: board preset 'e1m-evk' hosts SoM families
  ['alif-ensemble', 'nxp-imx9'], but E1M-V2N101 is family 'renesas-rzv2n'
  --> board.yaml:4:9
   |
 4 | preset: e1m-evk
   |         ^^^^^^^
   = hint: use a board preset whose hosts_som_families includes
     'renesas-rzv2n' (for example: e1m-x-evk), or define a compatible
     board inline
   = see: docs/diagnostics/ALP-B007.md
```

No `(for example: ...)` clause is appended to the hint when this checkout
ships no preset hosting that family at all -- that's the signal an inline
board (see Fix) is the only option, not a missing preset name.

To see every preset's declared families directly (read-only):

```sh
grep -l hosts_som_families metadata/boards/*.yaml | xargs grep -A1 hosts_som_families
```

## Fix

Either:

- Point `preset:` at a board preset whose `hosts_som_families:` includes the
  SoM's family (the hint names one when this checkout ships a match), or
- Drop `preset:` and declare the board inline instead (`name:` +
  `populated:` + `e1m_routes:` -- see
  [`docs/board-config-schema.md`](../board-config-schema.md)); an inline
  board carries no `hosts_som_families:` constraint because it isn't shared
  across SoM families the way a stock EVK preset is.

`preset:` and inline fields are mutually exclusive, so only one of the two
applies to a given `board.yaml`.

## Escalate

If you believe the SoM family genuinely should be portable to the preset
you picked (a carrier board that legitimately hosts both families), that's
an [ADR-0011](../adr/0011-intra-family-portability.md) policy question,
not a config mistake -- open an issue describing the carrier rather than
hand-editing `hosts_som_families:` in a stock EVK preset, which every other
project pointed at that preset would then inherit.
