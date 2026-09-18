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
import os
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
    """Invoke the linter as a subprocess.

    Both ends of the pipe must agree on the encoding.  `encoding="utf-8"`
    only sets how the PARENT decodes; the child Python writes its locale
    encoding, which is cp1252 on windows-latest.  The linter's own output
    carries a `§`, which cp1252 writes as the lone byte 0xa7 -- invalid
    UTF-8 -- so the decode fails in subprocess's reader thread, the
    exception is swallowed, and `stdout` comes back as None.  Forcing
    PYTHONIOENCODING makes the child write UTF-8 too.
    """
    return subprocess.run(
        [sys.executable, str(LINTER), *args],
        capture_output=True, text=True, check=False, encoding="utf-8",
        env={**os.environ, "PYTHONIOENCODING": "utf-8"},
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
    """Python source gets the IMPLICIT-ENCODING check (below), but
    none of the LINUX-ONLY-IDIOM text patterns -- a bare string
    literal containing `/dev/ttyUSB0` is not itself an encoding
    hazard, so it produces no findings."""
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
    """The default (no-flag) invocation against the live repo doesn't
    crash and exits 0 -- this mode stays soft-warn even with findings,
    by design (see the module docstring's Operating mode section).
    This regression-locks the no-crash promise."""
    rv = _run()
    assert rv.returncode == 0, (
        f"linter crashed on real repo:\n{rv.stderr}\n{rv.stdout}"
    )


def test_linter_fail_on_warning_against_real_repo_passes() -> None:
    """`--fail-on-warning` is what CI actually runs (alp-sdk#1032 A5,
    cross-platform-zephyr.yml's python-smoke step) -- assert 0 findings
    against the live repo here too, so a new Linux-only idiom is caught
    locally instead of only on three legs of a non-required workflow.

    #2195 grandfathered 478 pre-existing IMPLICIT-ENCODING sites via
    IMPLICIT_ENCODING_BASELINE (see that set's comment + #2197, the
    drain issue) -- those are expected to still be present and are
    NOT a --fail-on-warning failure.  What must be true is that there
    are zero findings OUTSIDE the baseline: no LINUX-ONLY-IDIOM /
    BASH-ONLY-SHEBANG finding anywhere, and no IMPLICIT-ENCODING
    finding in a file that isn't on the baseline list.

    The same `findings` list also pays for the two #2197 shrink-only
    guards below -- a size pin and a stale-entry check -- so the
    baseline can only ever get smaller."""
    rv = _run("--fail-on-warning")
    assert rv.returncode == 0, (
        f"check_cross_platform --fail-on-warning found drift outside "
        f"the IMPLICIT_ENCODING_BASELINE:\n{rv.stdout}\n{rv.stderr}"
    )
    findings = linter.scan(
        linter.discover_files(
            [(REPO / r).resolve() for r in linter.DEFAULT_ROOTS],
            linter.DEFAULT_EXCLUDES,
            REPO,
        ),
        base=REPO,
    )
    non_baseline = [f for f in findings if not f.baselined]
    assert non_baseline == [], (
        f"finding(s) outside IMPLICIT_ENCODING_BASELINE:\n"
        + "\n".join(f.render() for f in non_baseline)
    )

    # #2197 guard 1 -- size pin.  The baseline is the frozen day-#2195
    # backlog, not a dumping ground: the file-level shape means a new
    # entry silently exempts every implicit-encoding call in that file,
    # so growing the set has to be a deliberate, test-breaking act.
    assert len(linter.IMPLICIT_ENCODING_BASELINE) <= 2, (
        f"IMPLICIT_ENCODING_BASELINE grew to "
        f"{len(linter.IMPLICIT_ENCODING_BASELINE)} files; it holds what "
        f"is left of the frozen #2195 backlog (2 files) and may only "
        f"ever SHRINK as #2197 drains it.  A new implicit-encoding call "
        f"gets an explicit `encoding=` -- there is no inline exemption "
        f"-- not a baseline entry."
    )

    # #2197 guard 2 -- no stale entry.  Every listed file must still
    # produce a finding today; one that doesn't was already fixed,
    # moved, or deleted, and leaving it here would silently exempt a
    # future file that lands back at that path.  (`findings` is
    # entirely IMPLICIT-ENCODING-in-baseline here -- the assert above
    # just proved nothing else is in it.)
    stale = set(linter.IMPLICIT_ENCODING_BASELINE) - {f.path for f in findings}
    assert stale == set(), (
        f"IMPLICIT_ENCODING_BASELINE entries that no longer produce any "
        f"finding -- fixed, moved, or deleted.  Drop them from the set "
        f"(#2197):\n" + "\n".join(sorted(stale))
    )


def test_linter_module_help_includes_categories() -> None:
    """The module docstring documents all three categories the
    linter emits.  Locks the contract that the script
    self-documents."""
    assert "LINUX-ONLY-IDIOM" in linter.__doc__
    assert "BASH-ONLY-SHEBANG" in linter.__doc__
    assert "IMPLICIT-ENCODING" in linter.__doc__


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


# ---------------------------------------------------------------------
# 11. Pattern: IMPLICIT-ENCODING (Python, AST-based)
# ---------------------------------------------------------------------


def test_implicit_encoding_flags_path_read_text(tmp_path: Path) -> None:
    """`Path.read_text()` with no `encoding=` is flagged."""
    p = _write(tmp_path, "scripts/foo.py", """
        from pathlib import Path
        Path("x").read_text()
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "IMPLICIT-ENCODING"
    assert "read_text" in findings[0].matched_text


def test_implicit_encoding_flags_path_write_text(tmp_path: Path) -> None:
    """`Path.write_text()` with no `encoding=` is flagged."""
    p = _write(tmp_path, "scripts/foo.py", """
        from pathlib import Path
        Path("x").write_text("data")
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert "write_text" in findings[0].matched_text


def test_implicit_encoding_flags_bare_open_text_mode(tmp_path: Path) -> None:
    """A bare `open(path)` (default text mode) with no `encoding=`
    is flagged."""
    p = _write(tmp_path, "scripts/foo.py", """
        f = open("x")
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "IMPLICIT-ENCODING"


def test_implicit_encoding_flags_open_explicit_r_mode(tmp_path: Path) -> None:
    """`open(path, "r")` -- explicit text mode, still no encoding --
    is flagged."""
    p = _write(tmp_path, "scripts/foo.py", """
        f = open("x", "r")
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1


def test_implicit_encoding_flags_path_open(tmp_path: Path) -> None:
    """`Path.open()` (no mode -> text default) with no `encoding=`
    is flagged, same as bare `open()`."""
    p = _write(tmp_path, "scripts/foo.py", """
        from pathlib import Path
        f = Path("x").open()
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1


def test_implicit_encoding_flags_subprocess_run_text_true(
    tmp_path: Path,
) -> None:
    """`subprocess.run(..., text=True)` with no `encoding=` is
    flagged -- this is the real #2194 site's shape
    (tests/scripts/conftest.py:45, fixed in the same change)."""
    p = _write(tmp_path, "tests/foo.py", """
        import subprocess
        subprocess.run(["x", "--version"], capture_output=True, text=True, check=True)
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "IMPLICIT-ENCODING"
    assert "subprocess.run" in findings[0].suggestion


def test_implicit_encoding_flags_check_output_universal_newlines(
    tmp_path: Path,
) -> None:
    """`subprocess.check_output(..., universal_newlines=True)` with
    no `encoding=` is flagged."""
    p = _write(tmp_path, "tests/foo.py", """
        import subprocess
        subprocess.check_output(["x"], universal_newlines=True)
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1


def test_implicit_encoding_flags_popen_text_true(tmp_path: Path) -> None:
    """`subprocess.Popen(..., text=True)` with no `encoding=` is
    flagged."""
    p = _write(tmp_path, "tests/foo.py", """
        import subprocess
        p = subprocess.Popen(["x"], text=True)
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1


def test_implicit_encoding_multiline_call_still_caught(
    tmp_path: Path,
) -> None:
    """The real #2194 shape spreads kwargs across several lines --
    this is exactly what motivates the AST scan over a regex."""
    p = _write(tmp_path, "tests/foo.py", """
        import subprocess
        subprocess.run(
            ["x", "--version"],
            capture_output=True,
            text=True,
            check=True,
        )
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1


def test_implicit_encoding_binary_open_not_flagged(tmp_path: Path) -> None:
    """`open(path, "rb")` / `open(path, "wb")` are correct and must
    not fire."""
    p = _write(tmp_path, "scripts/foo.py", """
        a = open("x", "rb")
        b = open("x", "wb")
    """)
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


def test_implicit_encoding_read_bytes_write_bytes_not_flagged(
    tmp_path: Path,
) -> None:
    """`Path.read_bytes()` / `Path.write_bytes()` are a different
    method name entirely and are never matched."""
    p = _write(tmp_path, "scripts/foo.py", """
        from pathlib import Path
        Path("x").read_bytes()
        Path("x").write_bytes(b"data")
    """)
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


def test_implicit_encoding_explicit_encoding_not_flagged(
    tmp_path: Path,
) -> None:
    """`encoding=` present on `read_text` / `write_text` / `open`
    suppresses the finding -- the fix IS the suppression, no
    separate skip-marker exists for this category."""
    p = _write(tmp_path, "scripts/foo.py", """
        from pathlib import Path
        Path("x").read_text(encoding="utf-8")
        Path("x").write_text("d", encoding="utf-8")
        open("x", encoding="utf-8")
    """)
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


def test_implicit_encoding_locale_preferred_encoding_passes(
    tmp_path: Path,
) -> None:
    """`encoding=locale.getpreferredencoding()` passes -- the check
    verifies the keyword's presence, not a rationale comment on it
    (see the module docstring)."""
    p = _write(tmp_path, "scripts/foo.py", """
        import locale
        from pathlib import Path
        Path("x").read_text(encoding=locale.getpreferredencoding())
    """)
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


def test_implicit_encoding_text_true_with_encoding_not_flagged(
    tmp_path: Path,
) -> None:
    """`text=True` paired with an explicit `encoding=` is exactly
    correct and must not fire."""
    p = _write(tmp_path, "tests/foo.py", """
        import subprocess
        subprocess.run(["x"], text=True, encoding="utf-8")
    """)
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


def test_implicit_encoding_text_false_not_flagged(tmp_path: Path) -> None:
    """`text=False` is an explicit binary choice, not an encoding
    hazard -- must not fire."""
    p = _write(tmp_path, "tests/foo.py", """
        import subprocess
        subprocess.run(["x"], text=False)
    """)
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


def test_implicit_encoding_kwargs_unpack_not_flagged(tmp_path: Path) -> None:
    """A `**kwargs` unpack could carry `encoding=` the scan can't
    see into -- skipped rather than guessed at."""
    p = _write(tmp_path, "scripts/foo.py", """
        from pathlib import Path
        extra = {"encoding": "utf-8"}
        Path("x").read_text(**extra)
    """)
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


def test_implicit_encoding_subprocess_text_with_kwargs_unpack_flagged(
    tmp_path: Path,
) -> None:
    """A subprocess call whose explicit `text=True` proves text mode is
    flagged even with a `**kw` unpack -- the `_run(*args, **kw)` helper
    shape forwards cwd/env/check, not an encoding (#2197)."""
    p = _write(tmp_path, "tests/foo.py", """
        import subprocess
        def _run(*args, **kw):
            return subprocess.run(["x", *args], capture_output=True, text=True, **kw)
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert "subprocess.run" in findings[0].suggestion


def test_implicit_encoding_path_open_mode_is_first_positional(
    tmp_path: Path,
) -> None:
    """`Path.open(mode)` takes its mode as the FIRST positional, the
    builtin `open(file, mode)` (and `io.open`) as the second -- a binary
    mode is read from the right slot in every shape, and a text mode on
    `Path.open` is still flagged (#2197)."""
    p = _write(tmp_path, "scripts/foo.py", """
        import io
        from pathlib import Path
        a = Path("a").open("rb")
        b = Path("a").open(mode="rb")
        c = open("a", "rb")
        d = io.open("a", "rb")
        e = Path("a").open("r")
    """)
    findings = linter.scan([p], base=tmp_path)
    assert [f.line for f in findings] == [7]


def test_implicit_encoding_open_on_no_encoding_module_not_flagged(
    tmp_path: Path,
) -> None:
    """`.open()` on a module whose `open` takes no `encoding=` --
    `tokenize`, `tarfile`, `os` -- is not flagged, including an
    `import ... as` alias bound at function scope: the finding's fix
    would raise TypeError there (#2197).  An unknown owner still is
    (see test_implicit_encoding_unrelated_open_call_not_flagged)."""
    p = _write(tmp_path, "scripts/foo.py", """
        import os
        import tarfile
        import tokenize
        a = tokenize.open("x.py")
        b = tarfile.open(fileobj=buf, mode="w")
        c = os.open("x", os.O_RDONLY)
        d = os.open("x", flags=os.O_RDONLY)
        def f():
            import tarfile as _tf
            return _tf.open(fileobj=buf, mode="r")
    """)
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


def test_implicit_encoding_module_open_reads_mode_second(
    tmp_path: Path,
) -> None:
    """A module-level `m.open(file, mode)` and any 2+-positional
    `.open()` read the mode from the second positional, like the
    builtin: binary `gzip`/`lzma`/`wave` opens are not flagged (their
    `encoding=` would raise ValueError), a text `gzip.open(p, "rt")`
    and an unknown `fs.open(p, "r")` are, and a one-argument
    `Path.open("rb")` still reads its mode first (#2197)."""
    p = _write(tmp_path, "scripts/foo.py", """
        import gzip, lzma, wave
        from pathlib import Path
        a = gzip.open("x.gz", "rb")
        b = lzma.open("x.xz", "wb")
        c = wave.open("x.wav", "rb")
        d = gzip.open(p, "rt")
        e = fs.open(p, "r")
        f = Path(x).open("rb")
        g = gzip.open("x.gz", mode="rb")
    """)
    findings = linter.scan([p], base=tmp_path)
    assert [f.line for f in findings] == [6, 7]
    assert "gzip.open()" in findings[0].suggestion


def test_implicit_encoding_from_import_open_resolves_to_module(
    tmp_path: Path,
) -> None:
    """A module-level `from tokenize import open` makes the bare
    `open()` tokenize's, which takes no `encoding=` -- not flagged.  A
    function-scope one does NOT exempt the file's builtin `open()`
    calls (#2197)."""
    top = _write(tmp_path, "scripts/top.py", """
        from tokenize import open
        f = open("x.py")
    """)
    assert linter.scan([top], base=tmp_path) == []
    local = _write(tmp_path, "scripts/local.py", """
        def f():
            from tokenize import open
        g = open("x")
    """)
    assert [f.line for f in linter.scan([local], base=tmp_path)] == [3]


def test_implicit_encoding_os_fdopen_flagged_in_text_mode(
    tmp_path: Path,
) -> None:
    """`os.fdopen(fd, mode)` is builtin-shaped: text mode (explicit or
    default) is flagged, binary is not, a `from os import fdopen` alias
    counts, and an `fdopen` on an unknown owner is ignored (#2197)."""
    p = _write(tmp_path, "scripts/foo.py", """
        import os
        from os import fdopen
        a = os.fdopen(fd, "w")
        b = os.fdopen(fd, "wb")
        c = os.fdopen(fd)
        d = fdopen(fd, "w")
        e = thing.fdopen(fd, "w")
    """)
    findings = linter.scan([p], base=tmp_path)
    assert [f.line for f in findings] == [3, 5, 6]
    assert "os.fdopen()" in findings[0].suggestion


def test_implicit_encoding_unrelated_open_call_not_flagged(
    tmp_path: Path,
) -> None:
    """A totally unrelated no-arg call named `open` on some other
    object with no mode-shaped args at all is still flagged under
    the heuristic (name-based, like the rest of this linter) --
    this test locks that documented limitation rather than pretend
    it doesn't exist."""
    p = _write(tmp_path, "scripts/foo.py", """
        widget.open()
    """)
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "IMPLICIT-ENCODING"


def test_implicit_encoding_syntax_error_no_crash(tmp_path: Path) -> None:
    """A .py file that fails to parse produces no findings (and no
    crash) -- reporting a syntax error isn't this linter's job."""
    p = _write(tmp_path, "scripts/broken.py", "def f(:\n")
    findings = linter.scan([p], base=tmp_path)
    assert findings == []


def test_implicit_encoding_scan_helper_returns_tuples() -> None:
    """`scan_python_encoding` returns (line, col, matched, suggestion)
    tuples directly -- unit-test the helper without going through
    `Finding` construction."""
    text = 'open("x")\n'
    results = linter.scan_python_encoding(text)
    assert len(results) == 1
    line, col, matched, suggestion = results[0]
    assert line == 1
    assert col == 1
    assert "open" in matched
    assert "encoding" in suggestion


def test_implicit_encoding_scoped_to_scripts_and_tests_by_default(
    tmp_path: Path,
) -> None:
    """The default (root-scoped) walk only considers .py files under
    scripts/** and tests/** (PY_SCAN_ROOTS) -- a .py file under
    examples/ is not discovered even though it has the same hazard,
    matching the module docstring's Scope section."""
    _write(tmp_path, "scripts/inscope.py", 'open("x")\n')
    _write(tmp_path, "examples/foo/gen.py", 'open("x")\n')
    files = linter.discover_files(
        [tmp_path], excludes=linter.DEFAULT_EXCLUDES, base=tmp_path,
    )
    paths = {f.relative_to(tmp_path).as_posix() for f in files}
    assert "scripts/inscope.py" in paths
    assert "examples/foo/gen.py" not in paths


def test_implicit_encoding_explicit_path_scans_outside_py_scan_roots(
    tmp_path: Path,
) -> None:
    """An explicit `--path` target is scanned regardless of
    PY_SCAN_ROOTS -- the restriction only narrows the implicit
    default walk, not an explicit request."""
    p = _write(tmp_path, "examples/foo/gen.py", 'open("x")\n')
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "IMPLICIT-ENCODING"


def test_implicit_encoding_cli_fail_on_warning_exits_one(
    tmp_path: Path,
) -> None:
    """CLI wiring: an IMPLICIT-ENCODING finding flips the exit code
    under --fail-on-warning, same as the other categories."""
    _write(tmp_path, "scripts/foo.py", 'open("x")\n')
    rv = _run(
        "--root", str(tmp_path),
        "--base", str(tmp_path),
        "--fail-on-warning",
    )
    assert rv.returncode == 1
    assert "IMPLICIT-ENCODING" in rv.stdout


# ---------------------------------------------------------------------
# 12. IMPLICIT_ENCODING_BASELINE (grandfather baseline, #2195 / #2197)
# ---------------------------------------------------------------------


def test_baseline_file_does_not_fail_fail_on_warning(tmp_path: Path) -> None:
    """A file on IMPLICIT_ENCODING_BASELINE still gets its finding
    printed (it's a warning, not silence) but does NOT flip
    --fail-on-warning's exit code."""
    rel = "scripts/alp_quality.py"
    assert rel in linter.IMPLICIT_ENCODING_BASELINE
    _write(tmp_path, rel, 'open("x")\n')
    rv = _run(
        "--path", str(tmp_path / rel),
        "--base", str(tmp_path),
        "--fail-on-warning",
    )
    assert rv.returncode == 0, rv.stdout + rv.stderr
    assert "IMPLICIT-ENCODING" in rv.stdout
    assert "baselined" in rv.stdout


def test_non_baseline_file_fails_fail_on_warning(tmp_path: Path) -> None:
    """A file NOT on IMPLICIT_ENCODING_BASELINE with the exact same
    implicit-encoding call DOES flip --fail-on-warning to exit 1 --
    the baseline only covers the files it names."""
    rel = "scripts/brand_new_file_not_on_baseline.py"
    assert rel not in linter.IMPLICIT_ENCODING_BASELINE
    _write(tmp_path, rel, 'open("x")\n')
    rv = _run(
        "--path", str(tmp_path / rel),
        "--base", str(tmp_path),
        "--fail-on-warning",
    )
    assert rv.returncode == 1, rv.stdout + rv.stderr
    assert "IMPLICIT-ENCODING" in rv.stdout


def test_new_finding_in_baselined_file_still_reported_as_warning(
    tmp_path: Path,
) -> None:
    """A NEW implicit-encoding call added to an already-baselined
    file is still surfaced as a warning finding (not swallowed like
    the INTENTIONALLY_DISCUSSES_OS_PATHS allowlist would) -- it just
    doesn't fail the gate.  This is the documented file-level-vs-
    line-level weakness: the baseline can't distinguish an old site
    from a brand-new one in the same file, so both print and neither
    fails."""
    rel = "scripts/alp_quality.py"
    assert rel in linter.IMPLICIT_ENCODING_BASELINE
    p = _write(tmp_path, rel, 'open("brand_new_call_added_today")\n')
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].category == "IMPLICIT-ENCODING"
    assert findings[0].baselined is True
    # Still a real Finding object that renders (not a suppressed
    # allowlist summary) -- confirms it reaches --quiet-off output.
    assert "IMPLICIT-ENCODING" in findings[0].render()


def test_baseline_finding_dataclass_field_defaults_false(
    tmp_path: Path,
) -> None:
    """Every non-IMPLICIT-ENCODING finding, and every IMPLICIT-ENCODING
    finding outside the baseline, has baselined=False -- the default
    must not accidentally suppress a real failure."""
    p = _write(tmp_path, "scripts/never_baselined.py", 'open("x")\n')
    findings = linter.scan([p], base=tmp_path)
    assert len(findings) == 1
    assert findings[0].baselined is False


def test_conftest_known_site_now_clean() -> None:
    """Regression lock for the real #2195 trigger:
    tests/scripts/conftest.py:45 (`subprocess.run(..., text=True)`
    with no `encoding=`) was the only implicit-encoding call left in
    the pytest tests/scripts/ surface -- fixed in the same change
    that added this rule.  This asserts it stays fixed."""
    conftest = REPO / "tests" / "scripts" / "conftest.py"
    findings = linter.scan([conftest], base=REPO)
    encoding_findings = [
        f for f in findings if f.category == "IMPLICIT-ENCODING"
    ]
    assert encoding_findings == [], encoding_findings
