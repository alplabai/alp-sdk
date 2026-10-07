### Changed — the hot metadata YAML readers parse with libyaml's C parser when PyYAML has it (#2328)

A per-call-site timing hook over the whole `tests/scripts/` suite found three
YAML readers responsible for about 78% of the time spent inside PyYAML:
`strict_yaml_load` (`scripts/strict_loaders.py`), the validator's
`_load_metadata_yaml` (`scripts/alp_cli/validator.py`), and
`load_family_table` (`scripts/alp_orchestrate/sdk_compat.py`). All three used
the pure-Python parser.

- `strict_loaders._StrictLoader` now subclasses
  `getattr(yaml, "CSafeLoader", yaml.SafeLoader)`. That is libyaml's C parser
  when PyYAML was built with it (PyYAML's wheels are), and the pure-Python
  parser otherwise. The constructor is the same Python `SafeConstructor`, so
  values, the `DuplicateKeyError` rejection, and its line/column marks are
  unchanged. Merge keys (`<<`) were checked too.
- New `strict_loaders.fast_safe_load(text)`: exactly `yaml.safe_load`
  semantics (a duplicate key keeps the last value) on the same loader. The
  validator and `load_family_table` call it instead of `yaml.safe_load`, so
  neither gains duplicate-key rejection it did not have before.
- Across all 701 git-tracked `*.yaml` / `*.yml` files, both loaders parse
  about 11.5x faster: 1.40 s down to 0.12 s. Locally, one serial run of
  `pytest tests/scripts/` took 16:26 on the base commit and 11:33 with this
  change. Run-to-run variance on that machine is large, though, so treat the
  suite-level figure as indicative.
- `tests/scripts/test_strict_loaders.py` pins the behaviour.
  - When libyaml is present, both loaders must use it.
  - The C and pure-Python loaders must return the same value, or raise the
    same exception class, on every tracked YAML file and on five malformed
    inputs: a duplicate key, a duplicate key after a merge, a tab indent, an
    unclosed flow sequence, and two documents. The malformed inputs are
    asserted to raise, so the error half of the comparison is actually
    exercised.

For tan-cli: `scripts/strict_loaders.py` is one of its hand-ported files,
guarded by the `STRICT_LOADERS_PINNED_SDK_COMMIT` fork-audit pin (ADR-0029).
tan's copy keeps the pure-Python parser until that pin is next re-audited;
the port is a two-line change.
