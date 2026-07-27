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


def test_prose_outside_a_fence_does_not_misparse_as_a_subcommand(tmp_path):
    """A content word right after the bare word "tan" in REAL paragraph
    prose -- no backtick span, no fence -- must never be extracted as a
    subcommand. The test above puts its prose INSIDE a fenced block, so it
    only exercises the _ENGLISH_STOPWORDS set, not whether _code_corpus()
    actually strips raw prose: mutating _code_corpus to `return
    markdown_text` (skip fence/prose stripping entirely) still passes that
    test, because "is"/"on" are in the stopword set regardless of where
    they came from. "handles" is not a stopword, so it proves the corpus
    was actually built from code spans, not the raw file."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    readme = doc_root / "README.md"
    readme.write_text(
        readme.read_text(encoding="utf-8")
        + "\ntan handles retries automatically, with no flags of its own.\n",
        encoding="utf-8",
    )
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=_ALL_RECOGNIZED, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "`tan handles`" not in proc.stderr


def test_heredoc_only_verb_is_still_checked(tmp_path):
    """`tan support-bundle`, mentioned ONLY inside scripts/bootstrap.sh's
    printed heredoc body -- nowhere in README.md/docs/cli.md/
    docs/getting-started.md/docs/troubleshooting.md -- must still enter the
    checked surface. Mutating extract_heredoc_bodies() to `return ""` would
    silently drop it: the gate would stay green even if `tan
    support-bundle` no longer existed."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    (doc_root / "scripts" / "bootstrap.sh").write_text(
        textwrap.dedent(
            """\
            #!/usr/bin/env bash
            cat <<EOF

            Next steps:
            EOF
            cat <<'EOF'

              # needs tan on PATH -- see README.md
              tan doctor --build
              tan support-bundle
            EOF
            """
        ),
        encoding="utf-8",
    )
    tan_bin = _write_fake_tan(
        tmp_path / "bin", recognized=dict(_ALL_RECOGNIZED), missing={"support-bundle"},
    )

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`tan support-bundle`" in proc.stderr
    assert "no longer a recognised subcommand" in proc.stderr


def test_doctor_build_removed_from_help_is_caught(tmp_path):
    """`### `tan doctor --build`` -- the heading-EMBEDDED flag docs/cli.md
    encodes for `doctor` (the case the module docstring specifically calls
    out) -- must actually be verified. Dropping the loop that folds a
    heading's own embedded flags into verb_flags[current_verb] would
    silently stop checking `--build` at all: the fixture sets this path up,
    but nothing removes `--build` from the stub to prove it's checked."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    recognized = dict(_ALL_RECOGNIZED)
    recognized["doctor"] = set()  # --build silently dropped from real tan
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`tan doctor --build`" in proc.stderr
    assert "not listed in" in proc.stderr


def test_flag_prefix_collision_is_not_masked(tmp_path):
    """A `--build` -> `--build-root` rename must still be reported as
    drift: the substring "--build" is textually present inside
    "--build-root", so an unbounded `re.escape(flag)` match (no word-
    boundary lookaround) would incorrectly consider the documented
    `--build` still present. Proves the boundary regex at check_surface's
    flag-match step actually matters."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    recognized = dict(_ALL_RECOGNIZED)
    recognized["doctor"] = {"--build-root"}  # renamed, not the documented --build
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`tan doctor --build`" in proc.stderr
    assert "`tan doctor --build-root`" not in proc.stderr


def test_heading_embedded_non_flag_token_is_not_treated_as_a_flag(tmp_path):
    """Only a `--flag`-shaped token in a heading's own verb zone becomes a
    documented flag (`_parse_verb_span`'s `p.startswith("--")` filter); a
    bare word after the verb is prose, never a flag. Dropping that filter
    would fabricate a flag requirement from any such word and produce a
    false "documents this flag" problem for something docs/cli.md never
    claimed as a flag."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    cli_md = doc_root / "docs" / "cli.md"
    cli_md.write_text(
        cli_md.read_text(encoding="utf-8").replace(
            "### `tan doctor --build` -- build-readiness preflight",
            "### `tan doctor --build status` -- build-readiness preflight",
        ),
        encoding="utf-8",
    )
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=dict(_ALL_RECOGNIZED), missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "status" not in proc.stdout + proc.stderr


def test_multiverb_heading_does_not_leak_a_stray_flag_to_the_prior_verb(tmp_path):
    """A table-like line directly under a multi-verb heading (`` `tan
    build` / `flash` ``) must never be attributed to whichever single verb
    happened to precede it. Replacing the `else: current_verb = None` reset
    with a no-op would leave `current_verb` at the PRIOR single-verb
    section ('init' here), silently mis-attributing a stray flag row to
    `init` and producing a false "documented but not listed" problem for a
    flag docs/cli.md never actually associated with `init`."""
    doc_root = tmp_path / "repo"
    (doc_root / "docs").mkdir(parents=True)
    (doc_root / "scripts").mkdir(parents=True)
    (doc_root / "README.md").write_text("`tan init` scaffolds a project.\n", encoding="utf-8")
    (doc_root / "docs" / "getting-started.md").write_text(
        "`tan validate` checks board.yaml.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "troubleshooting.md").write_text(
        "`tan run` is the escape hatch.\n", encoding="utf-8",
    )
    (doc_root / "scripts" / "bootstrap.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
    (doc_root / "docs" / "cli.md").write_text(
        textwrap.dedent(
            """\
            # The `tan` CLI

            ### `tan init` -- scaffold a new project

            | Option | Meaning |
            |---|---|
            | `--som` | Target SoM SKU |

            ### `tan build` / `flash` -- build execution

            | Option | Meaning |
            |---|---|
            | `--stray` | must NOT attach to `init` -- no per-verb table here |

            ```bash
            tan build
            tan flash
            ```
            """
        ),
        encoding="utf-8",
    )

    recognized = {
        "init": {"--som"}, "build": set(), "flash": set(),
        "validate": set(), "run": set(),
    }
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "--stray" not in proc.stdout + proc.stderr


def test_getting_started_only_subcommand_is_checked(tmp_path):
    """`validate` is documented ONLY in docs/getting-started.md in the base
    fixture -- not in cli.md's headings/fenced code, README.md, or
    bootstrap.sh. Shrinking DOC_SOURCES to just docs/cli.md would silently
    stop checking it; no existing test ever marks `validate` MISSING to
    prove that file's scan is load-bearing."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    recognized = dict(_ALL_RECOGNIZED)
    del recognized["validate"]
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing={"validate"})

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`tan validate`" in proc.stderr


def test_second_verb_in_multiverb_heading_with_no_other_mention_is_checked(tmp_path):
    """The SECOND (bare, non-`tan `-prefixed) verb name in a multi-verb
    heading -- `` `flash` `` in `` ### `tan build` / `flash` -- ... `` --
    only ever enters `all_verbs` via `_verbs_in_heading`'s heading parse.
    Unlike a single-verb `` `tan <verb>` `` heading (whose own backtick
    span IS a valid `tan <verb>` match under the plain DOC_SOURCES scan,
    making `heading_verbs` redundant for it), a bare second name is
    invisible to that generic scan unless something else repeats `tan
    flash` in a fenced/inline span. Omit any such repeat here: zeroing the
    `subcommands = set(heading_verbs)` seed would silently drop `flash`
    from the checked surface with nothing else to catch it."""
    doc_root = tmp_path / "repo"
    (doc_root / "docs").mkdir(parents=True)
    (doc_root / "scripts").mkdir(parents=True)
    (doc_root / "README.md").write_text("`tan init` scaffolds a project.\n", encoding="utf-8")
    (doc_root / "docs" / "getting-started.md").write_text(
        "`tan validate` checks board.yaml.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "troubleshooting.md").write_text(
        "`tan run` is the escape hatch.\n", encoding="utf-8",
    )
    (doc_root / "scripts" / "bootstrap.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
    (doc_root / "docs" / "cli.md").write_text(
        "# The `tan` CLI\n\n"
        "### `tan build` / `flash` -- build execution\n\n"
        "No fenced example here -- `flash` is named nowhere else in the doc tree.\n",
        encoding="utf-8",
    )

    recognized = {"init": set(), "build": set(), "validate": set(), "run": set()}
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing={"flash"})

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`tan flash`" in proc.stderr


def test_empty_doc_tree_reports_broken_extraction(tmp_path):
    """A doc tree with zero `tan <verb>` mentions anywhere must fail loudly
    naming extraction as broken, never silently exit 0 (an empty
    documented surface is vacuously "fully checked" otherwise)."""
    doc_root = tmp_path / "repo"
    (doc_root / "docs").mkdir(parents=True)
    (doc_root / "scripts").mkdir(parents=True)
    (doc_root / "README.md").write_text("Nothing to see here.\n", encoding="utf-8")
    (doc_root / "docs" / "cli.md").write_text("# The `tan` CLI\n", encoding="utf-8")
    (doc_root / "docs" / "getting-started.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "troubleshooting.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "scripts" / "bootstrap.sh").write_text(
        "#!/usr/bin/env bash\necho hi\n", encoding="utf-8",
    )
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized={}, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "extraction is broken" in proc.stderr


def test_forwarding_verb_flag_check_is_skipped_not_matched_by_generic_blurb(tmp_path):
    """A FORWARDING verb's `--help` ends its `Usage:` line in `[ARGS]...`
    and prints a generic "Arguments forwarded verbatim ..." blurb naming a
    few EXAMPLE flags that belong to OTHER forwarding verbs, never its own.
    docs/cli.md's tabulated flags for that verb must never be matched
    against that blurb -- matching it is both noisy (fires regardless of
    this verb's real flags) and unsafe (stays present even if this verb's
    own forwarded flag support were dropped). The real, installed tan
    confirms `new-som --sdk-root ... --dry-run` genuinely works
    (alp-sdk#973-adjacent review); this check must not report it missing."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    cli_md = doc_root / "docs" / "cli.md"
    cli_md.write_text(
        cli_md.read_text(encoding="utf-8")
        + textwrap.dedent(
            """

            ### `tan new-som` -- scaffold a new SoM

            | Option | Meaning |
            |---|---|
            | `--sku` | Target SoM SKU |
            """
        ),
        encoding="utf-8",
    )
    readme = doc_root / "README.md"
    readme.write_text(
        readme.read_text(encoding="utf-8") + "\n`tan new-som` ports a SoM.\n",
        encoding="utf-8",
    )

    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    tan_path = bin_dir / "tan"
    tan_path.write_text(
        textwrap.dedent(
            """\
            #!/usr/bin/env python3
            import sys
            verb = sys.argv[1] if len(sys.argv) > 1 else ""
            HELP = {
                "init": "Usage: tan init [OPTIONS]\\n\\nOptions:\\n"
                        "      --sdk-root <PATH>\\n      --som <SOM>\\n",
                "build": "Usage: tan build [OPTIONS]\\n",
                "flash": "Usage: tan flash [OPTIONS]\\n",
                "validate": "Usage: tan validate [OPTIONS]\\n",
                "run": "Usage: tan run [OPTIONS]\\n",
                "doctor": "Usage: tan doctor [OPTIONS]\\n\\nOptions:\\n      --build\\n",
                "new-som": (
                    "Usage: tan new-som [OPTIONS] [ARGS]...\\n\\n"
                    "Arguments:\\n  [ARGS]...\\n"
                    "          Arguments forwarded verbatim to the underlying command "
                    "(e.g. app path, `--core <id>`, `--sequential`, `-b <board>`)\\n"
                ),
            }
            if verb == "--version":
                print("tan 0.0.0-test")
                sys.exit(0)
            if verb in HELP:
                print(HELP[verb])
                sys.exit(0)
            print(f"error: unrecognized subcommand {verb!r}", file=sys.stderr)
            sys.exit(2)
            """
        ),
        encoding="utf-8",
    )
    tan_path.chmod(tan_path.stat().st_mode | stat.S_IEXEC)

    proc = _run(doc_root, bin_dir)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "new-som" in proc.stdout  # named by the OK line's skip note
    assert "--sku" not in proc.stdout + proc.stderr
