# ALP-B000: board.yaml is not valid YAML

`board.yaml` failed to parse as YAML before the schema validator ever ran --
`scripts/alp_cli/validator.py` couldn't build a document tree to check
`som:`/`cores:`/etc against
[`metadata/schemas/board.schema.json`](../../metadata/schemas/board.schema.json)
at all, so every other `ALP-Bxxx` diagnostic is unreachable until this one
clears. The message is whatever the underlying YAML loader reports (e.g. a
tab used for indentation, an unterminated quote, a duplicate key, bad flow-
mapping syntax), reported verbatim.

## Cause

- Indentation uses a literal tab character -- YAML requires spaces.
- A mapping value collides with a YAML special character (`:`, `{`, `[`)
  that needed quoting and wasn't.
- An unterminated or mismatched quote (`"...` with no closing `"`).
- A duplicate key at the same mapping level.
- A stray merge conflict marker (`<<<<<<<`) left in the file.

## Diagnose

Read-only; validates the file without touching the build. Only the SDK's own
CLI prints the full diagnostic frame with the `ALP-B000` code:

```sh
python3 -m alp_cli validate board.yaml
```

The diagnostic carries the parser's own error text and always points at
`board.yaml:1:1`, because a parse failure has no reliable node to attach a
precise position to:

```
error[ALP-B000]: YAML parse error: mapping values are not allowed here
  --> board.yaml:1:1
   |
 1 | som:
   | ^
   = see: docs/diagnostics/ALP-B000.md
```

Most YAML loaders (including PyYAML, which `alp_cli` uses under the hood)
report a `line N, column M` inside the message text itself -- read that
embedded position, not the fixed `1:1` the diagnostic frame shows, to find
the actual offending line.

`tan validate --board-yaml board.yaml` (the default, SDK-spawning path) runs
that same validator as a subprocess but forwards only its message text, not
the code -- the `ALP-B000` code above never appears in its output. It prints:

```
validate: validation failure
YAML parse error: mapping values are not allowed here
```

`tan validate --format json --board-yaml board.yaml` and `--format
diagnostic-v1` carry a `tan`-own code instead of `ALP-B000`
(`validate.schema-violation` and `validate-schema-violation` respectively),
neither with a `documentationUri`. `tan validate --offline --board-yaml
board.yaml` hits the same parse failure through tan's own structural
pre-parse and also has no `ALP-B000` code anywhere in its output; it prints:

```
validate: validation failure
board.yaml is not valid: could not be parsed as YAML: mapping values are not allowed here
  in "<unicode string>", line 2, column 20:
      variant: e1m-x-v1: bogus
                       ^
```

## Fix

Open `board.yaml` in an editor with YAML syntax highlighting, jump to the
line/column embedded in the parse-error message, and correct the syntax.
Common one-line fixes: replace a leading tab with spaces, quote a value
that starts with a special character, close a dangling quote, or rename a
duplicate key.

## Escalate

If the file looks syntactically correct to you (renders fine in an online
YAML linter) but `python3 -m alp_cli validate` still reports ALP-B000 -- or
`tan validate` / `tan validate --offline` still fails to parse it -- open an
issue with the (sanitized) `board.yaml` attached; that's a loader
compatibility gap, not a config mistake.
