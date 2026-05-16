# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/lint_doc_yaml_fragments.py.

Covers:
  - fragment extraction (``` yaml … ``` fenced blocks)
  - schema_version + som gating (partial snippets are skipped)
  - schema validation (v2 success + failure modes)
  - exclude-prefix handling
  - --path single-file invocation
  - YAML parse failures are non-fatal (skipped, not crashed)
  - real-tree smoke: README + docs/ + tutorials/ stay clean

Run locally:

    python -m pytest tests/scripts/test_lint_doc_yaml_fragments.py -v
"""

from __future__ import annotations

import json
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest


REPO = Path(__file__).resolve().parents[2]
LINTER = REPO / "scripts" / "lint_doc_yaml_fragments.py"
SCHEMAS_DIR = REPO / "metadata" / "schemas"

# Import the linter module directly for unit tests against its helpers
# (the script's APIs are documented and intended for reuse).
sys.path.insert(0, str(REPO / "scripts"))
import lint_doc_yaml_fragments as linter  # noqa: E402


# ---------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------


def _write_md(tmp: Path, name: str, body: str) -> Path:
    """Write a markdown file with a dedented body.  Returns the path."""
    path = tmp / name
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _run_linter(*args: str) -> subprocess.CompletedProcess[str]:
    """Invoke the linter as a subprocess.  Uses the same interpreter
    the test suite runs under so deps line up."""
    return subprocess.run(
        [sys.executable, str(LINTER), *args],
        capture_output=True, text=True, check=False,
    )


# Minimal valid v2 fragment.  Used as a known-good baseline; tests
# mutate it to produce known-bad fragments.
_VALID_V2 = """
schema_version: 2

som:
  sku: E1M-AEN701

cores:
  m55_hp:
    app: ./src
"""


# ---------------------------------------------------------------------
# 1. Fragment extraction
# ---------------------------------------------------------------------


def test_extract_fragments_empty(tmp_path: Path) -> None:
    """A markdown file with no fenced blocks yields zero fragments."""
    p = _write_md(tmp_path, "doc.md", "Just prose, no code.\n")
    assert linter.extract_fragments(p) == []


def test_extract_fragments_yaml_fence(tmp_path: Path) -> None:
    """A ```yaml fence is captured."""
    p = _write_md(tmp_path, "doc.md", """
        # Header

        ```yaml
        schema_version: 2
        som:
          sku: E1M-AEN701
        ```
    """)
    fragments = linter.extract_fragments(p)
    assert len(fragments) == 1
    assert "schema_version: 2" in fragments[0].body
    assert fragments[0].line > 0


def test_extract_fragments_yml_alias(tmp_path: Path) -> None:
    """The ``` yml alias is also accepted."""
    p = _write_md(tmp_path, "doc.md", """
        ```yml
        schema_version: 2
        som: { sku: E1M-AEN701 }
        ```
    """)
    fragments = linter.extract_fragments(p)
    assert len(fragments) == 1


def test_extract_fragments_ignores_non_yaml_fences(tmp_path: Path) -> None:
    """``` bash / ``` python / no-language fences are ignored."""
    p = _write_md(tmp_path, "doc.md", """
        ```python
        x = 1
        ```

        ```
        plain block
        ```

        ```bash
        ls -la
        ```
    """)
    assert linter.extract_fragments(p) == []


def test_extract_fragments_multiple_blocks(tmp_path: Path) -> None:
    """Multiple ```yaml fences in one file all get captured."""
    p = _write_md(tmp_path, "doc.md", """
        ```yaml
        first: 1
        ```

        ```yaml
        second: 2
        ```

        ```yaml
        third: 3
        ```
    """)
    fragments = linter.extract_fragments(p)
    assert len(fragments) == 3


# ---------------------------------------------------------------------
# 2. parse_fragment gating
# ---------------------------------------------------------------------


def test_parse_fragment_requires_schema_version(tmp_path: Path) -> None:
    """A fenced fragment without `schema_version:` is NOT a board.yaml
    document and is silently skipped."""
    p = _write_md(tmp_path, "doc.md", """
        ```yaml
        som:
          sku: E1M-AEN701
        ```
    """)
    fragments = linter.extract_fragments(p)
    assert len(fragments) == 1
    assert linter.parse_fragment(fragments[0]) is None


def test_parse_fragment_requires_som(tmp_path: Path) -> None:
    """A fenced fragment with `schema_version` but no `som:` is also
    skipped -- could be a soc-spec or library-profile snippet."""
    p = _write_md(tmp_path, "doc.md", """
        ```yaml
        schema_version: 2
        library: tflite_micro
        ```
    """)
    fragments = linter.extract_fragments(p)
    assert linter.parse_fragment(fragments[0]) is None


def test_parse_fragment_invalid_yaml_skipped(tmp_path: Path) -> None:
    """Malformed YAML doesn't crash the linter; the fragment is
    silently skipped (fragments are often work-in-progress)."""
    p = _write_md(tmp_path, "doc.md", """
        ```yaml
        schema_version: 2
        som:
          sku: E1M-AEN701
        [unclosed bracket
        ```
    """)
    fragments = linter.extract_fragments(p)
    assert len(fragments) == 1
    assert linter.parse_fragment(fragments[0]) is None


def test_parse_fragment_accepts_valid_board_yaml(tmp_path: Path) -> None:
    """A fragment with both markers is parsed as a board.yaml document."""
    p = _write_md(tmp_path, "doc.md", f"""
        ```yaml
{textwrap.indent(_VALID_V2, "        ").rstrip()}
        ```
    """)
    fragments = linter.extract_fragments(p)
    assert len(fragments) == 1
    parsed = linter.parse_fragment(fragments[0])
    assert isinstance(parsed, dict)
    assert parsed.get("schema_version") == 2
    assert parsed.get("som", {}).get("sku") == "E1M-AEN701"


# ---------------------------------------------------------------------
# 3. Schema validation
# ---------------------------------------------------------------------


def test_validate_fragment_v2_clean(tmp_path: Path) -> None:
    """A minimal valid v2 fragment passes."""
    p = _write_md(tmp_path, "doc.md", f"""
        ```yaml
{textwrap.indent(_VALID_V2, "        ").rstrip()}
        ```
    """)
    results, checked = linter.lint([p], SCHEMAS_DIR)
    assert checked == 1
    assert len(results) == 1
    assert results[0].ok, results[0].error
    assert results[0].schema_version == 2


def test_validate_fragment_rejects_inference_backend(tmp_path: Path) -> None:
    """REGRESSION: the post-a3cd4fd schema removed `inference.backend`
    as a per-core field.  A doc fragment with it must fail."""
    bad = """
        schema_version: 2
        som:
          sku: E1M-AEN701
        cores:
          m55_hp:
            app: ./src
            inference:
              backend: cpu
    """
    p = _write_md(tmp_path, "doc.md", f"""
        ```yaml
{textwrap.indent(bad, "        ").rstrip()}
        ```
    """)
    results, checked = linter.lint([p], SCHEMAS_DIR)
    assert checked == 1
    assert not results[0].ok
    # Error mentions the offending field path.
    assert "backend" in (results[0].error or "")


def test_validate_fragment_rejects_top_level_os(tmp_path: Path) -> None:
    """REGRESSION: v1's top-level `os:` is explicitly rejected by the
    v2 schema (`not: { required: [os] }`)."""
    bad = """
        schema_version: 2
        som:
          sku: E1M-AEN701
        os: zephyr
        cores:
          m55_hp:
            app: ./src
    """
    p = _write_md(tmp_path, "doc.md", f"""
        ```yaml
{textwrap.indent(bad, "        ").rstrip()}
        ```
    """)
    results, checked = linter.lint([p], SCHEMAS_DIR)
    assert checked == 1
    assert not results[0].ok


def test_validate_fragment_rejects_unknown_peripheral(tmp_path: Path) -> None:
    """`peripherals:` is a fixed enum; `audio` (not in the enum) fails."""
    bad = """
        schema_version: 2
        som:
          sku: E1M-AEN701
        cores:
          m55_hp:
            app: ./src
            peripherals:
              - audio
    """
    p = _write_md(tmp_path, "doc.md", f"""
        ```yaml
{textwrap.indent(bad, "        ").rstrip()}
        ```
    """)
    results, checked = linter.lint([p], SCHEMAS_DIR)
    assert not results[0].ok
    assert "audio" in (results[0].error or "")


def test_validate_fragment_unknown_schema_version_passes(tmp_path: Path) -> None:
    """A fragment claiming an unsupported schema version is NOT the
    linter's concern -- the loader rejects it at build time.  The
    linter just warns no-schema and reports OK."""
    body = """
        schema_version: 99
        som:
          sku: E1M-AEN701
    """
    p = _write_md(tmp_path, "doc.md", f"""
        ```yaml
{textwrap.indent(body, "        ").rstrip()}
        ```
    """)
    results, checked = linter.lint([p], SCHEMAS_DIR)
    assert checked == 1
    assert results[0].ok
    assert results[0].schema_version == 99


def test_validate_fragment_non_integer_schema_version_fails(tmp_path: Path) -> None:
    """A `schema_version: "foo"` fragment fails with a clear message."""
    body = """
        schema_version: foo
        som:
          sku: E1M-AEN701
    """
    p = _write_md(tmp_path, "doc.md", f"""
        ```yaml
{textwrap.indent(body, "        ").rstrip()}
        ```
    """)
    results, checked = linter.lint([p], SCHEMAS_DIR)
    assert checked == 1
    assert not results[0].ok


# ---------------------------------------------------------------------
# 4. Exclude handling
# ---------------------------------------------------------------------


def test_excludes_prefix_skips_subtree(tmp_path: Path) -> None:
    """A path under an excluded prefix isn't walked."""
    (tmp_path / "build").mkdir()
    _write_md(tmp_path / "build", "broken.md", """
        ```yaml
        schema_version: 2
        som: { sku: NOPE }
        ```
    """)
    _write_md(tmp_path, "good.md", f"""
        ```yaml
{textwrap.indent(_VALID_V2, "        ").rstrip()}
        ```
    """)
    paths = linter.discover_markdown(tmp_path, excludes=("build",))
    # The `build/` doc must not appear in the walk.
    assert (tmp_path / "good.md") in paths
    assert (tmp_path / "build" / "broken.md") not in paths


def test_excludes_matches_path_components_not_substrings(
    tmp_path: Path,
) -> None:
    """Excluding `build` must not silently exclude `building-blocks/`
    (component-prefix match, not raw string prefix)."""
    (tmp_path / "building-blocks").mkdir()
    target = _write_md(tmp_path / "building-blocks", "doc.md", "x")
    paths = linter.discover_markdown(tmp_path, excludes=("build",))
    assert target in paths


# ---------------------------------------------------------------------
# 5. End-to-end CLI behaviour
# ---------------------------------------------------------------------


def test_cli_exit_zero_on_clean_path(tmp_path: Path) -> None:
    """A single-file run with a clean fragment exits 0."""
    p = _write_md(tmp_path, "doc.md", f"""
        ```yaml
{textwrap.indent(_VALID_V2, "        ").rstrip()}
        ```
    """)
    rv = _run_linter("--path", str(p))
    assert rv.returncode == 0, rv.stderr + rv.stdout
    assert "clean" in rv.stdout


def test_cli_exit_one_on_bad_fragment(tmp_path: Path) -> None:
    """A bad fragment causes exit code 1, surfaces the broken field."""
    bad = """
        schema_version: 2
        som:
          sku: E1M-AEN701
        cores:
          m55_hp:
            app: ./src
            inference:
              backend: cpu
    """
    p = _write_md(tmp_path, "doc.md", f"""
        ```yaml
{textwrap.indent(bad, "        ").rstrip()}
        ```
    """)
    rv = _run_linter("--path", str(p))
    assert rv.returncode == 1
    assert "FAIL" in rv.stdout
    assert "backend" in rv.stdout


def test_cli_exit_zero_when_no_board_yaml_fragments(tmp_path: Path) -> None:
    """An md tree with no board.yaml-shaped fragments exits 0 cleanly."""
    _write_md(tmp_path, "doc.md", "Just prose.\n")
    rv = _run_linter("--root", str(tmp_path))
    assert rv.returncode == 0
    assert "no board.yaml fragments found" in rv.stdout


def test_cli_missing_schemas_dir_exits_two(tmp_path: Path) -> None:
    """A nonexistent --schemas-dir exits 2 (invocation error)."""
    rv = _run_linter("--schemas-dir", str(tmp_path / "nope"))
    assert rv.returncode == 2


def test_cli_missing_path_exits_two(tmp_path: Path) -> None:
    """A nonexistent --path target exits 2."""
    rv = _run_linter("--path", str(tmp_path / "ghost.md"))
    assert rv.returncode == 2


# ---------------------------------------------------------------------
# 6. Real-tree smoke
# ---------------------------------------------------------------------


def test_repo_readme_lints_clean() -> None:
    """The shipped README must lint clean.  Regression-locks the
    a3cd4fd cleanup."""
    readme = REPO / "README.md"
    rv = _run_linter("--path", str(readme))
    assert rv.returncode == 0, rv.stderr + rv.stdout


def test_repo_default_walk_is_clean() -> None:
    """The full default-scope walk must be clean today (post-fix).
    Locks in the absence of in-tree drift."""
    rv = _run_linter()
    assert rv.returncode == 0, rv.stderr + rv.stdout


def test_schema_v2_file_exists() -> None:
    """The linter is useless without the v2 schema; if anyone moves
    it, fail loudly."""
    schema = SCHEMAS_DIR / "board-config-v2.schema.json"
    assert schema.is_file(), \
        f"missing v2 schema at {schema} -- did the metadata layout move?"
    data = json.loads(schema.read_text(encoding="utf-8"))
    assert data.get("$id", "").endswith("board-config-v2.schema.json")
