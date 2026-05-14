# Python fuzz harnesses (Atheris)

LibFuzzer-style fuzzers for the SDK's Python-side parser
surfaces.  Separate from `tests/fuzz/*.c` (which fuzzes C parsers
under clang's libFuzzer) because Python needs Atheris -- Google's
libFuzzer port that hooks Python coverage.

## Harnesses

| File                         | Target                                                                                |
|------------------------------|---------------------------------------------------------------------------------------|
| `board_yaml_loader_fuzz.py`  | `scripts/alp_project.py`'s `board.yaml` loader -- customer-authored project config.   |
| `som_preset_yaml_fuzz.py`    | The per-SKU SoM preset loader (`metadata/e1m_modules/E1M-<MPN>.yaml`).                |

## Install

```bash
pip install atheris pyyaml jsonschema
```

Atheris requires a clang toolchain matching the host's Python
build.  On macOS this typically works out of the box; on Linux
you may need `apt install clang-14` (or equivalent) first.

## Run one harness

```bash
# 30-second smoke run.  Drop seed inputs into the corpus/ dir
# alongside the harness for faster convergence.
mkdir -p tests/fuzz/python/corpus/board_yaml
python3 tests/fuzz/python/board_yaml_loader_fuzz.py \
    -max_total_time=30 \
    tests/fuzz/python/corpus/board_yaml/
```

## CI integration

A dedicated workflow (`pr-fuzz-python.yml` -- pending) runs the
Python harnesses for 60 seconds each on every PR touching:

- `scripts/alp_project.py`
- `scripts/validate_*.py`
- `metadata/schemas/*.json`
- `tests/fuzz/python/*`

Findings get logged + crash-input artefacts uploaded for
post-mortem.  Until the workflow lands, run the harnesses
manually before merging schema or loader changes.

## See also

- [`tests/fuzz/README.md`](../README.md) -- C-side libFuzzer
  harnesses + the design rationale shared with this dir.
- [`docs/threat-model.md`](../../../docs/threat-model.md) §3.5 --
  the supply-chain threat class these fuzzers harden against.
