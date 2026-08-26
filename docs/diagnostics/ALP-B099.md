# ALP-B099: unmapped schema violation

`board.yaml` violates
[`metadata/schemas/board.schema.json`](../../metadata/schemas/board.schema.json)
in a way `scripts/alp_cli/validator.py` hasn't given its own dedicated code
yet. The validator maps the common JSON Schema keyword violations to
specific codes -- `required` to
[ALP-B001](ALP-B001.md), `additionalProperties` to [ALP-B002](ALP-B002.md),
`enum`/`pattern` to [ALP-B003](ALP-B003.md), `type` to [ALP-B004](ALP-B004.md)
-- and ALP-B099 is the fallback for every other jsonschema `validator`
keyword the schema currently uses (`minimum`/`maximum`, `minItems`,
`minProperties`, `uniqueItems`, `oneOf`, `not`, and any other keyword a
future schema revision adds). `anyOf`, `allOf`, and `if`/`then` never reach
ALP-B099: `board.schema.json`'s only `if`/`then` site
(`board.schema.json:40-48`) surfaces its violation as `required` --
[ALP-B001](ALP-B001.md) -- and its two `anyOf` sites
(`board.schema.json:16`, `:41`) both sit inside a `not:`, which `jsonschema`
evaluates with `is_valid()` rather than `iter_errors()` -- it never yields an
`anyOf`/`allOf` error of its own, only the enclosing `not` (see the Cause
bullet below). The `message` text is whatever `jsonschema` produced for the
keyword that actually fired, forwarded verbatim; there is no `hint`.

## Cause

- A value violates a numeric bound, array-size, or object-size constraint
  (`minimum`/`maximum`, `minItems`, `minProperties`) in the schema that
  isn't one of `required`/`additionalProperties`/`enum`/`pattern`/`type`
  (those four map to [ALP-B001](ALP-B001.md)-[ALP-B004](ALP-B004.md) instead).
- A list that must have unique entries (`uniqueItems`) has a duplicate.
- A block that must satisfy exactly one of several shapes (`oneOf`) matches
  zero or more than one of them.
- A `not:` constraint fires -- the value matches a shape the schema
  explicitly forbids. `board.schema.json` uses this for two real cases: a
  top-level `os:` key (`"not": {"required": ["os"]}` -- `os` is
  silicon-determined and never customer-set), and mixing `preset:` with
  inline `name:`/`populated:`/`e1m_routes:` (mutually exclusive board
  declaration styles). The `message` for a `not` violation reads `{...}
  should not be valid under {...}` -- the first `{...}` is the whole
  document (or matched sub-schema) as jsonschema saw it, the second is the
  sub-schema it wasn't supposed to satisfy; read the second `{...}`'s
  `required` list (the `os:` case) or `anyOf`/`allOf` list (the `preset:`
  case) to see which forbidden combination fired.

## Diagnose

Read-only; validates the file without touching the build. Only `tan
validate` prints the full diagnostic frame with the `ALP-B099` code and the
`= see:` pointer below:

```sh
tan validate --board-yaml board.yaml
```

The diagnostic points at the offending block and carries the raw
`jsonschema` message for the keyword that fired. For example, a
`supported_boards:` list (`board.schema.json:470`) with a duplicate
`e1m-evk` entry:

```
error[ALP-B099]: ['e1m-evk', 'e1m-evk'] has non-unique elements
  --> board.yaml:1:1
   |
 1 | som:
   | ^
   = see: docs/diagnostics/ALP-B099.md
```

(`uniqueItems` fires on the array itself, so the reported position is the
first key of the enclosing mapping rather than the `supported_boards:` line.)

`tan validate --board-yaml board.yaml` runs that same validator as a
subprocess. `tan` `v0.5.1` prints only the raw `jsonschema` message,
no code:

```
validate: validation failure
['e1m-evk', 'e1m-evk'] has non-unique elements
```

`tan` `v0.6.0-rc1` and `dev` (checked as `0.6.0-rc2.dev0`) prefix the code
and add the `see:` pointer:

```
validate: validation failure
ALP-B099: ['e1m-evk', 'e1m-evk'] has non-unique elements
  see: docs/diagnostics/ALP-B099.md
```

`tan validate --format json --board-yaml board.yaml` carries a `tan`-own
code in `issues[].code` (`validate.schema-violation`) and no LSP-style
range. For the machine-readable form with ranges, use:

```sh
tan validate --format diagnostic-v1 --board-yaml board.yaml
```

Cross-check the field against
[`metadata/schemas/board.schema.json`](../../metadata/schemas/board.schema.json)
or [`docs/board-config-schema.md`](../board-config-schema.md) to see which
constraint the value is failing.

## Fix

Change the value so it satisfies the constraint the message names. For a
bound or uniqueness keyword, `jsonschema`'s message states the violation
directly and usually includes the limit (e.g. "has non-unique elements", "2
is less than the minimum of 4"). For a `not` violation the message states
what the value must NOT match instead of a limit -- "should not be valid
under {...}" -- so read the `{...}` sub-schema it names (see the Cause
bullet above) rather than looking for a number to satisfy. There's no
generic recipe beyond that: read the message, read the schema entry it
references, and bring the value into range or drop the forbidden
combination.

## Escalate

A schema keyword firing under ALP-B099 that comes up often enough to want
its own message and hint (the way [ALP-B001](ALP-B001.md)-[ALP-B004](ALP-B004.md)
do) is worth its own diagnostic code -- open an issue naming the keyword and
a sample `board.yaml` snippet that triggers it.
