# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/check_cross_platform.py.

Covers:
  - pattern detection for every category the linter knows about
    (LINUX-ONLY-IDIOM: /dev/tty*, ~/.bashrc, make-in-tutorial,
    forward-slash /home / /Users absolute paths; BASH-ONLY-SHEBANG)
  - the INTENTIONALLY_BASH_HELPERS whitelist + header-note
    requirement (whitelisted scripts with a header note pass;
    whitelisted scripts without a note still warn; non-whitelisted
    .sh files always warn)
  - default exit code is 0 even with findings (soft-warn semantics)
  - --fail-on-warning flips exit to 1 when findings exist
  - --quiet suppresses per-finding output, summary still printed
  - --json emits JSONL (one finding per line, no summary)
  - exclude-prefix handling (path components, not raw prefixes)
  - real-tree smoke: the linter runs cleanly against the live repo
    (exit 0 by default, regardless of findings count)

Run locally:

    python -m pytest tests/scripts/test_check_cross_platform.py -v
"""

from __future__ import annotations

import json
import subprocess
import sys
import textwrap
from pathlib import Path

import pytest


REPO = Path(__file__).resolve().parents[2]
LINTER = REPO / "scripts" / "check_cross_platform.py"

# Import the linter module directly so we can unit-test its
# helpers without spawning subprocesses for every case.
sys.path.insert(0, str(REPO / "scripts"))
import check_cross_platform as linter  # noqa: E402


# ---------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------


def _write(tmp: Path, name: str, body: str) -> Path:
    """Write a file under tmp with dedented body.  Returns the path."""
    path = tmp / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _run(*args: str) -> subprocess.CompletedProcess[str]:
    """Invoke the linter as a subprocess."""
    return subprocess.run(
        [sys.executable, str(LINTER), *args],
        capture_output=True, text=True, check=False,
    )


# ---------------------------------------------------------------------
# 1. Pattern: hard-coded /dev/tty* / /dev/cu* in docs
# ---------------------------------------------------------------------


def test_detect_dev_ttyusb_in_md(tmp_path: Path) -> None:
    """`/dev/ttyUSB0` in a .md file is flagged."""
    p = _write(tmp_path, "doc.md", """
        # Flashing

        Connect to /dev/ttyUSB0 with minicom.
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "LINUX-ONLY-IDIOM"
    assert "/dev/ttyUSB0" in findings[0].matched_text


def test_detect_dev_ttyacm_in_md(tmp_path: Path) -> None:
    """`/dev/ttyACM*` is also flagged (J-Link / DAPLink default)."""
    p = _write(tmp_path, "doc.md", "Use /dev/ttyACM0 for the J-Link.\n")
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert "/dev/ttyACM0" in findings[0].matched_text


def test_detect_dev_cu_macos_in_md(tmp_path: Path) -> None:
    """`/dev/cu.*` (macOS serial path) is also flagged -- still
    not cross-platform; the doc should use a placeholder."""
    p = _write(tmp_path, "doc.md", "Open /dev/cu.usbserial-DM12345\n")
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1


def test_dev_null_not_flagged(tmp_path: Path) -> None:
    """`/dev/null` is cross-shell (PowerShell 7+ has $null and
    accepts /dev/null in some contexts) and is not flagged."""
    p = _write(tmp_path, "doc.md", "Redirect to /dev/null to discard.\n")
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


def test_dev_paths_not_flagged_in_python(tmp_path: Path) -> None:
    """Python source isn't scanned -- the linter only walks
    .md and .sh files."""
    p = _write(tmp_path, "foo.py", "OPEN = '/dev/ttyUSB0'\n")
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


# ---------------------------------------------------------------------
# 2. Pattern: ~/.bashrc / ~/.profile / ~/.bash_profile
# ---------------------------------------------------------------------


def test_detect_bashrc_in_md(tmp_path: Path) -> None:
    """`~/.bashrc` in a .md file is flagged."""
    p = _write(tmp_path, "doc.md", """
        Add to your ~/.bashrc:

        ```bash
        export FOO=bar
        ```
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert "bashrc" in findings[0].matched_text


def test_detect_profile_in_md(tmp_path: Path) -> None:
    """`~/.profile` is also flagged (Linux-only convention)."""
    p = _write(tmp_path, "doc.md", "Edit ~/.profile to persist.\n")
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert "profile" in findings[0].matched_text


def test_zshrc_not_flagged(tmp_path: Path) -> None:
    """`~/.zshrc` is the macOS-Catalina+ default; not flagged."""
    p = _write(tmp_path, "doc.md", "Add to your ~/.zshrc on macOS.\n")
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


# ---------------------------------------------------------------------
# 3. Pattern: bash shebang on .sh files
# ---------------------------------------------------------------------


def test_detect_bash_shebang_in_sh(tmp_path: Path) -> None:
    """A .sh file with #!/usr/bin/env bash and no whitelist entry
    is flagged."""
    p = _write(tmp_path, "myscript.sh", """
        #!/usr/bin/env bash
        echo hello
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "BASH-ONLY-SHEBANG"


def test_bin_bash_shebang_flagged(tmp_path: Path) -> None:
    """`#!/bin/bash` is also flagged (same Linux assumption)."""
    p = _write(tmp_path, "myscript.sh", """
        #!/bin/bash
        echo hi
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1


def test_bash_shebang_whitelist_with_note_suppressed() -> None:
    """A whitelisted helper (bootstrap.sh) that has the header
    cross-platform note in its first 30 lines is suppressed
    -- this exercises the INTENTIONALLY_BASH_HELPERS path."""
    # We unit-test the helper directly rather than via tmp_path
    # because the whitelist keys are hard-coded paths under scripts/.
    # Create a fake bootstrap.sh in a controlled tree, point the
    # scanner at it with the appropriate relative path computation.
    # Easier: confirm at least one of the actual whitelisted scripts
    # exists in tree, then assert it produces 0 findings IF it
    # contains a cross-platform note.
    bootstrap = REPO / "scripts" / "bootstrap.sh"
    if not bootstrap.exists():
        pytest.skip("scripts/bootstrap.sh not present in tree")
    has_note = linter._bash_helper_has_note(bootstrap)
    findings = linter.scan([bootstrap], base=REPO)
    if has_note:
        # Header note present -> no findings.
        bash_findings = [
            f for f in findings if f.category == "BASH-ONLY-SHEBANG"
        ]
        assert not bash_findings, (
            "whitelisted helper with header note still produced "
            f"shebang findings: {bash_findings}"
        )


def test_bash_shebang_whitelist_without_note_still_flagged(
    tmp_path: Path,
) -> None:
    """If a whitelisted helper LACKS the cross-platform header
    note, the lint still warns (different message)."""
    # Synthesise a whitelisted-path situation: make a fake
    # `scripts/bootstrap.sh` under tmp_path that has no header
    # note, then scan it with the tmp tree as base + the path
    # as scripts/bootstrap.sh.  This exercises the whitelist
    # branch end-to-end.
    p = _write(tmp_path, "scripts/bootstrap.sh", """
        #!/usr/bin/env bash
        # SPDX-License-Identifier: Apache-2.0
        # Some helper that has no note about other host OSes.
        echo hello
    """)
    findings = linter.scan([p], base=tmp_path)
    bash = [f for f in findings if f.category == "BASH-ONLY-SHEBANG"]
    assert len(bash) == 1
    assert "header note" in bash[0].suggestion


# ---------------------------------------------------------------------
# 4. Pattern: `make` invocations in tutorial markdown
# ---------------------------------------------------------------------


def test_detect_make_invocation_in_tutorial(tmp_path: Path) -> None:
    """A bare `make build` in a fenced block is flagged."""
    p = _write(tmp_path, "doc.md", """
        # Build it

        ```bash
        make build
        ```
    """)
    findings = linter.scan([p], base=tmp_path)
    make_findings = [
        f for f in findings if "make" in f.matched_text
    ]
    assert len(make_findings) >= 1


def test_make_prose_outside_fence_not_flagged(tmp_path: Path) -> None:
    """Regression for issue #451: prose that reflows onto a new
    markdown line starting with the verb "make" (not the build tool)
    must NOT be flagged.  Real example that shipped in
    examples/aen/aen-rpc-pingpong/README.md:52 before the fix:
    a paragraph wraps as `...RESET=y`\\n`make the shared ... coherent`.
    """
    p = _write(tmp_path, "doc.md", """
        # Transport notes

        `CONFIG_DCACHE=n` + `CONFIG_IPC_SERVICE_BACKEND_RPMSG_SHMEM_RESET=y`
        make the shared `sram_ipc0` vrings coherent + zeroed.

        Sentence that says you should make sure everything works before
        you make the change.
    """)
    findings = linter.scan([p], base=tmp_path)
    make_findings = [f for f in findings if "make" in f.matched_text]
    assert make_findings == [], (
        f"prose starting with 'make the'/'make sure' outside a fenced "
        f"code block must not be flagged as a make invocation: "
        f"{make_findings}"
    )


def test_make_invocation_still_flagged_inside_fence_after_prose(
    tmp_path: Path,
) -> None:
    """The fence-scoping fix must not blind the linter to a REAL
    `make` invocation that happens to share a file with make-prose."""
    p = _write(tmp_path, "doc.md", """
        Prose: make the change carefully.

        ```bash
        make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image
        ```
    """)
    findings = linter.scan([p], base=tmp_path)
    make_findings = [f for f in findings if "make" in f.matched_text]
    assert len(make_findings) == 1
    assert "ARCH=arm64" in make_findings[0].matched_text


def test_compute_fence_lines_helper() -> None:
    """`_compute_fence_lines` returns only the CONTENT lines between
    a ``` open/close pair, not the fence markers themselves."""
    text = (
        "prose line 1\n"        # line 1
        "```bash\n"             # line 2 -- fence open
        "make foo\n"            # line 3 -- inside
        "echo hi\n"             # line 4 -- inside
        "```\n"                 # line 5 -- fence close
        "prose line 2\n"        # line 6
    )
    assert linter._compute_fence_lines(text) == {3, 4}


# ---------------------------------------------------------------------
# 5. Pattern: forward-slash absolute paths in markdown
# ---------------------------------------------------------------------


def test_detect_home_absolute_path(tmp_path: Path) -> None:
    """`/home/user/...` in a code block is flagged."""
    p = _write(tmp_path, "doc.md", """
        ```bash
        export FOO=/home/alice/projects/foo
        ```
    """)
    findings = linter.scan([p], base=tmp_path)
    assert any("/home/alice" in f.matched_text for f in findings)


def test_detect_users_absolute_path(tmp_path: Path) -> None:
    """`/Users/bob/...` (macOS home) is also flagged -- still
    not cross-platform."""
    p = _write(tmp_path, "doc.md", "cd /Users/bob/dev/foo\n")
    findings = linter.scan([p], base=tmp_path)
    assert any("/Users/bob" in f.matched_text for f in findings)


# ---------------------------------------------------------------------
# 6. Exclude handling
# ---------------------------------------------------------------------


def test_exclude_prefix_skips_subtree(tmp_path: Path) -> None:
    """A path under an excluded prefix is not walked."""
    _write(tmp_path, "vendors/bad.md", "Connect to /dev/ttyUSB0\n")
    _write(tmp_path, "docs/good.md", "Clean content.\n")
    files = linter.discover_files(
        [tmp_path], excludes=("vendors",), base=tmp_path,
    )
    paths = {f.relative_to(tmp_path).as_posix() for f in files}
    assert "docs/good.md" in paths
    assert "vendors/bad.md" not in paths


def test_default_excludes_include_superpowers_plans(tmp_path: Path) -> None:
    """Regression for issue #451: docs/superpowers/plans/ carries dated
    bench-session planning notes with real, personal paths (e.g.
    /home/alplab/..., /Users/caner/...) -- archival working notes, not
    customer tutorials.  Excluded by default, parallel to the existing
    docs/superpowers/specs/ carve-out and to
    lint_doc_yaml_fragments.py's default excludes."""
    assert "docs/superpowers/plans" in linter.DEFAULT_EXCLUDES
    _write(
        tmp_path,
        "docs/superpowers/plans/2026-01-01-example.md",
        "workspace: /home/alplab/zephyrproject\n",
    )
    _write(tmp_path, "docs/good.md", "Clean content.\n")
    files = linter.discover_files(
        [tmp_path], excludes=linter.DEFAULT_EXCLUDES, base=tmp_path,
    )
    paths = {f.relative_to(tmp_path).as_posix() for f in files}
    assert "docs/good.md" in paths
    assert "docs/superpowers/plans/2026-01-01-example.md" not in paths


def test_exclude_path_components_not_substrings(tmp_path: Path) -> None:
    """Excluding `build` must not silently skip `building-blocks/`."""
    _write(tmp_path, "building-blocks/doc.md", "hi\n")
    files = linter.discover_files(
        [tmp_path], excludes=("build",), base=tmp_path,
    )
    paths = {f.relative_to(tmp_path).as_posix() for f in files}
    assert "building-blocks/doc.md" in paths


# ---------------------------------------------------------------------
# 7. CLI exit-code semantics
# ---------------------------------------------------------------------


def test_cli_default_exit_zero_with_findings(tmp_path: Path) -> None:
    """Default mode exits 0 even when findings exist (soft warn)."""
    _write(tmp_path, "doc.md", "Bad: /dev/ttyUSB0\n")
    rv = _run("--root", str(tmp_path), "--base", str(tmp_path))
    assert rv.returncode == 0, rv.stderr + rv.stdout
    assert "LINUX-ONLY-IDIOM" in rv.stdout
    assert "WARN" in rv.stdout


def test_cli_fail_on_warning_exits_one(tmp_path: Path) -> None:
    """--fail-on-warning flips the exit code when findings exist."""
    _write(tmp_path, "doc.md", "Bad: /dev/ttyUSB0\n")
    rv = _run(
        "--root", str(tmp_path),
        "--base", str(tmp_path),
        "--fail-on-warning",
    )
    assert rv.returncode == 1
    assert "LINUX-ONLY-IDIOM" in rv.stdout


def test_cli_clean_tree_exits_zero(tmp_path: Path) -> None:
    """A tree with no findings exits 0 even with --fail-on-warning."""
    _write(tmp_path, "doc.md", "All-portable content.\n")
    rv = _run(
        "--root", str(tmp_path),
        "--base", str(tmp_path),
        "--fail-on-warning",
    )
    assert rv.returncode == 0
    assert "clean" in rv.stdout


def test_cli_quiet_suppresses_findings(tmp_path: Path) -> None:
    """--quiet hides per-finding output; the summary line is kept."""
    _write(tmp_path, "doc.md", "Bad: /dev/ttyUSB0\n")
    rv = _run(
        "--root", str(tmp_path),
        "--base", str(tmp_path),
        "--quiet",
    )
    assert rv.returncode == 0
    # No per-finding line, but the summary survives.
    assert "/dev/ttyUSB0" not in rv.stdout
    assert "WARN" in rv.stdout


def test_cli_json_emits_jsonl(tmp_path: Path) -> None:
    """--json emits one JSON object per line; no summary."""
    _write(tmp_path, "doc.md", "Bad: /dev/ttyUSB0\n")
    rv = _run(
        "--root", str(tmp_path),
        "--base", str(tmp_path),
        "--json",
    )
    assert rv.returncode == 0
    # Every non-empty stdout line must be a JSON object.
    lines = [ln for ln in rv.stdout.splitlines() if ln.strip()]
    assert lines, "no JSON output produced"
    for ln in lines:
        obj = json.loads(ln)
        assert {"path", "line", "category", "matched_text"}.issubset(obj.keys())


def test_cli_path_overrides_root(tmp_path: Path) -> None:
    """--path scoping picks a single file regardless of --root."""
    bad = _write(tmp_path, "subdir/bad.md", "Bad: /dev/ttyUSB0\n")
    _write(tmp_path, "subdir/good.md", "OK content.\n")
    rv = _run(
        "--path", str(bad),
        "--base", str(tmp_path),
        "--fail-on-warning",
    )
    assert rv.returncode == 1
    assert "subdir/bad.md" in rv.stdout
    assert "subdir/good.md" not in rv.stdout


def test_cli_missing_path_exits_two(tmp_path: Path) -> None:
    """A nonexistent --path target exits 2 (invocation error)."""
    rv = _run("--path", str(tmp_path / "ghost.md"))
    assert rv.returncode == 2


# ---------------------------------------------------------------------
# 8. Real-tree smoke
# ---------------------------------------------------------------------


def test_linter_runs_against_real_repo_without_crash() -> None:
    """The default invocation against the live repo doesn't crash
    and exits 0 (soft warn).  Findings are expected; the lint is
    soft today per ADR 0012.  This regression-locks the
    no-crash promise."""
    rv = _run()
    assert rv.returncode == 0, (
        f"linter crashed on real repo:\n{rv.stderr}\n{rv.stdout}"
    )


def test_linter_module_help_includes_categories() -> None:
    """The module docstring documents both categories the linter
    emits.  Locks the contract that the script self-documents."""
    assert "LINUX-ONLY-IDIOM" in linter.__doc__
    assert "BASH-ONLY-SHEBANG" in linter.__doc__


# ---------------------------------------------------------------------
# 9. Inline skip marker
# ---------------------------------------------------------------------


def test_skip_marker_suppresses_block(tmp_path: Path) -> None:
    """`<!-- cross-platform-lint:ignore -->` ... `:resume -->` blocks
    have their findings suppressed entirely."""
    p = _write(tmp_path, "doc.md", """
        # Portable doc

        Connect to /dev/ttyUSB0 here -- this SHOULD warn.

        <!-- cross-platform-lint:ignore -->
        On Linux: `/dev/ttyUSB0` -- inside ignore block, NO warning.
        On macOS: `/dev/cu.usbserial-XYZ`
        On Windows: `COM3`
        <!-- cross-platform-lint:resume -->

        Add to your ~/.bashrc -- this SHOULD warn (after resume).
    """)
    findings = linter.scan([p], base=tmp_path)
    # The two outside-block findings remain; everything inside the
    # block (including the /dev/cu.usbserial-XYZ macOS path) is gone.
    matched = sorted(f.matched_text for f in findings)
    assert "/dev/ttyUSB0" in matched[0] or "ttyUSB0" in matched[0]
    assert any("bashrc" in m for m in matched)
    # And NOTHING from the ignore block leaked.
    assert not any("usbserial-XYZ" in f.matched_text for f in findings)


def test_skip_marker_open_to_eof(tmp_path: Path) -> None:
    """An `:ignore -->` marker without a matching `:resume -->`
    suppresses everything to end-of-file."""
    p = _write(tmp_path, "doc.md", """
        Outside: /dev/ttyUSB0 -- should warn.

        <!-- cross-platform-lint:ignore -->
        Linux serial path: /dev/ttyACM0
        Mac serial path: /dev/cu.usbserial-DM12345
        Edit ~/.bashrc to source the env.
    """)
    findings = linter.scan([p], base=tmp_path)
    # Only the outside-block finding remains.
    assert len(findings) == 1
    assert "ttyUSB0" in findings[0].matched_text


def test_skip_marker_compute_helper_directly(tmp_path: Path) -> None:
    """The `_compute_skip_lines` helper returns 1-based line numbers
    covering the marker line itself + the block contents."""
    text = (
        "line1\n"
        "<!-- cross-platform-lint:ignore -->\n"  # line 2 -- marker
        "ignored1\n"                              # line 3
        "ignored2\n"                              # line 4
        "<!-- cross-platform-lint:resume -->\n"  # line 5 -- marker
        "line6\n"
    )
    skip = linter._compute_skip_lines(text)
    assert skip == {2, 3, 4, 5}


def test_skip_marker_only_applies_to_markdown(tmp_path: Path) -> None:
    """Skip markers in .sh files are NOT honoured -- the marker
    syntax is HTML-comment shaped, which is only valid in
    markdown.  Shell scripts get their own bash-shebang gate."""
    # A .sh with an HTML-comment marker should NOT escape the
    # shebang check (the marker syntax doesn't apply to .sh files).
    p = _write(tmp_path, "myscript.sh", """
        #!/usr/bin/env bash
        # <!-- cross-platform-lint:ignore -->
        echo hi
    """)
    findings = linter.scan([p], base=tmp_path)
    bash = [f for f in findings if f.category == "BASH-ONLY-SHEBANG"]
    assert len(bash) == 1


# ---------------------------------------------------------------------
# 10. File-level allowlist (INTENTIONALLY_DISCUSSES_OS_PATHS)
# ---------------------------------------------------------------------


def test_allowlist_collapses_findings_to_summary(tmp_path: Path) -> None:
    """A file in INTENTIONALLY_DISCUSSES_OS_PATHS emits zero
    per-line findings + one AllowlistSummary carrying the count."""
    # Pick a real allowlisted path so the scanner sees the rel path
    # correctly.  We synthesise it under tmp_path so we control the
    # content.
    rel = "docs/cross-platform-setup.md"
    assert rel in linter.INTENTIONALLY_DISCUSSES_OS_PATHS
    p = _write(tmp_path, rel, """
        On Linux: /dev/ttyUSB0 and /dev/ttyACM0
        On macOS: /dev/cu.usbserial-DM12345
        Edit your ~/.bashrc to persist.
    """)
    findings, summaries = linter.scan_with_summaries([p], base=tmp_path)
    assert findings == []
    assert len(summaries) == 1
    assert summaries[0].path == rel
    # 4 OS-specific references (2x /dev/tty*, 1x /dev/cu.*, 1x bashrc).
    assert summaries[0].reference_count == 4


def test_allowlist_summary_not_counted_as_finding(tmp_path: Path) -> None:
    """An allowlisted-only run has zero findings and so
    --fail-on-warning stays exit 0."""
    rel = "docs/cross-platform-setup.md"
    _write(tmp_path, rel, "Linux: /dev/ttyUSB0; macOS: /dev/cu.usbserial-*\n")
    rv = _run(
        "--root", str(tmp_path),
        "--base", str(tmp_path),
        "--fail-on-warning",
    )
    assert rv.returncode == 0, rv.stdout + rv.stderr
    # Summary line is informational, present in stdout.
    assert "allowlisted" in rv.stdout
    assert "OS-specific reference" in rv.stdout
    # Per-finding lines should NOT appear -- the summary replaces them.
    assert "LINUX-ONLY-IDIOM:" not in rv.stdout


def test_allowlist_does_not_swallow_non_allowlisted_findings(
    tmp_path: Path,
) -> None:
    """Mixed run: one allowlisted file + one ordinary file with
    real findings -> findings emitted; allowlisted file summarised."""
    _write(tmp_path, "docs/cross-platform-setup.md",
           "Linux: /dev/ttyUSB0\n")
    _write(tmp_path, "docs/other.md", "Bad: /dev/ttyUSB0\n")
    rv = _run(
        "--root", str(tmp_path),
        "--base", str(tmp_path),
        "--fail-on-warning",
    )
    # The ordinary file's finding flips the exit code.
    assert rv.returncode == 1
    # Summary line for the allowlisted file is present too.
    assert "allowlisted" in rv.stdout
    assert "docs/other.md" in rv.stdout


def test_allowlist_summary_json_serialisable(tmp_path: Path) -> None:
    """In --json mode, allowlist summaries serialise to JSONL with
    a `kind: allowlist_summary` discriminator."""
    rel = "docs/cross-platform-setup.md"
    _write(tmp_path, rel, "Linux: /dev/ttyUSB0\n")
    rv = _run(
        "--root", str(tmp_path),
        "--base", str(tmp_path),
        "--json",
    )
    assert rv.returncode == 0
    lines = [ln for ln in rv.stdout.splitlines() if ln.strip()]
    assert lines, "no JSON output produced"
    summary_lines = [
        ln for ln in lines if '"kind": "allowlist_summary"' in ln
    ]
    assert len(summary_lines) == 1
    obj = json.loads(summary_lines[0])
    assert obj["path"] == rel
    assert obj["reference_count"] >= 1


def test_allowlist_skip_marker_interaction(tmp_path: Path) -> None:
    """Skip markers run BEFORE the allowlist short-circuit, so the
    summary count reflects skip-marker suppression."""
    rel = "docs/cross-platform-setup.md"
    p = _write(tmp_path, rel, """
        Linux: /dev/ttyUSB0 -- counts in summary.

        <!-- cross-platform-lint:ignore -->
        Linux: /dev/ttyACM0 -- skipped, does NOT count.
        macOS: /dev/cu.usbserial-XYZ -- skipped.
        <!-- cross-platform-lint:resume -->

        Edit ~/.bashrc -- counts in summary.
    """)
    findings, summaries = linter.scan_with_summaries([p], base=tmp_path)
    assert findings == []
    assert len(summaries) == 1
    # 2 references count (ttyUSB0 outside, bashrc after resume);
    # the 2 inside the ignore block don't.
    assert summaries[0].reference_count == 2


def test_allowlist_includes_expected_files() -> None:
    """The allowlist contains exactly the docs the cleanup task
    blessed: the cross-platform setup doc, ADR 0012, the HiL CI
    runner doc, and the HiL test README.  Locks the scope; any
    addition is a deliberate act."""
    expected = {
        "docs/cross-platform-setup.md",
        "docs/adr/0012-cross-platform-developer-host.md",
        "docs/ci/HW-IN-LOOP.md",
        "tests/hil/README.md",
    }
    assert linter.INTENTIONALLY_DISCUSSES_OS_PATHS == frozenset(expected)


def test_allowlist_summary_render_humanreadable() -> None:
    """The summary's `render()` method produces a single line that
    names the path, marks it as allowlisted, and reports the
    reference count.  Locks the format for downstream consumers
    that grep for `allowlisted`."""
    s = linter.AllowlistSummary(
        path="docs/cross-platform-setup.md",
        reference_count=12,
    )
    rendered = s.render()
    assert "docs/cross-platform-setup.md" in rendered
    assert "allowlisted" in rendered
    assert "12" in rendered
    assert "informational" in rendered
