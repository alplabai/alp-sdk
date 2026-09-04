# ALP-B004: value has the wrong type

A `board.yaml` value's YAML type doesn't match what
[`metadata/schemas/board.schema.json`](../../metadata/schemas/board.schema.json)
declares for that key. The two shapes seen most often: `som:` or `cores:`
written as a bare scalar instead of a mapping, and a per-core
`peripherals:` written as a bare string instead of a list.

## Cause

- `som: E1M-AEN801` instead of `som: {sku: E1M-AEN801}` -- `som:` is always
  a mapping (`sku:` + optional `hw_rev:`), never a bare string.
- `cores: m55_hp` instead of a mapping keyed by core id -- `cores:` is
  always a mapping, one entry per core.
- `peripherals: gpio` instead of `peripherals: [gpio]` -- every peripheral
  list is a YAML sequence, even with a single entry.

## Diagnose

```sh
tan validate --board-yaml board.yaml
```

```
error[ALP-B004]: 'wrong-type' is not of type 'object'
  --> board.yaml:1:6
   |
 1 | som: wrong-type
   |      ^
   = hint: expected type: object
   = see: docs/diagnostics/ALP-B004.md
```

## Fix

Reshape the value to the type the hint names. `som:` and each `cores.<id>:`
entry are always mappings; `peripherals:`, `chips:`, `libraries:`, `pins:`,
`ipc:`, and `storage:` are always lists -- see
[`docs/board-config-schema.md`](../board-config-schema.md) for the
type of every field.

## Escalate

A malformed `som:` or `cores:` used to crash the validator with an
`AttributeError` downstream instead of reporting ALP-B004 cleanly (see the
`#602` regression guards in `scripts/alp_cli/validator.py`). If you hit a
crash (a traceback, not a clean `error[ALP-B004]` line) instead of this
diagnostic, that's a validator bug -- open an issue with the (sanitized)
`board.yaml` attached, not a config mistake to work around.
