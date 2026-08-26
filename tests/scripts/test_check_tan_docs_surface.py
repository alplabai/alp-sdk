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

import importlib.util
import os
import stat
import subprocess
import sys
import textwrap
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_tan_docs_surface.py"

_spec = importlib.util.spec_from_file_location("check_tan_docs_surface", SCRIPT)
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)  # direct import for pure-function-level tests below


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


def _install_tan_stub(bin_dir: Path, body: str) -> Path:
    """Install a Python-source `body` as an executable named `tan`,
    resolvable by `shutil.which("tan")` on this OS.

    POSIX: the kernel's shebang support makes an extensionless file with
    `#!/usr/bin/env python3` on its first line + the exec bit directly
    executable, and `which` finds any executable-bit file regardless of
    name/extension -- this is the historical shape of this stub.

    Windows has no kernel shebang support, and `shutil.which()` there only
    resolves names ending in a `PATHEXT` extension (.exe/.bat/.cmd/...) --
    an extensionless `tan` is invisible to it (alp-sdk#993). So on Windows
    we ship the same body as `tan.py` plus a `tan.bat` shim that PATHEXT
    picks up, mirroring how a real `tan.exe` release is found. `body` never
    itself contains OS-specific logic; only this
    installer branches. A real Tan release may be a frozen Python archive or
    the older Rust executable; executable discovery is identical here."""
    bin_dir.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        script_path = bin_dir / "tan.py"
        script_path.write_text(body, encoding="utf-8")
        tan_path = bin_dir / "tan.bat"
        tan_path.write_text(
            f'@"{sys.executable}" "{script_path}" %*\r\nexit /b %errorlevel%\r\n',
            encoding="utf-8",
        )
    else:
        tan_path = bin_dir / "tan"
        tan_path.write_text("#!/usr/bin/env python3\n" + body, encoding="utf-8")
        tan_path.chmod(tan_path.stat().st_mode | stat.S_IEXEC)
    return tan_path


def _write_fake_tan(bin_dir: Path, *, recognized: dict[str, set[str]], missing: set[str]) -> Path:
    """A stub `tan` whose `<verb> --help` prints the given flags for a
    recognized verb (exit 0) or errors like real clap does for `missing`
    verbs (exit 2, message on stderr, nothing on stdout)."""
    lines = [
        "import sys",
        f"RECOGNIZED = {recognized!r}",
        f"MISSING = {missing!r}",
        "verb = sys.argv[1] if len(sys.argv) > 1 else ''",
        # --version is checked before MISSING/RECOGNIZED: it's a real clap
        # global flag, not a subcommand, so it must resolve the same way
        # regardless of what a test happens to put in MISSING/RECOGNIZED
        # (no test currently puts '--version' in MISSING, but the check
        # order shouldn't depend on that).
        "if verb == '--version':",
        "    print('tan 0.0.0-test')",
        "    sys.exit(0)",
        "if verb in MISSING:",
        "    print(f\"error: unrecognized subcommand {verb!r}\", file=sys.stderr)",
        "    sys.exit(2)",
        "if verb in RECOGNIZED:",
        "    print('Options:')",
        "    for f in sorted(RECOGNIZED[verb]):",
        "        print(f'      {f} <VALUE>')",
        "    sys.exit(0)",
        "print(f\"error: unrecognized subcommand {verb!r}\", file=sys.stderr)",
        "sys.exit(2)",
    ]
    return _install_tan_stub(bin_dir, "\n".join(lines) + "\n")


def _write_ansi_colored_fake_tan(bin_dir: Path, *, recognized: dict[str, set[str]]) -> Path:
    """A stub `tan` that prints each flag's two leading dashes split across
    separate ANSI SGR escape runs -- exactly how real Typer/Rich renders
    `--help` when `typer.rich_utils.FORCE_TERMINAL` is true (it checks
    `GITHUB_ACTIONS`/`FORCE_COLOR`/`PY_COLORS`, unconditionally on whether
    stdout is an actual terminal). `--template` becomes
    `\\x1b[1;36m-\\x1b[0m\\x1b[1;36m-template\\x1b[0m`: a literal substring
    search for `--template` over the raw text finds nothing, which is
    exactly the tan-docs-drift false-failure this stub reproduces."""
    lines = [
        "import sys",
        f"RECOGNIZED = {recognized!r}",
        "verb = sys.argv[1] if len(sys.argv) > 1 else ''",
        "if verb == '--version':",
        "    print('tan 0.0.0-test')",
        "    sys.exit(0)",
        "if verb in RECOGNIZED:",
        "    print('Options:')",
        "    for f in sorted(RECOGNIZED[verb]):",
        r"        colored = '\x1b[1;36m' + f[:1] + '\x1b[0m\x1b[1;36m' + f[1:] + '\x1b[0m'",
        "        print(f'      {colored} <VALUE>')",
        "    sys.exit(0)",
        "print(f\"error: unrecognized subcommand {verb!r}\", file=sys.stderr)",
        "sys.exit(2)",
    ]
    return _install_tan_stub(bin_dir, "\n".join(lines) + "\n")


def test_ansi_colored_help_flags_are_still_matched(tmp_path):
    """tan-cli `dev`, real Typer, `GITHUB_ACTIONS=1` -- force-coloured
    `--help` split every flag's own two dashes across separate ANSI runs
    and the raw-stdout substring check in `check_surface` found NOTHING for
    ANY docs/cli.md-tabulated flag of `init`, reporting the whole
    documented surface as missing. This is the exact false-failure, pinned
    against a fake `tan` shaped like the real one rather than the real
    binary, so it runs everywhere with no installed `tan`."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    tan_bin = _write_ansi_colored_fake_tan(tmp_path / "bin", recognized=_ALL_RECOGNIZED)

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "OK" in proc.stdout


def test_install_tan_stub_takes_the_windows_branch_when_forced(tmp_path, monkeypatch):
    """This host is Linux -- a `.bat` cannot actually be executed here, so
    this only proves branch SELECTION and the shim's shape (a PATHEXT-
    recognised name, the right `sys.executable` + script path, `%*` arg
    passthrough, explicit exit-code propagation since cmd.exe does not
    reliably surface the last command's exit code as the .bat's own
    without it). Actually running a `tan.bat` through `subprocess.run` and
    resolving it via `shutil.which()` on real PATHEXT needs a real Windows
    host -- unverified here (alp-sdk#993)."""
    monkeypatch.setattr(os, "name", "nt")
    bin_dir = tmp_path / "winbin"
    body = "import sys\nprint('hi')\nsys.exit(0)\n"
    tan_path = _install_tan_stub(bin_dir, body)

    assert tan_path == bin_dir / "tan.bat"  # PATHEXT default includes .BAT
    assert not (bin_dir / "tan").exists()  # no extensionless file on this branch
    assert (bin_dir / "tan.py").read_text(encoding="utf-8") == body

    bat = tan_path.read_text(encoding="utf-8")
    assert bat.startswith(f'@"{sys.executable}"')
    assert str(bin_dir / "tan.py") in bat
    assert "%*" in bat
    assert "exit /b %errorlevel%" in bat


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


_RICH_HELP_STUB = r'''
import sys

sys.stdout.reconfigure(encoding="utf-8")
sys.stderr.reconfigure(encoding="utf-8")

TABLE = __TABLE__

argv = sys.argv[1:]
if not argv or argv[0] == "--help":
    print("Usage: tan [OPTIONS] COMMAND [ARGS]...")
    sys.exit(0)
if argv[0] == "--version":
    print("tan 9.9.9-stub")
    sys.exit(0)

verb = argv[0]
if verb not in TABLE:
    sys.stderr.write("error: unrecognized subcommand '" + verb + "'\n")
    sys.exit(2)

print(" Usage: tan " + verb + " [OPTIONS]")
print("╭─ Options ──╮")
for flag in sorted(TABLE[verb]):
    print("│ " + flag + "   TEXT   what it does │")
print("╰────╯")
sys.exit(0)
'''


def test_rich_table_help_is_decoded_as_utf8_under_a_non_utf8_locale(tmp_path):
    """Typer renders `--help` as a Rich table whose box-drawing characters
    are not encodable in cp1252 (a Windows console) or ASCII (`LC_ALL=C`).

    Reading that through `subprocess.run(..., text=True)` with no explicit
    `encoding=` decodes with `locale.getencoding()`. That raised
    UnicodeDecodeError inside subprocess's reader thread, leaving
    `proc.stdout` as None so the check died in `_usage_line(None)` -- and,
    against a build whose help decoded far enough to parse, reported flags
    as missing that `--help` plainly lists. Neither showed up in CI, whose
    runner is Linux with a UTF-8 default locale.

    Every other stub in this file prints plain ASCII, which cp1252 and ASCII
    both round-trip unharmed, so none of them can see this class of bug --
    the box-drawing characters are the point of `_RICH_HELP_STUB`. Forcing a
    non-UTF-8 locale is what makes the test able to fail at all: PYTHONUTF8=0
    restores the cp1252 default on Windows, LC_ALL/LANG=C gives ASCII on
    POSIX.
    """
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    bin_dir = tmp_path / "bin"
    _install_tan_stub(
        bin_dir,
        _RICH_HELP_STUB.replace(
            "__TABLE__", repr({v: sorted(f) for v, f in _ALL_RECOGNIZED.items()})
        ),
    )

    env = dict(os.environ)
    env["PATH"] = f"{bin_dir}{os.pathsep}{env.get('PATH', '')}"
    env["PYTHONUTF8"] = "0"
    env["PYTHONCOERCECLOCALE"] = "0"
    env["LC_ALL"] = "C"
    env["LANG"] = "C"
    env.pop("PYTHONIOENCODING", None)
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--repo-root", str(doc_root)],
        capture_output=True, text=True, encoding="utf-8", errors="replace", env=env,
    )

    combined = proc.stdout + proc.stderr
    assert "UnicodeDecodeError" not in combined, combined
    assert "AttributeError" not in combined, combined
    assert "Traceback" not in combined, combined
    # --sdk-root and --som ARE listed in the stub's own Rich options table,
    # so nothing may be reported missing; the locale-decoded run invented
    # exactly these.
    assert "is not listed in" not in combined, combined
    assert "is not a recognised flag" not in combined, combined
    assert "no longer a recognised subcommand" not in combined, combined
    assert proc.returncode == 0, combined


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
    """A FORWARDING verb's `--help` ends its `Usage:` line in `[ARGS...]`
    (the current Python `tan`'s Typer/Click rendering of the catch-all; Clap's
    frozen-Rust spelling `[ARGS]...` is covered separately, see
    `_has_legacy_passthrough_args`) and prints a generic "Arguments forwarded
    verbatim ..." blurb naming a few EXAMPLE flags that belong to OTHER
    forwarding verbs, never its own. docs/cli.md's tabulated flags for that
    verb must never be matched against that blurb -- matching it is both
    noisy (fires regardless of this verb's real flags) and unsafe (stays
    present even if this verb's own forwarded flag support were dropped).
    `tan lock` is a real forwarding verb (tan-cli
    `python/tan/commands/west_forward_cmd.py`); this check must not report
    its docs/cli.md-tabulated flags missing."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    cli_md = doc_root / "docs" / "cli.md"
    cli_md.write_text(
        cli_md.read_text(encoding="utf-8")
        + textwrap.dedent(
            """

            ### `tan lock` -- refresh the dependency lockfile

            | Option | Meaning |
            |---|---|
            | `--sku` | Target SoM SKU |
            """
        ),
        encoding="utf-8",
    )
    readme = doc_root / "README.md"
    readme.write_text(
        readme.read_text(encoding="utf-8") + "\n`tan lock` refreshes the lockfile.\n",
        encoding="utf-8",
    )

    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    _install_tan_stub(
        bin_dir,
        textwrap.dedent(
            """\
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
                "lock": (
                    "Usage: tan lock [OPTIONS] [ARGS...]\\n\\n"
                    "Arguments:\\n  [ARGS...]\\n"
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
    )

    proc = _run(doc_root, bin_dir)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "lock" in proc.stdout  # named by the OK line's skip note
    assert "--sku" not in proc.stdout + proc.stderr


def test_strip_shell_comment_tail_edge_cases():
    """Direct unit coverage of `_strip_shell_comment_tail`'s edges: a
    full-line comment truncates to empty, a trailing comment keeps the code
    before it, a `#` inside a quoted string is data (not a comment), a `#`
    glued to a non-whitespace character is data, and a `#!` shebang is left
    alone -- the exact edges the follow-up task called out by name."""
    strip = _mod._strip_shell_comment_tail
    assert strip("# whole line is a comment") == ""
    assert strip("tan build   # trailing comment") == "tan build   "
    assert strip('echo "value #not-a-comment"') == 'echo "value #not-a-comment"'
    assert strip("echo foo#bar") == "echo foo#bar"
    assert strip("#!/usr/bin/env bash") == "#!/usr/bin/env bash"


def test_flag_row_naming_another_front_door_is_not_attributed_to_section_verb(tmp_path):
    """The real docs/cli.md shape: `### `tan generate`` contrasts its own
    narrow `--target` catalog against the WIDER `python -m alp_cli emit`
    one, and one of that catalog's rows tabulates `--template`/`--sku` --
    real flags of `python -m alp_cli emit`, never `tan generate`. Those
    flags sitting physically inside the `tan generate` section must not be
    demanded of `tan generate --help`."""
    doc_root = tmp_path / "repo"
    (doc_root / "docs").mkdir(parents=True)
    (doc_root / "scripts").mkdir(parents=True)
    (doc_root / "README.md").write_text(
        "`tan generate` writes a config artefact.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "getting-started.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "troubleshooting.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "scripts" / "bootstrap.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
    (doc_root / "docs" / "cli.md").write_text(
        textwrap.dedent(
            """\
            # The `tan` CLI

            ### `tan generate` -- materialise a board-derived config artefact

            | Mode | Artefact | Owned by | Reachable via |
            |---|---|---|---|
            | `zephyr-conf` | Zephyr Kconfig fragment | `alp_project.py` | `tan generate` |
            | `scaffold` | New-project envelope for a template (`--template`/`--sku`) | `alp_project.py` | `python -m alp_cli emit` only |

            | Option | Meaning |
            |---|---|
            | `--target` | Which target to generate |
            """
        ),
        encoding="utf-8",
    )

    recognized = {"generate": {"--target"}}
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "--sku" not in proc.stdout + proc.stderr
    assert "--template" not in proc.stdout + proc.stderr


def test_flag_row_naming_only_its_own_verb_still_catches_drift(tmp_path):
    """Nearest true positive for the front-door-attribution rule above: a
    catalog-shaped row inside `tan generate`'s OWN section whose 'Reachable
    via' column names `tan generate` itself (no other front door) must
    still have its flag checked -- proves the rule skips a row for naming
    ANOTHER front door, not merely for having a 4-column catalog shape."""
    doc_root = tmp_path / "repo"
    (doc_root / "docs").mkdir(parents=True)
    (doc_root / "scripts").mkdir(parents=True)
    (doc_root / "README.md").write_text(
        "`tan generate` writes a config artefact.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "getting-started.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "troubleshooting.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "scripts" / "bootstrap.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
    (doc_root / "docs" / "cli.md").write_text(
        textwrap.dedent(
            """\
            # The `tan` CLI

            ### `tan generate` -- materialise a board-derived config artefact

            | Mode | Artefact | Owned by | Reachable via |
            |---|---|---|---|
            | `dts-overlay` | Board DTS overlay (needs `--force` to overwrite) | `alp_project.py` | `tan generate` |
            """
        ),
        encoding="utf-8",
    )

    recognized = {"generate": set()}  # --force silently dropped
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`tan generate --force`" in proc.stderr
    assert "not listed in" in proc.stderr


def test_comment_inside_bash_fence_does_not_misparse_as_a_subcommand(tmp_path):
    """The real docs/cli.md shape: a `#`-led English sentence inside a
    ```bash fence ('...its own top-level tan verb -- NOT tan generate...')
    must never mint a fake `tan verb` (or `tan generate`) subcommand.
    `_ENGLISH_STOPWORDS` deliberately does not (and must not) contain
    "verb" -- this is handled structurally by stripping the comment tail,
    not by growing the denylist."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    cli_md = doc_root / "docs" / "cli.md"
    cli_md.write_text(
        cli_md.read_text(encoding="utf-8")
        + textwrap.dedent(
            """

            ```bash
            # also: tan kconfig --core m55_he         (its own top-level tan verb --
            #                                           NOT tan generate, which has no
            #                                           front door for this target)
            ```
            """
        ),
        encoding="utf-8",
    )
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=_ALL_RECOGNIZED, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "`tan verb`" not in proc.stdout + proc.stderr
    assert "`tan generate`" not in proc.stdout + proc.stderr


def test_comment_inside_unlabeled_fence_does_not_misparse_as_a_subcommand(tmp_path):
    """Same false-positive shape as the ```bash test above, but the fence
    carries NO language tag at all -- proves comment-tail stripping is not
    gated on a fence-language allowlist (alp-sdk#994-adjacent finding: a
    ```bash/```sh/```shell-only allowlist reproduces the exact same false
    positive one fence tag away, since it's a denylist wearing a different
    hat). `docs/cli.md` has real unlabeled fences (`tan doctor --build`
    sample output) today; a comment sentence in one of those must not
    misparse either."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    cli_md = doc_root / "docs" / "cli.md"
    cli_md.write_text(
        cli_md.read_text(encoding="utf-8")
        + textwrap.dedent(
            """

            ```
            # use its own top-level tan verb -- NOT tan generate
            tan build
            ```
            """
        ),
        encoding="utf-8",
    )
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=_ALL_RECOGNIZED, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "`tan verb`" not in proc.stdout + proc.stderr
    assert "`tan generate`" not in proc.stdout + proc.stderr


def test_multitoken_span_stopword_is_not_treated_as_a_verb_in_cli_md(tmp_path):
    """docs/cli.md's own reference-quality scan trusts a MULTI-token inline
    span (`` `tan <verb> <more>` ``) unconditionally -- before this fix that
    path never subtracted `_ENGLISH_STOPWORDS`, so a stray bare sentence
    like `` `tan is the executor` `` (no fence, no `#`) minted a fake `tan
    is` subcommand. The module docstring names this exact span as the
    residual case `_ENGLISH_STOPWORDS` exists to catch; this proves it
    actually does on the multi-token path, not just the bare one."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    cli_md = doc_root / "docs" / "cli.md"
    cli_md.write_text(
        cli_md.read_text(encoding="utf-8")
        + "\nNote that `tan is the executor` for build slices.\n",
        encoding="utf-8",
    )
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=_ALL_RECOGNIZED, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "`tan is`" not in proc.stdout + proc.stderr


def test_bare_span_stopword_in_table_row_is_not_treated_as_a_verb_in_cli_md(tmp_path):
    """Sibling to the multi-token case above, for the OTHER path that reads
    `_ENGLISH_STOPWORDS` in `extract_cli_md_referenced_subcommands`: a bare
    two-word span (`` `tan is` ``, nothing else) sitting in a table CELL is
    reference-quality by the table-row rule, so a stray stopword there must
    also be filtered -- not just the multi-token sentence case."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    cli_md = doc_root / "docs" / "cli.md"
    cli_md.write_text(
        cli_md.read_text(encoding="utf-8")
        + textwrap.dedent(
            """

            | Note | Detail |
            |---|---|
            | Reminder | `tan is` the standalone executor, not a subcommand |
            """
        ),
        encoding="utf-8",
    )
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=_ALL_RECOGNIZED, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "`tan is`" not in proc.stdout + proc.stderr


def test_front_door_row_skip_is_named_in_ok_line(tmp_path):
    """The front-door row skip (a table row inside a verb's own section that
    names `python -m alp_cli` / `west alp-*` / `alp_orchestrate` and is
    therefore not attributed to the section's own verb) must be visible in
    the OK line, the same way the forwarding-verb flag-check skip already
    is named there -- an unannounced exclusion is easy to mistake for full
    coverage."""
    doc_root = tmp_path / "repo"
    (doc_root / "docs").mkdir(parents=True)
    (doc_root / "scripts").mkdir(parents=True)
    (doc_root / "README.md").write_text(
        "`tan generate` writes a config artefact.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "getting-started.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "troubleshooting.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "scripts" / "bootstrap.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
    (doc_root / "docs" / "cli.md").write_text(
        textwrap.dedent(
            """\
            # The `tan` CLI

            ### `tan generate` -- materialise a board-derived config artefact

            | Mode | Artefact | Owned by | Reachable via |
            |---|---|---|---|
            | `scaffold` | New-project envelope for a template (`--template`/`--sku`) | `alp_project.py` | `python -m alp_cli emit` only |

            | Option | Meaning |
            |---|---|
            | `--target` | Which target to generate |
            """
        ),
        encoding="utf-8",
    )

    recognized = {"generate": {"--target"}}
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "generate" in proc.stdout
    assert "1 row" in proc.stdout


def test_real_command_after_a_stripped_comment_is_still_checked(tmp_path):
    """Nearest true positive for comment-tail stripping: a REAL, un-commented
    `tan kconfig --core m55_he` invocation sitting in the SAME fenced block
    as `#`-led commentary must still enter the checked surface and still
    fail loudly if `tan kconfig` stops existing -- stripping the comment
    lines must not also swallow the real command line beneath them."""
    doc_root = tmp_path / "repo"
    _write_docroot(doc_root)
    cli_md = doc_root / "docs" / "cli.md"
    cli_md.write_text(
        cli_md.read_text(encoding="utf-8")
        + textwrap.dedent(
            """

            ```bash
            # also: tan kconfig --core m55_he         (its own top-level tan verb --
            #                                           NOT tan generate, which has no
            #                                           front door for this target)
            tan kconfig --core m55_he
            ```
            """
        ),
        encoding="utf-8",
    )
    recognized = dict(_ALL_RECOGNIZED)
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing={"kconfig"})

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`tan kconfig`" in proc.stderr
    assert "no longer a recognised subcommand" in proc.stderr


def test_heredoc_comment_line_does_not_misparse_as_a_subcommand(tmp_path):
    """bootstrap.sh's next-steps heredoc mixes commentary with real
    commands the same way docs/cli.md's fenced blocks do -- comment-tail
    stripping must apply to the heredoc body too, not just markdown
    fences, or the same false-positive shape recurs there."""
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

              # Sanity-check the host build environment (needs tan on PATH -- see
              # README.md for the tan-cli `install.sh` one-liner): `tan explain --core`
              # is a different, unrelated tool -- see docs/cli.md.
              tan doctor --build
            EOF
            """
        ),
        encoding="utf-8",
    )
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=_ALL_RECOGNIZED, missing=set())

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "`tan explain`" not in proc.stdout + proc.stderr


def test_retired_verb_bare_prose_mention_does_not_misparse_as_live(tmp_path):
    """The real docs/cli.md shape once a sibling branch retires `tan emit`
    from the how-to-use sections and leaves only the historical note: 'This
    replaces the retired `tan emit` command ...' and 'The rest of the old
    `tan emit` catalog ...' are both bare `` `tan emit` `` spans in ordinary
    prose, with no heading, fence, table cell, or fuller invocation for
    `emit` anywhere in the file. That prose is correct and must not turn
    into a false `tan emit` drift report."""
    doc_root = tmp_path / "repo"
    (doc_root / "docs").mkdir(parents=True)
    (doc_root / "scripts").mkdir(parents=True)
    (doc_root / "README.md").write_text(
        "`tan generate` writes a config artefact.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "getting-started.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "troubleshooting.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "scripts" / "bootstrap.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
    (doc_root / "docs" / "cli.md").write_text(
        textwrap.dedent(
            """\
            # The `tan` CLI

            ### `tan generate` -- materialise a board-derived config artefact

            ```bash
            tan generate --target zephyr-conf
            ```

            This replaces the retired `tan emit` command (a positional `tan
            emit <mode>`, one flat catalog printed to stdout).  The rest of
            the old `tan emit` catalog has no `tan` front door at all.
            """
        ),
        encoding="utf-8",
    )

    # `--target` must be RECOGNIZED (not an empty set): the fixture's own
    # fenced `tan generate --target zephyr-conf` line is now also walked by
    # check_invocation_shapes (added alongside the example-README glob --
    # see that function's docstring), so an empty flag set here would fail
    # this test for an unrelated reason (a fabricated "`--target` unknown"
    # shape problem) and mask what this test actually exercises: the
    # retired-verb bare-prose-mention rule below.
    recognized = {"generate": {"--target"}}
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing={"emit"})

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "`tan emit`" not in proc.stdout + proc.stderr


def test_table_only_verb_with_no_heading_still_catches_drift(tmp_path):
    """Nearest true positive #1 for the docs/cli.md reference-quality rule:
    a verb named ONLY via a bare `` `tan kconfig` `` table CELL -- no
    `### `tan kconfig`` heading, no fence -- must still enter the checked
    surface and still fail loudly if it stops existing (the real corpus
    shape: `kconfig` has no heading of its own, only a 'Reachable via'
    table-cell mention)."""
    doc_root = tmp_path / "repo"
    (doc_root / "docs").mkdir(parents=True)
    (doc_root / "scripts").mkdir(parents=True)
    (doc_root / "README.md").write_text(
        "`tan generate` writes a config artefact.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "getting-started.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "troubleshooting.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "scripts" / "bootstrap.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
    (doc_root / "docs" / "cli.md").write_text(
        textwrap.dedent(
            """\
            # The `tan` CLI

            ### `tan generate` -- materialise a board-derived config artefact

            | Mode | Reachable via |
            |---|---|
            | `kconfig` | `west alp-emit`, `tan kconfig` |
            """
        ),
        encoding="utf-8",
    )

    recognized = {"generate": set()}
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing={"kconfig"})

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`tan kconfig`" in proc.stderr
    assert "no longer a recognised subcommand" in proc.stderr


def test_multitoken_invocation_in_prose_with_no_heading_still_catches_drift(tmp_path):
    """Nearest true positive #2 for the docs/cli.md reference-quality rule:
    a verb named via a flag/arg-bearing span in ordinary prose (`` `tan
    kconfig --core <id>` `` -- a fuller invocation, not a passing name-drop)
    -- no heading, no table, no fence -- must still enter the checked
    surface and still fail loudly if it stops existing."""
    doc_root = tmp_path / "repo"
    (doc_root / "docs").mkdir(parents=True)
    (doc_root / "scripts").mkdir(parents=True)
    (doc_root / "README.md").write_text(
        "`tan generate` writes a config artefact.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "getting-started.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "docs" / "troubleshooting.md").write_text(
        "No commands documented yet.\n", encoding="utf-8",
    )
    (doc_root / "scripts" / "bootstrap.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
    (doc_root / "docs" / "cli.md").write_text(
        textwrap.dedent(
            """\
            # The `tan` CLI

            ### `tan generate` -- materialise a board-derived config artefact

            You can also run it as the top-level `tan kconfig --core <id>`
            verb (wraps this emit in a friendlier UI).
            """
        ),
        encoding="utf-8",
    )

    recognized = {"generate": set()}
    tan_bin = _write_fake_tan(tmp_path / "bin", recognized=recognized, missing={"kconfig"})

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`tan kconfig`" in proc.stderr
    assert "no longer a recognised subcommand" in proc.stderr


# --- check_invocation_shapes: the alp-sdk#1137-round-2 regression coverage -
#
# Round 1 fixed the three example READMEs its issue named and missed six
# more of the identical `tan build <path>` / `tan build --board <sku> <path>`
# shape sitting in OTHER example READMEs -- files check_tan_docs_surface
# never scanned (DOC_SOURCES only ever listed four top-level docs) and whose
# bug shape (positional arg / unrecognised flag) the existence-only check
# structurally cannot see. These tests exercise the fix for both halves:
# EXAMPLE_README_GLOB widening what gets scanned, and check_invocation_shapes
# widening what gets checked once scanned.

_MINIMAL_TAN_HELP = "Usage: tan [OPTIONS] <COMMAND>\n\nOptions:\n      --project <PATH>\n"


def _write_minimal_docroot(root: Path, example_readme_body: str) -> Path:
    """The smallest doc tree `check_invocation_shapes` (and the
    `check_surface` existence pass that always runs alongside it in `main`)
    both need: the four DOC_SOURCES files (near-empty, no verb of their own,
    so they don't add noise to the checked surface) plus ONE example
    project's README carrying whatever `tan ...` invocation a test wants to
    exercise. Returns the example README's path."""
    (root / "docs").mkdir(parents=True)
    (root / "scripts").mkdir(parents=True)
    # A bare `tan build` mention keeps `collect_documented_surface` (the
    # existence pass check_surface always runs alongside the shape check)
    # from tripping its own "extraction is broken" guard: `_TAN_INVOCATION_RE`
    # requires a lowercase-letter token right after `tan `, so a shape test
    # whose ONLY invocation is `tan --project <path> build` (the flag comes
    # first) contributes nothing to that regex and would otherwise leave the
    # existence surface empty -- a test-fixture edge case, not a real-corpus
    # one (the real docs always have a simpler `tan build` mention elsewhere
    # too).
    (root / "README.md").write_text("`tan build` builds a project.\n", encoding="utf-8")
    (root / "docs" / "cli.md").write_text("# The `tan` CLI\n", encoding="utf-8")
    (root / "docs" / "getting-started.md").write_text("No commands here.\n", encoding="utf-8")
    (root / "docs" / "troubleshooting.md").write_text("No commands here.\n", encoding="utf-8")
    (root / "scripts" / "bootstrap.sh").write_text("#!/usr/bin/env bash\n", encoding="utf-8")
    example_dir = root / "examples" / "peripheral-io" / "widget-blink"
    example_dir.mkdir(parents=True)
    readme = example_dir / "README.md"
    readme.write_text(example_readme_body, encoding="utf-8")
    return readme


def _write_shape_fake_tan(bin_dir: Path, help_by_verb: dict[str, str]) -> Path:
    """A stub `tan` that returns CALLER-SUPPLIED full `--help` text (a real
    `Usage:` line plus a real `Options:` block) per verb -- gives shape
    tests control over a verb's positional marker and per-flag arity, which
    `_write_fake_tan`'s generic recognized/missing dict cannot express (it
    never prints a `Usage:` line, so every verb it stubs looks positional-
    less regardless of what's being tested)."""
    lines = [
        "import sys",
        f"HELP = {help_by_verb!r}",
        f"GLOBAL_HELP = {_MINIMAL_TAN_HELP!r}",
        "argv = sys.argv[1:]",
        "if argv == ['--version']:",
        "    print('tan 0.0.0-test'); sys.exit(0)",
        "if argv == ['--help']:",
        "    print(GLOBAL_HELP); sys.exit(0)",
        "verb = argv[0] if argv else ''",
        "if verb in HELP:",
        "    print(HELP[verb]); sys.exit(0)",
        "print(f\"error: unrecognized subcommand {verb!r}\", file=sys.stderr)",
        "sys.exit(2)",
    ]
    return _install_tan_stub(bin_dir, "\n".join(lines) + "\n")


_BUILD_HELP = (
    "Usage: tan build [OPTIONS]\n\n"
    "Options:\n      --project <PATH>\n      --native\n"
)
_FLASH_HELP = (
    "Usage: tan flash [OPTIONS] [APP_PATH]\n\n"
    "Options:\n      --project <PATH>\n      --helper <NAME>\n"
)
_NEW_SOM_HELP = (
    "Usage: tan new-som [OPTIONS]\n\n"
    "Options:\n      --sku <SKU>\n      --dry-run\n"
)
_LOCK_HELP = (
    "Usage: tan lock [OPTIONS] [ARGS...]\n\n"
    "Arguments:\n  [ARGS...]\n"
    "          Arguments forwarded verbatim to the underlying command\n"
)

_RICH_BUILD_HELP = """\
                                                                                
 Usage: tan build [OPTIONS]                                                     
                                                                                
 Build every slice of the project's build plan.                                 
                                                                                
╭─ Options ────────────────────────────────────────────────────────────────────╮
│ --plan-from                FILE         Read the build plan from a JSON file │
│                                         instead of invoking the SDK planner. │
│                                         Implies --plan: shows the plan and   │
│                                         exits unless --materialise or        │
│                                         --execute is given.                  │
│ --materialise                           Write the plan's generated files     │
│                                         (shared artefacts + per-slice        │
│                                         config) under the build root and     │
│                                         stop, instead of just showing the    │
│                                         plan.                                │
│ --native                                Build natively: materialise the      │
│                                         plan, then run each slice's command. │
│                                         The default when no plan-mode flag   │
│                                         is given. Like v0.4.1, this does NOT │
│                                         override the --plan implied by       │
│                                         --plan-from -- use --execute for     │
│                                         that.                                │
│ --execute                               Materialise the plan AND run each    │
│                                         slice's command, even when the plan  │
│                                         came from --plan-from -- run a       │
│                                         pinned, reviewed plan file           │
│                                         reproducibly. Implies --materialise  │
│                                         (nothing can run that was never      │
│                                         written); reports the ordinary build │
│                                         result. ADDED BY THIS PORT, not a    │
│                                         v0.4.1 flag: there --plan-from       │
│                                         implies --plan and outranks          │
│                                         --native, so a file-supplied plan    │
│                                         cannot be dispatched at all.         │
│                                         Deliberate, not a parity gap.        │
│ --build-root               DIR          Project tree the slices run under    │
│                                         and artefacts are written below      │
│                                         (default: the board.yaml's           │
│                                         directory, else the current          │
│                                         directory).                          │
│ --sdk-root                 PATH         alp-sdk checkout root.               │
│ --board-yaml               PATH         Explicit board.yaml path.            │
│ --project                  PATH         Project root (defaults to '.').      │
│ --format                   <text|json>  Output format. [default: text]       │
│ --plan                                  Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --target                   EMIT         Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --all                                   Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --manifest                              Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --manifest-from            FILE         Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --no-auto-bootstrap                     Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --pristine                              Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --verbose                               Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --quiet                                 Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --no-color                              Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --non-interactive                       Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --ci                                    Deferred, not implemented in this    │
│                                         build (tan-cli#427).                 │
│ --help                                  Show this message and exit.          │
╰──────────────────────────────────────────────────────────────────────────────╯
"""

_RICH_FLASH_HELP = """\
                                                                                
 Usage: tan flash [OPTIONS] [APP_PATH]                                          
                                                                                
 Program every slice + helper MCU in the project's system manifest.             
                                                                                
╭─ Arguments ──────────────────────────────────────────────────────────────────╮
│   APP_PATH      <str>  Application source directory (default: the current    │
│                        directory). `build_root` defaults to                  │
│                        <APP_PATH>/build.                                     │
│                        [default: .]                                          │
╰──────────────────────────────────────────────────────────────────────────────╯
╭─ Options ────────────────────────────────────────────────────────────────────╮
│ --project                   PATH         Project root (defaults to '.').     │
│ --build-root                PATH         Override the build root holding     │
│                                          system-manifest.yaml (default:      │
│                                          <APP_PATH>/build).                  │
│ --sdk-root                  PATH         alp-sdk checkout root.              │
│ --board-yaml                PATH         Explicit board.yaml path.           │
│ --core                      CORE_ID      Flash only the slice with this      │
│                                          core_id (skips every other slice    │
│                                          AND all helpers).                   │
│ --helper                    NAME         Flash only the helper MCU with this │
│                                          name (skips ALL slices and every    │
│                                          other helper).                      │
│ --dry-run                                Print the flash command each        │
│                                          backend WOULD run and return ok     │
│                                          without spawning; also bypasses the │
│                                          required-tool PATH gate.            │
│ --skip-missing-tools                     When a backend's required tools are │
│                                          all absent from PATH, warn + skip   │
│                                          the entry instead of failing it. No │
│                                          effect under --dry-run.             │
│ --setools-dir               PATH         Alif SETOOLS install used to        │
│                                          auto-sign a Flow D slot0 ATOC       │
│                                          (license-gated; obtained from Alif, │
│                                          never redistributed by tan).        │
│                                          Precedence: this flag, then the     │
│                                          SETOOLS_DIR environment variable,   │
│                                          then flash_args.setools_dir in the  │
│                                          manifest (lowest -- and rebuilt     │
│                                          over by the next `tan build`, see   │
│                                          docs/setools.md).                   │
│ --format                    <text|json>  Output format.                      │
│ --help                                   Show this message and exit.         │
╰──────────────────────────────────────────────────────────────────────────────╯
"""


def test_typer_rich_help_option_arity_is_parsed():
    """The Python port renders box-drawing tables, not Clap's `Options:` rows.
    `_RICH_BUILD_HELP` is a VERBATIM capture of a real `tan build --help` at
    `COLUMNS=80` (`v0.5.0`), not a hand-composed, artificially tidy box --
    a hand-composed fixture with a narrower box and no multi-line HELP-TEXT
    wrapping passed while the real, wider, longer-description output tripped
    the parser (tan-cli's real Rich renderer wraps a flag's own DESCRIPTION
    across multiple continuation rows for a long one, e.g. `--plan-from` /
    `--execute` below -- none of those continuation rows start with `--`, so
    they must be silently skipped rather than mis-parsed as a second flag)."""
    assert _mod._parse_option_arity(_RICH_BUILD_HELP) == {
        "--plan-from": True,
        "--materialise": False,
        "--native": False,
        "--execute": False,
        "--build-root": True,
        "--sdk-root": True,
        "--board-yaml": True,
        "--project": True,
        "--format": True,
        "--plan": False,
        "--target": True,
        "--all": False,
        "--manifest": False,
        "--manifest-from": True,
        "--no-auto-bootstrap": False,
        "--pristine": False,
        "--verbose": False,
        "--quiet": False,
        "--no-color": False,
        "--non-interactive": False,
        "--ci": False,
        "--help": False,
    }


# Real capture, `COLUMNS=80`, `tan validate --help` (`v0.5.0`): Rich wraps a
# long `<a|b|c|d>` choice metavar itself across the option's own continuation
# row (`<text|json|diagnostic-v1|sa` / `rif>`) when the box is too narrow for
# it -- the ORIGINAL, narrower-box `_RICH_BUILD_HELP` fixture never had a
# metavar long enough to wrap, so this exact shape had no regression coverage
# until now. `check_tan_docs_surface: OK` against a real `tan 0.5.0` depends
# on `--format` still registering `takes_value=True` here.
_RICH_WRAPPED_METAVAR_HELP = """\
 Usage: tan validate [OPTIONS]

╭─ Options ────────────────────────────────────────────────────────────────────╮
│ --offline                                        Run only the structural     │
│                                                  checks that ship in tan.    │
│ --project           PATH                         Project root (defaults to   │
│                                                  '.').                       │
│ --board-yaml        PATH                         Explicit board.yaml path.   │
│ --sdk-root          PATH                         alp-sdk checkout root.      │
│ --format            <text|json|diagnostic-v1|sa  Output format.              │
│                     rif>                         [default: text]             │
│ --help                                           Show this message and exit. │
╰──────────────────────────────────────────────────────────────────────────────╯
"""


def test_wrapped_metavar_option_still_registers_as_taking_a_value():
    arity = _mod._parse_option_arity(_RICH_WRAPPED_METAVAR_HELP)
    assert arity["--format"] is True
    assert arity["--offline"] is False


# Real capture, `COLUMNS=80`, `tan model --help` (`v0.5.0`): Typer/Rich joins
# two long names for the SAME option with a bare comma and NO space
# (`--board,--board-yaml`) -- distinct from the `-w, --flag` short-alias
# comma-SPACE form the original regex modelled. Both names must resolve to
# the identical arity, or a doc using whichever alias the regex missed would
# be misjudged (this exact shape is why `tan model build --board
# path/to/board.yaml --out build/models` in docs/cli.md false-failed).
_RICH_COMMA_JOINED_ALIASES_HELP = """\
 Usage: tan model [OPTIONS] [SUBCOMMAND]

╭─ Options ────────────────────────────────────────────────────────────────────╮
│ --board,--board-yaml        PATH         Path to board.yaml.                 │
│                                          [default: board.yaml]               │
│ --out                       PATH         Output directory.                   │
│                                          [default: build/models]             │
│ --metadata-root             PATH         Path to the metadata/ root          │
│                                          (default: <sdk-root>/metadata).     │
│ --project                   PATH         Project root (defaults to '.').     │
│ --sdk-root                  PATH         alp-sdk checkout root.              │
│ --format                    <text|json>  Output format. [default: text]      │
│ --help                                   Show this message and exit.         │
╰──────────────────────────────────────────────────────────────────────────────╯
"""


def test_comma_joined_long_aliases_both_register_the_same_arity():
    arity = _mod._parse_option_arity(_RICH_COMMA_JOINED_ALIASES_HELP)
    assert arity["--board"] is True
    assert arity["--board-yaml"] is True


def test_typer_rich_help_usage_preserves_positional_contract():
    assert _mod._usage_line(_RICH_BUILD_HELP) == "Usage: tan build [OPTIONS]"
    assert not _mod._verb_accepts_positional(_mod._usage_line(_RICH_BUILD_HELP))
    assert _mod._verb_accepts_positional(_mod._usage_line(_RICH_FLASH_HELP))


def test_typer_rich_help_drives_full_invocation_check():
    cache = {"build": _RICH_BUILD_HELP}
    assert (
        _mod._check_one_invocation(
            "tan build --project examples/widget --sdk-root /sdk --native",
            {},
            cache,
            "unused",
        )
        is None
    )
    problem = _mod._check_one_invocation(
        "tan build examples/widget", {}, cache, "unused"
    )
    assert problem is not None
    assert "takes no positional argument" in problem


def test_example_readme_positional_argument_on_build_fails(tmp_path):
    """The exact alp-sdk#1137 round-1 residual shape: `tan build <path>` in
    an example project's own README -- a file EXAMPLE_README_GLOB now scans
    that the four original DOC_SOURCES entries never covered. `build`'s real
    Usage line takes no positional, so this must fail, naming the file and
    the rejected invocation."""
    doc_root = tmp_path / "repo"
    readme = _write_minimal_docroot(
        doc_root,
        "# widget-blink\n\n```bash\ntan build examples/peripheral-io/widget-blink\n```\n",
    )
    tan_bin = _write_shape_fake_tan(tmp_path / "bin", {"build": _BUILD_HELP})

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert str(readme.relative_to(doc_root)) in proc.stderr
    assert "takes no positional argument" in proc.stderr


def test_example_readme_project_flag_fixes_it(tmp_path):
    """The proven fix for the case above: `--project <path>` instead of a
    bare positional. Nearest-true-negative pair to the failing test above --
    same doc content shape, same fake `tan`, only the invocation's syntax
    changes; must now pass, proving the check isn't just failing on every
    example README unconditionally."""
    doc_root = tmp_path / "repo"
    _write_minimal_docroot(
        doc_root,
        "# widget-blink\n\n"
        "```bash\ntan build --project examples/peripheral-io/widget-blink\n```\n",
    )
    tan_bin = _write_shape_fake_tan(tmp_path / "bin", {"build": _BUILD_HELP})

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_unrecognised_flag_on_build_fails(tmp_path):
    """The other alp-sdk#1137 round-1 residual shape: `tan build --board
    <sku>` -- `--board` is real on some OTHER verbs (`tan size`/`tan
    renode`) but never `tan build`. Proves the flag check is genuinely
    per-verb, not "the flag exists somewhere in tan"."""
    doc_root = tmp_path / "repo"
    _write_minimal_docroot(
        doc_root,
        "# widget-blink\n\n```bash\ntan build --board alp_e1m_aen301_m55_he\n```\n",
    )
    tan_bin = _write_shape_fake_tan(tmp_path / "bin", {"build": _BUILD_HELP})

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode != 0
    assert "`--board` is not a recognised flag of `tan build`" in proc.stderr


def test_global_project_flag_before_the_subcommand_passes(tmp_path):
    """`tan --project <path> build` -- a real, live-`tan`-proven ordering
    (the global `--project` flag before the subcommand token). Proves
    `_find_verb` correctly walks past a leading global flag+value pair to
    find the actual verb, rather than misreading `--project`'s VALUE token
    as the verb (which would falsely resolve to a nonexistent `examples/...`
    subcommand and report a spurious "no longer recognised" problem)."""
    doc_root = tmp_path / "repo"
    _write_minimal_docroot(
        doc_root,
        "# widget-blink\n\n"
        "```bash\ntan --project examples/peripheral-io/widget-blink build\n```\n",
    )
    tan_bin = _write_shape_fake_tan(tmp_path / "bin", {"build": _BUILD_HELP})

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_verb_with_a_real_positional_accepts_one(tmp_path):
    """`tan flash`'s own `--help` genuinely carries `[APP_PATH]` after
    `[OPTIONS]` (verified by hand against a real, installed tan) -- a bare
    positional path is legal there, unlike `build`. Proves the check reads
    the per-verb Usage line rather than assuming every verb is positional-
    less."""
    doc_root = tmp_path / "repo"
    _write_minimal_docroot(
        doc_root,
        "# widget-blink\n\n"
        "```bash\ntan flash examples/peripheral-io/widget-blink --helper gd32_bridge\n```\n",
    )
    tan_bin = _write_shape_fake_tan(
        tmp_path / "bin", {"build": _BUILD_HELP, "flash": _FLASH_HELP}
    )

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_forwarding_verb_shape_is_never_checked(tmp_path):
    """A FORWARDING verb's `[ARGS...]` catch-all (Typer/Click's rendering,
    see `_has_legacy_passthrough_args`) means ANYTHING after it is legal by
    design -- `tan lock` forwards verbatim to the underlying command, whose
    own flags never appear in `tan lock`'s own --help. This must never be
    reported as an unrecognised-flag or stray-positional problem."""
    doc_root = tmp_path / "repo"
    _write_minimal_docroot(
        doc_root,
        "# widget-blink\n\n```bash\ntan lock --core m55 somepath\n```\n",
    )
    tan_bin = _write_shape_fake_tan(
        tmp_path / "bin", {"build": _BUILD_HELP, "lock": _LOCK_HELP}
    )

    proc = _run(doc_root, tmp_path / "bin")
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_new_som_options_only_usage_is_not_treated_as_forwarding(tmp_path):
    """`tan new-som` is a native `[OPTIONS]`-only verb, not a FORWARDING one
    -- its real docs/cli.md-tabulated flags must be mechanically checked
    against its own --help, not skipped as a forwarding blurb. Regression
    guard for the drift where `[ARGS]...`/`[ARGS...]` matching was ambiguous
    enough to swallow verbs that never carry the catch-all at all."""
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
            | `--dry-run` | Preview without writing |
            | `--missing-flag` | Not really in --help |
            """
        ),
        encoding="utf-8",
    )
    readme = doc_root / "README.md"
    readme.write_text(
        readme.read_text(encoding="utf-8") + "\n`tan new-som` ports a SoM.\n",
        encoding="utf-8",
    )
    tan_bin = _write_shape_fake_tan(
        tmp_path / "bin", {"build": _BUILD_HELP, "new-som": _NEW_SOM_HELP}
    )

    proc = _run(doc_root, tan_bin.parent)
    # A flag docs/cli.md tabulates but --help never lists must be reported --
    # proving new-som's flags are actually checked, not skipped as forwarding.
    assert proc.returncode != 0
    assert "--missing-flag" in proc.stdout + proc.stderr


def test_sample_output_in_an_untagged_fence_is_not_treated_as_an_invocation(tmp_path):
    """`docs/cli.md`'s real shape: `tan doctor`'s OWN printed report header
    (`  tan doctor  native-host . none`) sits in an UNTAGGED ``` fence
    showing sample OUTPUT, not a command to type. Without the fence-language
    allowlist in `extract_tan_invocations`, this reads as `tan doctor
    native-host . none` -- a positional on a verb (`doctor`) that takes
    none -- and manufactures a false failure with nothing to do with what a
    customer types. Direct unit test of the pure extractor (not the full
    `main()` path) -- proves the untagged block contributes NOTHING, not
    just that some other passing thing outweighs it."""
    untagged_output_block = (
        "# doctor\n\n"
        "```\n"
        "  tan doctor  native-host . none\n\n"
        "  [+]  workspaceRoot   /work/alp-sdk\n"
        "```\n"
    )
    assert _mod.extract_tan_invocations(untagged_output_block) == []


def test_bash_tagged_fence_invocation_is_still_extracted(tmp_path):
    """Sibling of the untagged-fence test above: a ```bash-tagged fence
    (the real, consistent convention every example README + docs/cli.md's
    own runnable snippets use) must still be scanned -- proves the fence-
    language allowlist narrows correctly rather than accidentally emptying
    the whole extractor."""
    tagged_block = "```bash\ntan build --project examples/peripheral-io/widget-blink\n```\n"
    found = _mod.extract_tan_invocations(tagged_block)
    assert found == ["tan build --project examples/peripheral-io/widget-blink"]
