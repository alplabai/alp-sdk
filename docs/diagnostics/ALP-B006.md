# ALP-B006: board preset does not exist

`preset:` is set but doesn't name a real file at
`metadata/boards/<preset>.yaml`. `preset:` is the SDK-internal shortcut its
own example projects use to share the EVK board definitions; per
[`docs/board-config-schema.md`](../board-config-schema.md) customer
projects usually write the board out inline instead (`name:` +
`populated:` + `e1m_routes:`) and don't need `preset:` at all.

## Cause

- A typo in the preset name.
- A preset that was renamed or never existed in this checkout.
- Confusing a preset name with a SoM SKU or a board's human-readable
  name -- `preset:` takes the file *stem* under `metadata/boards/`, not a
  product name.

## Diagnose

```sh
tan validate --board-yaml board.yaml
```

```
error[ALP-B006]: board preset 'nope-doesnt-exist' does not exist
```

List every preset this checkout actually ships (read-only):

```sh
ls metadata/boards/*.yaml
```

The SDK's stock EVK presets are documented in
[`docs/board-config.md`](../board-config.md) (the "Stock board presets"
table); a typo close to one of those names also gets a "did you mean"
suggestion from the validator.

## Fix

Correct the preset name to one that exists, or drop `preset:` and declare
the board inline (`name:` + `populated:` + `e1m_routes:`) -- see
[`docs/board-config-schema.md`](../board-config-schema.md) for the inline
shape. `preset:` and inline fields are mutually exclusive.

## Escalate

If you expected a specific EVK preset to exist (it shipped in an older SDK
release, or a doc references it) and it genuinely isn't in
`metadata/boards/`, that's a missing/renamed preset -- check
[`docs/board-config.md`](../board-config.md) for the current stock list and
file an issue if the doc and the tree disagree.
