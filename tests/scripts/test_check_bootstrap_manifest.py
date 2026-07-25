# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_bootstrap_manifest.py.

The gate is 100% regex-driven against metadata/bootstrap.json + its schema +
scripts/bootstrap.sh + scripts/bootstrap.ps1 + west.yml + README.md + four CI
workflows. Each test here mutates a TEMP COPY of that corpus and asserts the
gate actually fires for the documented failure mode -- a green run on the
real repo alone proves nothing about whether the gate catches drift.

Run locally:

    python -m pytest tests/scripts/test_check_bootstrap_manifest.py -q
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_bootstrap_manifest.py"

# Import the gate module directly so tests can monkeypatch its path
# constants at the module (each corpus file the gate reads is a hardcoded
# Path built from its own REPO, not a --root CLI flag) rather than spawning
# a subprocess per case.
sys.path.insert(0, str(REPO / "scripts"))
import check_bootstrap_manifest as gate  # noqa: E402

# The exact relative-path corpus the gate reads (mirrors gate.CI_WORKFLOWS +
# its other module-level Path constants).
_CORPUS_RELPATHS = [
    "metadata/bootstrap.json",
    "metadata/schemas/bootstrap-v1.schema.json",
    "west.yml",
    "scripts/bootstrap.sh",
    "scripts/bootstrap.ps1",
    "README.md",
    ".github/workflows/pr-twister.yml",
    ".github/workflows/pr-tier-a-libraries.yml",
    ".github/workflows/nightly-aen-hil.yml",
    ".github/workflows/pr-getting-started-aen801.yml",
]


def _scaffold(tmp_path: Path) -> None:
    """Copy the real corpus into tmp_path byte-for-byte -- tests mutate
    this COPY, never the real repo."""
    for rel in _CORPUS_RELPATHS:
        src = REPO / rel
        dst = tmp_path / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def _point_gate_at(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """Repoint every module-level Path constant the gate reads at the
    tmp_path copy, and give it a clean sys.argv (main()'s own
    `ap.parse_args()` would otherwise choke on pytest's own argv)."""
    monkeypatch.setattr(sys, "argv", ["check_bootstrap_manifest.py"])
    monkeypatch.setattr(gate, "REPO", tmp_path)
    monkeypatch.setattr(gate, "MANIFEST", tmp_path / "metadata/bootstrap.json")
    monkeypatch.setattr(gate, "SCHEMA", tmp_path / "metadata/schemas/bootstrap-v1.schema.json")
    monkeypatch.setattr(gate, "WEST_YML", tmp_path / "west.yml")
    monkeypatch.setattr(gate, "BOOTSTRAP_SH", tmp_path / "scripts/bootstrap.sh")
    monkeypatch.setattr(gate, "BOOTSTRAP_PS1", tmp_path / "scripts/bootstrap.ps1")
    monkeypatch.setattr(gate, "README_MD", tmp_path / "README.md")
    monkeypatch.setattr(gate, "CI_WORKFLOWS", [
        tmp_path / ".github/workflows/pr-twister.yml",
        tmp_path / ".github/workflows/pr-tier-a-libraries.yml",
        tmp_path / ".github/workflows/nightly-aen-hil.yml",
        tmp_path / ".github/workflows/pr-getting-started-aen801.yml",
    ])


def _edit_manifest(tmp_path: Path, mutate) -> None:
    p = tmp_path / "metadata/bootstrap.json"
    data = json.loads(p.read_text(encoding="utf-8"))
    mutate(data)
    p.write_text(json.dumps(data, indent=2), encoding="utf-8")


def _replace(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    assert old in text, f"fixture assumption broken: {old!r} not found in {path}"
    path.write_text(text.replace(old, new, 1), encoding="utf-8")


# ---------------------------------------------------------------------
# 0. Baseline: the real repo passes cleanly (subprocess smoke test).
# ---------------------------------------------------------------------


def test_default_corpus_passes():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT)], capture_output=True, text=True,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "OK" in proc.stdout


def test_scaffolded_copy_passes_unmodified(tmp_path, monkeypatch, capsys):
    """Sanity check for the scaffold/monkeypatch machinery itself: an
    untouched copy of the real corpus must also pass, or every failure-mode
    test below would be meaningless (could be failing for an unrelated
    scaffold bug, not the mutation under test)."""
    _scaffold(tmp_path)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "OK" in out


# ---------------------------------------------------------------------
# 1. Schema validation
# ---------------------------------------------------------------------


def test_schema_missing_required_key_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d.pop("venv"))
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "schema:" in err


# ---------------------------------------------------------------------
# 2. west.yml disagreement
# ---------------------------------------------------------------------


def test_west_yml_revision_disagreement_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _replace(tmp_path / "west.yml", "revision: v4.4.0", "revision: v4.5.0")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "west.yml pins zephyr revision" in err
    assert "v4.5.0" in err


# ---------------------------------------------------------------------
# 3. Hardcoded Zephyr version literal
# ---------------------------------------------------------------------


def test_hardcoded_literal_in_bootstrap_sh_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    sh = tmp_path / "scripts/bootstrap.sh"
    text = sh.read_text(encoding="utf-8")
    # Inject a literal (non-comment) hardcode of the pinned version -- the
    # exact "read from JSON, not baked in" regression this check exists for.
    sh.write_text(text + '\nZEPHYR_VERSION_SHADOW="v4.4.0"\n', encoding="utf-8")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "scripts/bootstrap.sh" in err
    assert "hardcodes the pinned Zephyr version" in err


def test_hardcoded_literal_in_comment_is_not_flagged(tmp_path, monkeypatch, capsys):
    """A rationale COMMENT is allowed to name the version it's explaining --
    only non-comment code is a problem (item 12 of the review)."""
    _scaffold(tmp_path)
    sh = tmp_path / "scripts/bootstrap.sh"
    text = sh.read_text(encoding="utf-8")
    sh.write_text(text + "\n# bumped from v3.7.0 LTS to v4.4.0 in v0.5\n", encoding="utf-8")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out


def test_hardcoded_literal_inside_heredoc_body_fails(tmp_path, monkeypatch, capsys):
    """Reproduces the review repro verbatim (item 2): a '#'-prefixed line
    INSIDE bootstrap.sh's `cat <<'EOF'` heredoc body is printed OUTPUT, not
    a genuine source comment -- the naive `line.strip().startswith("#")`
    skip this replaces let a Zephyr version literal printed verbatim to the
    user slip the gate entirely. Rewriting the existing '# Run the local
    test suite:' output line to name the pinned version must now fail."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "scripts/bootstrap.sh",
        "  # Run the local test suite:",
        "  # Zephyr v4.4.0 is required for this suite:",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "scripts/bootstrap.sh" in err
    assert "hardcodes the pinned Zephyr version" in err


def test_unrelated_version_strings_not_flagged(tmp_path, monkeypatch, capsys):
    """west>=0.14.0 (west.pipSpec, already consumed) and a Zephyr *SDK*
    version are NOT the pinned Zephyr version and must not trip this check
    -- regression for the false positives named in the review (item 12)."""
    _scaffold(tmp_path)
    sh = tmp_path / "scripts/bootstrap.sh"
    text = sh.read_text(encoding="utf-8")
    sh.write_text(
        text + '\nSOME_OTHER_SPEC="west>=0.14.0"\nSDK_VER="1.0.1"\n',
        encoding="utf-8",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out


# ---------------------------------------------------------------------
# 4. CI-workflow pin disagreement
# ---------------------------------------------------------------------


def test_ci_workflow_mr_pin_disagreement_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _replace(
        tmp_path / ".github/workflows/pr-tier-a-libraries.yml",
        "--mr v4.4.0", "--mr v4.5.0",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "pr-tier-a-libraries.yml" in err
    assert "pins Zephyr" in err


def test_ci_cache_key_disagreement_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _replace(
        tmp_path / ".github/workflows/pr-twister.yml",
        "key: zephyr-v4.4.0-host-${{ runner.os }}",
        "key: zephyr-v4.5.0-host-${{ runner.os }}",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "pr-twister.yml" in err


def test_zephyr_sdk_cache_key_ignored(tmp_path, monkeypatch, capsys):
    """The zephyr-SDK toolchain cache key (a DIFFERENT release, pinned
    independently) must not be compared against zephyr.version -- item 11
    of the review. Changing ONLY that key must not fail the gate."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / ".github/workflows/pr-twister.yml",
        "key: zephyr-sdk-arm-zephyr-eabi-v4.4.0-${{ runner.os }}",
        "key: zephyr-sdk-arm-zephyr-eabi-v9.9.9-${{ runner.os }}",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out


# ---------------------------------------------------------------------
# 5. README badge disagreement
# ---------------------------------------------------------------------


def test_readme_badge_disagreement_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _replace(
        tmp_path / "README.md",
        "Zephyr-v4.4.0-blue",
        "Zephyr-v4.5.0-blue",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "README.md badge pins Zephyr" in err


# ---------------------------------------------------------------------
# 6. prerequisites drift (posix / windows) + python floor drift
# ---------------------------------------------------------------------


def test_prerequisites_posix_drift_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["prerequisites"].__setitem__(
        "posix", ["git", "python3"]))  # dropped "cmake"
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "REQUIRED_BINS" in err
    assert "prerequisites.posix" in err


def test_prerequisites_windows_drift_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["prerequisites"].__setitem__(
        "windows", ["git", "cmake", "python"]))  # dropped "ninja"
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "$Prereqs" in err
    assert "prerequisites.windows" in err


def test_python_min_version_posix_drift_fails(tmp_path, monkeypatch, capsys):
    """bootstrap.sh's own PYTHON_MIN_VERSION drifts from the manifest while
    bootstrap.ps1 stays in sync -- isolates the POSIX-side check."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "scripts/bootstrap.sh",
        'PYTHON_MIN_VERSION="3.10"', 'PYTHON_MIN_VERSION="3.11"',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "scripts/bootstrap.sh hardcodes Python floor" in err


def test_python_min_version_windows_drift_fails(tmp_path, monkeypatch, capsys):
    """bootstrap.ps1's own floor check drifts from the manifest while
    bootstrap.sh stays in sync -- isolates the Windows-side check (this is
    the floor bootstrap.sh didn't even have before this change, item 7 of
    the review)."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "scripts/bootstrap.ps1",
        '-lt [version]"3.10"', '-lt [version]"3.11"',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "scripts/bootstrap.ps1 hardcodes Python floor" in err


# ---------------------------------------------------------------------
# 7. Orphaned nested leaf (item 10 of the review -- the west.pipSpec bug)
# ---------------------------------------------------------------------


def test_orphaned_nested_leaf_fails(tmp_path, monkeypatch, capsys):
    """Break BOTH scripts' reference to an existing, schema-valid leaf
    (pip.editableInstall) without touching the manifest -- reproduces the
    exact "declared but nothing reads it" shape west.pipSpec shipped in
    (the schema doesn't know the leaf became unread; only the generic
    per-leaf scan catches it)."""
    _scaffold(tmp_path)
    # A genuinely DIFFERENT identifier, not a superset-string of the
    # original needle (e.g. a mere "Renamed" suffix would still contain the
    # original `...editableInstall` substring inside it and silently pass
    # -- this exercises the real failure, not a fixture artefact).
    _replace(
        tmp_path / "scripts/bootstrap.sh",
        'd["pip"]["editableInstall"]', 'd["pip"]["renamedTarget"]',
    )
    _replace(
        tmp_path / "scripts/bootstrap.ps1",
        "$Manifest.pip.editableInstall", "$Manifest.pip.renamedTarget",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "pip.editableInstall" in err
    assert "not read by" in err


def test_leaf_consumed_by_only_one_script_is_not_orphaned(tmp_path, monkeypatch, capsys):
    """Fixture-assumption guard (not a mutation test): venv.posixBinDir is
    legitimately read by bootstrap.sh only in the real corpus (native
    Windows has no posix bin dir concept, so bootstrap.ps1 never references
    it) -- confirms that assumption still holds before the mutation test
    below relies on it, and that "at least one script", not "both", really
    is the bar the gate applies (item 10 of the original review)."""
    _scaffold(tmp_path)
    sh_text = (tmp_path / "scripts/bootstrap.sh").read_text(encoding="utf-8")
    ps1_text = (tmp_path / "scripts/bootstrap.ps1").read_text(encoding="utf-8")
    assert 'd["venv"]["posixBinDir"]' in sh_text
    assert "posixBinDir" not in ps1_text
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out


def test_leaf_orphaned_when_its_one_and_only_reader_stops_reading_it(tmp_path, monkeypatch, capsys):
    """The actual mutation test for "at least one script" (item 10 of the
    later review, which found the test above merely restated the fixture
    assumption without ever mutating anything): venv.posixBinDir has
    exactly ONE reader, bootstrap.sh -- break that one reference and the
    leaf must now be flagged orphaned, proving the single-reader special
    case is really exercised and not just coincidentally green."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "scripts/bootstrap.sh",
        'd["venv"]["posixBinDir"]', 'd["venv"]["renamedPosixBinDir"]',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "venv.posixBinDir" in err
    assert "not read by" in err


# ---------------------------------------------------------------------
# 8. Unknown top-level key (item 9 of the review -- must be reachable even
#    though additionalProperties:false ALSO trips schema validation)
# ---------------------------------------------------------------------


def test_unknown_top_level_key_fails_with_known_keys_guidance(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d.__setitem__("bogusFutureKey", "x"))
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    # Both the raw schema error AND the KNOWN_KEYS guidance must be visible
    # -- the whole point of running _check_known_keys unconditionally.
    assert "bogusFutureKey" in err
    assert "KNOWN_KEYS" in err


# ---------------------------------------------------------------------
# 9. Schema `minItems` + each script's own schemaVersion refusal (item 9 of
#    the later review -- both guards existed in the corpus already but had
#    no test locking them in).
# ---------------------------------------------------------------------


def test_empty_west_init_args_fails_schema_minitems(tmp_path, monkeypatch, capsys):
    """west.initArgs (like every other argv array in this schema) declares
    `minItems: 1` -- an empty array would silently produce `west  <repo>`
    (missing the actual init flags) at run time; the schema must catch it
    before either script ever sees it."""
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["west"].__setitem__("initArgs", []))
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "schema:" in err
    assert "west/initArgs" in err


def _bash_available_with_python3() -> bool:
    """bootstrap.sh's schemaVersion refusal sits AFTER the fact-loading
    step, which shells out to python3 even for --print-env -- skip cleanly
    (not fail) on a bash without a working python3 on PATH, same spirit as
    skipping the pwsh half below when pwsh isn't installed."""
    if shutil.which("bash") is None:
        return False
    proc = subprocess.run(["bash", "-c", "command -v python3"], capture_output=True)
    return proc.returncode == 0


def test_bootstrap_sh_refuses_unknown_schema_version(tmp_path):
    """scripts/bootstrap.sh:235's `[ "${SCHEMA_VERSION}" = "1" ] || die ...`
    guard, exercised end-to-end (not through the gate module -- this is the
    SCRIPT's own defence against a manifest shaped for a schema version it
    doesn't understand, e.g. if check_bootstrap_manifest.py never ran)."""
    if not _bash_available_with_python3():
        pytest.skip("bash + python3 not both available on PATH")
    scaffold_root = tmp_path / "sh-repo"
    (scaffold_root / "scripts").mkdir(parents=True)
    (scaffold_root / "metadata").mkdir(parents=True)
    shutil.copy2(REPO / "scripts" / "bootstrap.sh", scaffold_root / "scripts" / "bootstrap.sh")
    manifest = json.loads((REPO / "metadata" / "bootstrap.json").read_text(encoding="utf-8"))
    manifest["schemaVersion"] = 2
    (scaffold_root / "metadata" / "bootstrap.json").write_text(
        json.dumps(manifest), encoding="utf-8",
    )
    # --print-env is the cheapest path that still reaches the schemaVersion
    # guard (it loads the manifest facts, just like the full run) without
    # needing git/cmake/ninja or actually touching the network.
    proc = subprocess.run(
        ["bash", str(scaffold_root / "scripts" / "bootstrap.sh"), "--print-env"],
        capture_output=True, text=True, cwd=str(scaffold_root),
    )
    assert proc.returncode != 0, proc.stdout + proc.stderr
    assert "schemaVersion" in (proc.stdout + proc.stderr)


def test_bootstrap_ps1_refuses_unknown_schema_version(tmp_path):
    """scripts/bootstrap.ps1's mirrored `if ($Manifest.schemaVersion -ne 1)
    { Fail ... }` guard, exercised end-to-end. Skipped cleanly (not failed)
    when pwsh isn't installed on the host running the suite."""
    if shutil.which("pwsh") is None:
        pytest.skip("pwsh not available on PATH")
    scaffold_root = tmp_path / "ps1-repo"
    (scaffold_root / "scripts").mkdir(parents=True)
    (scaffold_root / "metadata").mkdir(parents=True)
    shutil.copy2(REPO / "scripts" / "bootstrap.ps1", scaffold_root / "scripts" / "bootstrap.ps1")
    manifest = json.loads((REPO / "metadata" / "bootstrap.json").read_text(encoding="utf-8"))
    manifest["schemaVersion"] = 2
    (scaffold_root / "metadata" / "bootstrap.json").write_text(
        json.dumps(manifest), encoding="utf-8",
    )
    # -PrintEnv needs no external tool at all (native ConvertFrom-Json, and
    # the python-version floor is skipped for -PrintEnv -- see the script's
    # own "Python version floor" comment), so it reaches the schemaVersion
    # guard with nothing else on PATH required.
    proc = subprocess.run(
        ["pwsh", "-NoProfile", "-File", str(scaffold_root / "scripts" / "bootstrap.ps1"), "-PrintEnv"],
        capture_output=True, text=True, cwd=str(scaffold_root),
    )
    assert proc.returncode != 0, proc.stdout + proc.stderr
    assert "schemaVersion" in (proc.stdout + proc.stderr)
