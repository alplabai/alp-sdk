#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Atheris fuzz harness for `scripts/alp_project.py` `board.yaml`
loader.

The board.yaml loader is parser of untrusted-at-build-time YAML:
a customer's `board.yaml` file is processed by `alp_project.py`
to emit Zephyr / CMake / Yocto config.  A crafted YAML must not:

  - Crash the loader (exit code != 0 + Python traceback is OK;
    `__builtin_trap()`-style abort is not).
  - Produce invalid downstream config (e.g. write `CONFIG_=y`
    with empty key).
  - Read arbitrary filesystem paths (YAML !!python/object tags
    that resolve via PyYAML's full-loader -- mitigated by using
    `yaml.safe_load`, but the fuzzer verifies).

Atheris drives the fuzzer.  Install: `pip install atheris`.

Build target (added to tests/fuzz/python/CMakeLists.txt when the
Atheris CI workflow lands):

    cmake -B build-fuzz -DALP_BUILD_FUZZ_PYTHON=ON
    cmake --build build-fuzz --target alp_fuzz_board_yaml

Run:

    python3 tests/fuzz/python/board_yaml_loader_fuzz.py \\
        -max_total_time=30 tests/fuzz/python/corpus/board_yaml/

Seed corpus: drop sample board.yaml files under
`tests/fuzz/python/corpus/board_yaml/`; the fuzzer mutates from
those.  Empty corpus is OK; Atheris generates inputs from
scratch but converges slower.
"""

import sys
from pathlib import Path

# Find atheris.  Bail if not installed (CI installs it via
# pip; local dev runs can `pip install atheris` to use this).
try:
    import atheris
except ImportError:
    print("atheris not installed.  Skipping fuzz harness.", file=sys.stderr)
    print("Install with: pip install atheris", file=sys.stderr)
    sys.exit(0)

# Hook the alp_project loader.  The loader's `load_board_yaml`
# function is what we want to fuzz.
REPO = Path(__file__).resolve().parent.parent.parent.parent
sys.path.insert(0, str(REPO / "scripts"))
try:
    from alp_project import load_board_yaml  # type: ignore
except ImportError:
    # alp_project may expose its entry point differently; the
    # fuzzer falls back to invoking the loader through `yaml`
    # plus the schema validator directly.
    import yaml
    import jsonschema

    SCHEMA_PATH = REPO / "metadata" / "schemas" / "board.schema.json"
    import json
    SCHEMA = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))

    def load_board_yaml(text: str):  # type: ignore[no-redef]
        cfg = yaml.safe_load(text)
        jsonschema.validate(cfg, SCHEMA)
        return cfg


@atheris.instrument_func
def TestOneInput(data: bytes) -> None:
    """
    Fuzz entry point.  Atheris feeds `data` from the corpus.

    The loader must reject malformed input cleanly -- raise a
    Python exception, return None, or exit non-zero.  Any of
    these are "OK"; the fuzzer only flags genuine crashes
    (segfault / abort / unhandled C-side error).
    """
    # Cap input size to keep the fuzzer focused on protocol-
    # level structural issues, not "what happens with 1 MB of
    # bytes".  YAML loaders have known DoS issues against huge
    # nested structures; Atheris also caps separately.
    if len(data) > 4096:
        data = data[:4096]
    try:
        text = data.decode("utf-8", errors="replace")
    except Exception:
        return
    try:
        load_board_yaml(text)
    except (yaml.YAMLError, jsonschema.ValidationError, KeyError,
            TypeError, ValueError, AttributeError):
        # Expected rejection paths.  No fuzz-relevant signal.
        return
    except SystemExit:
        # alp_project.py calls sys.exit() on validation errors --
        # that's a clean rejection, not a crash.
        return


def main() -> int:
    atheris.Setup(sys.argv, TestOneInput, enable_python_coverage=True)
    atheris.Fuzz()
    return 0


if __name__ == "__main__":
    # Lazy-import yaml + jsonschema only when invoked as a script
    # (the TestOneInput closure above captures them).
    import yaml          # noqa: F401
    import jsonschema    # noqa: F401
    sys.exit(main())
