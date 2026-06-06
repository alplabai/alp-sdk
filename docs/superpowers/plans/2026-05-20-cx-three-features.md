# CX Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship three CX features — an `alp` Python CLI with `init`/`run`/`validate`, Rust-style diagnostics for `board.yaml`, and an `ALP_HAS`/`alp_has` capability API — sharing common infrastructure (alp_cli package, position-aware YAML loader, SoC JSON source of truth).

**Architecture:** A new `scripts/alp_cli/` Python package is the seam: it holds the position-aware YAML loader and the diagnostic data type that the validator (Feature 2) and the CLI (Feature 1) both consume. Feature 3 extends `scripts/gen_soc_caps.py` to emit a portable `ALP_HAS(cap)` macro layer plus an `include/alp/cap.h` + `src/cap.c` runtime pair derived from the existing per-SoC capability macros. Phases 1 and 2 are independent and parallel-dispatch friendly; phase 3 consumes phase 1's validator.

**Tech Stack:** Python 3.10+, `click`, `questionary`, `colorama`, `PyYAML`, `jsonschema`, `pytest`, `pytest-snapshot`; Zephyr Ztest for the C unit test; existing `gen_soc_caps.py` generator for the capability headers.

**Spec:** `docs/superpowers/specs/2026-05-20-cx-three-features-design.md`

---

## File Structure

**Create:**
- `pyproject.toml` — package metadata, deps, `alp` console_script entry.
- `scripts/alp_cli/__init__.py`
- `scripts/alp_cli/main.py` — click group registering subcommands.
- `scripts/alp_cli/yaml_pos.py` — position-aware YAML loader (foundation).
- `scripts/alp_cli/diagnostic.py` — `Diagnostic` dataclass + renderer (foundation).
- `scripts/alp_cli/validator.py` — schema + cross-ref + compatibility passes.
- `scripts/alp_cli/validate.py` — `alp validate` subcommand.
- `scripts/alp_cli/init.py` — `alp init` wizard.
- `scripts/alp_cli/run.py` — `alp run` build+exec on native_sim (or `--board` for west).
- `tests/scripts/test_yaml_pos.py`
- `tests/scripts/test_diagnostic.py`
- `tests/scripts/test_board_yaml_diagnostics.py`
- `tests/scripts/test_alp_cli.py`
- `tests/scripts/test_gen_soc_caps_cap_layer.py`
- `tests/fixtures/board_yaml_good/minimal.yaml`
- `tests/fixtures/board_yaml_bad/ALP-B001-missing-required.yaml`
- `tests/fixtures/board_yaml_bad/ALP-B002-unknown-key.yaml`
- `tests/fixtures/board_yaml_bad/ALP-B003-bad-enum.yaml`
- `tests/fixtures/board_yaml_bad/ALP-B004-wrong-type.yaml`
- `tests/fixtures/board_yaml_bad/ALP-B005-bad-sku.yaml`
- `tests/fixtures/board_yaml_bad/ALP-B006-bad-preset.yaml`
- `tests/fixtures/board_yaml_bad/ALP-B010-peripheral-not-on-soc.yaml`
- `tests/unit/cap/CMakeLists.txt` — Ztest harness for cap API.
- `tests/unit/cap/prj.conf`
- `tests/unit/cap/testcase.yaml`
- `tests/unit/cap/src/test_cap.c`
- `include/alp/cap.h` — generated.
- `src/cap.c` — generated.
- `docs/diagnostics/ALP-B001.md` … `ALP-B010.md` — one stub per code.

**Modify:**
- `scripts/gen_soc_caps.py` — emit `ALP_HAS()` macro layer + write `include/alp/cap.h` and `src/cap.c`.
- `scripts/alp_project.py` — route validation through `alp_cli.validator`; keep external CLI compatible.
- `examples/peripheral-io/hello-world/src/main.c` — add `ALP_HAS` + `alp_has` teaching block.
- `README.md` — quickstart with `pip install -e .` → `alp init` → `alp run`.

---

# Phase 0 — Foundation (sequential, prerequisite for Phases 1 + 3)

### Task 0.1: Bootstrap `alp_cli` package + `pyproject.toml`

**Files:**
- Create: `pyproject.toml`
- Create: `scripts/alp_cli/__init__.py`
- Create: `scripts/alp_cli/main.py`
- Test: `tests/scripts/test_alp_cli.py`

- [ ] **Step 1: Write the failing test**

Create `tests/scripts/test_alp_cli.py`:

```python
from click.testing import CliRunner

from alp_cli.main import cli


def test_alp_cli_help_lists_subcommands():
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    for sub in ("init", "run", "validate"):
        assert sub in result.output


def test_alp_cli_reports_version():
    result = CliRunner().invoke(cli, ["--version"])
    assert result.exit_code == 0
    assert "alp" in result.output.lower()
```

- [ ] **Step 2: Run test to verify it fails**

```bash
pip install -e ".[dev]" 2>/dev/null || true
pytest tests/scripts/test_alp_cli.py -v
```
Expected: collection error (`alp_cli` not importable) or `pyproject.toml` missing.

- [ ] **Step 3: Write `pyproject.toml`**

Create `pyproject.toml`:

```toml
[build-system]
requires = ["setuptools>=68", "wheel"]
build-backend = "setuptools.build_meta"

[project]
name = "alp-sdk-cli"
version = "0.6.0"
description = "Command-line front-end for the Alp SDK"
requires-python = ">=3.10"
dependencies = [
    "PyYAML>=6.0",
    "jsonschema>=4.0",
    "click>=8.1",
    "questionary>=2.0",
    "colorama>=0.4.6",
]

[project.optional-dependencies]
dev = [
    "pytest>=7.0",
    "pytest-snapshot>=0.9",
]

[project.scripts]
alp = "alp_cli.main:cli"

[tool.setuptools]
package-dir = {"" = "scripts"}

[tool.setuptools.packages.find]
where = ["scripts"]
include = ["alp_cli*"]
```

- [ ] **Step 4: Write the package skeleton**

Create `scripts/alp_cli/__init__.py`:

```python
"""Alp SDK command-line interface."""

__version__ = "0.6.0"
```

Create `scripts/alp_cli/main.py`:

```python
"""Top-level click group for the `alp` CLI."""

from __future__ import annotations

import click

from alp_cli import __version__


@click.group(help="Alp SDK command-line interface.")
@click.version_option(__version__, prog_name="alp")
def cli() -> None:
    """Alp SDK command-line interface."""


@cli.command(help="(stub) Scaffold a new project.")
def init() -> None:
    click.echo("init: not yet implemented")


@cli.command(help="(stub) Build and run on native_sim.")
def run() -> None:
    click.echo("run: not yet implemented")


@cli.command(help="(stub) Validate board.yaml.")
def validate() -> None:
    click.echo("validate: not yet implemented")


if __name__ == "__main__":
    cli()
```

- [ ] **Step 5: Install the package + run the tests**

```bash
pip install -e ".[dev]"
pytest tests/scripts/test_alp_cli.py -v
```
Expected: both tests PASS.

- [ ] **Step 6: Commit**

```bash
git add pyproject.toml scripts/alp_cli/__init__.py scripts/alp_cli/main.py tests/scripts/test_alp_cli.py
git commit -m "feat(cli): bootstrap alp_cli package + pyproject.toml"
```

---

### Task 0.2: Position-aware YAML loader

**Files:**
- Create: `scripts/alp_cli/yaml_pos.py`
- Test: `tests/scripts/test_yaml_pos.py`

- [ ] **Step 1: Write the failing test**

Create `tests/scripts/test_yaml_pos.py`:

```python
from textwrap import dedent

from alp_cli.yaml_pos import load_with_positions, node_position


def test_mapping_carries_line_and_column():
    src = dedent(
        """\
        som:
          sku: E1M-AEN701
        preset: e1m-evk
        """
    )
    data = load_with_positions(src, source="board.yaml")
    line, col = node_position(data, "som")
    assert line == 1
    assert col == 1
    line, col = node_position(data["som"], "sku")
    assert line == 2
    assert col == 3


def test_sequence_items_carry_position():
    src = dedent(
        """\
        peripherals:
          - id: i2c0
          - id: spi0
        """
    )
    data = load_with_positions(src, source="board.yaml")
    items = data["peripherals"]
    assert items[0]["__line__"] == 2
    assert items[1]["__line__"] == 3


def test_scalar_value_position_via_helper():
    src = "som:\n  sku: E1M-AEN701\n"
    data = load_with_positions(src, source="board.yaml")
    line, col = node_position(data["som"], "sku", target="value")
    assert line == 2
    # "E1M-AEN701" starts after "sku: " on column 8 (1-based).
    assert col == 8
```

- [ ] **Step 2: Run test to verify it fails**

```bash
pytest tests/scripts/test_yaml_pos.py -v
```
Expected: ImportError on `alp_cli.yaml_pos`.

- [ ] **Step 3: Implement the loader**

Create `scripts/alp_cli/yaml_pos.py`:

```python
"""Position-aware YAML loader.

Subclasses PyYAML's SafeLoader so every constructed mapping and
sequence carries `__line__`, `__column__`, `__end_line__`,
`__end_column__` (1-based) and a per-key `__keys__` table that
maps key name -> (line, col, value_line, value_col).
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Literal

import yaml


class _PosLoader(yaml.SafeLoader):
    pass


def _construct_mapping(loader: _PosLoader, node: yaml.MappingNode) -> dict[str, Any]:
    loader.flatten_mapping(node)
    mapping: dict[str, Any] = {}
    keys: dict[str, dict[str, int]] = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=True)
        value = loader.construct_object(value_node, deep=True)
        mapping[key] = value
        keys[key] = {
            "line": key_node.start_mark.line + 1,
            "col": key_node.start_mark.column + 1,
            "value_line": value_node.start_mark.line + 1,
            "value_col": value_node.start_mark.column + 1,
            "value_end_line": value_node.end_mark.line + 1,
            "value_end_col": value_node.end_mark.column + 1,
        }
    mapping["__line__"] = node.start_mark.line + 1
    mapping["__column__"] = node.start_mark.column + 1
    mapping["__end_line__"] = node.end_mark.line + 1
    mapping["__end_column__"] = node.end_mark.column + 1
    mapping["__keys__"] = keys
    return mapping


def _construct_sequence(loader: _PosLoader, node: yaml.SequenceNode) -> list[Any]:
    items: list[Any] = []
    for child in node.value:
        item = loader.construct_object(child, deep=True)
        if isinstance(item, dict):
            item.setdefault("__line__", child.start_mark.line + 1)
            item.setdefault("__column__", child.start_mark.column + 1)
        items.append(item)
    return items


_PosLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _construct_mapping
)
_PosLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_SEQUENCE_TAG, _construct_sequence
)


def load_with_positions(text: str, source: str | Path) -> dict[str, Any]:
    """Parse a YAML document, attaching position metadata to mappings."""
    data = yaml.load(text, Loader=_PosLoader)
    if not isinstance(data, dict):
        raise ValueError(f"{source}: top-level YAML is not a mapping")
    return data


def node_position(
    mapping: dict[str, Any],
    key: str,
    target: Literal["key", "value"] = "key",
) -> tuple[int, int]:
    """Return (line, column), 1-based, for a key or its value."""
    table = mapping.get("__keys__")
    if not table or key not in table:
        return (mapping.get("__line__", 1), mapping.get("__column__", 1))
    entry = table[key]
    if target == "value":
        return (entry["value_line"], entry["value_col"])
    return (entry["line"], entry["col"])
```

- [ ] **Step 4: Re-run the tests**

```bash
pytest tests/scripts/test_yaml_pos.py -v
```
Expected: all three tests PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/yaml_pos.py tests/scripts/test_yaml_pos.py
git commit -m "feat(cli): position-aware YAML loader"
```

---

### Task 0.3: `Diagnostic` data type + renderer

**Files:**
- Create: `scripts/alp_cli/diagnostic.py`
- Test: `tests/scripts/test_diagnostic.py`

- [ ] **Step 1: Write the failing test**

Create `tests/scripts/test_diagnostic.py`:

```python
from pathlib import Path

from alp_cli.diagnostic import Diagnostic, render


def _sample_source() -> str:
    return (
        "som:\n"
        "  sku: E1M-AEN701\n"
        "preset: e1m-evk\n"
        "peripherals:\n"
        "  - { pad: P21, signal: I2C0_SCL }\n"
    )


def test_render_error_includes_code_path_caret_hint_and_doclink(tmp_path: Path):
    src = _sample_source()
    fixture = tmp_path / "board.yaml"
    fixture.write_text(src)
    diag = Diagnostic(
        severity="error",
        path=fixture,
        line=5,
        col=11,
        span=3,
        code="ALP-B005",
        message="pad 'P21' not present on E1M-AEN701",
        hint="did you mean 'P20'? (closest match, distance 1)",
        doc_url=None,
    )

    out = render(diag, source_text=src, color=False)
    assert "error[ALP-B005]" in out
    assert "board.yaml:5:11" in out
    assert "  - { pad: P21" in out
    assert "^^^" in out
    assert "did you mean 'P20'" in out
    assert "docs/diagnostics/ALP-B005.md" in out


def test_render_omits_color_codes_when_color_false(tmp_path: Path):
    src = "som:\n  sku: bogus\n"
    fixture = tmp_path / "board.yaml"
    fixture.write_text(src)
    diag = Diagnostic(
        severity="error",
        path=fixture,
        line=2,
        col=8,
        span=5,
        code="ALP-B003",
        message="bad sku",
        hint=None,
        doc_url=None,
    )
    out = render(diag, source_text=src, color=False)
    assert "\x1b[" not in out
```

- [ ] **Step 2: Run test to verify it fails**

```bash
pytest tests/scripts/test_diagnostic.py -v
```
Expected: ImportError on `alp_cli.diagnostic`.

- [ ] **Step 3: Implement the diagnostic module**

Create `scripts/alp_cli/diagnostic.py`:

```python
"""Diagnostic data type + Rust-style renderer for the alp validator."""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

try:
    from colorama import Fore, Style
    from colorama import init as _colorama_init

    _colorama_init()
except ImportError:  # pragma: no cover - colorama is listed as a dep
    class _Stub:
        def __getattr__(self, _: str) -> str:
            return ""

    Fore = _Stub()  # type: ignore[assignment]
    Style = _Stub()  # type: ignore[assignment]


Severity = Literal["error", "warning", "note"]


@dataclass(slots=True)
class Diagnostic:
    severity: Severity
    path: Path
    line: int
    col: int
    span: int
    code: str
    message: str
    hint: str | None = None
    doc_url: str | None = None


def _doc_url(code: str) -> str:
    base = os.environ.get("ALP_DIAG_BASE_URL", "docs/diagnostics")
    return f"{base}/{code}.md"


def _use_color(color: bool | None) -> bool:
    if color is False:
        return False
    if color is True:
        return True
    if os.environ.get("NO_COLOR"):
        return False
    return sys.stdout.isatty()


def render(diag: Diagnostic, source_text: str, color: bool | None = None) -> str:
    """Render a diagnostic as a multi-line Rust-style block."""
    use_color = _use_color(color)

    def paint(s: str, hue: str) -> str:
        return f"{hue}{s}{Style.RESET_ALL}" if use_color else s

    sev_hue = {"error": Fore.RED, "warning": Fore.YELLOW, "note": Fore.CYAN}[
        diag.severity
    ]
    header = f"{paint(f'{diag.severity}[{diag.code}]', sev_hue)}: {diag.message}"
    arrow = f"  --> {diag.path}:{diag.line}:{diag.col}"

    lines = source_text.splitlines()
    if 1 <= diag.line <= len(lines):
        src_line = lines[diag.line - 1]
    else:
        src_line = ""

    gutter_w = max(2, len(str(diag.line)))
    blank_gutter = " " * gutter_w
    src_block = (
        f"{blank_gutter} |\n"
        f"{str(diag.line).rjust(gutter_w)} | {src_line}\n"
        f"{blank_gutter} | {' ' * (diag.col - 1)}{paint('^' * max(1, diag.span), sev_hue)}"
    )

    tail: list[str] = []
    if diag.hint:
        tail.append(f"{blank_gutter} = hint: {diag.hint}")
    tail.append(f"{blank_gutter} = see: {diag.doc_url or _doc_url(diag.code)}")

    return "\n".join([header, arrow, src_block, *tail, ""])


class DiagnosticCollector:
    """Collect diagnostics across multiple validation passes."""

    def __init__(self) -> None:
        self._items: list[Diagnostic] = []

    def add(self, diag: Diagnostic) -> None:
        self._items.append(diag)

    def __iter__(self):
        return iter(self._items)

    def __len__(self) -> int:
        return len(self._items)

    def has_errors(self) -> bool:
        return any(d.severity == "error" for d in self._items)

    def emit(self, source_text: str, color: bool | None = None) -> None:
        for diag in self._items:
            print(render(diag, source_text=source_text, color=color))
```

- [ ] **Step 4: Re-run the tests**

```bash
pytest tests/scripts/test_diagnostic.py -v
```
Expected: both tests PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/diagnostic.py tests/scripts/test_diagnostic.py
git commit -m "feat(cli): Diagnostic dataclass + Rust-style renderer"
```

---

# Phase 1 — Rich diagnostics for `board.yaml` (depends on Phase 0)

### Task 1.1: Validator scaffolding + happy-path fixture

**Files:**
- Create: `scripts/alp_cli/validator.py`
- Create: `tests/fixtures/board_yaml_good/minimal.yaml`
- Test: `tests/scripts/test_board_yaml_diagnostics.py`

- [ ] **Step 1: Write the happy-path fixture**

Create `tests/fixtures/board_yaml_good/minimal.yaml`:

```yaml
som:
  sku: E1M-AEN701
preset: e1m-evk
cores:
  m55_hp:
    app: ./src
    peripherals: []
diagnostics:
  log_level: info
```

- [ ] **Step 2: Write the failing test**

Create `tests/scripts/test_board_yaml_diagnostics.py`:

```python
from pathlib import Path

from alp_cli.validator import validate_board_yaml


FIX_GOOD = Path(__file__).parent.parent / "fixtures" / "board_yaml_good"
FIX_BAD = Path(__file__).parent.parent / "fixtures" / "board_yaml_bad"


def test_minimal_happy_path_emits_no_diagnostics():
    collector = validate_board_yaml(FIX_GOOD / "minimal.yaml")
    assert len(collector) == 0
    assert not collector.has_errors()
```

- [ ] **Step 3: Run test to verify it fails**

```bash
pytest tests/scripts/test_board_yaml_diagnostics.py::test_minimal_happy_path_emits_no_diagnostics -v
```
Expected: ImportError on `alp_cli.validator`.

- [ ] **Step 4: Implement validator skeleton**

Create `scripts/alp_cli/validator.py`:

```python
"""board.yaml validator with rich diagnostics.

Runs three passes:
  1. schema_pass     - JSON Schema violations (codes ALP-B001..B004).
  2. xref_pass       - cross-references to SoM / preset / pad metadata
                       (codes ALP-B005..B009).
  3. compat_pass     - peripherals vs. SoC capability table
                       (codes ALP-B010+).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import jsonschema

from alp_cli.diagnostic import Diagnostic, DiagnosticCollector
from alp_cli.yaml_pos import load_with_positions, node_position

REPO = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO / "metadata" / "schemas" / "board.schema.json"


def _load_schema() -> dict[str, Any]:
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


def validate_board_yaml(path: Path) -> DiagnosticCollector:
    """Validate a board.yaml file. Returns a DiagnosticCollector."""
    collector = DiagnosticCollector()
    text = path.read_text(encoding="utf-8")
    try:
        data = load_with_positions(text, source=path)
    except Exception as exc:  # YAML parse error
        collector.add(
            Diagnostic(
                severity="error",
                path=path,
                line=1,
                col=1,
                span=1,
                code="ALP-B000",
                message=f"YAML parse error: {exc}",
                hint=None,
            )
        )
        return collector

    schema = _load_schema()
    _schema_pass(data, schema, path, collector)
    # xref + compat passes added in subsequent tasks.
    return collector


def _schema_pass(
    data: dict[str, Any],
    schema: dict[str, Any],
    path: Path,
    collector: DiagnosticCollector,
) -> None:
    # Strip __pos__ keys before handing to jsonschema (they're not in the schema).
    clean = _strip_pos(data)
    validator = jsonschema.Draft7Validator(schema)
    for err in validator.iter_errors(clean):
        diag = _schema_error_to_diagnostic(err, data, path)
        if diag is not None:
            collector.add(diag)


def _strip_pos(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            k: _strip_pos(v)
            for k, v in value.items()
            if not (isinstance(k, str) and k.startswith("__"))
        }
    if isinstance(value, list):
        return [_strip_pos(v) for v in value]
    return value


def _walk(data: dict[str, Any], path_seq: list[Any]) -> dict[str, Any] | None:
    """Walk a path through the position-augmented document."""
    cursor: Any = data
    for step in path_seq:
        if isinstance(cursor, dict) and step in cursor:
            cursor = cursor[step]
        elif isinstance(cursor, list) and isinstance(step, int) and step < len(cursor):
            cursor = cursor[step]
        else:
            return None
    return cursor if isinstance(cursor, dict) else None


def _schema_error_to_diagnostic(
    err: jsonschema.ValidationError, data: dict[str, Any], path: Path
) -> Diagnostic | None:
    abs_path = list(err.absolute_path)
    parent = _walk(data, abs_path[:-1]) if abs_path else data
    line = parent.get("__line__", 1) if parent else 1
    col = parent.get("__column__", 1) if parent else 1
    span = 1

    if err.validator == "required":
        missing = err.message.split("'")[1] if "'" in err.message else "?"
        return Diagnostic(
            severity="error",
            path=path,
            line=line,
            col=col,
            span=span,
            code="ALP-B001",
            message=f"required key '{missing}' is missing",
            hint=f"add a '{missing}:' entry to this block",
        )

    if err.validator == "additionalProperties":
        bad_key = abs_path[-1] if abs_path else "?"
        if parent and "__keys__" in parent and bad_key in parent["__keys__"]:
            line, col = node_position(parent, bad_key, target="key")
            span = len(str(bad_key))
        allowed = list(err.schema.get("properties", {}).keys())
        from difflib import get_close_matches

        suggestion = get_close_matches(str(bad_key), allowed, n=1)
        hint = f"did you mean '{suggestion[0]}'?" if suggestion else None
        return Diagnostic(
            severity="error",
            path=path,
            line=line,
            col=col,
            span=span,
            code="ALP-B002",
            message=f"unknown key '{bad_key}'",
            hint=hint,
        )

    if err.validator in {"enum", "pattern"}:
        if abs_path and parent and "__keys__" in parent:
            key = abs_path[-1]
            if key in parent["__keys__"]:
                line, col = node_position(parent, key, target="value")
                span = max(1, len(str(parent.get(key, ""))))
        if err.validator == "enum":
            allowed = err.schema.get("enum", [])
            hint = f"expected one of: {', '.join(map(repr, allowed))}"
        else:
            hint = f"value must match pattern: {err.schema.get('pattern')}"
        return Diagnostic(
            severity="error",
            path=path,
            line=line,
            col=col,
            span=span,
            code="ALP-B003",
            message=err.message,
            hint=hint,
        )

    if err.validator == "type":
        if abs_path and parent and "__keys__" in parent:
            key = abs_path[-1]
            if key in parent["__keys__"]:
                line, col = node_position(parent, key, target="value")
        return Diagnostic(
            severity="error",
            path=path,
            line=line,
            col=col,
            span=1,
            code="ALP-B004",
            message=err.message,
            hint=f"expected type: {err.schema.get('type')}",
        )

    # Fallback for any validator we haven't mapped yet.
    return Diagnostic(
        severity="error",
        path=path,
        line=line,
        col=col,
        span=1,
        code="ALP-B099",
        message=err.message,
        hint=None,
    )
```

- [ ] **Step 5: Run the happy-path test**

```bash
pytest tests/scripts/test_board_yaml_diagnostics.py::test_minimal_happy_path_emits_no_diagnostics -v
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add scripts/alp_cli/validator.py tests/fixtures/board_yaml_good/minimal.yaml tests/scripts/test_board_yaml_diagnostics.py
git commit -m "feat(validator): schema-pass scaffolding + happy-path fixture"
```

---

### Task 1.2: Schema-pass fixtures + tests (ALP-B001…B004)

**Files:**
- Create: 4 fixture files under `tests/fixtures/board_yaml_bad/`
- Modify: `tests/scripts/test_board_yaml_diagnostics.py`

- [ ] **Step 1: Write the fixtures**

Create `tests/fixtures/board_yaml_bad/ALP-B001-missing-required.yaml`:

```yaml
# 'som' is required at the top level.
preset: e1m-evk
cores:
  m55_hp:
    app: ./src
    peripherals: []
```

Create `tests/fixtures/board_yaml_bad/ALP-B002-unknown-key.yaml`:

```yaml
som:
  sku: E1M-AEN701
preset: e1m-evk
cores:
  m55_hp:
    app: ./src
    peripherals: []
diagostics:           # typo: should be 'diagnostics'
  log_level: info
```

Create `tests/fixtures/board_yaml_bad/ALP-B003-bad-enum.yaml`:

```yaml
som:
  sku: E1M-AEN701
preset: e1m-evk
cores:
  m55_hp:
    app: ./src
    peripherals: []
diagnostics:
  log_level: verbose   # not in enum: [debug, info, warn, error]
```

Create `tests/fixtures/board_yaml_bad/ALP-B004-wrong-type.yaml`:

```yaml
som:
  sku: E1M-AEN701
preset: e1m-evk
cores:
  m55_hp:
    app: ./src
    peripherals: "should be a list"
diagnostics:
  log_level: info
```

- [ ] **Step 2: Add the failing tests**

Append to `tests/scripts/test_board_yaml_diagnostics.py`:

```python
def _codes(collector) -> list[str]:
    return [d.code for d in collector]


def test_missing_required_key_emits_ALP_B001():
    c = validate_board_yaml(FIX_BAD / "ALP-B001-missing-required.yaml")
    assert "ALP-B001" in _codes(c)


def test_unknown_key_emits_ALP_B002_with_didyoumean():
    c = validate_board_yaml(FIX_BAD / "ALP-B002-unknown-key.yaml")
    diags = [d for d in c if d.code == "ALP-B002"]
    assert diags, "ALP-B002 expected"
    assert "diagnostics" in (diags[0].hint or "")


def test_bad_enum_emits_ALP_B003():
    c = validate_board_yaml(FIX_BAD / "ALP-B003-bad-enum.yaml")
    diags = [d for d in c if d.code == "ALP-B003"]
    assert diags, "ALP-B003 expected"
    assert "enum" in (diags[0].hint or "") or "one of" in (diags[0].hint or "")


def test_wrong_type_emits_ALP_B004():
    c = validate_board_yaml(FIX_BAD / "ALP-B004-wrong-type.yaml")
    assert "ALP-B004" in _codes(c)
```

- [ ] **Step 3: Run tests, confirm pass**

```bash
pytest tests/scripts/test_board_yaml_diagnostics.py -v
```
Expected: all four new tests PASS (schema-pass logic was already written in 1.1).

- [ ] **Step 4: Adjust validator if the schema rejects fixtures for a different reason than expected**

Inspect the actual `board.schema.json` for `additionalProperties` and `enum` declarations. If the schema doesn't currently use `additionalProperties: false`, add it to the affected sections — the diagnostic infrastructure only fires on the corresponding error. (Schema changes ride along with the fixture commit.) Re-run tests.

- [ ] **Step 5: Commit**

```bash
git add tests/fixtures/board_yaml_bad/ tests/scripts/test_board_yaml_diagnostics.py metadata/schemas/board.schema.json
git commit -m "feat(validator): ALP-B001..B004 schema diagnostics + fixtures"
```

---

### Task 1.3: Cross-reference pass (ALP-B005, ALP-B006)

**Files:**
- Modify: `scripts/alp_cli/validator.py`
- Create: 2 fixtures
- Modify: `tests/scripts/test_board_yaml_diagnostics.py`

- [ ] **Step 1: Write the fixtures**

Create `tests/fixtures/board_yaml_bad/ALP-B005-bad-sku.yaml`:

```yaml
som:
  sku: E1M-AEN999   # not a real SKU
preset: e1m-evk
cores:
  m55_hp:
    app: ./src
    peripherals: []
```

Create `tests/fixtures/board_yaml_bad/ALP-B006-bad-preset.yaml`:

```yaml
som:
  sku: E1M-AEN701
preset: nope-doesnt-exist
cores:
  m55_hp:
    app: ./src
    peripherals: []
```

- [ ] **Step 2: Write the failing tests**

Append to `tests/scripts/test_board_yaml_diagnostics.py`:

```python
def test_bad_sku_emits_ALP_B005():
    c = validate_board_yaml(FIX_BAD / "ALP-B005-bad-sku.yaml")
    assert "ALP-B005" in _codes(c)


def test_bad_preset_emits_ALP_B006():
    c = validate_board_yaml(FIX_BAD / "ALP-B006-bad-preset.yaml")
    assert "ALP-B006" in _codes(c)
```

Run:
```bash
pytest tests/scripts/test_board_yaml_diagnostics.py -v
```
Expected: two new tests FAIL (xref pass not implemented yet).

- [ ] **Step 3: Add the xref pass to `validator.py`**

In `scripts/alp_cli/validator.py`, add after `_schema_pass`:

```python
METADATA = REPO / "metadata"
SOM_DIR = METADATA / "e1m_modules"
PRESET_DIR = METADATA / "boards"


def _xref_pass(
    data: dict[str, Any], path: Path, collector: DiagnosticCollector
) -> None:
    som = data.get("som") or {}
    sku = som.get("sku")
    if isinstance(sku, str):
        if not _sku_resolves(sku):
            line, col = node_position(som, "sku", target="value")
            collector.add(
                Diagnostic(
                    severity="error",
                    path=path,
                    line=line,
                    col=col,
                    span=len(sku),
                    code="ALP-B005",
                    message=f"SoM SKU '{sku}' does not resolve to a known module",
                    hint=_sku_suggestion(sku),
                )
            )

    preset = data.get("preset")
    if isinstance(preset, str):
        if not (PRESET_DIR / f"{preset}.yaml").is_file():
            line, col = node_position(data, "preset", target="value")
            collector.add(
                Diagnostic(
                    severity="error",
                    path=path,
                    line=line,
                    col=col,
                    span=len(preset),
                    code="ALP-B006",
                    message=f"board preset '{preset}' does not exist",
                    hint=_preset_suggestion(preset),
                )
            )


def _sku_resolves(sku: str) -> bool:
    for candidate in SOM_DIR.rglob(f"{sku}.yaml"):
        return True
    return False


def _all_skus() -> list[str]:
    return sorted(p.stem for p in SOM_DIR.rglob("*.yaml") if p.stem.startswith("E1M-"))


def _all_presets() -> list[str]:
    return sorted(p.stem for p in PRESET_DIR.glob("*.yaml"))


def _sku_suggestion(sku: str) -> str | None:
    from difflib import get_close_matches

    match = get_close_matches(sku, _all_skus(), n=1)
    return f"did you mean '{match[0]}'?" if match else None


def _preset_suggestion(preset: str) -> str | None:
    from difflib import get_close_matches

    match = get_close_matches(preset, _all_presets(), n=1)
    return f"did you mean '{match[0]}'?" if match else None
```

And wire it into `validate_board_yaml`:

```python
    _schema_pass(data, schema, path, collector)
    _xref_pass(data, path, collector)
    # compat pass added in 1.4.
    return collector
```

- [ ] **Step 4: Re-run tests**

```bash
pytest tests/scripts/test_board_yaml_diagnostics.py -v
```
Expected: ALP-B005 and ALP-B006 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/validator.py tests/fixtures/board_yaml_bad/ALP-B005-bad-sku.yaml tests/fixtures/board_yaml_bad/ALP-B006-bad-preset.yaml tests/scripts/test_board_yaml_diagnostics.py
git commit -m "feat(validator): ALP-B005/B006 cross-reference diagnostics"
```

---

### Task 1.4: Compatibility pass (ALP-B010)

**Files:**
- Modify: `scripts/alp_cli/validator.py`
- Create: `tests/fixtures/board_yaml_bad/ALP-B010-peripheral-not-on-soc.yaml`
- Modify: `tests/scripts/test_board_yaml_diagnostics.py`

- [ ] **Step 1: Pick a SoC that lacks a specific peripheral**

Open one of the SoC JSON files under `metadata/socs/` and find a peripheral whose count is 0 for some SoM. Example: a SoC that reports `can: 0` in `peripherals`. Pick a SoM whose `silicon` ref points at that SoC.

- [ ] **Step 2: Write the fixture**

Create `tests/fixtures/board_yaml_bad/ALP-B010-peripheral-not-on-soc.yaml` using the SoM identified in step 1 — e.g.:

```yaml
som:
  sku: E1M-NX9301   # i.MX 93 — has no on-die CAN (verify against metadata/socs/nxp/imx9/imx93.json before committing)
preset: e1m-evk
cores:
  cm33_app:
    app: ./src
    peripherals:
      - id: can0
        kind: can
```

Engineer note: if the chosen SKU does NOT actually lack CAN, swap the peripheral `kind:` to one its SoC truly lacks. The point of this fixture is to trigger `ALP-B010`.

- [ ] **Step 3: Write the failing test**

Append to `tests/scripts/test_board_yaml_diagnostics.py`:

```python
def test_peripheral_not_on_soc_emits_ALP_B010():
    c = validate_board_yaml(FIX_BAD / "ALP-B010-peripheral-not-on-soc.yaml")
    assert "ALP-B010" in _codes(c)
```

Run and confirm FAIL.

- [ ] **Step 4: Implement the compatibility pass**

Add to `scripts/alp_cli/validator.py`:

```python
SOC_DIR = METADATA / "socs"


def _compat_pass(
    data: dict[str, Any], path: Path, collector: DiagnosticCollector
) -> None:
    silicon_ref = _silicon_ref_for_sku(data.get("som", {}).get("sku"))
    if silicon_ref is None:
        return
    soc_caps = _load_soc_caps(silicon_ref)
    if soc_caps is None:
        return

    for core_name, core in (data.get("cores") or {}).items():
        if not isinstance(core, dict):
            continue
        peripherals = core.get("peripherals") or []
        if not isinstance(peripherals, list):
            continue
        for idx, periph in enumerate(peripherals):
            if not isinstance(periph, dict):
                continue
            kind = periph.get("kind")
            if not isinstance(kind, str):
                continue
            if _soc_has_kind(soc_caps, kind):
                continue
            line = periph.get("__line__", 1)
            col = periph.get("__column__", 1)
            collector.add(
                Diagnostic(
                    severity="error",
                    path=path,
                    line=line,
                    col=col,
                    span=len(kind),
                    code="ALP-B010",
                    message=(
                        f"core '{core_name}': peripheral kind '{kind}' is not "
                        f"available on silicon '{silicon_ref}'"
                    ),
                    hint=(
                        f"remove this peripheral or switch som.sku to a part "
                        f"with {kind} support"
                    ),
                )
            )


def _silicon_ref_for_sku(sku: str | None) -> str | None:
    if not sku:
        return None
    for path in SOM_DIR.rglob(f"{sku}.yaml"):
        import yaml as _yaml

        text = path.read_text(encoding="utf-8")
        doc = _yaml.safe_load(text) or {}
        ref = doc.get("silicon")
        if isinstance(ref, str):
            return ref
    return None


def _load_soc_caps(silicon_ref: str) -> dict[str, int] | None:
    vendor, family, part = silicon_ref.split(":")
    fp = SOC_DIR / vendor / family / f"{part}.json"
    if not fp.is_file():
        return None
    doc = json.loads(fp.read_text(encoding="utf-8"))
    peripherals = doc.get("peripherals", {}) if isinstance(doc, dict) else {}
    return peripherals if isinstance(peripherals, dict) else {}


def _soc_has_kind(caps: dict[str, int], kind: str) -> bool:
    """Map a peripheral 'kind' onto SoC capability keys.

    'kind' is the user-facing peripheral category (i2c, spi, can, ...).
    The SoC JSON uses the same names plus _lp suffixes for low-power
    variants; presence in EITHER counts.
    """
    keys = (kind, f"{kind}_lp")
    return any((caps.get(k, 0) or 0) > 0 for k in keys)
```

Wire into `validate_board_yaml`:

```python
    _schema_pass(data, schema, path, collector)
    _xref_pass(data, path, collector)
    _compat_pass(data, path, collector)
    return collector
```

- [ ] **Step 5: Run the test**

```bash
pytest tests/scripts/test_board_yaml_diagnostics.py::test_peripheral_not_on_soc_emits_ALP_B010 -v
```
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add scripts/alp_cli/validator.py tests/fixtures/board_yaml_bad/ALP-B010-peripheral-not-on-soc.yaml tests/scripts/test_board_yaml_diagnostics.py
git commit -m "feat(validator): ALP-B010 SoC-compatibility diagnostics"
```

---

### Task 1.5: Wire validator into `alp_project.py` + emit diagnostics on CLI

**Files:**
- Modify: `scripts/alp_project.py`
- Modify: `tests/scripts/test_alp_orchestrate.py` (or add new test if absent)

- [ ] **Step 1: Write a failing test**

Add a test that asserts `alp_project.py` exits non-zero on a known-bad fixture and prints the expected code. Create `tests/scripts/test_alp_project_diagnostics.py`:

```python
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "alp_project.py"
FIX_BAD = REPO / "tests" / "fixtures" / "board_yaml_bad"


def test_alp_project_exits_nonzero_on_bad_yaml():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--input",
         str(FIX_BAD / "ALP-B001-missing-required.yaml"),
         "--emit", "zephyr-conf"],
        capture_output=True, text=True,
    )
    assert proc.returncode != 0
    assert "ALP-B001" in proc.stderr or "ALP-B001" in proc.stdout
```

Run and confirm FAIL.

- [ ] **Step 2: Rewire `alp_project.py`**

In `scripts/alp_project.py`, find the existing `_load_yaml` + jsonschema validation block. Replace with a call into `alp_cli.validator.validate_board_yaml`. Before emitting any config, if `collector.has_errors()`, call `collector.emit(source_text)` to stderr and `sys.exit(1)`.

A minimal patch sketch (the engineer adapts to the file's actual structure):

```python
from alp_cli.validator import validate_board_yaml


def _validate_and_load(path: Path) -> dict:
    collector = validate_board_yaml(path)
    source_text = path.read_text(encoding="utf-8")
    if collector.has_errors():
        import sys as _sys
        for diag in collector:
            from alp_cli.diagnostic import render
            print(render(diag, source_text=source_text), file=_sys.stderr)
        _sys.exit(1)
    return _strip_pos(_load_yaml(path))
```

- [ ] **Step 3: Run the integration test**

```bash
pytest tests/scripts/test_alp_project_diagnostics.py -v
```
Expected: PASS.

- [ ] **Step 4: Smoke test the happy path**

```bash
python scripts/alp_project.py --input examples/peripheral-io/hello-world/board.yaml --emit zephyr-conf
```
Expected: emits the Kconfig fragment to stdout, exit code 0 (no diagnostics).

- [ ] **Step 5: Stub doc pages**

Create empty stubs under `docs/diagnostics/` so the doc URLs resolve in-tree. For each code (`ALP-B001`, `ALP-B002`, `ALP-B003`, `ALP-B004`, `ALP-B005`, `ALP-B006`, `ALP-B010`), write a one-line file:

```bash
for code in ALP-B001 ALP-B002 ALP-B003 ALP-B004 ALP-B005 ALP-B006 ALP-B010; do
  cat > docs/diagnostics/${code}.md <<EOF
# ${code}

(Diagnostic landing page — narrative to be added.)
EOF
done
```

(PowerShell equivalent: use `Set-Content` per file.)

- [ ] **Step 6: Commit**

```bash
git add scripts/alp_project.py tests/scripts/test_alp_project_diagnostics.py docs/diagnostics/
git commit -m "feat(validator): wire rich diagnostics into alp_project.py + doc stubs"
```

---

# Phase 2 — Capability API (independent of Phase 1; parallel-dispatch viable)

### Task 2.1: Extend `gen_soc_caps.py` to emit `ALP_HAS()` macro layer

**Files:**
- Modify: `scripts/gen_soc_caps.py`
- Create: `tests/scripts/test_gen_soc_caps_cap_layer.py`

- [ ] **Step 1: Write the failing test**

Create `tests/scripts/test_gen_soc_caps_cap_layer.py`:

```python
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_soc_caps.py"
HEADER = REPO / "include" / "alp" / "soc_caps.h"


def test_header_contains_alp_has_macro_and_cap_definitions():
    subprocess.run([sys.executable, str(SCRIPT)], check=True)
    text = HEADER.read_text(encoding="utf-8")
    assert "#define ALP_HAS(cap)" in text
    # Every ALP_SOC_*_COUNT field should have a matching ALP_CAP_HW_* alias.
    assert "#define ALP_CAP_HW_I2C" in text
    assert "#define ALP_CAP_HW_SPI" in text
    assert "#define ALP_CAP_NPU_DRPAI" in text
    assert "#define ALP_CAP_HELIUM_MVE" in text
```

Run and confirm FAIL.

- [ ] **Step 2: Add the capability map to `gen_soc_caps.py`**

Open `scripts/gen_soc_caps.py`. After the `CAPS` table, add the soc-cap-to-capability map. Naming convention: `_COUNT` fields → `ALP_CAP_HW_<NAME>` defined as `(ALP_SOC_<NAME>_COUNT > 0)`; boolean fields → `ALP_CAP_<NAME>` defined as `(ALP_SOC_<NAME>)`. Add near the bottom of the existing emission function, after the last `#elif`/`#endif`:

```python
# Map ALP_SOC_* field name -> ALP_CAP_* name.
# Count-style fields produce HW_<NAME> (presence boolean from count > 0).
# Boolean / flag fields keep their name verbatim.
CAP_ALIASES: list[tuple[str, str]] = [
    # (soc_macro_name, cap_macro_name, kind: "count" | "bool")
    ("I2C_COUNT", "HW_I2C", "count"),
    ("SPI_COUNT", "HW_SPI", "count"),
    ("UART_COUNT", "HW_UART", "count"),
    ("I2S_COUNT", "HW_I2S", "count"),
    ("PDM_COUNT", "HW_PDM", "count"),
    ("ADC_COUNT", "HW_ADC", "count"),
    ("DAC_COUNT", "HW_DAC", "count"),
    ("CAN_COUNT", "HW_CAN", "count"),
    ("CAN_FD_SUPPORTED", "HW_CAN_FD", "bool"),
    ("RTC_COUNT", "HW_RTC", "count"),
    ("WDT_COUNT", "HW_WDT", "count"),
    ("QENC_COUNT", "HW_QENC", "count"),
    ("TIMER_COUNT", "HW_TIMER", "count"),
    ("PWM_COUNT", "HW_PWM", "count"),
    ("ETHERNET_COUNT", "HW_ETHERNET", "count"),
    ("USB_COUNT", "HW_USB", "count"),
    ("MIPI_CSI_COUNT", "HW_MIPI_CSI", "count"),
    ("MIPI_DSI_COUNT", "HW_MIPI_DSI", "count"),
    ("XSPI_DMA", "XSPI_DMA", "bool"),
    ("HEXSPI_DMA", "HEXSPI_DMA", "bool"),
    ("EMMC_DMA", "EMMC_DMA", "bool"),
    ("QUADSPI_DMA", "QUADSPI_DMA", "bool"),
    ("DRP_AI", "NPU_DRPAI", "bool"),
    ("HELIUM_MVE", "HELIUM_MVE", "bool"),
    ("NEON", "NEON", "bool"),
    ("GPU2D", "GPU2D", "bool"),
    ("DAVE2D", "DAVE2D", "bool"),
    ("CRYPTOCELL", "CRYPTOCELL", "bool"),
    ("INLINE_AES", "INLINE_AES", "bool"),
    ("CAU", "CAU", "bool"),
    ("DMA2D", "DMA2D", "bool"),
]


def _emit_cap_layer(out: list[str]) -> None:
    out.append("")
    out.append("/* ---------------------------------------------------------------")
    out.append(" * Capability layer -- portable, SoM-agnostic.  Derived from the")
    out.append(" * active CONFIG_ALP_SOC_* selection via the macros above.")
    out.append(" *")
    out.append(" * Counts collapse to 0/1 via `> 0`, so ALP_HAS() is always a")
    out.append(" * constant expression and safe inside #if and static_assert.")
    out.append(" * --------------------------------------------------------------- */")
    for soc_name, cap_name, kind in CAP_ALIASES:
        if kind == "count":
            out.append(f"#define ALP_CAP_{cap_name} (ALP_SOC_{soc_name} > 0)")
        else:
            out.append(f"#define ALP_CAP_{cap_name} (ALP_SOC_{soc_name})")
    out.append("")
    out.append("#define ALP_HAS(cap) (ALP_CAP_##cap)")
```

Then in the existing main emission function, insert `_emit_cap_layer(out)` just before the closing `#endif` of the header guard.

- [ ] **Step 3: Run the generator + tests**

```bash
python scripts/gen_soc_caps.py
pytest tests/scripts/test_gen_soc_caps_cap_layer.py -v
```
Expected: regenerated `soc_caps.h` contains all expected `ALP_CAP_*` lines; test PASSES.

- [ ] **Step 4: Sanity-check the regenerated header compiles**

```bash
gcc -E -DCONFIG_ALP_SOC_ALIF_ENSEMBLE_E7 -x c - <<'EOF' >/dev/null
#include "include/alp/soc_caps.h"
#if ALP_HAS(HW_I2C)
int has_i2c = 1;
#endif
EOF
```
Expected: exit code 0 (no preprocessing error).

- [ ] **Step 5: Commit**

```bash
git add scripts/gen_soc_caps.py include/alp/soc_caps.h tests/scripts/test_gen_soc_caps_cap_layer.py
git commit -m "feat(soc-caps): emit ALP_HAS() macro layer over existing ALP_SOC_* set"
```

---

### Task 2.2: Generate `include/alp/cap.h` and `src/cap.c`

**Files:**
- Modify: `scripts/gen_soc_caps.py`
- Create (regenerated): `include/alp/cap.h`
- Create (regenerated): `src/cap.c`

- [ ] **Step 1: Write the failing test**

Append to `tests/scripts/test_gen_soc_caps_cap_layer.py`:

```python
CAP_H = REPO / "include" / "alp" / "cap.h"
CAP_C = REPO / "src" / "cap.c"


def test_cap_h_emits_enum_and_function_prototypes():
    subprocess.run([sys.executable, str(SCRIPT)], check=True)
    text = CAP_H.read_text(encoding="utf-8")
    assert "typedef enum" in text
    assert "ALP_CAP_ID_HW_I2C" in text
    assert "ALP_CAP_ID_COUNT" in text
    assert "bool alp_has(alp_cap_id_t cap);" in text
    assert "const char *alp_cap_name(alp_cap_id_t cap);" in text


def test_cap_c_emits_table():
    subprocess.run([sys.executable, str(SCRIPT)], check=True)
    text = CAP_C.read_text(encoding="utf-8")
    assert "static const bool _cap_table" in text
    assert "alp_has" in text
    assert "alp_cap_name" in text
```

Run and confirm FAIL.

- [ ] **Step 2: Implement emission**

Add to `scripts/gen_soc_caps.py`:

```python
CAP_H_OUT = REPO / "include" / "alp" / "cap.h"
CAP_C_OUT = REPO / "src" / "cap.c"


def _emit_cap_h() -> str:
    lines: list[str] = [
        "/**",
        " * @file cap.h",
        " * @brief Portable hardware-capability API (auto-generated).",
        " *",
        " * Auto-generated by scripts/gen_soc_caps.py from",
        " * metadata/socs/**/*.json. DO NOT EDIT BY HAND -- regenerate.",
        " *",
        " * Copyright 2026 Alp Lab AB",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        " * @par ABI status: [ABI-STABLE]",
        " *      v0.7 generated; capability identifiers + lookup.",
        " *      See docs/abi-markers.md for the convention.",
        " */",
        "",
        "#ifndef ALP_CAP_H",
        "#define ALP_CAP_H",
        "",
        "#include <stdbool.h>",
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        "typedef enum {",
    ]
    for _soc, cap_name, _kind in CAP_ALIASES:
        lines.append(f"    ALP_CAP_ID_{cap_name},")
    lines.append("    ALP_CAP_ID_COUNT")
    lines.append("} alp_cap_id_t;")
    lines.append("")
    lines.append("/**")
    lines.append(" * @brief Test whether the active SoC offers a hardware capability.")
    lines.append(" * @param cap  Capability id from @ref alp_cap_id_t.")
    lines.append(" * @return true if the capability is present, false otherwise.")
    lines.append(" */")
    lines.append("bool alp_has(alp_cap_id_t cap);")
    lines.append("")
    lines.append("/**")
    lines.append(" * @brief Return the symbolic name of a capability (e.g. \"HW_I2C\").")
    lines.append(" * @param cap  Capability id; out-of-range returns NULL.")
    lines.append(" * @return Pointer to a static string, or NULL.")
    lines.append(" */")
    lines.append("const char *alp_cap_name(alp_cap_id_t cap);")
    lines.append("")
    lines.append("#ifdef __cplusplus")
    lines.append("}")
    lines.append("#endif")
    lines.append("")
    lines.append("#endif /* ALP_CAP_H */")
    return "\n".join(lines) + "\n"


def _emit_cap_c() -> str:
    lines = [
        "/*",
        " * SPDX-License-Identifier: Apache-2.0",
        " * Auto-generated by scripts/gen_soc_caps.py.  DO NOT EDIT.",
        " */",
        "",
        "#include <stddef.h>",
        "#include <alp/cap.h>",
        "#include <alp/soc_caps.h>",
        "",
        "static const bool _cap_table[ALP_CAP_ID_COUNT] = {",
    ]
    for _soc, cap_name, _kind in CAP_ALIASES:
        lines.append(f"    [ALP_CAP_ID_{cap_name}] = ALP_CAP_{cap_name},")
    lines.append("};")
    lines.append("")
    lines.append("static const char *const _cap_names[ALP_CAP_ID_COUNT] = {")
    for _soc, cap_name, _kind in CAP_ALIASES:
        lines.append(f"    [ALP_CAP_ID_{cap_name}] = \"{cap_name}\",")
    lines.append("};")
    lines.append("")
    lines.append("bool alp_has(alp_cap_id_t cap) {")
    lines.append("    if ((unsigned)cap >= (unsigned)ALP_CAP_ID_COUNT) {")
    lines.append("        return false;")
    lines.append("    }")
    lines.append("    return _cap_table[cap];")
    lines.append("}")
    lines.append("")
    lines.append("const char *alp_cap_name(alp_cap_id_t cap) {")
    lines.append("    if ((unsigned)cap >= (unsigned)ALP_CAP_ID_COUNT) {")
    lines.append("        return NULL;")
    lines.append("    }")
    lines.append("    return _cap_names[cap];")
    lines.append("}")
    return "\n".join(lines) + "\n"
```

In the generator's main entry point, write the two files after writing `soc_caps.h`:

```python
CAP_H_OUT.write_text(_emit_cap_h(), encoding="utf-8")
CAP_C_OUT.write_text(_emit_cap_c(), encoding="utf-8")
```

- [ ] **Step 3: Regenerate + run tests**

```bash
python scripts/gen_soc_caps.py
pytest tests/scripts/test_gen_soc_caps_cap_layer.py -v
```
Expected: PASS.

- [ ] **Step 4: Add `src/cap.c` to the Zephyr build**

Open `src/zephyr/CMakeLists.txt` (or wherever the existing module sources are listed) and append:

```cmake
zephyr_library_sources(${CMAKE_SOURCE_DIR}/../../../src/cap.c)
```

If a sources list already exists in the module, add `src/cap.c` to it in the appropriate way (mirror the existing pattern). Engineer note: don't double-add; check first with `grep -n cap.c`.

- [ ] **Step 5: Commit**

```bash
git add scripts/gen_soc_caps.py include/alp/cap.h src/cap.c src/zephyr/CMakeLists.txt tests/scripts/test_gen_soc_caps_cap_layer.py
git commit -m "feat(cap): generate include/alp/cap.h + src/cap.c runtime layer"
```

---

### Task 2.3: Ztest unit test for `cap.h` / `cap.c`

**Files:**
- Create: `tests/unit/cap/CMakeLists.txt`
- Create: `tests/unit/cap/prj.conf`
- Create: `tests/unit/cap/testcase.yaml`
- Create: `tests/unit/cap/src/test_cap.c`

- [ ] **Step 1: Create the test harness**

Create `tests/unit/cap/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_cap)

target_sources(app PRIVATE
    src/test_cap.c
    ${CMAKE_SOURCE_DIR}/../../../src/cap.c
)

target_include_directories(app PRIVATE
    ${CMAKE_SOURCE_DIR}/../../../include
)
```

Create `tests/unit/cap/prj.conf`:

```
CONFIG_ZTEST=y
CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y
```

Create `tests/unit/cap/testcase.yaml`:

```yaml
tests:
  alp.cap.runtime:
    platform_allow: native_sim
    integration_platforms:
      - native_sim
    tags: alp cap unit
```

- [ ] **Step 2: Write the failing test**

Create `tests/unit/cap/src/test_cap.c`:

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ztest unit-test for alp_has() / alp_cap_name().  Runs on native_sim
 * under twister; selects ALIF_ENSEMBLE_E7 so we have a SoC whose
 * capability set is non-trivial.
 */

#include <stddef.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/cap.h>
#include <alp/soc_caps.h>

ZTEST_SUITE(alp_cap, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_cap, runtime_matches_compile_time_for_each_id) {
    /* Every ALP_CAP_ID_* must agree with the ALP_HAS() macro of the
     * same name. */
    zassert_equal((bool)ALP_HAS(HW_I2C), alp_has(ALP_CAP_ID_HW_I2C));
    zassert_equal((bool)ALP_HAS(HW_SPI), alp_has(ALP_CAP_ID_HW_SPI));
    zassert_equal((bool)ALP_HAS(NPU_DRPAI), alp_has(ALP_CAP_ID_NPU_DRPAI));
    zassert_equal((bool)ALP_HAS(HELIUM_MVE), alp_has(ALP_CAP_ID_HELIUM_MVE));
}

ZTEST(alp_cap, name_returns_expected_string) {
    zassert_str_equal(alp_cap_name(ALP_CAP_ID_HW_I2C), "HW_I2C");
    zassert_str_equal(alp_cap_name(ALP_CAP_ID_NPU_DRPAI), "NPU_DRPAI");
}

ZTEST(alp_cap, out_of_bounds_id_returns_safe_defaults) {
    zassert_false(alp_has(ALP_CAP_ID_COUNT));
    zassert_is_null(alp_cap_name(ALP_CAP_ID_COUNT));
}
```

- [ ] **Step 3: Run twister**

```bash
west twister -T tests/unit/cap -p native_sim --inline-logs
```
Expected: 3 testcases PASS.

If twister isn't on `PATH`, source the SDK env first:
```bash
source zephyr/zephyr-env.sh    # or equivalent
```

- [ ] **Step 4: Commit**

```bash
git add tests/unit/cap/
git commit -m "test(cap): Ztest unit test for alp_has + alp_cap_name"
```

---

### Task 2.4: Update hello-world example with capability teaching block

**Files:**
- Modify: `examples/peripheral-io/hello-world/src/main.c`

- [ ] **Step 1: Add the teaching block**

Open `examples/peripheral-io/hello-world/src/main.c`. Above the existing `printf("[hello] done\n");` line, insert:

```c
    /* Capability-API teaching block.
     *
     * `ALP_HAS()` is a compile-time constant expression.  Use it for
     * #if / static_assert -- the unused branch disappears entirely
     * from the binary, so this is zero-cost on parts that lack the
     * feature. */
#if ALP_HAS(HELIUM_MVE)
    printf("[hello] this build targets a Helium-capable SoC\n");
#else
    printf("[hello] no Helium MVE on this SoC -- scalar path\n");
#endif

    /* `alp_has()` is the runtime equivalent.  Useful when the same
     * binary may run on different SoCs (rare on Zephyr, common in
     * board-bringup tooling) or when the branch only matters for
     * logging rather than codegen. */
    if (alp_has(ALP_CAP_ID_HW_I2C)) {
        printf("[hello] HW I2C available (could probe sensors here)\n");
    } else {
        printf("[hello] no HW I2C on this SoC\n");
    }
```

At the top of the file, add the includes (near the existing `#include <zephyr/kernel.h>`):

```c
#include <alp/cap.h>
#include <alp/soc_caps.h>
```

- [ ] **Step 2: Build + twister-run the example to confirm**

```bash
west twister -T examples/peripheral-io/hello-world -p native_sim --inline-logs
```
Expected: PASS, console contains the two new lines.

- [ ] **Step 3: Commit**

```bash
git add examples/peripheral-io/hello-world/src/main.c
git commit -m "examples(hello-world): teach ALP_HAS + alp_has via printable branches"
```

---

# Phase 3 — `alp` CLI subcommands (depends on Phase 1)

### Task 3.1: `alp validate`

**Files:**
- Create: `scripts/alp_cli/validate.py`
- Modify: `scripts/alp_cli/main.py` (replace `validate` stub)
- Modify: `tests/scripts/test_alp_cli.py`

- [ ] **Step 1: Write the failing test**

Add to `tests/scripts/test_alp_cli.py`:

```python
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def test_validate_passes_on_good_fixture():
    good = REPO / "tests" / "fixtures" / "board_yaml_good" / "minimal.yaml"
    result = CliRunner().invoke(cli, ["validate", str(good)])
    assert result.exit_code == 0


def test_validate_fails_on_bad_fixture_and_prints_code():
    bad = REPO / "tests" / "fixtures" / "board_yaml_bad" / "ALP-B001-missing-required.yaml"
    result = CliRunner().invoke(cli, ["validate", str(bad)])
    assert result.exit_code != 0
    assert "ALP-B001" in result.output
```

- [ ] **Step 2: Implement the subcommand**

Create `scripts/alp_cli/validate.py`:

```python
"""`alp validate` -- run the board.yaml validator on a file."""

from __future__ import annotations

from pathlib import Path

import click

from alp_cli.diagnostic import render
from alp_cli.validator import validate_board_yaml


@click.command(name="validate", help="Validate a board.yaml file.")
@click.argument("path", type=click.Path(exists=True, dir_okay=False, path_type=Path),
                required=False, default=Path("board.yaml"))
@click.option("--no-color", is_flag=True, help="Disable ANSI colours.")
def validate_cmd(path: Path, no_color: bool) -> None:
    collector = validate_board_yaml(path)
    source_text = path.read_text(encoding="utf-8")
    for diag in collector:
        click.echo(render(diag, source_text=source_text, color=not no_color))
    if collector.has_errors():
        raise SystemExit(1)
```

In `scripts/alp_cli/main.py`, replace the `validate` stub with:

```python
from alp_cli.validate import validate_cmd

cli.add_command(validate_cmd)
```

(Also remove the `@cli.command def validate` stub.)

- [ ] **Step 3: Run tests**

```bash
pytest tests/scripts/test_alp_cli.py -v
```
Expected: both new tests PASS.

- [ ] **Step 4: Commit**

```bash
git add scripts/alp_cli/validate.py scripts/alp_cli/main.py tests/scripts/test_alp_cli.py
git commit -m "feat(cli): alp validate subcommand"
```

---

### Task 3.2: `alp init` wizard

**Files:**
- Create: `scripts/alp_cli/init.py`
- Modify: `scripts/alp_cli/main.py`
- Modify: `tests/scripts/test_alp_cli.py`

- [ ] **Step 1: Write the failing test (non-interactive path)**

Add to `tests/scripts/test_alp_cli.py`:

```python
def test_init_non_interactive_scaffolds_project(tmp_path: Path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    result = CliRunner().invoke(
        cli,
        ["init", "my-app", "--som", "E1M-AEN701", "--preset", "e1m-evk",
         "--peripherals", "uart,gpio"],
    )
    assert result.exit_code == 0, result.output
    proj = tmp_path / "my-app"
    assert (proj / "board.yaml").is_file()
    assert (proj / "src" / "main.c").is_file()
    assert (proj / "CMakeLists.txt").is_file()
    board_yaml = (proj / "board.yaml").read_text(encoding="utf-8")
    assert "E1M-AEN701" in board_yaml
    assert "e1m-evk" in board_yaml


def test_init_refuses_existing_directory(tmp_path: Path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    (tmp_path / "already-there").mkdir()
    result = CliRunner().invoke(
        cli,
        ["init", "already-there", "--som", "E1M-AEN701", "--preset", "e1m-evk"],
    )
    assert result.exit_code != 0
    assert "already exists" in result.output
```

- [ ] **Step 2: Implement the subcommand**

Create `scripts/alp_cli/init.py`:

```python
"""`alp init` -- scaffold a new project from a template."""

from __future__ import annotations

import shutil
from pathlib import Path

import click
import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
TEMPLATE_DIR = REPO_ROOT / "examples" / "hello-world"
SOM_DIR = REPO_ROOT / "metadata" / "e1m_modules"
PRESET_DIR = REPO_ROOT / "metadata" / "boards"
STARTER_PERIPHERALS = ("uart", "gpio", "i2c", "spi", "pwm")


def _list_skus() -> list[str]:
    return sorted(
        p.stem for p in SOM_DIR.rglob("*.yaml") if p.stem.startswith("E1M-")
    )


def _list_presets() -> list[str]:
    return sorted(p.stem for p in PRESET_DIR.glob("*.yaml"))


def _interactive(som: str | None, preset: str | None,
                 peripherals: tuple[str, ...] | None) -> tuple[str, str, list[str]]:
    import questionary

    if som is None:
        som = questionary.select(
            "Pick a SoM SKU:", choices=_list_skus()
        ).unsafe_ask()
    if preset is None:
        preset = questionary.select(
            "Pick a board preset:", choices=_list_presets()
        ).unsafe_ask()
    if peripherals is None:
        choices = list(STARTER_PERIPHERALS)
        peripherals = tuple(
            questionary.checkbox(
                "Starter peripherals (optional, space to toggle):",
                choices=choices,
            ).unsafe_ask()
            or []
        )
    return som, preset, list(peripherals)


def _scaffold(dest: Path, som: str, preset: str, peripherals: list[str]) -> None:
    shutil.copytree(TEMPLATE_DIR, dest)
    board_yaml = dest / "board.yaml"
    text = board_yaml.read_text(encoding="utf-8")
    text = text.replace("E1M-AEN701", som)
    text = text.replace("preset: e1m-evk", f"preset: {preset}")
    if peripherals:
        # Replace empty peripherals: [] with a stub list.
        stub = "\n".join(f"      - id: {p}0\n        kind: {p}" for p in peripherals)
        text = text.replace("    peripherals: []", "    peripherals:\n" + stub)
    board_yaml.write_text(text, encoding="utf-8")
    (dest / "README.md").write_text(
        f"# {dest.name}\n\nGenerated by `alp init`. See docs/tutorials/.\n",
        encoding="utf-8",
    )


@click.command(name="init", help="Scaffold a new Alp SDK project.")
@click.argument("name", type=str)
@click.option("--som", default=None, help="SoM SKU (e.g. E1M-AEN701).")
@click.option("--preset", default=None, help="Board preset (e.g. e1m-evk).")
@click.option(
    "--peripherals", default=None,
    callback=lambda _ctx, _param, value: (
        None if value is None else tuple(s.strip() for s in value.split(",") if s.strip())
    ),
    help="Comma-separated starter peripherals (uart,gpio,i2c,spi,pwm).",
)
def init_cmd(name: str, som: str | None, preset: str | None,
             peripherals: tuple[str, ...] | None) -> None:
    dest = Path.cwd() / name
    if dest.exists():
        click.echo(f"alp init: '{name}' already exists")
        raise SystemExit(1)

    if som is None or preset is None or peripherals is None:
        som, preset, periph_list = _interactive(som, preset, peripherals)
    else:
        periph_list = list(peripherals)

    _scaffold(dest, som, preset, periph_list)
    click.echo(f"Created {name}/ with som={som}, preset={preset}.")
    click.echo("Next: cd " + name + " && alp run")
```

Wire into `scripts/alp_cli/main.py`:

```python
from alp_cli.init import init_cmd

cli.add_command(init_cmd)
```

(Remove the `init` stub.)

- [ ] **Step 3: Run tests**

```bash
pytest tests/scripts/test_alp_cli.py -v
```
Expected: both new tests PASS.

- [ ] **Step 4: Smoke-test interactively (manual)**

```bash
cd /tmp && rm -rf wizard-test && cd /tmp && alp init wizard-test
```
Expected: prompts appear, project scaffolded.

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/init.py scripts/alp_cli/main.py tests/scripts/test_alp_cli.py
git commit -m "feat(cli): alp init interactive wizard + non-interactive flags"
```

---

### Task 3.3: `alp run` (native_sim default + `--board` for west)

**Files:**
- Create: `scripts/alp_cli/run.py`
- Modify: `scripts/alp_cli/main.py`
- Modify: `tests/scripts/test_alp_cli.py`

- [ ] **Step 1: Write the failing test**

Add to `tests/scripts/test_alp_cli.py`:

```python
def test_run_reports_missing_board_yaml(tmp_path: Path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    result = CliRunner().invoke(cli, ["run"])
    assert result.exit_code != 0
    assert "no board.yaml" in result.output


def test_run_finds_board_yaml_from_subdirectory(tmp_path: Path, monkeypatch, mocker):
    monkeypatch.chdir(tmp_path)
    proj = tmp_path / "proj"
    (proj / "src").mkdir(parents=True)
    (proj / "board.yaml").write_text("som:\n  sku: E1M-AEN701\npreset: e1m-evk\n")
    subdir = proj / "src"
    monkeypatch.chdir(subdir)
    # Mock the actual build/exec so the test doesn't shell out.
    called = {}
    mocker.patch("alp_cli.run._build_and_exec_native_sim",
                 side_effect=lambda project_dir: called.setdefault("dir", project_dir) or 0)
    result = CliRunner().invoke(cli, ["run"])
    assert result.exit_code == 0
    assert called["dir"] == proj
```

(If `pytest-mock` is not already a dev dep, add it to `pyproject.toml`'s `[project.optional-dependencies] dev` list: `"pytest-mock>=3.10"`.)

- [ ] **Step 2: Implement the subcommand**

Create `scripts/alp_cli/run.py`:

```python
"""`alp run` -- build the current project + run it on native_sim."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import click


def _find_project(start: Path) -> Path | None:
    cursor = start.resolve()
    while True:
        if (cursor / "board.yaml").is_file():
            return cursor
        if cursor.parent == cursor:
            return None
        cursor = cursor.parent


def _build_and_exec_native_sim(project_dir: Path) -> int:
    build = project_dir / "build" / "native_sim"
    build.mkdir(parents=True, exist_ok=True)
    cmake = ["west", "build", "-b", "native_sim", "-d", str(build), str(project_dir)]
    proc = subprocess.run(cmake)
    if proc.returncode != 0:
        return proc.returncode
    exe = build / "zephyr" / "zephyr.exe"
    if not exe.is_file():
        click.echo(f"alp run: built binary not found at {exe}", err=True)
        return 1
    return subprocess.run([str(exe)]).returncode


def _build_for_board(project_dir: Path, board: str, flash: bool) -> int:
    build = project_dir / "build" / board.replace("/", "_")
    build.mkdir(parents=True, exist_ok=True)
    rc = subprocess.run(
        ["west", "build", "-b", board, "-d", str(build), str(project_dir)]
    ).returncode
    if rc != 0:
        return rc
    if flash:
        return subprocess.run(["west", "flash", "-d", str(build)]).returncode
    return 0


@click.command(name="run", help="Build and run the current project on native_sim.")
@click.option("--board", default=None, help="Real-hardware build (skips native_sim).")
@click.option("--flash", is_flag=True, help="With --board: flash after build.")
def run_cmd(board: str | None, flash: bool) -> None:
    project = _find_project(Path.cwd())
    if project is None:
        click.echo("alp run: no board.yaml found in this directory or any parent",
                   err=True)
        raise SystemExit(1)
    if board:
        rc = _build_for_board(project, board, flash)
    else:
        rc = _build_and_exec_native_sim(project)
    if rc != 0:
        raise SystemExit(rc)
```

Wire into `scripts/alp_cli/main.py`:

```python
from alp_cli.run import run_cmd

cli.add_command(run_cmd)
```

- [ ] **Step 3: Run tests**

```bash
pip install -e ".[dev]"
pytest tests/scripts/test_alp_cli.py -v
```
Expected: both new tests PASS.

- [ ] **Step 4: Smoke-test on the hello-world example**

```bash
cd examples/peripheral-io/hello-world && alp run
```
Expected: native_sim build runs, prints `[hello] tick 0`…`[hello] done`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add scripts/alp_cli/run.py scripts/alp_cli/main.py tests/scripts/test_alp_cli.py pyproject.toml
git commit -m "feat(cli): alp run on native_sim (default) + --board real-hw path"
```

---

### Task 3.4: README quickstart

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add a quickstart section near the top**

Open `README.md`. After the project intro/title block, add:

```markdown
## Quickstart

```bash
# Install the CLI (once per clone)
pip install -e .

# Scaffold + run a hello-world on native_sim — no hardware needed
alp init my-app
cd my-app
alp run
```

`alp init` walks you through SoM SKU + board preset + starter peripherals interactively, or accepts `--som`, `--preset`, `--peripherals` flags for CI. `alp run` builds for `native_sim` by default and prints the app's stdout straight through; pass `--board <name>` for a real-hardware build (`--flash` to chain flash).

`alp validate board.yaml` runs the diagnostic-rich validator standalone — try it on a fixture under `tests/fixtures/board_yaml_bad/` to see the format.
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs(readme): add alp init/run/validate quickstart"
```

---

## Self-review checklist

After all tasks complete, verify:

1. **Spec coverage** — every spec section maps to at least one task:
   - Feature 1 packaging + init + run + tests → tasks 0.1, 3.1, 3.2, 3.3, 3.4. ✓
   - Feature 2 loader + diagnostic + 3 passes + integration + doc stubs → tasks 0.2, 0.3, 1.1–1.5. ✓
   - Feature 3 compile-time + runtime + tests + example → tasks 2.1, 2.2, 2.3, 2.4. ✓
   - Cross-cutting: shared `alp_cli/` package (0.1), shared SoC JSON source of truth (1.4 + 2.x). ✓
2. **Method-name consistency** — `validate_board_yaml`, `DiagnosticCollector`, `_schema_pass`, `_xref_pass`, `_compat_pass`, `_emit_cap_layer`, `_emit_cap_h`, `_emit_cap_c`, `_find_project`, `_build_and_exec_native_sim`, `_build_for_board` used consistently across tasks. ✓
3. **No placeholders** — every step has either a code block or an explicit command + expected output. ✓
4. **Type consistency** — `Diagnostic` field set used in task 0.3 (`severity`, `path`, `line`, `col`, `span`, `code`, `message`, `hint`, `doc_url`) is the same set consumed by tests in 1.x. ✓

## Execution sequence summary

- **Phase 0 (sequential):** 0.1 → 0.2 → 0.3.
- **Phase 1 (sequential within phase):** 1.1 → 1.2 → 1.3 → 1.4 → 1.5. Depends on Phase 0.
- **Phase 2 (sequential within phase):** 2.1 → 2.2 → 2.3 → 2.4. Independent of Phase 1 — can run in parallel with it.
- **Phase 3 (sequential within phase):** 3.1 → 3.2 → 3.3 → 3.4. Depends on Phase 1 (uses validator).

Subagent dispatch plan: after Phase 0 lands, dispatch one agent to Phase 1 and one to Phase 2 in parallel. When Phase 1 lands, dispatch a third agent to Phase 3.
