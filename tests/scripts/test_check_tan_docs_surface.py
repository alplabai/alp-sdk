# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_tan_docs_surface.py.

Builds a small fake doc tree + a fake `tan` stub per test, entirely under
tmp_path -- never mutates the repo tree (alp-sdk#973). A green run against
the REAL repo + a REAL installed `tan` proves nothing about whether the
extraction/comparison logic actually fires; these tests control both sides
of the comparison instead.

Run locally:

    python -m pytest tests/scripts/test_check_tan_docs_surface.py -q
"""
from __future__ import annotations

import os
import stat
import subprocess
import sys
import textwrap
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_tan_docs_surface.py"


def _write_docroot(root: Path) -> None:
    """A minimal doc tree exercising every extraction path the real gate
    walks: an inline-code verb mention, a fenced-code verb mention, a
    docs/cli.md single-verb section with a flag table, a multi-verb heading
    (no flag table), a `tan doctor --build` header-embedded flag, a plain-
    English sentence inside a fenced comment that must NOT parse as a fake
    subcommand, and a bootstrap.sh next-steps heredoc."""
    (root / "docs").mkdir(parents=True)
    (root / "scripts").mkdir(parents=True)

    (root / "README.md").write_text(
        textwrap.dedent(
            """\
            # README

            `tan init` scaffolds a project.

            ```bash
            # Zephyr (heterogeneous slice) -- tan is the executor; it consumes
            tan build
            ```
            """
        ),
        encoding="utf-8",
    )

    (root / "docs" / "cli.md").write_text(
        textwrap.dedent(
            """\
            # The `tan` CLI

            ### `tan init` -- scaffold a new project

            | Option | Meaning |
            |---|---|
            | `--sdk-root` | Path to the alp-sdk checkout |
            | `--som` | Target SoM SKU |

            ### `tan build` / `flash` -- build execution

            ```bash
            tan build
            tan flash
            ```

            ### `tan doctor --build` -- build-readiness preflight

            ```bash
            tan doctor --build
            ```
            """
        ),
        encoding="utf-8",
    )

    (root / "docs" / "getting-started.md").write_text(
        "See `tan validate` for board.yaml checks.\n", encoding="utf-8",
    )
    (root / "docs" / "troubleshooting.md").write_text(
        "`tan run` is the single-image escape hatch.\n", encoding="utf-8",
    )

    (root / "scripts" / "bootstrap.sh").write_text(
        textwrap.dedent(
            """\
            #!/usr/bin/env bash
            cat <<EOF

            Next steps:
            EOF
            cat <<'EOF'

              # needs tan on PATH -- see README.md
              tan doctor --build
            EOF
            """
        ),
        encoding="utf-8",
    )


def _write_fake_tan(bin_dir: Path, *, recognized: dict[str, set[str]], missing: set[str]) -> Path:
    """A stub `tan` whose `<verb> --help` prints the given flags for a
    recognized verb (exit 0) or errors like real clap does for `missing`
    verbs (exit 2, message on stderr, nothing on stdout)."""
    bin_dir.mkdir(parents=True, exist_ok=True)
    tan_path = bin_dir / "tan"
    lines = [
        "#!/usr/bin/env python3",
        "import sys",
        f"RECOGNIZED = {recognized!r}",
        f"MISSING = {missing!r}",
        "verb = sys.argv[1] if len(sys.argv) > 1 else ''",
        "if verb in MISSING:",
        "    print(f\"error: unrecognized subcommand {verb!r}\", file=sys.stderr)",
        "    sys.exit(2)",
        "if verb == '--version':",
        "    print('tan 0.0.0-test')",
        "    sys.exit(0)",
        "if verb in RECOGNIZED:",
        "    print('Options:')",
        "    for f in sorted(RECOGNIZED[verb]):",
        "        print(f'      {f} <VALUE>')",
        "    sys.exit(0)",
        "print(f\"error: unrecognized subcommand {verb!r}\", file=sys.stderr)",
        "sys.exit(2)",
    ]
    tan_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    tan_path.chmod(tan_path.stat().st_mode | stat.S_IEXEC)
    return tan_path


def _run(repo_root: Path, tan_bin_dir: Path, **kw):
    env = dict(os.environ)
    env["PATH"] = f"{tan_bin_dir}{os.pathsep}{env.get('PATH', '')}"
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--repo-root", str(repo_root)],
        capture_output=True, text=True, env=env, **kw,
    )


# The gate's own documented surface, extracted straight from `_write_docroot`
# above: subcommands init/build/flash/validate/run/doctor, and init's own
# flags (--sdk-root/--som) + doctor's header-embedded --build.
_ALL_VERBS = {"init", "build", "flash", "validate", "run", "doctor"}
_ALL_RECOGNIZED = {
    "init": {"--sdk-root", "--som"},
    "build": set(),
    "flash": set(),
    "validate": set(),
    "run": set(),
    "doctor": {"--build"},
}


def test_fully_recognized_surface_passes(tmp_path):
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=_ALL_RECOGNIZED, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "OK" in proc.stdout


def test_removed_subcommand_fails(tmp_path):
    """The real-world case: a subcommand documented in prose/tables no
    longer exists in `tan` (e.g. `emit` renamed to `generate`)."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    recognized = dict(_ALL_RECOGNIZED)
    del recognized["build"]
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing={"build"})

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`tan build`" in proc.stderr
    assert "no longer a recognised subcommand" in proc.stderr


def test_removed_flag_fails(tmp_path):
    """A documented flag (docs/cli.md table) that no longer parses."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    recognized = dict(_ALL_RECOGNIZED)
    recognized["init"] = {"--som"}  # --sdk-root silently dropped
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`tan init --sdk-root`" in proc.stderr
    assert "not listed in" in proc.stderr


def test_tan_not_on_path_fails_loudly_never_skips(tmp_path):
    """Skip-as-pass is failure: no `tan` on PATH must exit non-zero with a
    message naming why, never exit 0."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    empty_bin = tmp_path / "empty-bin"
    empty_bin.mkdir()

    env = dict(os.environ)
    env["PATH"] = str(empty_bin)  # deliberately excludes any real `tan` already on this host
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--repo-root", str(doc_root)],
        capture_output=True, text=True, env=env,
    )
    assert proc.returncode != 0
    assert "not on PATH" in proc.stderr


def test_prose_after_tan_does_not_misparse_as_a_subcommand(tmp_path):
    """'tan is the executor' / 'needs tan on PATH' (real prose from
    README.md and bootstrap.sh) must never be extracted as a fake `is` or
    `on` subcommand -- a fake `tan` that errors on every unrecognized verb
    would otherwise redden this run for something that was never real."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=_ALL_RECOGNIZED, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "`tan is" not in proc.stderr
    assert "`tan on" not in proc.stderr
