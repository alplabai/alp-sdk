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

import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

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
    """A minimal fake repo root: metadata/sdk_version.yaml, a real copy
    of scripts/abi_snapshot.py (abi_current_snapshot() now shells out
    to it for the single parse, issue #826), plus the extracted
    `abi_current_snapshot` function -- so the test proves the
    function's OWN derivation logic rather than anything about the
    real repo's current version.  abi_snapshot.py resolves its own
    REPO from `__file__`, so this copy reads ITS OWN sibling
    metadata/sdk_version.yaml, not the real repo's."""

    def make(version: str) -> Path:
        root = tmp_path / f"repo-{version}"
        (root / "metadata").mkdir(parents=True)
        (root / "metadata" / "sdk_version.yaml").write_text(
            f"version: {version}\nstatus: development\n", encoding="utf-8"
        )
        (root / "scripts").mkdir(parents=True)
        shutil.copy(REPO / "scripts" / "abi_snapshot.py", root / "scripts" / "abi_snapshot.py")
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
    equal what metadata/sdk_version.yaml declares here and now --
    proving the function is wired to the actual single-source file,
    not a copy."""
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


# ---------------------------------------------------------------------
# Issue #1232: a `--output` rerun that changes no public symbol must
# not rewrite the file at all -- not even to bump `generated` -- so a
# full `scripts/test-all.sh` pass on an unrelated PR doesn't dirty
# `docs/abi/<current>-snapshot.json` with a same-day, zero-substance
# diff.
# ---------------------------------------------------------------------

_EMPTY_HEADER = {"functions": {}, "typedefs": {}, "macros": {}, "variables": {}}


def test_main_does_not_rewrite_output_when_only_generated_date_differs(tmp_path, monkeypatch):
    """The core acceptance criterion: headers unchanged from what's
    already committed -> the file on disk must come out BYTE-IDENTICAL
    to before the run, even though today's date differs from the
    committed `generated` value."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)

    fixed_headers = {"alp/x.h": dict(_EMPTY_HEADER)}
    monkeypatch.setattr(abi, "collect", lambda include_root: fixed_headers)

    out = tmp_path / "v0.10-snapshot.json"
    existing = {"version": "v0.10", "generated": "2020-01-01", "headers": fixed_headers}
    # newline="": this fixture stands in for a file git checked out onto
    # disk, which (.gitattributes: `* text=auto eol=lf`) is LF-exact --
    # plain write_text() would let Windows' text-mode default silently
    # write CRLF here, which the guard's byte-exact read must NOT
    # treat as equivalent to canonical LF (the CRLF case this suite also
    # covers below); a wrong-mode fixture would trip that same
    # guard for the wrong reason.
    out.write_text(json.dumps(existing, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="")
    before = out.read_bytes()

    monkeypatch.setattr(
        sys, "argv", ["abi_snapshot.py", "--version", "v0.10", "--output", str(out)]
    )
    rc = abi.main()
    assert rc == 0
    assert out.read_bytes() == before, "unchanged ABI must leave the file byte-identical"


def test_main_still_rewrites_output_when_abi_content_actually_changes(tmp_path, monkeypatch):
    """The guard must not swallow a REAL change: a new public function
    still gets written, `generated` included."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)

    new_headers = {
        "alp/x.h": {
            "functions": {
                "alp_foo": {"signature": "void alp_foo(void);", "hash": "deadbeefcafebabe"}
            },
            "typedefs": {},
            "macros": {},
            "variables": {},
        }
    }
    monkeypatch.setattr(abi, "collect", lambda include_root: new_headers)

    out = tmp_path / "v0.10-snapshot.json"
    existing = {"version": "v0.10", "generated": "2020-01-01", "headers": {"alp/x.h": dict(_EMPTY_HEADER)}}
    # newline="": without it Windows text mode writes CRLF, and the byte
    # compare would then differ on line endings alone -- the rewrite this
    # test asserts would happen for a reason that has nothing to do with
    # the ABI content having changed.
    out.write_text(json.dumps(existing, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="")

    monkeypatch.setattr(
        sys, "argv", ["abi_snapshot.py", "--version", "v0.10", "--output", str(out)]
    )
    rc = abi.main()
    assert rc == 0
    after = json.loads(out.read_text(encoding="utf-8"))
    assert after["headers"] == new_headers
    assert after["generated"] != "2020-01-01"


def test_main_rewrites_a_reformatted_existing_file_even_though_headers_match(
    tmp_path, monkeypatch
):
    """Comparing PARSED JSON would let a hand-reindented, unsorted-keys
    copy of the committed snapshot -- identical ABI, wildly different
    bytes -- read as "unchanged" and stay corrupted forever, invisible to the
    "generated files in sync" gate.  The guard must compare against
    the CANONICAL bytes this script itself writes, so any formatting
    drift on the committed file is treated as real content that needs
    rewriting, not silently preserved."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)

    fixed_headers = {"alp/x.h": dict(_EMPTY_HEADER)}
    monkeypatch.setattr(abi, "collect", lambda include_root: fixed_headers)

    out = tmp_path / "v0.10-snapshot.json"
    existing = {"version": "v0.10", "generated": "2020-01-01", "headers": fixed_headers}
    # Same content, deliberately NOT canonical: indent=4, keys unsorted.
    # newline="" so the ONLY difference from canonical is the formatting
    # under test; Windows CRLF would otherwise force the rewrite by itself.
    out.write_text(json.dumps(existing, indent=4, sort_keys=False) + "\n", encoding="utf-8", newline="")
    before = out.read_bytes()

    monkeypatch.setattr(
        sys, "argv", ["abi_snapshot.py", "--version", "v0.10", "--output", str(out)]
    )
    rc = abi.main()
    assert rc == 0
    after = out.read_bytes()
    assert after != before, "reformatted-but-equivalent file must be rewritten canonical"
    after_json = json.loads(after.decode("utf-8"))
    assert after_json["headers"] == fixed_headers


def test_main_rewrites_when_existing_generated_field_is_not_a_valid_date(tmp_path, monkeypatch):
    """`generated` is spliced from the existing file into `patched`
    before the byte compare, so the byte compare alone can never catch
    corruption confined to that one field -- a hand-edited
    `"generated": "NOT-A-DATE"` would splice straight through and read
    as "unchanged" forever.  The guard validates the existing value is
    a real ISO date first, so a corrupt value falls through to a real
    write and is replaced with a fresh valid one instead of surviving
    every future no-op rerun."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)

    fixed_headers = {"alp/x.h": dict(_EMPTY_HEADER)}
    monkeypatch.setattr(abi, "collect", lambda include_root: fixed_headers)

    out = tmp_path / "v0.10-snapshot.json"
    existing = {"version": "v0.10", "generated": "NOT-A-DATE", "headers": fixed_headers}
    # newline="": a plain write_text() lets Windows' text-mode default
    # write CRLF, which would already fail the guard's byte compare on
    # line endings alone -- exercising the write-fallback path for the
    # wrong reason and never reaching the ISO-date validation this test
    # targets. See the identical note on the sibling fixture above.
    out.write_text(json.dumps(existing, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="")

    monkeypatch.setattr(
        sys, "argv", ["abi_snapshot.py", "--version", "v0.10", "--output", str(out)]
    )
    rc = abi.main()
    assert rc == 0
    after = json.loads(out.read_text(encoding="utf-8"))
    assert after["generated"] != "NOT-A-DATE", "corrupt generated field must not survive a rerun"


def test_main_rewrites_a_crlf_existing_file_even_though_content_matches(tmp_path, monkeypatch):
    """The guard's read path decodes bytes directly, with no
    universal-newline translation, so a CRLF copy of an otherwise-
    canonical snapshot must not compare equal to the LF text this
    script generates in memory.  Read and write must agree on the
    exact-bytes axis: a CRLF file must be treated as different from
    the canonical LF bytes and get rewritten."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)

    fixed_headers = {"alp/x.h": dict(_EMPTY_HEADER)}
    monkeypatch.setattr(abi, "collect", lambda include_root: fixed_headers)

    out = tmp_path / "v0.10-snapshot.json"
    existing = {"version": "v0.10", "generated": "2020-01-01", "headers": fixed_headers}
    canonical = json.dumps(existing, indent=2, sort_keys=True) + "\n"
    out.write_bytes(canonical.replace("\n", "\r\n").encode("utf-8"))
    before = out.read_bytes()

    monkeypatch.setattr(
        sys, "argv", ["abi_snapshot.py", "--version", "v0.10", "--output", str(out)]
    )
    rc = abi.main()
    assert rc == 0
    after = out.read_bytes()
    assert after != before, "a CRLF-only difference must still trigger a rewrite"
    assert b"\r\n" not in after, "the rewritten file must be LF-exact, matching the write side"


@pytest.mark.parametrize("existing_payload", ['["a", "b"]', "5"])
def test_main_does_not_crash_on_a_json_valid_non_object_existing_file(
    tmp_path, monkeypatch, existing_payload
):
    """A JSON-valid but non-object existing snapshot (a bare list, a
    bare int) must not crash `main()` with a raw traceback.  It is not
    comparable, so the guard falls through to the real write path and
    overwrites it."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)
    monkeypatch.setattr(abi, "collect", lambda include_root: {"alp/x.h": dict(_EMPTY_HEADER)})

    out = tmp_path / "v0.10-snapshot.json"
    out.write_text(existing_payload, encoding="utf-8")

    monkeypatch.setattr(
        sys, "argv", ["abi_snapshot.py", "--version", "v0.10", "--output", str(out)]
    )
    rc = abi.main()
    assert rc == 0
    after = json.loads(out.read_text(encoding="utf-8"))
    assert after["headers"] == {"alp/x.h": dict(_EMPTY_HEADER)}


def test_main_does_not_crash_on_a_non_utf8_existing_file(tmp_path, monkeypatch):
    """A non-UTF-8 existing snapshot must not crash `main()` with a raw
    `UnicodeDecodeError` traceback -- `.decode("utf-8")` raises that, a
    `ValueError` subclass the guard's `except (OSError, ValueError)`
    clause catches.  The file is not decodable, so the guard falls
    through to the real write path and overwrites it."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)
    monkeypatch.setattr(abi, "collect", lambda include_root: {"alp/x.h": dict(_EMPTY_HEADER)})

    out = tmp_path / "v0.10-snapshot.json"
    out.write_bytes(b'{"version": "v0.10", "generated": "2020-01-01", "b": "\xff"}')

    monkeypatch.setattr(
        sys, "argv", ["abi_snapshot.py", "--version", "v0.10", "--output", str(out)]
    )
    rc = abi.main()
    assert rc == 0
    after = json.loads(out.read_text(encoding="utf-8"))
    assert after["headers"] == {"alp/x.h": dict(_EMPTY_HEADER)}


def test_main_writes_output_on_first_write_with_no_existing_file(tmp_path, monkeypatch):
    """No prior file on disk -- there is nothing to compare against, so
    the guard must not block the very first write."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)
    monkeypatch.setattr(abi, "collect", lambda include_root: {"alp/x.h": dict(_EMPTY_HEADER)})

    out = tmp_path / "v0.10-snapshot.json"
    assert not out.exists()
    monkeypatch.setattr(
        sys, "argv", ["abi_snapshot.py", "--version", "v0.10", "--output", str(out)]
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


def test_main_diff_corrupt_json_returns_2_not_1(tmp_path, monkeypatch, capsys):
    """A truncated/corrupt snapshot must not collide with exit code 1
    ("ABI changed") -- a bare caller (e.g. `release.yml`'s
    `abi_snapshot.py --diff`, no `tee`/grep to tell the codes apart)
    would otherwise read a corrupt file on disk as a real ABI
    regression at tag time."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.10.1\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)

    prior = tmp_path / "corrupt.json"
    prior.write_text('{"head', encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["abi_snapshot.py", "--diff", str(prior)])
    rc = abi.main()
    assert rc == 2
    assert "cannot parse" in capsys.readouterr().err


# ---------------------------------------------------------------------
# 3. `--print-current-version`: the label CI names the snapshot with
# ---------------------------------------------------------------------


def test_print_current_version_emits_the_label_ci_builds_the_path_from(
    tmp_path, monkeypatch, capsys
):
    """`.github/workflows/pr-generated-files.yml` composes the snapshot
    path from this output, so it must print the bare `vMAJOR.MINOR`
    label and nothing else -- a trailing note or a `0.11.0` here would
    send the regen at `docs/abi/<junk>-snapshot.json`."""
    sdk_yaml = tmp_path / "sdk_version.yaml"
    sdk_yaml.write_text("version: 0.11.0\nstatus: released\n", encoding="utf-8")
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", sdk_yaml)
    monkeypatch.setattr(sys, "argv", ["abi_snapshot.py", "--print-current-version"])

    rc = abi.main()

    assert rc == 0
    assert capsys.readouterr().out == "v0.11\n"


def test_print_current_version_fails_loudly_when_unresolvable(tmp_path, monkeypatch):
    """A silent empty string would make CI compose
    `docs/abi/-snapshot.json`, miss it, and fail somewhere far from the
    cause.  Exit nonzero instead so the step stops there."""
    monkeypatch.setattr(abi, "SDK_VERSION_YAML", tmp_path / "does-not-exist.yaml")
    monkeypatch.setattr(sys, "argv", ["abi_snapshot.py", "--print-current-version"])

    assert abi.main() != 0


# ---------------------------------------------------------------------
# 4. issue #826: the CI gate derives the version, never names it
# ---------------------------------------------------------------------

GENERATED_FILES_WORKFLOW = REPO / ".github" / "workflows" / "pr-generated-files.yml"
ABI_SNAPSHOT_WORKFLOW = REPO / ".github" / "workflows" / "pr-abi-snapshot.yml"


@pytest.mark.parametrize("workflow", [GENERATED_FILES_WORKFLOW, ABI_SNAPSHOT_WORKFLOW])
def test_ci_gate_contains_no_abi_version_literal(workflow):
    """The class fix for #826.  A hardcoded `v0.11` in either workflow
    fails silently at the next release: the steps keep regenerating the
    PREVIOUS (frozen) snapshot and diffing it against its own committed
    copy, so the gate stays green while testing nothing about the
    current snapshot.  No literal can be left for a release cut to
    forget -- the version has to be derived at run time.  Parametrized
    over both workflows: `pr-abi-snapshot.yml` derives the same way as
    `pr-generated-files.yml` and carries no literal-guard of its own, so
    a regression there would go unnoticed if only one workflow were
    checked.

    Scoped to the executable body: prose in comments is not what runs.
    The ABI freeze gate (issue #996) derives its baseline from `ls
    docs/abi/v*-snapshot.json | sort -V`, excluding whatever
    `--print-current-version` names as current -- a glob, not a version
    literal -- so it carries no exception here.
    """
    text = workflow.read_text(encoding="utf-8")
    body = "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#")
    )

    offenders = [m.group(0) for m in re.finditer(r"v\d+\.\d+-snapshot\.json", body)]
    assert not offenders, (
        f"{workflow.name} names snapshot file(s) {offenders} "
        f"literally; derive from metadata/sdk_version.yaml instead (#826)"
    )
    assert "--print-current-version" in body, (
        "the workflow must resolve the current version from the single source"
    )


def _extract_workflow_run_step(workflow_path: Path, job: str, step_id: str) -> str:
    """Return the `run:` script body of the `jobs.<job>.steps` entry
    with the given `id`, parsed out of the workflow YAML -- so a test
    can execute the WORKFLOW'S OWN text instead of a hand-typed copy
    of it (a copy can drift from the workflow and still pass)."""
    doc = yaml.safe_load(workflow_path.read_text(encoding="utf-8"))
    for step in doc["jobs"][job]["steps"]:
        if step.get("id") == step_id:
            return step["run"]
    raise AssertionError(f"no step id={step_id!r} in {job!r} of {workflow_path}")


@pytestmark_bash
def test_ci_gate_and_test_all_sh_resolve_the_same_snapshot(tmp_path):
    """Runs the WORKFLOW'S OWN `run:` script -- extracted from the YAML
    by `_extract_workflow_run_step`, not re-typed here -- and proves it
    lands on the same snapshot path `abi_current_snapshot` does.  The
    two gates must agree, or a green local run means nothing about the
    PR gate: that divergence WAS the bug in #795 (test-all.sh checked
    v0.9 while CI checked v0.10).  Mutation-proven: point the workflow's
    path composition at a different suffix and this goes red, because
    it runs that composition rather than a copy of it."""
    run_script = _extract_workflow_run_step(GENERATED_FILES_WORKFLOW, "check", "abi")
    github_output = tmp_path / "github_output"
    github_output.write_text("", encoding="utf-8")
    proc = subprocess.run(
        ["bash", "-c", run_script],
        cwd=str(REPO),
        env={**os.environ, "GITHUB_OUTPUT": str(github_output)},
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    outputs = dict(
        line.split("=", 1) for line in github_output.read_text(encoding="utf-8").splitlines()
    )
    ci_snapshot = outputs["snapshot"]
    assert (REPO / ci_snapshot).is_file(), (
        f"{ci_snapshot} is what the CI step resolves to but is not committed"
    )

    func = _extract_bash_function(TEST_ALL.read_text(encoding="utf-8"), "abi_current_snapshot")
    local = subprocess.run(
        ["bash", "-c", f"{func}\nabi_current_snapshot"],
        cwd=str(REPO),
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert local.returncode == 0, local.stdout + local.stderr
    assert local.stdout.strip() == ci_snapshot


@pytestmark_bash
def test_ci_gate_fails_cleanly_when_snapshot_not_committed(sdk_repo_with_version):
    """Mutation-disproven gap: `test_ci_gate_and_test_all_sh_resolve_the_same_snapshot`
    above only ever runs against a repo where the snapshot exists, so
    neither replacing this guard's `if [ ! -f "${snapshot}" ]; then`
    with `if false; then`, nor deleting the whole block through its
    trailing `fi`, turned that test red.  Run the WORKFLOW'S OWN
    `id: abi` step -- extracted, not retyped -- against a fake root
    whose sdk_version.yaml declares a version with NO committed
    snapshot (the exact release-cut window this guard exists for) and
    assert it fails loudly instead of silently letting the untracked
    regen slip past `git diff`."""
    root = sdk_repo_with_version("9.9.9")
    run_script = _extract_workflow_run_step(GENERATED_FILES_WORKFLOW, "check", "abi")
    github_output = root / "github_output"
    github_output.write_text("", encoding="utf-8")
    proc = subprocess.run(
        ["bash", "-c", run_script],
        cwd=str(root),
        env={**os.environ, "GITHUB_OUTPUT": str(github_output)},
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert proc.returncode != 0, proc.stdout + proc.stderr
    assert "::error::" in proc.stdout
    assert "v9.9" in proc.stdout
    assert not (root / "docs" / "abi" / "v9.9-snapshot.json").exists()


def test_generated_files_stage_does_not_swallow_a_failed_regen():
    """#795's residue: the regen carried `|| true`, so a snapshot that
    could not be regenerated (e.g. the write guard rejecting a stale
    label, exit 2) left the stage green while the diff below it checked
    a stale file -- the local run passes, the PR gate fails."""
    body = _extract_bash_function(
        TEST_ALL.read_text(encoding="utf-8"), "stage_generated_files"
    )
    # Join continuations first: the pre-fix code carried `|| true` on the
    # `--output ... || true` continuation LINE, not on the line naming
    # abi_snapshot.py, so a per-line scan reads clean while the bug is
    # right there.  Match the statement the shell actually executes.
    statements = re.sub(r"\\\n\s*", " ", body).splitlines()
    abi_statements = [s for s in statements if "abi_snapshot.py" in s]
    assert abi_statements, "stage_generated_files no longer regenerates the ABI snapshot"
    assert not any("|| true" in s for s in abi_statements), (
        "a failed ABI regen must fail the stage, not pass silently (#795)"
    )


# ---------------------------------------------------------------------
# 5. issue #996: the freeze gate itself -- blocks a REMOVED symbol vs
# the last released snapshot, does not block a CHANGED-only diff.
# ---------------------------------------------------------------------


@pytestmark_bash
def test_freeze_gate_baseline_is_the_real_last_released_snapshot():
    """`test_freeze_gate_fails_on_a_removed_symbol` and
    `..._passes_on_a_changed_only_diff` below only ever exercise the
    gate against a PLANTED `v99.9x-snapshot.json` -- a version number
    engineered to sort above anything real, which proves the gate
    picks whatever `sort -V | tail -1` names, but not that this
    resolves to the actual previous release on a real, unmodified
    `docs/abi/`.  That's the exact claim `docs/abi/README.md`'s
    freeze-gate carve-out makes ("once CURRENT is off the list, the
    highest remaining label genuinely is the last release"): run the
    baseline computation alone, against the real committed snapshots,
    with no fixture, and check it against a Python-side recomputation
    of "second-highest committed version" -- an independent
    implementation, not a copy of the bash line under test."""
    run_script = _extract_workflow_run_step(
        GENERATED_FILES_WORKFLOW, "check", "abi_freeze_gate"
    )
    current = abi.current_snapshot_version()
    assert current is not None
    proc = subprocess.run(
        ["bash", "-c", run_script],
        cwd=str(REPO),
        env={**os.environ, "ABI_VERSION": current},
        capture_output=True,
        text=True,
        timeout=60,
    )
    # REMOVED (rc=1) or clean (rc=0) both print the "Comparing..." line
    # before the diff even runs; a hard failure (rc>=2, a real
    # abi_snapshot.py error) would mean the baseline was never resolved.
    assert proc.returncode in (0, 1), proc.stdout + proc.stderr

    released = [
        p
        for p in (REPO / "docs" / "abi").glob("v*-snapshot.json")
        if p.name != f"{current}-snapshot.json"
    ]
    assert released, "no released snapshot on disk to compare against"

    def _key(p: Path) -> tuple[int, int]:
        m = re.match(r"v(\d+)\.(\d+)-snapshot\.json$", p.name)
        assert m, f"unexpected snapshot filename: {p.name}"
        return (int(m.group(1)), int(m.group(2)))

    expected = max(released, key=_key)
    assert f"Comparing current ABI against docs/abi/{expected.name}" in proc.stdout, (
        f"expected the gate to pick {expected.name} as the last released "
        f"snapshot; got:\n{proc.stdout}"
    )


def _run_freeze_gate(baseline_path: Path, payload: dict) -> subprocess.CompletedProcess:
    """Write `payload` to `baseline_path` (a throwaway file under the
    real docs/abi/ -- the step's `python3 scripts/abi_snapshot.py`
    call is a relative path, so it must run with cwd=REPO), execute
    the WORKFLOW'S OWN `id: abi_freeze_gate` step against it, then
    remove the file -- proving the gate without mutating anything real
    under include/alp/."""
    assert not baseline_path.exists(), f"would clobber a real file: {baseline_path}"
    baseline_path.write_text(json.dumps(payload), encoding="utf-8")
    try:
        run_script = _extract_workflow_run_step(
            GENERATED_FILES_WORKFLOW, "check", "abi_freeze_gate"
        )
        current = abi.current_snapshot_version()
        assert current is not None
        return subprocess.run(
            ["bash", "-c", run_script],
            cwd=str(REPO),
            env={**os.environ, "ABI_VERSION": current},
            capture_output=True,
            text=True,
            timeout=60,
        )
    finally:
        baseline_path.unlink()


@pytestmark_bash
def test_freeze_gate_fails_on_a_removed_symbol():
    """A symbol present in the baseline but absent from every real
    header today (a fabricated name, not a mutated one) must fail the
    step and name the removed symbol -- the exact case #996 reported
    as never having been checked."""
    baseline = REPO / "docs" / "abi" / "v99.99-snapshot.json"
    payload = {
        "version": "v99.99",
        "generated": "1970-01-01",
        "headers": {
            "alp/peripheral.h": {
                "functions": {
                    "alp___freeze_gate_regression_test_only": {
                        "signature": "void alp___freeze_gate_regression_test_only(void);",
                        "hash": "0000000000000000",
                    }
                },
                "typedefs": {},
                "macros": {},
                "variables": {},
            }
        },
    }
    proc = _run_freeze_gate(baseline, payload)
    assert proc.returncode == 1, proc.stdout + proc.stderr
    assert (
        "REMOVED function alp/peripheral.h::alp___freeze_gate_regression_test_only"
        in proc.stdout
    )
    assert "::error::Public symbol(s) removed" in proc.stdout


@pytestmark_bash
def test_freeze_gate_passes_on_a_moved_symbol():
    """A relocated symbol must NOT fail the step.

    This pins the coupling between `diff()`'s MOVED verdict and the
    WORKFLOW's own `grep -q '^  REMOVED '` -- the unit tests in
    test_abi_snapshot.py prove diff() emits MOVED, but only running the
    real step proves the gate does not match it. A header split that
    keeps every symbol reachable is allowed; see docs/abi/README.md.

    Built from a real, currently-reachable relocation: a macro that lives
    in a board header today is recorded in the baseline as having lived in
    the hand-written parent that unconditionally includes it, so the
    current tree reads as a move INTO the routes header.
    """
    baseline = REPO / "docs" / "abi" / "v99.97-snapshot.json"
    curr = json.loads(
        subprocess.run(
            [sys.executable, "scripts/abi_snapshot.py", "--version", "v99.97"],
            cwd=REPO,
            capture_output=True,
            text=True,
            check=True,
        ).stdout
    )
    owner = "alp/boards/alp_e1m_evk.h"
    routes = "alp/boards/alp_e1m_evk_routes.h"
    moved_sym = next(iter(curr["headers"][routes]["macros"]))
    # Baseline = today's tree with one macro relocated back to the parent
    # header, i.e. exactly the shape a header split produces.
    payload = json.loads(json.dumps(curr))
    payload["headers"][owner]["macros"][moved_sym] = payload["headers"][routes][
        "macros"
    ].pop(moved_sym)

    proc = _run_freeze_gate(baseline, payload)

    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert f"MOVED   macro {moved_sym}: {owner} -> {routes}" in proc.stdout, proc.stdout
    assert "::error::Public symbol(s) removed" not in proc.stdout
    assert not any(
        line.startswith("  REMOVED ") for line in proc.stdout.splitlines()
    ), proc.stdout


@pytestmark_bash
def test_freeze_gate_passes_on_a_changed_only_diff():
    """A CHANGED entry (a real symbol, deliberately mis-hashed so it
    still exists but its recorded signature differs) must NOT fail the
    step -- docs/contribution.md's ABI policy allows a signature/field
    change between pre-1.0 minors; only a silent REMOVED blocks."""
    baseline = REPO / "docs" / "abi" / "v99.98-snapshot.json"
    payload = {
        "version": "v99.98",
        "generated": "1970-01-01",
        "headers": {
            "alp/peripheral.h": {
                "functions": {
                    "alp_gpio_read": {
                        "signature": "not the real signature",
                        "hash": "0000000000000000",
                    }
                },
                "typedefs": {},
                "macros": {},
                "variables": {},
            }
        },
    }
    proc = _run_freeze_gate(baseline, payload)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "CHANGED function alp/peripheral.h::alp_gpio_read" in proc.stdout


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-v"]))
