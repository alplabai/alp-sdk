# ALP-B001: required key is missing

`board.yaml` violates a `required` constraint in
[`metadata/schemas/board.schema.json`](../../metadata/schemas/board.schema.json):
an object the schema demands a key for doesn't have it. The two
always-required top-level keys are `som:` and `cores:`; several nested blocks
add their own `required` list once you opt into them -- `som:` requires
`sku:`, a `storage:` entry requires `name:` + `size_kib:`, a `boot.signing:`
block requires `algorithm:` + `key_file:`, `ota:` requires `provider:`,
and an `ipc:` entry requires `kind:`,
`endpoints:`, `carve_out_kb:`, `name:`. See
[`docs/board-config-schema.md`](../board-config-schema.md) for the full
field table.

## Cause

- A brand-new `board.yaml` missing the mandatory `som:` or `cores:` block.
- A block that was partially filled in -- e.g. a `storage:` entry with
  `fs:` and `mount:` but no `size_kib:`, or a `boot.signing:` block with
  `algorithm:` but no `key_file:`.
- A copy/paste from an example that trimmed a line the schema still expects.

## Diagnose

Read-only; validates the file without touching the build:

```sh
tan validate --board-yaml board.yaml
# or, for the machine-readable form (LSP-style ranges):
tan validate --format json --board-yaml board.yaml
```

The diagnostic names the missing key and points at the enclosing block:

```
error[ALP-B001]: required key 'som' is missing
  --> board.yaml:2:1
   |
 2 | preset: e1m-evk
   | ^
   = hint: add a 'som:' entry to this block
   = see: docs/diagnostics/ALP-B001.md
```

## Fix

Add the named key to the block the diagnostic points at. Cross-check what
belongs there against [`docs/board-config-schema.md`](../board-config-schema.md)
or the schema file directly -- both list every key's type and whether it's
required.

## Escalate

If `tan validate` reports ALP-B001 for a key you can see is genuinely
present in the file (for example, indented under the wrong parent), that's
a validator positioning bug rather than a config mistake -- open an issue
with the (sanitized) `board.yaml` attached.
