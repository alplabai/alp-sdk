# SPDX-License-Identifier: Apache-2.0
"""Guard against a silently-uncovered metadata/ file type (issue #1045).

`alp_lock._digests()['metadata']` only hashes what `_METADATA_DIGEST_GLOBS`
matches. A new machine-read file type added under metadata/ without widening
that tuple would build a lock that verifies clean while the file stays
invisible to `--check` -- the exact gap that let metadata/socs/**/*.json
(the SoC specs -- memory maps, variant resolution, core topology) drift
undetected before this fix.

Two checks:
  1. every git-tracked file under metadata/ is either matched by
     `_METADATA_DIGEST_GLOBS` or explicitly named (with a reason) in
     `_UNCOVERED_SUFFIXES` below -- an explicit bounded allowlist, not an
     open door (same shape as the allowlists in check_bootstrap_manifest.py).
  2. the widening actually bites: mutating a `metadata/socs/**/*.json` file
     must trip `digests.metadata` drift.
"""
import fnmatch
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

# Suffixes deliberately outside `alp_lock._METADATA_DIGEST_GLOBS`, each with
# a reason a build cannot depend on it changing. Keep this in lockstep with
# the comment above `_METADATA_DIGEST_GLOBS` in scripts/alp_lock/__init__.py.
_UNCOVERED_SUFFIXES = {
    ".md": "documentation prose -- cannot change what a build produces",
    ".gitkeep": "empty placeholder marking a tracked-but-empty directory",
}


def _git_ls_files_metadata():
    git = shutil.which("git")
    if git is None:
        return None
    try:
        out = subprocess.run([git, "-C", str(REPO), "ls-files", "metadata"],
                             capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError:
        return None
    return [line for line in out.stdout.splitlines() if line]


def test_every_tracked_metadata_file_is_covered_or_allowlisted():
    files = _git_ls_files_metadata()
    if not files:
        pytest.skip("git ls-files unavailable")
    import alp_lock
    globs = alp_lock._METADATA_DIGEST_GLOBS
    uncovered = []
    for f in files:
        name = Path(f).name
        if any(name.endswith(suf) for suf in _UNCOVERED_SUFFIXES):
            continue
        if any(fnmatch.fnmatch(f, "*/" + g) or fnmatch.fnmatch(f, g) for g in globs):
            continue
        uncovered.append(f)
    assert uncovered == [], (
        f"metadata/ file(s) matched by no alp_lock digest glob and not in "
        f"_UNCOVERED_SUFFIXES: {uncovered} -- widen "
        f"alp_lock._METADATA_DIGEST_GLOBS, or add an allowlist reason if "
        f"the file is genuinely not build input")


def _fixture_ws(tmp_path):
    (tmp_path / "scripts").mkdir(parents=True, exist_ok=True)
    (tmp_path / "scripts" / "requirements.txt").write_text("")
    (tmp_path / "metadata").mkdir(parents=True, exist_ok=True)
    (tmp_path / "metadata" / "sdk_version.yaml").write_text(
        "version: 9.9.9\nstatus: released\n")
    (tmp_path / "west.yml").write_text("manifest:\n  projects: []\n")
    return tmp_path


def test_metadata_digest_catches_soc_spec_drift(tmp_path):
    """A SoC spec (metadata/socs/**/*.json) is as load-bearing as metadata
    gets -- memory maps, variant resolution, core topology all derive from
    it. Editing one must trip `digests.metadata` drift.

    Confirmed manually against the pre-fix single-glob `_dir_digest` (only
    `**/*.yaml`) that this test FAILS there -- it only passes once JSON is
    in `_METADATA_DIGEST_GLOBS`.
    """
    import alp_lock
    ws = _fixture_ws(tmp_path)
    socs = ws / "metadata" / "socs" / "alif" / "ensemble"
    socs.mkdir(parents=True)
    (socs / "e8.json").write_text('{"variant": "e8", "cores": 1}')

    locked = alp_lock.build_lock(ws)
    assert alp_lock.verify_lock(locked, ws) == []

    (socs / "e8.json").write_text('{"variant": "e8", "cores": 2}')
    drifts = alp_lock.verify_lock(locked, ws)
    assert any(d.path == "digests.metadata" for d in drifts), drifts
