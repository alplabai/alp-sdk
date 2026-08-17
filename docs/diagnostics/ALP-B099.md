# ALP-B099: unmapped schema violation

`board.yaml` violates
[`metadata/schemas/board.schema.json`](../../metadata/schemas/board.schema.json)
in a way `scripts/alp_cli/validator.py` hasn't given its own dedicated code
yet. The validator maps the common JSON Schema keyword violations to
specific codes -- `required` to
[ALP-B001](ALP-B001.md), `additionalProperties` to [ALP-B002](ALP-B002.md),
`enum`/`pattern` to [ALP-B003](ALP-B003.md), `type` to [ALP-B004](ALP-B004.md)
-- and ALP-B099 is the fallback for every other jsonschema `validator`
keyword (`minimum`/`maximum`, `minLength`/`maxLength`, `uniqueItems`,
`minItems`/`maxItems`, `oneOf`/`anyOf`/`allOf`, `const`, `dependentRequired`,
and so on). The `message` text is whatever `jsonschema` produced for that
keyword, forwarded verbatim; there is no `hint`.

## Cause

- A value violates a numeric bound, string length, or array-size constraint
  in the schema that isn't one of the four keywords above.
- A list that must have unique entries (`uniqueItems`) has a duplicate.
- A block that must satisfy exactly one of several shapes (`oneOf`) matches
  zero or more than one of them.

## Diagnose

Read-only; validates the file without touching the build:

```sh
tan validate --board-yaml board.yaml
# or, for the machine-readable form (LSP-style ranges):
tan validate --format json --board-yaml board.yaml
```

The diagnostic points at the offending block and carries the raw
`jsonschema` message for the keyword that fired:

```
error[ALP-B099]: [1, 2, 3, 1] has non-unique elements
  --> board.yaml:14:3
   |
14 |   irq_priorities: [1, 2, 3, 1]
   | ^
   = see: docs/diagnostics/ALP-B099.md
```

Cross-check the field against
[`metadata/schemas/board.schema.json`](../../metadata/schemas/board.schema.json)
or [`docs/board-config-schema.md`](../board-config-schema.md) to see which
constraint the value is failing.

## Fix

Change the value so it satisfies the constraint the message names --
`jsonschema`'s message states the keyword and the limit directly (e.g. "is
too long", "is not one of", "has non-unique elements"). There's no generic
recipe beyond that: read the message, read the schema entry it references,
and bring the value into range.

## Escalate

A schema keyword firing under ALP-B099 that comes up often enough to want
its own message and hint (the way [ALP-B001](ALP-B001.md)-[ALP-B004](ALP-B004.md)
do) is worth its own diagnostic code -- open an issue naming the keyword and
a sample `board.yaml` snippet that triggers it.
