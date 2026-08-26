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

Read-only; validates the file without touching the build. In an alp-sdk
checkout, `scripts/validate_board_yaml.py` prints the full diagnostic frame
with the `ALP-B000` code directly -- it is the script `tan validate` itself
spawns:

```sh
python3 scripts/validate_board_yaml.py --input board.yaml
```

With a separate `tan` install:

```sh
tan validate --board-yaml board.yaml
```

The diagnostic carries the parser's own error text and always points at
`board.yaml:1:1`, because a parse failure has no reliable node to attach a
precise position to:

```
error[ALP-B000]: YAML parse error: mapping values are not allowed here
  in "<unicode string>", line 2, column 20:
      variant: e1m-x-v1: bogus
                       ^
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
that same validator as a subprocess. Only `tan` `v0.5.1` and older drop the
code and forward just the message text; `v0.6.0-rc1` and the `dev` line
(checked as `0.6.0-rc2.dev0`) prefix the code back onto the message:

```
validate: validation failure
ALP-B000: YAML parse error: mapping values are not allowed here
```

(`tan` `v0.5.1` prints the same two lines minus the `ALP-B000: ` prefix on
the second one.)

`tan validate --format json --board-yaml board.yaml` still carries a
`tan`-own code in `issues[].code` (`validate.schema-violation`), but on
`v0.6.0-rc1`/`dev` its `message` is prefixed `ALP-B000: ` too. `tan validate
--format diagnostic-v1 --board-yaml board.yaml` instead carries the exact
SDK code, `"code": "ALP-B000"` -- no `tan`-own substitute there -- on both
`v0.6.0-rc1` and `dev`. Neither `--format` carries a `documentationUri` for
this code: the YAML-parse-failure path never reaches the SDK's rich
diagnostic object that carries one. `tan validate --offline --board-yaml
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
YAML linter) but `tan validate` / `tan validate --offline` still fails to
parse it, open an issue with the (sanitized) `board.yaml` attached; that's a
loader compatibility gap, not a config mistake.
