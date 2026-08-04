# ALP-B003: value violates an enum or pattern constraint

A `board.yaml` string value fails a JSON Schema `enum` or `pattern`
constraint. Examples in
[`metadata/schemas/board.schema.json`](../../metadata/schemas/board.schema.json):
`diagnostics.log_level` (enum: `error`, `warn`, `info`, `debug`, `trace`),
`boot.method` (enum: `mcuboot`, `none`), `ota.provider` (enum: `mender`,
`hawkbit`, `mcumgr`, `none`), `som.sku` (pattern:
`^E1M-(AEN[3-8][0-9]{2}|V2N[0-9]{3}|V2M[0-9]{3}|NX9[0-9]{3})$`), and
`preset` (pattern: `^[a-z][a-z0-9-]*$`).

## Cause

- A value with the right idea but the wrong spelling or casing --
  `log_level: verbose` (not in the enum; the SDK's levels are `error` /
  `warn` / `info` / `debug` / `trace`) or `log_level: Info` (enums are
  case-sensitive).
- A `som.sku` or `preset` string that isn't shaped like a real one at all
  (wrong prefix, mixed case, stray characters) -- this fires independently
  of, and usually alongside, [ALP-B005](ALP-B005.md) /
  [ALP-B006](ALP-B006.md), which check whether the value (once
  correctly-shaped) actually resolves to a known SoM / preset.

## Diagnose

```sh
tan validate --board-yaml board.yaml
```

Enum violations name every allowed value; pattern violations echo the
regex:

```
error[ALP-B003]: 'verbose' is not one of ['error', 'warn', 'info', 'debug', 'trace']
  --> board.yaml:7:14
   |
 7 |   log_level: verbose
   |              ^^^^^^^
   = hint: expected one of: 'error', 'warn', 'info', 'debug', 'trace'
   = see: docs/diagnostics/ALP-B003.md
```

## Fix

Use one of the listed enum values (verbatim, case included), or reshape the
string to match the printed pattern. For `som.sku` specifically, the
pattern only proves the SKU is *shaped* like a real one -- once it matches,
`tan validate` also runs the SoM lookup covered by
[ALP-B005](ALP-B005.md).

## Escalate

If a value should legitimately be added to an enum (a new bootloader
method, a new OTA provider), that's a schema-extension request -- file an
issue rather than hand-patching a local copy of `board.schema.json`, which
every other validator (the loader, `tan validate`, alp-studio) would then
disagree with.
