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
    monkeypatch.setattr(gate, "LIBRARIES_DIR", tmp_path / "metadata/libraries")
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
    _replace(tmp_path / "west.yml", "revision: v4.4.1", "revision: v4.5.0")
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
    sh.write_text(text + '\nZEPHYR_VERSION_SHADOW="v4.4.1"\n', encoding="utf-8")
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
        "  # Zephyr v4.4.1 is required for this suite:",
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
        "--mr v4.4.1", "--mr v4.5.0",
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
        "key: zephyr-v4.4.1-host-${{ runner.os }}",
        "key: zephyr-v4.5.0-host-${{ runner.os }}",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "pr-twister.yml" in err


def test_zephyr_sdk_cache_key_ignored(tmp_path, monkeypatch, capsys):
    """The zephyr-SDK toolchain cache key (a DIFFERENT release, pinned
    independently and gated separately by scripts/check_toolchain_lock.py
    against metadata/toolchains.json, issue #949 item 3) must not be
    compared against zephyr.version -- item 11 of the review. The real
    corpus no longer hardcodes a literal SDK-cache-key version at all (the
    #949 fix keys pr-twister.yml's cache on `${{ env.ZEPHYR_SDK_VERSION }}`
    instead) -- inject a synthetic literal-version line so this regex-
    exclusion behaviour itself stays regression-locked."""
    _scaffold(tmp_path)
    twister = tmp_path / ".github/workflows/pr-twister.yml"
    twister.write_text(
        twister.read_text(encoding="utf-8")
        + "          key: zephyr-sdk-arm-zephyr-eabi-v9.9.9-${{ runner.os }}\n",
        encoding="utf-8",
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
        "Zephyr-v4.4.1-blue",
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


# ---------------------------------------------------------------------
# 10. --fix propagator
# ---------------------------------------------------------------------


def test_fix_propagates_bumped_version_to_every_site(tmp_path, monkeypatch, capsys):
    """Targets v4.10.0 -- a different STRING LENGTH than v4.4.1 -- so the
    back-to-front `reversed(matches)` walk in `_apply_version_fix` (which
    exists solely to survive a length-changing target) is actually
    exercised; a same-length target like v4.5.0 would pass even with a
    broken offset-bookkeeping implementation."""
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["zephyr"].__setitem__("version", "v4.10.0"))
    _point_gate_at(tmp_path, monkeypatch)
    monkeypatch.setattr(sys, "argv", ["check_bootstrap_manifest.py", "--fix"])
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out

    # A change report line per rewritten site, plus the summary line.
    assert "west.yml:" in out
    assert "v4.4.1 -> v4.10.0" in out

    # The ordinary (no --fix) verify pass must now agree -- this IS the
    # point: --fix output is provable by the same gate that flagged drift.
    monkeypatch.setattr(sys, "argv", ["check_bootstrap_manifest.py"])
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "OK" in out

    assert "revision: v4.10.0" in (tmp_path / "west.yml").read_text(encoding="utf-8")
    assert "Zephyr-v4.10.0-blue" in (tmp_path / "README.md").read_text(encoding="utf-8")

    twister = (tmp_path / ".github/workflows/pr-twister.yml").read_text(encoding="utf-8")
    assert "--mr v4.10.0" in twister
    assert "key: zephyr-v4.10.0-host-${{ runner.os }}" in twister

    tier_a = (tmp_path / ".github/workflows/pr-tier-a-libraries.yml").read_text(encoding="utf-8")
    assert "--mr v4.10.0" in tier_a
    assert "key: zephyr-v4.10.0-tier-a-${{ runner.os }}" in tier_a

    nightly = (tmp_path / ".github/workflows/nightly-aen-hil.yml").read_text(encoding="utf-8")
    assert "--mr v4.10.0" in nightly

    getting_started = (tmp_path / ".github/workflows/pr-getting-started-aen801.yml").read_text(
        encoding="utf-8")
    assert "key: getting-started-aen801-zephyr-v4.10.0-${{ runner.os }}" in getting_started


def test_fix_is_idempotent(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["zephyr"].__setitem__("version", "v4.5.0"))
    _point_gate_at(tmp_path, monkeypatch)
    monkeypatch.setattr(sys, "argv", ["check_bootstrap_manifest.py", "--fix"])

    assert gate.main() == 0
    # `_apply_version_fix` writes with `newline=""` specifically so the
    # rewrite never flips these LF-pinned files to CRLF on a Windows host --
    # the other --fix tests use newline-agnostic `in` checks against
    # `.read_text()`, which stay green even if `newline=""` were deleted
    # (Windows universal-newline decoding hides the CRLF). Assert on the raw
    # bytes so that regression actually has a test.
    assert b"\r\n" not in (tmp_path / "west.yml").read_bytes()
    capsys.readouterr()
    before = {rel: (tmp_path / rel).read_bytes() for rel in _CORPUS_RELPATHS}

    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "nothing to do" in out

    after = {rel: (tmp_path / rel).read_bytes() for rel in _CORPUS_RELPATHS}
    assert before == after, "second --fix run must not touch a single byte"


def test_fix_does_not_touch_zephyr_sdk_cache_key(tmp_path, monkeypatch, capsys):
    """Item 11 of the original review, re-exercised through --fix this
    time: the Zephyr *SDK* toolchain cache key tracks a separate release
    and must survive a Zephyr revision bump untouched. The real corpus no
    longer hardcodes a literal SDK-cache-key version (issue #949 item 3
    keys it on `${{ env.ZEPHYR_SDK_VERSION }}` instead), so this test
    injects a synthetic literal-version line to keep the --fix-must-not-
    touch-this-key behaviour regression-locked."""
    _scaffold(tmp_path)
    twister_path = tmp_path / ".github/workflows/pr-twister.yml"
    twister_path.write_text(
        twister_path.read_text(encoding="utf-8")
        + "          key: zephyr-sdk-arm-zephyr-eabi-v4.4.0-${{ runner.os }}\n",
        encoding="utf-8",
    )
    _edit_manifest(tmp_path, lambda d: d["zephyr"].__setitem__("version", "v4.5.0"))
    _point_gate_at(tmp_path, monkeypatch)
    monkeypatch.setattr(sys, "argv", ["check_bootstrap_manifest.py", "--fix"])
    assert gate.main() == 0

    twister = twister_path.read_text(encoding="utf-8")
    assert "zephyr-sdk-arm-zephyr-eabi-v4.4.0" in twister
    assert "zephyr-sdk-arm-zephyr-eabi-v4.5.0" not in twister


def test_fix_fails_loudly_on_unmatchable_site(tmp_path, monkeypatch, capsys):
    """A regex that stops matching must never be a silent --fix no-op --
    break the one west.yml site's shape and --fix must name it and fail,
    not quietly leave west.yml unrewritten and exit 0."""
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["zephyr"].__setitem__("version", "v4.5.0"))
    _replace(
        tmp_path / "west.yml",
        "- name: zephyr\n      revision:",
        "- name: zephyr-renamed\n      revision:",
    )
    _point_gate_at(tmp_path, monkeypatch)
    monkeypatch.setattr(sys, "argv", ["check_bootstrap_manifest.py", "--fix"])
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "west.yml" in err
    assert "not found" in err


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


# ---------------------------------------------------------------------
# 11. In-tree Zephyr library manifest `version:` guard (finding: coap/
#     lwm2m/modbus's `version:` field is a live pin nothing verified --
#     it would stay stale after a future bump even with a green gate).
# ---------------------------------------------------------------------

# The three real in-tree-Zephyr-subsystem manifests, plus one real
# `module: null` manifest that must NOT be swept in (it pins its own
# upstream release, not Zephyr's) -- the negative case that proves the
# derivation is `module: null` AND `requires.os == ["zephyr"]`, not
# `module: null` alone.
_LIBRARY_RELPATHS = [
    "metadata/libraries/coap.yaml",
    "metadata/libraries/lwm2m.yaml",
    "metadata/libraries/modbus.yaml",
    "metadata/libraries/nlohmann-json.yaml",
]


def _scaffold_with_libraries(tmp_path: Path) -> None:
    _scaffold(tmp_path)
    for rel in _LIBRARY_RELPATHS:
        src = REPO / rel
        dst = tmp_path / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)


def test_library_manifest_version_drift_fails(tmp_path, monkeypatch, capsys):
    _scaffold_with_libraries(tmp_path)
    _replace(tmp_path / "metadata/libraries/coap.yaml", 'version: "4.4.1"', 'version: "4.4.0"')
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "metadata/libraries/coap.yaml" in err
    assert "4.4.0" in err


def test_library_manifest_with_own_upstream_pin_is_not_checked(tmp_path, monkeypatch, capsys):
    """nlohmann-json.yaml also has `integration.zephyr.module: null`, but it
    pins its OWN upstream release (3.11.3) and does not declare
    `requires.os: [zephyr]` -- it must never be compared against
    zephyr.version. Mutating it far away from 4.4.1 must NOT fail the gate."""
    _scaffold_with_libraries(tmp_path)
    text = (tmp_path / "metadata/libraries/nlohmann-json.yaml").read_text(encoding="utf-8")
    assert 'version: "3.11.3"' in text
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "OK" in out


def test_fix_rewrites_library_manifest_versions(tmp_path, monkeypatch, capsys):
    _scaffold_with_libraries(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["zephyr"].__setitem__("version", "v4.10.0"))
    _point_gate_at(tmp_path, monkeypatch)
    monkeypatch.setattr(sys, "argv", ["check_bootstrap_manifest.py", "--fix"])
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out

    for name in ("coap.yaml", "lwm2m.yaml", "modbus.yaml"):
        text = (tmp_path / "metadata/libraries" / name).read_text(encoding="utf-8")
        assert 'version: "4.10.0"' in text, f"{name} not rewritten: {text}"

    # nlohmann-json is NOT a --fix site -- it must survive the sweep
    # byte-for-byte untouched.
    nlohmann_text = (tmp_path / "metadata/libraries/nlohmann-json.yaml").read_text(
        encoding="utf-8")
    assert 'version: "3.11.3"' in nlohmann_text

    # The ordinary (no --fix) verify pass must now agree.
    monkeypatch.setattr(sys, "argv", ["check_bootstrap_manifest.py"])
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "OK" in out


# ---------------------------------------------------------------------
# 12. prerequisites.install (issue #949)
# ---------------------------------------------------------------------


def test_install_missing_tool_command_fails(tmp_path, monkeypatch, capsys):
    """A tool listed in prerequisites.windows with no matching
    install.windows entry is the exact hole that shipped the drifted/
    incomplete ninja hint in scripts/alp_cli/doctor.py -- the completeness
    assertion must catch it."""
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["prerequisites"]["install"]["windows"].pop("ninja"))
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "prerequisites.install.windows" in err
    assert "prerequisites.windows" in err


def test_install_linux_missing_tool_command_fails(tmp_path, monkeypatch, capsys):
    """The gate's own docstring (point 1 in `_check_install_commands`) calls
    this "the ONLY assertion covering install.linux / install.macos" -- yet
    nothing in this file ever mutated `install.linux`/`install.macos` before
    this test. Branch coverage reported the `if os_install != posix_tools:`
    line covered by two unrelated fixtures tripping it incidentally; only a
    real `if False:` mutation of that line exposed the gap (it still stayed
    42 passed, 1 skipped). Popping a tool from install.linux must fail."""
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["prerequisites"]["install"]["linux"].pop("cmake"))
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "prerequisites.install.linux" in err
    assert "prerequisites.posix" in err


def test_install_macos_missing_tool_command_fails(tmp_path, monkeypatch, capsys):
    """Same defect, macos side -- see test_install_linux_missing_tool_command_fails."""
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["prerequisites"]["install"]["macos"].pop("git"))
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "prerequisites.install.macos" in err
    assert "prerequisites.posix" in err


def test_install_command_missing_winget_id_fails_instead_of_going_dark(tmp_path, monkeypatch, capsys):
    """Reproduces the review finding verbatim: `_winget_ids_and_commands`
    must not silently drop an `install.windows` command whose shape
    `_WINGET_ID_RE` doesn't match -- if it did, the literal scan would cover
    nothing for that tool repo-wide even with a mirrored drift planted in
    both bootstrap.ps1's Hint= and README.md, and all three assertions
    would stay green."""
    _scaffold(tmp_path)
    drifted = "winget install Ninja-build.Ninja"  # no `--id`
    _edit_manifest(
        tmp_path,
        lambda d: d["prerequisites"]["install"]["windows"].__setitem__("ninja", drifted),
    )
    _replace(
        tmp_path / "scripts/bootstrap.ps1",
        'Hint = "winget install -e --id Ninja-build.Ninja"',
        f'Hint = "{drifted}"',
    )
    readme = tmp_path / "README.md"
    readme.write_text(
        readme.read_text(encoding="utf-8") + "\nwinget install --exact Ninja-build.Ninja\n",
        encoding="utf-8",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "prerequisites.install.windows.ninja" in err
    assert "--id <PackageId>" in err


def test_install_ps1_hint_disagreement_fails(tmp_path, monkeypatch, capsys):
    """scripts/bootstrap.ps1's own $Prereqs Hint= value for a tool must
    agree with prerequisites.install.windows[<tool>] byte-for-byte -- this
    is the exact shape of the shipped drift (missing `-e --id`)."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "scripts/bootstrap.ps1",
        'Hint = "winget install -e --id Ninja-build.Ninja"',
        'Hint = "winget install Ninja-build.Ninja"',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "$Prereqs entry 'ninja'" in err
    assert "disagrees with prerequisites.install.windows.ninja" in err


def test_install_ps1_hint_deleted_does_not_silently_drop_tool(tmp_path, monkeypatch, capsys):
    """The sibling of `_winget_ids_and_commands`'s "goes dark" fix, one
    level down: deleting a $Prereqs entry's `Hint = "..."` field entirely
    (leaving only `Name = "ninja"`) makes `_PS1_PREREQ_ENTRY_RE` skip that
    one entry -- `if not entries:` only fires when ZERO entries parse, so a
    partial parse used to pass silently while bootstrap.ps1:117 prints an
    empty hint (`ninja  ->  `) to the user."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "scripts/bootstrap.ps1",
        '@{ Name = "ninja";  Hint = "winget install -e --id Ninja-build.Ninja" }',
        '@{ Name = "ninja" }',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "no parseable" in err
    assert "prerequisites.install.windows.ninja" in err


def test_install_ps1_hint_reordered_does_not_silently_drop_tool(tmp_path, monkeypatch, capsys):
    """Same defect, different malformed shape: reordering an entry's fields
    to `Hint = "..."; Name = "..."` (rather than the `Name = "..."; Hint =
    "..."` order `_PS1_PREREQ_ENTRY_RE` requires) also makes that one entry
    unparseable -- and must be caught the same way, not silently pass."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "scripts/bootstrap.ps1",
        '@{ Name = "ninja";  Hint = "winget install -e --id Ninja-build.Ninja" }',
        '@{ Hint = "winget install -e --id Ninja-build.Ninja"; Name = "ninja" }',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "no parseable" in err
    assert "prerequisites.install.windows.ninja" in err


def test_install_literal_scan_catches_drifted_winget_id(tmp_path, monkeypatch, capsys):
    """A winget PACKAGE ID from install.windows (`Ninja-build.Ninja`)
    appearing anywhere in the scanned file set WITHOUT its full canonical
    command alongside it must fail -- this is exactly the shape
    scripts/alp_cli/doctor.py's drifted ninja hint had (`winget install
    Ninja-build.Ninja.`, missing `-e --id`)."""
    _scaffold(tmp_path)
    readme = tmp_path / "README.md"
    readme.write_text(
        readme.read_text(encoding="utf-8") + "\nwinget install Ninja-build.Ninja\n",
        encoding="utf-8",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "README.md" in err
    assert "winget package id 'Ninja-build.Ninja' found without its canonical command" in err


def test_install_literal_scan_catches_drifted_winget_id_in_markdown_heading(
    tmp_path, monkeypatch, capsys
):
    """A Markdown `#`-heading is not a comment -- `.md` files have no comment
    syntax at all, so every line must count. `.py`'s plain `#`-prefixed
    comment skip must not leak onto `.md` (issue #949 review): it used to,
    letting a drifted winget id hide inside a heading."""
    _scaffold(tmp_path)
    readme = tmp_path / "README.md"
    readme.write_text(
        readme.read_text(encoding="utf-8") +
        "\n## Install ninja: winget install Ninja-build.Ninja\n",
        encoding="utf-8",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "README.md" in err
    assert "winget package id 'Ninja-build.Ninja' found without its canonical command" in err


def test_install_literal_scan_ignores_unrelated_winget_id(tmp_path, monkeypatch, capsys):
    """dorssel.usbipd-win is not a winget ID `prerequisites.install.windows`
    declares -- the scan's trigger set is derived from the manifest, so this
    line is never even looked at (no allowlist entry needed, unlike the
    verb-triggered design this replaces)."""
    _scaffold(tmp_path)
    readme = tmp_path / "README.md"
    readme.write_text(
        readme.read_text(encoding="utf-8") +
        "\nwinget install -e --id dorssel.usbipd-win\n",
        encoding="utf-8",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "OK" in out


def test_install_literal_scan_ignores_combined_posix_line(tmp_path, monkeypatch, capsys):
    """Regression lock for the redesign: a combined multi-package posix
    one-liner (docs/getting-started.md's real shape) must pass with NO
    allowlist entry -- the literal scan doesn't trigger on posix installs
    at all (`git`/`cmake`/`python3` are bare words, not distinctive winget
    IDs; posix coverage stops at the completeness assertion instead, see
    `_check_install_commands`'s docstring)."""
    _scaffold(tmp_path)
    readme = tmp_path / "README.md"
    readme.write_text(
        readme.read_text(encoding="utf-8") +
        "\nbrew install cmake ninja python git\n"
        "sudo apt install -y cmake ninja-build python3 python3-pip git\n",
        encoding="utf-8",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "OK" in out


def test_install_clean_tree_passes(tmp_path, monkeypatch, capsys):
    """The unmodified scaffold (real prerequisites.install + real
    bootstrap.ps1 + real README.md) must pass with no --fix involved."""
    _scaffold(tmp_path)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "OK" in out


# ---------------------------------------------------------------------
# 12b. scripts/bootstrap.sh PREREQ_HINT_* agreement (issue #978 gate review)
# ---------------------------------------------------------------------


def test_bootstrap_sh_hint_value_drift_fails(tmp_path, monkeypatch, capsys):
    """A PREREQ_HINT_LINUX entry's command must agree with
    prerequisites.install.linux[<tool>] byte-for-byte -- the POSIX-side
    analogue of test_install_ps1_hint_disagreement_fails."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "scripts/bootstrap.sh",
        '"sudo apt-get install -y git"',
        '"sudo apt-get install git"',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "PREREQ_HINT_LINUX entry 'git'" in err
    assert "disagrees with prerequisites.install.linux.git" in err


def test_bootstrap_sh_hint_length_mismatch_fails(tmp_path, monkeypatch, capsys):
    """PREREQ_HINT_NAMES and PREREQ_HINT_LINUX/_MACOS are matched up by
    array POSITION (bash 3.2 has no `declare -A`) -- a length mismatch
    between them must be reported directly, not silently truncated by
    `zip`."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "scripts/bootstrap.sh",
        '    "sudo apt-get install -y cmake"\n',
        "",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "PREREQ_HINT_LINUX has 2 entries but PREREQ_HINT_NAMES has 3" in err
    assert "must stay parallel arrays" in err


def test_bootstrap_sh_hint_deleted_in_lockstep_does_not_silently_drop_tool(
    tmp_path, monkeypatch, capsys
):
    """Reproduces the review finding verbatim: deleting a tool's entry from
    PREREQ_HINT_NAMES + PREREQ_HINT_LINUX + PREREQ_HINT_MACOS in lockstep
    keeps the two arrays parallel (no length mismatch) and every remaining
    zip pair still agrees -- the old zip-only check went dark on this.
    metadata/bootstrap.json's prerequisites.install.linux/.macos still
    declare a command for the deleted tool, so bootstrap.sh:174 falls
    through to the bare-name `warn "  ${bin}"` branch (the #978 defect,
    restored) with nothing here to catch it before this completeness
    assertion existed."""
    _scaffold(tmp_path)
    sh_path = tmp_path / "scripts/bootstrap.sh"
    _replace(sh_path, "PREREQ_HINT_NAMES=(git cmake python3)", "PREREQ_HINT_NAMES=(cmake python3)")
    _replace(sh_path, '    "sudo apt-get install -y git"\n', "")
    _replace(sh_path, '    "brew install git"\n', "")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "prerequisites.install.linux.git has no PREREQ_HINT_NAMES entry" in err
    assert "prerequisites.install.macos.git has no PREREQ_HINT_NAMES entry" in err


# ---------------------------------------------------------------------
# 13. literal scan file-set coverage (issue #949 review: the scaffold above
#    has no docs/ tree and no scripts/*.py, so the doc-dir exclusion, the
#    .py path, and the gate's by-name self-exclusion were all untested)
# ---------------------------------------------------------------------


def test_install_literal_scan_covers_non_readme_docs_file(tmp_path, monkeypatch, capsys):
    """A drifted winget id under docs/ (not just README.md, the only doc
    the scaffold exercised before) must still be caught."""
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "some-guide.md"
    doc.parent.mkdir(parents=True, exist_ok=True)
    doc.write_text("winget install Ninja-build.Ninja\n", encoding="utf-8")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "docs/some-guide.md" in err


def test_install_literal_scan_excludes_docs_adr_dir(tmp_path, monkeypatch, capsys):
    """The same drifted id under docs/adr/ must NOT be flagged --
    `_LITERAL_SCAN_EXCLUDE_DOC_DIRS`'s historical-record exclusion."""
    _scaffold(tmp_path)
    doc = tmp_path / "docs" / "adr" / "0099-some-decision.md"
    doc.parent.mkdir(parents=True, exist_ok=True)
    doc.write_text("winget install Ninja-build.Ninja\n", encoding="utf-8")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "OK" in out


def test_install_literal_scan_covers_python_scripts(tmp_path, monkeypatch, capsys):
    """A `.py` file under scripts/ (not just README.md/*.sh/*.ps1) must be
    scanned too, with only a plain `#`-prefixed line skipped as a comment
    there (unlike bootstrap.sh/.ps1's heredoc/here-string-aware tracking)."""
    _scaffold(tmp_path)
    py = tmp_path / "scripts" / "some_helper.py"
    py.write_text(
        "# rationale comment: winget install Ninja-build.Ninja is the old form\n"
        "print('winget install Ninja-build.Ninja')\n",
        encoding="utf-8",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "scripts/some_helper.py:2:" in err
    assert "scripts/some_helper.py:1:" not in err


def test_install_literal_scan_self_excludes_gate_script_by_name(tmp_path, monkeypatch, capsys):
    """This gate's own source necessarily quotes winget package IDs in its
    docstrings/comments without the full canonical command on the same
    line (e.g. the `Git.Git`, `Kitware.CMake`, ... enumeration) -- it is
    excluded from the scan BY NAME (`scripts/check_bootstrap_manifest.py`),
    not by content, or it would flag itself."""
    _scaffold(tmp_path)
    shutil.copy2(SCRIPT, tmp_path / "scripts" / "check_bootstrap_manifest.py")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "OK" in out


def test_install_literal_scan_flags_gate_script_content_under_another_name(
    tmp_path, monkeypatch, capsys
):
    """Proves the self-exclusion above does real work, not merely
    absent-by-coincidence: the SAME source content, copied under a name
    other than `check_bootstrap_manifest.py`, must fail."""
    _scaffold(tmp_path)
    shutil.copy2(SCRIPT, tmp_path / "scripts" / "check_bootstrap_manifest_copy.py")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "scripts/check_bootstrap_manifest_copy.py" in err


# ---------------------------------------------------------------------
# 14. Hardcoded duplicate beside a correct read (issue #965)
# ---------------------------------------------------------------------


def test_hardcoded_duplicate_of_read_leaf_fails(tmp_path, monkeypatch, capsys):
    """The exact reproduction from issue #965: re-insert the hardcoded
    here-string block PR #961 (a27757b8) deleted immediately above the
    `foreach ($line in $ManualInstallNote)` render loop in
    scripts/bootstrap.ps1. `$Manifest.manualInstallHints.windows.note` is
    still present at the foreach line below the reinsertion, so the OLD
    `_check_no_orphaned_leaves` (needle-presence only) stayed green through
    this for as long as it shipped -- a plain "is this read by something"
    scan cannot tell a single correct read from a correct read sitting next
    to a hardcoded duplicate of the same fact. This is the regression test
    for the fix: the duplicated Arm-toolchain installer URL must now be
    caught."""
    _scaffold(tmp_path)
    ps1_path = tmp_path / "scripts/bootstrap.ps1"
    needle = "foreach ($line in $ManualInstallNote) {"
    duplicate_block = (
        '@"\n\n'
        "  # Arm GNU Toolchain (cross-compiles for real silicon) -- installer EXE:\n"
        "  #   https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads\n\n"
        '"@ | Write-Host\n'
    )
    _replace(ps1_path, needle, duplicate_block + needle)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "scripts/bootstrap.ps1" in err
    assert "https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads" in err
    assert "manualInstallHints.windows.note" in err


def test_hardcoded_duplicate_of_punctuation_wrapped_leaf_fragment_fails(
    tmp_path, monkeypatch, capsys
):
    """`_DUPLICATE_LITERAL_STRIP_CHARS` is what makes the duplicate scan
    above catch a fact that only appears in the manifest wrapped in sentence
    punctuation. `manualInstallHints.windows.note` carries "...install them
    (see docs/cross-platform-setup.md); WARN-only..." -- the raw
    whitespace-delimited token is `(see` / `docs/cross-platform-setup.md);`,
    not the bare doc path. Without the strip, the literal search key stays
    `docs/cross-platform-setup.md);` (with its wrapping punctuation) and a
    hardcoded duplicate that spells the bare path -- exactly how a script
    would legitimately reference the doc, and exactly how a real duplicate
    would read -- goes uncaught. Injecting the bare path as a hardcoded
    duplicate here must fail; `_DUPLICATE_LITERAL_STRIP_CHARS = ""` must
    turn this RED (see the constant's own mutation-testing note)."""
    _scaffold(tmp_path)
    ps1_path = tmp_path / "scripts/bootstrap.ps1"
    needle = "foreach ($line in $ManualInstallNote) {"
    duplicate_block = (
        '@"\n\n'
        "  # See docs/cross-platform-setup.md for manual Windows toolchain steps.\n\n"
        '"@ | Write-Host\n'
    )
    _replace(ps1_path, needle, duplicate_block + needle)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "scripts/bootstrap.ps1" in err
    assert "docs/cross-platform-setup.md" in err
    assert "manualInstallHints.windows.note" in err


def test_short_leaf_fragment_duplicate_is_not_flagged(tmp_path, monkeypatch, capsys):
    """Fixture-assumption + design guard: `west.extensionGuardCommand`
    ("alp-migrate", 11 chars) is BOTH read from the manifest (via
    `$WestExtGuard`/`$WEST_EXT_GUARD`) AND separately spelled out literally
    in bootstrap.ps1's own user-facing Fail/Write-Ok messages -- a real,
    pre-existing, harmless repeat that is exactly the shape a hardcoded
    exemption list would otherwise have to carry. Confirms the length floor
    (`_DUPLICATE_LITERAL_MIN_LEN`), not a growing allowlist, is what keeps
    this gate green on it."""
    _scaffold(tmp_path)
    ps1_text = (tmp_path / "scripts/bootstrap.ps1").read_text(encoding="utf-8")
    assert "'west alp-migrate'" in ps1_text
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
