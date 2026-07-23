# `--emit kconfig` cross-repo contract fixture

`emit-kconfig.golden.json` is the CANONICAL cross-repo contract anchor for
alp-sdk's `--emit kconfig` (#893/#894): a small, realistic sample of the
emit's own output object (what `alp_orchestrate.kconfig_symbols._envelope()`
returns), exercising every field and every Kconfig type
(`bool`/`int`/`hex`/`string`/`tristate`). It is **not** wrapped in tan-cli's
`Envelope<KconfigData>` -- tan wraps this shape, it doesn't replace it.

Both downstream consumers test against this exact file:

- tan-cli's `parse_kconfig` (`alplabai/tan-cli`, `crates/tan-core`)
- alp-sdk-vscode's `kconfigSymbolsFromEnvelope` (`alplabai/alp-sdk-vscode`,
  `src/`)

alp-sdk itself enforces its own real emit's key shape against this file in
`scripts/check_emit_kconfig_contract.py` (pr-twister, workspace-dependent)
and `tests/scripts/test_emit_kconfig.py` (hermetic).

**Any change to the key names/shape here REQUIRES a `schemaVersion` bump and
coordinated updates in tan-cli + alp-sdk-vscode.** A silent field rename here
is exactly the drift this fixture exists to catch before it reaches either
downstream repo.
