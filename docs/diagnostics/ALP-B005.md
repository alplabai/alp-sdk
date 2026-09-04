# ALP-B005: SoM SKU does not resolve to a known module

`som.sku` doesn't resolve to a `metadata/e1m_modules/<SKU>.yaml` preset (or
the equivalent path under a `--metadata-root` override for an out-of-tree
metadata copy). This check is independent of the `som.sku` pattern check --
see [ALP-B003](ALP-B003.md) -- so it fires on any unresolvable SKU string,
whether or not the string is even shaped like a real one; a SKU that fails
the pattern check usually fails this lookup too and both diagnostics appear
together.

## Cause

- A typo or an off-by-one digit in the SKU (`E1M-AEN399` instead of a real
  configuration).
- A SKU for a module that genuinely hasn't been onboarded into this SDK
  checkout yet.
- `--metadata-root` (or the equivalent alp-studio / alp-orchestrate
  override) pointed at a metadata tree that doesn't carry that SKU's
  preset.

## Diagnose

```sh
tan validate --board-yaml board.yaml
```

```
error[ALP-B005]: SoM SKU 'E1M-AEN399' does not resolve to a known module
  --> board.yaml:2:8
   |
 2 |   sku: E1M-AEN399
   |        ^^^^^^^^^^
   = hint: did you mean 'E1M-AEN301'?
   = see: docs/diagnostics/ALP-B005.md
```

The hint is a closest-match suggestion, not a guarantee -- when nothing is
close, no hint is printed. To see every SKU this checkout actually knows
about, list the preset files directly (read-only):

```sh
ls metadata/e1m_modules/E1M-*.yaml
```

## Fix

Correct the SKU spelling. If you're validating against an out-of-tree
metadata root, confirm it's the one that actually carries the SoM you
mean -- `tan validate` itself always resolves against the SDK's own
`metadata/`; to point the same validator at a different root, use the
standalone pre-flight script instead, which takes an explicit override:

```sh
python3 scripts/validate_board_yaml.py --input board.yaml --metadata-root <path>
```

## Escalate

If the SKU names a real, ordered module that simply has no preset in this
SDK yet, that's a new-SoM onboarding task, not a `board.yaml` fix -- see
[`docs/porting-new-som.md`](../porting-new-som.md) (or `tan new-som`) and
file/track it as such rather than guessing at a preset shape by hand.
