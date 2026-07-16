# SPDX-License-Identifier: Apache-2.0
"""
Regression tests for issue #803: `scripts/test-all.sh` regenerated the
FROZEN historical `docs/abi/v0.9-snapshot.json` against today's
headers on every run, reported the drift, and told the author to
commit it -- even though `metadata/sdk_version.yaml` had long since
moved to `0.10.1` and `docs/abi/v0.10-snapshot.json` was the actual
current snapshot.  Per `docs/abi/README.md` a snapshot fingerprints
the public surface "at a specific release tag" so reviewers can spot
real ABI regressions between releases; a baseline that keeps tracking
`HEAD` after it should have frozen makes a real regression against
that release invisible, because the baseline moves with the change
that broke it.

These tests pin the CLASS fix, not the v0.9 instance:

  1. `scripts/test-all.sh` derives "the current snapshot" from
     `metadata/sdk_version.yaml` (single source), not a version
     literal baked into the script -- so the very next release cut
     can never again leave this gate pointed at a frozen file.
  2. `scripts/abi_snapshot.py --output` refuses to write a snapshot
     labelled anything other than the version `metadata/sdk_version.yaml`
     currently declares -- so even a caller (a hand-run command, a
     forgotten-to-update CI step) that still names an old/frozen
     version by hand gets a loud failure instead of silently
     corrupting the frozen file.

Both are verified by EXECUTING the real code (extracting and running
the actual bash function; calling the actual Python guard), not by
asserting agreement with the new implementation's own text.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
TEST_ALL = REPO / "scripts" / "test-all.sh"
SDK_VERSION_YAML = REPO / "metadata" / "sdk_version.yaml"

sys.path.insert(0, str(REPO / "scripts"))
import abi_snapshot as abi  # noqa: E402

pytestmark_bash = pytest.mark.skipif(
    sys.platform.startswith("win"),
    reason="scripts/test-all.sh is a POSIX bash script; run this test on "
    "Linux/macOS/WSL, matching test-all.sh's own cross-platform scope.",
)


# ---------------------------------------------------------------------
# Helper: pull ONE function's body out of scripts/test-all.sh so the
# test can execute it in isolation, without running the whole
# (slow, tool-dependent) test-all.sh suite.
# ---------------------------------------------------------------------


def _extract_bash_function(script_text: str, name: str) -> str:
    """Return the full `name() { ... }` block, matched non-greedily up
    to the first column-0 `}` line -- every stage/helper function in
    this script closes that way, and the heredoc body in between never
    contains a bare `}` line of its own."""
    m = re.search(
        rf"^{re.escape(name)}\(\) \{{\n(.*?\n)\}}\n", script_text, re.DOTALL | re.MULTILINE
    )
    assert m is not None, f"{name}() not found in {TEST_ALL}"
    return f"{name}() {{\n{m.group(1)}}}\n"


@pytest.fixture
def sdk_repo_with_version(tmp_path):
    """A minimal fake repo root: just metadata/sdk_version.yaml plus
    the extracted `abi_current_snapshot` function, so the test proves
    the function's OWN derivation logic rather than anything about the
    real repo's current version."""

    def make(version: str) -> Path:
        root = tmp_path / f"repo-{version}"
        (root / "metadata").mkdir(parents=True)
        (root / "metadata" / "sdk_version.yaml").write_text(
            f"version: {version}\nstatus: development\n", encoding="utf-8"
        )
        func = _extract_bash_function(TEST_ALL.read_text(encoding="utf-8"), "abi_current_snapshot")
        (root / "func.sh").write_text(func, encoding="utf-8")
        return root

    return make


# ---------------------------------------------------------------------
# 1. scripts/test-all.sh: the snapshot path is DERIVED, not hardcoded
# ---------------------------------------------------------------------


@pytestmark_bash
@pytest.mark.parametrize(
    "version,expected",
    [
        ("0.10.1", "docs/abi/v0.10-snapshot.json"),
        ("0.9.4", "docs/abi/v0.9-snapshot.json"),
        ("3.7.2", "docs/abi/v3.7-snapshot.json"),
        ("1.0.0", "docs/abi/v1.0-snapshot.json"),
    ],
)
def test_abi_current_snapshot_tracks_sdk_version_yaml(sdk_repo_with_version, version, expected):
    """The CLASS property the issue asks for: the path test-all.sh's
    ABI stages target always matches whatever metadata/sdk_version.yaml
    declares as current -- for ANY version, not just today's v0.10.
    A hardcoded literal (the pre-fix bug) would return the same path
    regardless of what sdk_version.yaml says; this parametrization
    would catch that immediately (three of the four cases are not
    "0.10")."""
    root = sdk_repo_with_version(version)
    proc = subprocess.run(
        ["bash", "-c", "source func.sh && abi_current_snapshot"],
        cwd=str(root),
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert proc.stdout.strip() == expected


@pytestmark_bash
def test_abi_current_snapshot_matches_the_real_repos_declared_version():
    """Integration check against the REAL repo: the derived path must
    equal what metadata/sdk_version.yaml declares here and now (today:
    0.10.1 -> docs/abi/v0.10-snapshot.json) -- proving the function is
    wired to the actual single-source file, not a copy."""
    m = re.search(
        r"^version:\s*(\d+)\.(\d+)\.(\d+)\s*$",
        SDK_VERSION_YAML.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    assert m is not None
    expected = f"docs/abi/v{m.group(1)}.{m.group(2)}-snapshot.json"

    func = _extract_bash_function(TEST_ALL.read_text(encoding="utf-8"), "abi_current_snapshot")
    proc = subprocess.run(
        ["bash", "-c", f"{func}\nabi_current_snapshot"],
        cwd=str(REPO),
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert proc.stdout.strip() == expected
    # And the (restored, frozen) v0.9 file must NOT be what today's
    # gate targets -- pins the exact regression this issue reported.
    assert proc.stdout.strip() != "docs/abi/v0.9-snapshot.json"


@pytestmark_bash
def test_stage_functions_call_the_derived_helper_not_a_version_literal():
    """`stage_abi_strict` and the ABI half of `stage_generated_files`
    must call `abi_current_snapshot` -- not contain their own
    `docs/abi/v<N>-snapshot.json` string literal.  A literal here is
    exactly how a future release cut could reintroduce this bug
    (someone "fixes" it once by editing the number, instead of the
    class staying fixed)."""
    text = TEST_ALL.read_text(encoding="utf-8")
    strict_body = _extract_bash_function(text, "stage_abi_strict")
    generated_body = _extract_bash_function(text, "stage_generated_files")

    assert "abi_current_snapshot" in strict_body
    assert "abi_current_snapshot" in generated_body
    assert not re.search(r"docs/abi/v\d+\.\d+-snapshot\.json", strict_body)
    assert not re.search(r"docs/abi/v\d+\.\d+-snapshot\.json", generated_body)
    assert "v0.9" not in strict_body
    assert "v0.9" not in generated_body


# ---------------------------------------------------------------------
# 2. scripts/abi_snapshot.py: refuse to WRITE a non-current version
# ---------------------------------------------------------------------


def test_current_snapshot_version_reads_sdk_version_yaml(tmp_path):
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 2.3.4\nstatus: development\n", encoding="utf-8")
    assert abi.current_snapshot_version(sdk_yaml) == "v2.3"


def test_current_snapshot_version_none_when_missing(tmp_path):
    assert abi.current_snapshot_version(tmp_path / "does-not-exist.yaml") is None


def test_main_refuses_to_write_a_stale_version_label(tmp_path, monkeypatch):
    """The actual acceptance criterion: `abi_snapshot.py --version
    v0.9 --output <path>` must be REFUSED when sdk_version.yaml
    declares a newer current release -- exactly the call
    `scripts/test-all.sh` used to make every run, pre-fix."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)

    out = tmp_path / "v0.9-snapshot.json"
    monkeypatch.setattr(
        sys,
        "argv",
        ["abi_snapshot.py", "--version", "v0.9", "--output", str(out)],
    )
    rc = abi.main()
    assert rc != 0
    assert not out.exists(), "must not write the file when the version is refused"


def test_main_allows_writing_the_current_version(tmp_path, monkeypatch):
    """The flip side: writing the version sdk_version.yaml actually
    declares must still work (the release flow -- bump_version.py --
    depends on this)."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)

    out = tmp_path / "v0.10-snapshot.json"
    monkeypatch.setattr(
        sys,
        "argv",
        ["abi_snapshot.py", "--version", "v0.10", "--output", str(out)],
    )
    rc = abi.main()
    assert rc == 0
    assert out.exists()


def test_main_diff_mode_is_unaffected_by_the_write_guard(tmp_path, monkeypatch):
    """`--diff` never writes a snapshot, so it must not be blocked by
    the write-only guard even when its --version (defaulting to
    "dev") doesn't match the current release -- `pr-abi-snapshot.yml`
    and `stage_abi_strict` both rely on plain `--diff` working
    regardless of sdk_version.yaml's contents."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)

    prior = tmp_path / "prior.json"
    prior.write_text('{"headers": {}}', encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["abi_snapshot.py", "--diff", str(prior)])
    rc = abi.main()
    assert rc == 0


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
