# ALP-B002: unknown key

`board.yaml` (and every nested object inside it -- `som:`, `cores.<id>:`,
`diagnostics:`, `boot:`, `ota:`, and so on) declares
`"additionalProperties": false` in
[`metadata/schemas/board.schema.json`](../../metadata/schemas/board.schema.json).
Any key that isn't in that object's `properties` list is rejected outright.
Two objects are the exception, by design rather than oversight: `populated:`
(a boolean map keyed by chip-driver name) and `diagnostics.modules:` (a
log-level map keyed by SDK module name) both declare a *typed*
`additionalProperties` schema instead, so they accept any key -- a typo'd
chip name or module name under either one is silently accepted, not flagged
by ALP-B002.

## Cause

- A typo in a key name (`diagostics:` instead of `diagnostics:`,
  `perhiperals:` instead of `peripherals:`).
- A field name copied from a different schema version, an older example, or
  a mis-remembered name that was never actually part of the schema.
- A field that genuinely doesn't exist yet -- the schema hasn't grown a knob
  for what you're trying to configure.

## Diagnose

```sh
tan validate --board-yaml board.yaml
```

The diagnostic names the offending key and, when it's close enough to a
real one, suggests the fix:

```
error[ALP-B002]: unknown key 'diagostics'
  --> board.yaml:6:1
   |
 6 | diagostics:
   | ^^^^^^^^^^
   = hint: did you mean 'diagnostics'?
   = see: docs/diagnostics/ALP-B002.md
```

No suggestion is printed when nothing in that block's schema is a close
match -- that's the signal you're not dealing with a typo.

## Fix

Fix the typo, or remove the key if it doesn't belong. Check
[`docs/board-config-schema.md`](../board-config-schema.md) (or the schema
file itself) for the exact key spelling and which object it lives under --
`additionalProperties: false` is per-object, so a key that's valid at the
top level is still rejected if it's placed one level too deep (or too
shallow).

## Escalate

If the key names a feature you believe the SDK should support but the
schema has no slot for it, that's a schema-addition request, not a
`board.yaml` bug -- open an issue describing the field instead of trying to
work around `additionalProperties: false` locally (there is no override).
