# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_bootstrap_manifest.py.

The gate is 100% regex-driven against metadata/bootstrap.json + its schema +
scripts/bootstrap.sh + scripts/bootstrap.ps1 + west.yml + README.md + three CI
workflows + tools/native-sim-container/Containerfile. Each test here mutates
a TEMP COPY of that corpus and asserts the gate actually fires for the
documented failure mode -- a green run on the real repo alone proves nothing
about whether the gate catches drift.

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
    ".github/workflows/pr-getting-started-aen801.yml",
    "tools/native-sim-container/Containerfile",
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
    monkeypatch.setattr(
        gate, "CONTAINERFILE", tmp_path / "tools/native-sim-container/Containerfile"
    )
    monkeypatch.setattr(gate, "CI_WORKFLOWS", [
        tmp_path / ".github/workflows/pr-twister.yml",
        tmp_path / ".github/workflows/pr-tier-a-libraries.yml",
        tmp_path / ".github/workflows/pr-getting-started-aen801.yml",
    ])
    # The zephyr.pythonMinVersion <-> pinned-Zephyr cross-check (issue #1078)
    # is exercised by its own dedicated tests below (with a throwaway fake
    # Zephyr git repo); default it to "unresolvable" here so every other
    # test isn't coupled to whatever real Zephyr checkout (if any -- and one
    # genuinely does sit next to this repo on dev machines) happens to be on
    # the machine running the suite, mirroring
    # test_check_toolchain_lock.py's identical precaution for its own
    # SDK/Zephyr cross-check.
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: tmp_path / "no-such-zephyr-checkout")
    monkeypatch.delenv("ALP_REQUIRE_ZEPHYR_ORACLE", raising=False)


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
# 5b. native-sim-container Containerfile ARG ZEPHYR_REV disagreement
#     (issue #1458)
# ---------------------------------------------------------------------


def test_containerfile_arg_zephyr_rev_disagreement_fails(tmp_path, monkeypatch, capsys):
    """Reproduces issue #1458: the Containerfile's `ARG ZEPHYR_REV` default
    stuck one patch release behind west.yml/zephyr.version, silently, with
    nothing to catch it -- this is the gate that now does."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "tools/native-sim-container/Containerfile",
        "ARG ZEPHYR_REV=v4.4.1",
        "ARG ZEPHYR_REV=v4.4.0",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "Containerfile pins ARG ZEPHYR_REV" in err
    assert "'v4.4.0'" in err


def test_containerfile_missing_arg_fails(tmp_path, monkeypatch, capsys):
    _scaffold(tmp_path)
    _replace(
        tmp_path / "tools/native-sim-container/Containerfile",
        "ARG ZEPHYR_REV=v4.4.1",
        "ARG ZEPHYR_REV_RENAMED=v4.4.1",
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "no `ARG ZEPHYR_REV=...` default found" in err


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


def test_prerequisites_macos_drift_fails(tmp_path, monkeypatch, capsys):
    """POSIX twin of test_prerequisites_posix_drift_fails: prerequisites.macos
    must agree with scripts/bootstrap.sh's Darwin-branch REQUIRED_BINS
    reassignment (the macOS xz/wget exemption) -- `_check_prerequisites_macos`
    is a SEPARATE regex from `_check_prerequisites_posix`'s (which only ever
    sees the first/canonical REQUIRED_BINS literal), so this needs its own
    drift test rather than assuming the posix test above covers it."""
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["prerequisites"].__setitem__(
        "macos", ["git", "python3"]))  # dropped "cmake" and "ninja"
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "Darwin-branch REQUIRED_BINS" in err
    assert "prerequisites.macos" in err


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


def test_bootstrap_sh_darwin_excludes_xz_and_wget_from_refusal(tmp_path):
    """scripts/bootstrap.sh's Darwin branch reassigns REQUIRED_BINS to drop
    xz/wget (the #949 macOS fix this branch adds) -- nothing else exercises
    that branch: the only CI container is ubuntu:24.04, and no other test
    stubs `uname`. Fake a Darwin `uname` on PATH, restrict the rest of PATH
    to just `dirname`/`git`/`python3` (real, via symlink) so `cmake`/`ninja`
    are genuinely absent too -- the only way to force an actual refusal --
    and assert the refusal names cmake/ninja but never xz/wget."""
    if sys.platform == "win32":
        # The whole premise -- fake a Darwin host by putting a `uname` shim
        # first on a stripped PATH -- does not hold on native Windows. Git
        # Bash resolves its own `uname` (`MINGW64_NT-...`) ahead of an
        # extensionless shim, and `Path.symlink_to` needs a privilege the
        # runner does not have, so the Darwin branch never fires and the
        # refusal correctly lists xz/wget. Nothing about scripts/bootstrap.sh
        # is under test here on Windows: it is POSIX-only (tan bootstrap
        # refuses native Windows outright with `windows-unsupported`), and
        # Windows users run scripts/bootstrap.ps1. The macOS branch this
        # covers is exercised on the ubuntu and macos legs, which is where
        # the assertion means something.
        pytest.skip("bootstrap.sh is POSIX-only; the Darwin uname shim cannot work on native Windows")
    bash_path = shutil.which("bash")
    if bash_path is None:
        pytest.skip("bash not available on PATH")
    real_tools = {}
    for tool in ("dirname", "git", "python3"):
        found = shutil.which(tool)
        if found is None:
            pytest.skip(f"{tool} not available on PATH -- cannot build the restricted PATH shim")
        real_tools[tool] = found

    scaffold_root = tmp_path / "darwin-repo"
    (scaffold_root / "scripts").mkdir(parents=True)
    shutil.copy2(REPO / "scripts" / "bootstrap.sh", scaffold_root / "scripts" / "bootstrap.sh")

    shim_dir = tmp_path / "shim"
    shim_dir.mkdir()
    uname_shim = shim_dir / "uname"
    uname_shim.write_text("#!/bin/sh\necho Darwin\n", encoding="utf-8")
    uname_shim.chmod(0o755)
    for tool, real_path in real_tools.items():
        (shim_dir / tool).symlink_to(real_path)

    proc = subprocess.run(
        [bash_path, str(scaffold_root / "scripts" / "bootstrap.sh")],
        capture_output=True, text=True, cwd=str(scaffold_root),
        env={"PATH": str(shim_dir)},
    )
    out = proc.stdout + proc.stderr
    assert proc.returncode != 0, out
    assert "cmake" in out
    assert "ninja" in out
    assert "xz" not in out
    assert "wget" not in out


def test_bootstrap_sh_refuses_unknown_schema_version(tmp_path):
    """scripts/bootstrap.sh:235's `[ "${SCHEMA_VERSION}" = "1" ] || die ...`
    guard, exercised end-to-end (not through the gate module -- this is the
    SCRIPT's own defence against a manifest shaped for a schema version it
    doesn't understand, e.g. if check_bootstrap_manifest.py never ran)."""
    if not _bash_available_with_python3():
        pytest.skip("bash + python3 not both available on PATH")
    # Resolve bash to a concrete path rather than passing the bare name
    # "bash" -- on a Windows box with the WSL feature enabled, spawning an
    # unqualified "bash" from a native (non-Bash) Python process can lose
    # the PATH-order race to C:\Windows\System32\bash.exe (the WSL
    # launcher stub) even when a real POSIX bash.exe sits earlier in
    # %PATH%, silently testing a different interpreter -- with a
    # different filesystem namespace ("C:/..." isn't the WSL root) -- than
    # the one this test means to exercise. Same precedent as
    # test_bootstrap_sh_darwin_excludes_xz_and_wget_from_refusal above.
    bash_path = shutil.which("bash")
    if bash_path is None:
        pytest.skip("bash not available on PATH")
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
    # .as_posix() (not str()) -- on Windows, str() yields a backslashed
    # path; Git Bash strips the backslashes, so bash tries to open a
    # mangled filename and reports "No such file or directory" instead of
    # ever reaching the schemaVersion guard this test asserts on
    # (alp-sdk#1110).
    proc = subprocess.run(
        [bash_path, (scaffold_root / "scripts" / "bootstrap.sh").as_posix(), "--print-env"],
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

    getting_started = (tmp_path / ".github/workflows/pr-getting-started-aen801.yml").read_text(
        encoding="utf-8")
    assert "key: getting-started-aen801-zephyr-v4.10.0-${{ runner.os }}" in getting_started

    containerfile = (tmp_path / "tools/native-sim-container/Containerfile").read_text(
        encoding="utf-8")
    assert "ARG ZEPHYR_REV=v4.10.0" in containerfile


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
    assert "missing install command" in err
    assert "ninja" in err


def test_install_linux_missing_tool_command_fails(tmp_path, monkeypatch, capsys):
    """The gate's own docstring (point 1 in `_check_install_commands`) calls
    this "the ONLY assertion covering install.linux / install.macos" -- yet
    nothing in this file ever mutated `install.linux`/`install.macos` before
    this test. Branch coverage reported the `if os_install != posix_tools:`
    line covered by two unrelated fixtures tripping it incidentally; only a
    real `if False:` mutation of that line exposed the gap (it still stayed
    42 passed, 1 skipped). Popping a tool from install.linux.apt (issue
    #1464 -- linux is keyed by package manager; apt is the required,
    complete map) must fail."""
    _scaffold(tmp_path)
    _edit_manifest(
        tmp_path, lambda d: d["prerequisites"]["install"]["linux"]["apt"].pop("cmake")
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "prerequisites.install.linux.apt" in err
    assert "prerequisites.posix" in err


def test_install_linux_dnf_partial_map_passes(tmp_path, monkeypatch, capsys):
    """issue #1464: install.linux.dnf is OPTIONAL and need not cover every
    prerequisites.posix tool -- popping `xz` from BOTH the manifest and
    bootstrap.sh's PREREQ_HINT_DNF (in lockstep -- an in-sync partial
    removal, mirroring how `ninja` is already shipped) must still pass; a
    dnf sub-map missing a tool is the shipped, correct shape, not drift.
    (Popping the manifest side ALONE is covered by
    test_bootstrap_sh_hint_dnf_value_drift_fails's sibling shape -- that
    correctly fails, since bootstrap.sh would then carry a phantom hint with
    no manifest backing.)"""
    _scaffold(tmp_path)
    manifest_path = tmp_path / "metadata/bootstrap.json"
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert "ninja" not in data["prerequisites"]["install"]["linux"]["dnf"], (
        "fixture assumption broken: the real manifest's install.linux.dnf "
        "already carries a ninja entry"
    )
    _edit_manifest(
        tmp_path, lambda d: d["prerequisites"]["install"]["linux"]["dnf"].pop("xz")
    )
    _replace(
        tmp_path / "scripts/bootstrap.sh",
        '    "sudo dnf install -y xz"\n',
        '    ""\n',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "OK" in out


def test_install_linux_dnf_unknown_tool_fails(tmp_path, monkeypatch, capsys):
    """install.linux.dnf's keys must still be a SUBSET of prerequisites.posix
    -- a typo'd/unknown tool name must fail even though the map is allowed
    to be partial (issue #1464).

    Mutation-tested (review finding on #1471): the ORIGINAL version of this
    test only asserted `"prerequisites.install.linux.dnf" in err` and
    `"nnija" in err` -- both weak substrings are ALSO produced by
    `_check_bootstrap_sh_install_hints`'s own, unrelated "no
    PREREQ_HINT_NAMES entry" completeness problem (since 'nnija' isn't in
    bootstrap.sh's PREREQ_HINT_NAMES either -- a real, independent gap this
    same mutation happens to also trip), so neutering the actual
    `unknown_dnf` check this test names (`if False:` in place of the real
    condition) still left the test reporting "1 passed". Asserting the
    FULL, exact problem line instead -- unique to `unknown_dnf`'s
    "entr(y/ies) ... with no matching prerequisites.posix tool" phrasing, no
    other check in this file produces that text -- means a disabled
    `unknown_dnf` check makes this exact string absent from `err` and the
    test correctly goes red, regardless of what else the same mutation also
    happens to trip. (Silencing the confound at the source by making
    'nnija' fully recognised everywhere else was tried and rejected: it
    requires giving `install.linux.apt` / `install.macos` a matching
    'nnija' entry too, which then fails THEIR OWN exact-equality
    completeness check against `prerequisites.posix` -- a structural
    conflict, not a workaround-able gap.)"""
    _scaffold(tmp_path)
    _edit_manifest(
        tmp_path,
        lambda d: d["prerequisites"]["install"]["linux"]["dnf"].__setitem__(
            "nnija", "sudo dnf install -y ninja-build"
        ),
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert (
        "prerequisites.install.linux.dnf has entr(y/ies) ['nnija'] with no "
        "matching prerequisites.posix tool"
    ) in err, err


def test_install_linux_unknown_package_manager_fails_schema(tmp_path, monkeypatch, capsys):
    """A hand-added package-manager key (e.g. `pacman`) must be rejected by
    schema validation -- issue #1464's manifest description explicitly rules
    out shipping one without a container job proving it, and the schema's
    `additionalProperties: false` on install.linux is what actually enforces
    that (not this gate's own Python, which trusts schema validation ran
    first)."""
    _scaffold(tmp_path)
    _edit_manifest(
        tmp_path,
        lambda d: d["prerequisites"]["install"]["linux"].__setitem__(
            "pacman", {"git": "sudo pacman -S --noconfirm git"}
        ),
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "schema:" in err
    assert "pacman" in err


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


def test_install_windows_allowlisted_entry_with_no_gate_or_prereqs_entry_passes(
    tmp_path, monkeypatch, capsys
):
    """`install.windows` may carry a tool with no matching
    `prerequisites.windows` gate and no `$Prereqs` entry at all, PROVIDED it
    is named in this script's own `_WINDOWS_INSTALL_ONLY_TOOLS` allowlist
    (issue #1036's `7zip` -- gates `west sdk install`, not bootstrap.ps1) --
    the completeness contract is one-directional (gate tools subset-of
    install commands) AND bounded on the reverse direction by that
    allowlist, not an open-ended superset. Overwrites the real `7zip` value
    here (rather than merely relying on it already being present) so this
    test proves the ALLOWLIST behaviour itself, not just that the real
    manifest happens to carry a passing entry today. See
    `test_install_windows_extra_entry_not_allowlisted_fails` for the
    negative twin: an entry NOT on this allowlist (`unzip`, `nnija`, ...)
    must fail, not pass, unlike before this bound existed."""
    _scaffold(tmp_path)
    _edit_manifest(
        tmp_path,
        lambda d: d["prerequisites"]["install"]["windows"].__setitem__(
            "7zip", "winget install -e --id 7zip.7zip"
        ),
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out


def test_install_windows_extra_entry_not_allowlisted_fails(tmp_path, monkeypatch, capsys):
    """The reverse-direction bound on the #1036 superset relaxation: an
    `install.windows` key with no matching `prerequisites.windows` gate AND
    no matching `_WINDOWS_INSTALL_ONLY_TOOLS` allowlist entry must be
    rejected. Before this bound existed, a garbage key with a garbage ID
    (like this one) sat completely undetected -- nothing else in the gate
    ever looks at an `install.windows` key that isn't on one of those two
    lists (the schema's `additionalProperties: {type: string, minLength: 1}`
    accepts any key name, and the `$Prereqs` / literal-scan checks only ever
    walk FROM `prerequisites.windows` / `install.windows`'s current values).
    Sensitivity: on the code as it stood before `extra_windows` was added
    (completeness checked only `windows_tools - windows_install`, never the
    reverse), this exact mutation passed with rv == 0 -- this test is the
    regression lock for that gap."""
    _scaffold(tmp_path)
    _edit_manifest(
        tmp_path,
        lambda d: d["prerequisites"]["install"]["windows"].__setitem__(
            "nnija", "winget install -e --id Nnija.Typo"
        ),
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "nnija" in err
    assert "_WINDOWS_INSTALL_ONLY_TOOLS" in err


def test_install_windows_stale_entry_after_gate_removal_fails(tmp_path, monkeypatch, capsys):
    """The realistic shape of the same gap: a tool removed from
    `prerequisites.windows` AND its `scripts/bootstrap.ps1` `$Prereqs`
    entry in lockstep (the "this tool stopped gating bootstrap" refactor)
    but left behind in `install.windows` must still fail -- a stale install
    command silently strands itself with nothing pointing a future reader
    at whether it is still wired to anything. `ninja` is not in
    `_WINDOWS_INSTALL_ONLY_TOOLS`, so removing its gate must not let its
    `install.windows` entry go quiet. Before `extra_windows` was added this
    mutation passed with rv == 0, the same regression `_WINDOWS_INSTALL_ONLY_TOOLS`
    now closes."""
    _scaffold(tmp_path)
    _edit_manifest(
        tmp_path,
        lambda d: d["prerequisites"].__setitem__(
            "windows", ["git", "cmake", "python"]  # dropped "ninja"
        ),
    )
    _replace(
        tmp_path / "scripts/bootstrap.ps1",
        '    @{ Name = "python"; Hint = "winget install -e --id Python.Python.3.12" },\n'
        '    @{ Name = "ninja";  Hint = "winget install -e --id Ninja-build.Ninja" }\n)',
        '    @{ Name = "python"; Hint = "winget install -e --id Python.Python.3.12" }\n)',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "ninja" in err
    assert "_WINDOWS_INSTALL_ONLY_TOOLS" in err


# ---------------------------------------------------------------------
# 12b. scripts/bootstrap.sh PREREQ_HINT_* agreement (issue #978 gate review)
# ---------------------------------------------------------------------


def test_bootstrap_sh_hint_value_drift_fails(tmp_path, monkeypatch, capsys):
    """A PREREQ_HINT_APT entry's command must agree with
    prerequisites.install.linux.apt[<tool>] byte-for-byte (issue #1464
    renamed PREREQ_HINT_LINUX -> PREREQ_HINT_APT) -- the POSIX-side analogue
    of test_install_ps1_hint_disagreement_fails."""
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
    assert "PREREQ_HINT_APT entry 'git'" in err
    assert "disagrees with prerequisites.install.linux.apt.git" in err


def test_bootstrap_sh_hint_dnf_value_drift_fails(tmp_path, monkeypatch, capsys):
    """Same shape, dnf side (issue #1464) -- a PREREQ_HINT_DNF entry's
    command must agree with prerequisites.install.linux.dnf[<tool>]
    byte-for-byte."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "scripts/bootstrap.sh",
        '"sudo dnf install -y git"',
        '"sudo dnf install git"',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "PREREQ_HINT_DNF entry 'git'" in err
    assert "disagrees with prerequisites.install.linux.dnf.git" in err


def test_bootstrap_sh_hint_dnf_empty_slot_with_real_manifest_entry_fails(
    tmp_path, monkeypatch, capsys
):
    """The "absent means empty" allowance (issue #1464) only covers a tool
    with NO manifest entry at all -- blanking a DNF hint slot for a tool the
    manifest DOES carry a real command for (here: `git`) must still fail,
    not silently pass as though it were the sanctioned ninja-shaped gap."""
    _scaffold(tmp_path)
    _replace(
        tmp_path / "scripts/bootstrap.sh",
        '"sudo dnf install -y git"',
        '""',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "PREREQ_HINT_DNF entry 'git'" in err
    assert "disagrees with prerequisites.install.linux.dnf.git" in err


def test_bootstrap_sh_hint_dnf_absent_ninja_slot_passes(tmp_path, monkeypatch, capsys):
    """The real, unmodified PREREQ_HINT_DNF ninja slot (empty string,
    matching install.linux.dnf's genuine absence of a `ninja` key, issue
    #1464) must NOT be reported as a problem -- this is the sanctioned gap,
    not drift."""
    _scaffold(tmp_path)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "ninja" not in out


def test_bootstrap_sh_hint_empty_slot_unbacked_by_apt_or_macos_still_fails(
    tmp_path, monkeypatch, capsys
):
    """The "unbacked empty slot is fine" allowance is DNF-ONLY (review
    finding on #1471 -- both install.linux.apt and install.macos are
    REQUIRED-complete maps, so a canonical-less entry on either is never
    legitimate the way it is for the optional, partial dnf map). Adds one
    brand-new name to all four bootstrap.sh arrays, blanked to `""`
    everywhere -- DNF's own genuine partial-map allowance stays quiet (its
    behaviour is unchanged by this fix), while apt and macos, now exactly as
    strict as before dnf ever gained a lenient sibling, must both still
    fail."""
    _scaffold(tmp_path)
    sh_path = tmp_path / "scripts/bootstrap.sh"
    _replace(
        sh_path,
        "PREREQ_HINT_NAMES=(git cmake python3 ninja xz wget)",
        "PREREQ_HINT_NAMES=(git cmake python3 ninja xz wget bogustool)",
    )
    _replace(
        sh_path,
        '    "sudo apt-get install -y wget"\n)',
        '    "sudo apt-get install -y wget"\n    ""\n)',
    )
    _replace(
        sh_path,
        '    "sudo dnf install -y wget"\n)',
        '    "sudo dnf install -y wget"\n    ""\n)',
    )
    _replace(
        sh_path,
        '    "brew install wget"\n)',
        '    "brew install wget"\n    ""\n)',
    )
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert (
        "scripts/bootstrap.sh PREREQ_HINT_APT has an entry for 'bogustool', "
        "but metadata/bootstrap.json has no prerequisites.install.linux.apt.bogustool"
    ) in err
    assert (
        "scripts/bootstrap.sh PREREQ_HINT_MACOS has an entry for 'bogustool', "
        "but metadata/bootstrap.json has no prerequisites.install.macos.bogustool"
    ) in err
    assert "PREREQ_HINT_DNF" not in err


def test_bootstrap_sh_hint_length_mismatch_fails(tmp_path, monkeypatch, capsys):
    """PREREQ_HINT_NAMES and PREREQ_HINT_APT/_DNF/_MACOS are matched up by
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
    assert "PREREQ_HINT_APT has 5 entries but PREREQ_HINT_NAMES has 6" in err
    assert "must stay parallel arrays" in err


def test_bootstrap_sh_hint_deleted_in_lockstep_does_not_silently_drop_tool(
    tmp_path, monkeypatch, capsys
):
    """Reproduces the review finding verbatim: deleting a tool's entry from
    PREREQ_HINT_NAMES + PREREQ_HINT_APT + PREREQ_HINT_DNF + PREREQ_HINT_MACOS
    in lockstep keeps every array parallel (no length mismatch) and every
    remaining zip pair still agrees -- the old zip-only check went dark on
    this. metadata/bootstrap.json's prerequisites.install.linux.apt /
    .linux.dnf / .macos still declare a command for the deleted tool, so
    bootstrap.sh:174-ish falls through to the bare-name `warn "  ${bin}"`
    branch (the #978 defect, restored) with nothing here to catch it before
    this completeness assertion existed."""
    _scaffold(tmp_path)
    sh_path = tmp_path / "scripts/bootstrap.sh"
    _replace(sh_path, "PREREQ_HINT_NAMES=(git cmake python3 ninja xz wget)", "PREREQ_HINT_NAMES=(cmake python3 ninja xz wget)")
    _replace(sh_path, '    "sudo apt-get install -y git"\n', "")
    _replace(sh_path, '    "sudo dnf install -y git"\n', "")
    _replace(sh_path, '    "brew install git"\n', "")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "prerequisites.install.linux.apt.git has no PREREQ_HINT_NAMES entry" in err
    assert "prerequisites.install.linux.dnf.git has no PREREQ_HINT_NAMES entry" in err
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


def test_hardcoded_duplicate_of_posix_manual_install_note_fails(tmp_path, monkeypatch, capsys):
    """POSIX twin of `test_hardcoded_duplicate_of_read_leaf_fails` above.
    `manualInstallHints.posix.note` (issue #949 addendum A4 -- POSIX finally
    got the same manual-install-hints render `manualInstallHints.windows.note`
    already had) is read generically by `_check_no_orphaned_leaves`'s
    per-leaf scan the same as every other leaf, but until now nothing proved
    that scan actually fires for scripts/bootstrap.sh's own render site --
    only the bootstrap.ps1/windows side had a regression test. Re-inject a
    hardcoded copy of `docs/cross-platform-setup.md` (a distinctive >= 20
    char fragment of `manualInstallHints.posix.note`) immediately above
    bootstrap.sh's `MANUAL_INSTALL_POSIX_NOTE` render loop -- the only
    existing appearance of that path in bootstrap.sh is inside the header
    comment at the top of the file (exempt), so this is a genuine second,
    hardcoded copy outside a comment and must be caught."""
    _scaffold(tmp_path)
    sh_path = tmp_path / "scripts/bootstrap.sh"
    needle = 'for line in "${MANUAL_INSTALL_POSIX_NOTE[@]}"; do echo "  ${line}"; done'
    duplicate_line = (
        'echo "See docs/cross-platform-setup.md for the manual install steps."\n        '
    )
    _replace(sh_path, needle, duplicate_line + needle)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "scripts/bootstrap.sh" in err
    assert "docs/cross-platform-setup.md" in err
    assert "manualInstallHints.posix.note" in err


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


# ---------------------------------------------------------------------
# 15. zephyr.pythonMinVersion <-> pinned Zephyr's own PYTHON_MINIMUM_REQUIRED
#     (issue #1078)
# ---------------------------------------------------------------------


def _make_fake_zephyr_repo(tmp_path: Path, python_min_at_tag: str, tag: str) -> Path:
    """A throwaway git repo standing in for a Zephyr checkout, with
    `cmake/modules/python.cmake` carrying
    `set(PYTHON_MINIMUM_REQUIRED <python_min_at_tag>)` committed and tagged
    `tag` -- lets tests exercise
    `git show <tag>:cmake/modules/python.cmake` without a real,
    multi-hundred-MB Zephyr clone. Mirrors
    test_check_toolchain_lock.py's `_make_fake_zephyr_repo` (same
    technique, different file)."""
    zephyr_dir = tmp_path / "fake-zephyr"
    zephyr_dir.mkdir()
    run = lambda *args: subprocess.run(  # noqa: E731
        ["git", *args], cwd=zephyr_dir, check=True, capture_output=True, text=True,
    )
    run("init", "-q")
    run("config", "user.email", "test@example.invalid")
    run("config", "user.name", "test")
    cmake_dir = zephyr_dir / "cmake" / "modules"
    cmake_dir.mkdir(parents=True)
    (cmake_dir / "python.cmake").write_text(
        f"include_guard(GLOBAL)\n\nset(PYTHON_MINIMUM_REQUIRED {python_min_at_tag})\n",
        encoding="utf-8",
    )
    run("add", "cmake/modules/python.cmake")
    run("commit", "-q", "-m", "seed")
    run("tag", tag)
    return zephyr_dir


def test_zephyr_python_min_version_matches_real_pin_passes(tmp_path, monkeypatch):
    manifest = {"zephyr": {"version": "v4.4.1", "pythonMinVersion": "3.12"}}
    fake_zephyr = _make_fake_zephyr_repo(tmp_path, "3.12", "v4.4.1")
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: fake_zephyr)
    monkeypatch.delenv("ALP_REQUIRE_ZEPHYR_ORACLE", raising=False)
    problems, skip_reason = gate._check_zephyr_python_min_version(manifest)
    assert problems == []
    assert skip_reason is None


def test_zephyr_python_min_version_disagreement_with_real_pin_fails(tmp_path, monkeypatch):
    """The exact regression issue #1078 exists for: a manifest
    zephyr.pythonMinVersion that disagrees with the real
    PYTHON_MINIMUM_REQUIRED at the pinned Zephyr revision."""
    manifest = {"zephyr": {"version": "v4.4.1", "pythonMinVersion": "3.10"}}
    fake_zephyr = _make_fake_zephyr_repo(tmp_path, "3.12", "v4.4.1")
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: fake_zephyr)
    monkeypatch.delenv("ALP_REQUIRE_ZEPHYR_ORACLE", raising=False)
    problems, skip_reason = gate._check_zephyr_python_min_version(manifest)
    assert skip_reason is None
    assert len(problems) == 1
    assert "3.10" in problems[0]
    assert "3.12" in problems[0]
    assert "PYTHON_MINIMUM_REQUIRED" in problems[0]


def test_zephyr_python_min_version_check_skips_with_no_zephyr_checkout(tmp_path, monkeypatch):
    manifest = {"zephyr": {"version": "v4.4.1", "pythonMinVersion": "3.12"}}
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: tmp_path / "does-not-exist")
    monkeypatch.delenv("ALP_REQUIRE_ZEPHYR_ORACLE", raising=False)
    problems, skip_reason = gate._check_zephyr_python_min_version(manifest)
    assert problems == []
    assert skip_reason is not None
    assert "no Zephyr checkout resolved" in skip_reason


def test_zephyr_python_min_version_check_hard_fails_when_oracle_required_but_absent(
    tmp_path, monkeypatch,
):
    manifest = {"zephyr": {"version": "v4.4.1", "pythonMinVersion": "3.12"}}
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: tmp_path / "does-not-exist")
    monkeypatch.setenv("ALP_REQUIRE_ZEPHYR_ORACLE", "1")
    problems, skip_reason = gate._check_zephyr_python_min_version(manifest)
    assert skip_reason is None
    assert len(problems) == 1
    assert "ALP_REQUIRE_ZEPHYR_ORACLE=1" in problems[0]


def test_zephyr_python_min_version_check_uses_git_show_not_working_tree_checkout(
    tmp_path, monkeypatch,
):
    """The oracle reads the pinned revision via
    `git show <rev>:cmake/modules/python.cmake` from the object store, NOT
    the working tree's currently-checked-out ref -- a checkout currently
    sitting on a different tag must still resolve correctly as long as the
    pinned tag exists as a git object."""
    manifest = {"zephyr": {"version": "v4.4.1", "pythonMinVersion": "3.12"}}
    fake_zephyr = _make_fake_zephyr_repo(tmp_path, "3.12", "v4.4.1")
    # Move HEAD to a second commit/tag so the working tree is NOT checked
    # out at v4.4.1 any more, mirroring a stale local dev clone.
    cmake_file = fake_zephyr / "cmake" / "modules" / "python.cmake"
    cmake_file.write_text(
        "include_guard(GLOBAL)\n\nset(PYTHON_MINIMUM_REQUIRED 3.13)\n", encoding="utf-8",
    )
    subprocess.run(
        ["git", "add", "cmake/modules/python.cmake"],
        cwd=fake_zephyr, check=True, capture_output=True,
    )
    subprocess.run(
        ["git", "commit", "-q", "-m", "later"], cwd=fake_zephyr, check=True, capture_output=True,
    )
    subprocess.run(["git", "tag", "v4.5.0"], cwd=fake_zephyr, check=True, capture_output=True)
    monkeypatch.setattr(gate, "_resolve_zephyr_dir", lambda: fake_zephyr)
    monkeypatch.delenv("ALP_REQUIRE_ZEPHYR_ORACLE", raising=False)
    problems, skip_reason = gate._check_zephyr_python_min_version(manifest)
    assert problems == []
    assert skip_reason is None


def test_zephyr_python_min_version_leaf_is_gate_asserted_not_orphaned(tmp_path, monkeypatch, capsys):
    """`zephyr.pythonMinVersion` is exempted from the generic per-leaf
    orphan scan (`_GATE_ASSERTED_LEAVES`) -- neither bootstrap script reads
    it, by design, so it must NOT trip `_check_no_orphaned_leaves` the way
    an ordinary unread leaf would (only the SKIP note from the dedicated
    cross-check, which `_point_gate_at` forces by pointing
    `_resolve_zephyr_dir` at a nonexistent checkout, should mention it)."""
    _scaffold(tmp_path)
    assert 'd["zephyr"]["pythonMinVersion"]' not in (tmp_path / "scripts/bootstrap.sh").read_text(
        encoding="utf-8"
    )
    assert "$Manifest.zephyr.pythonMinVersion" not in (
        tmp_path / "scripts/bootstrap.ps1"
    ).read_text(encoding="utf-8")
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "is not read by" not in out


# ---------------------------------------------------------------------
# 10. artifactProvenance (issue #1574, ADR 0021 §3 consent-screen facts):
#     key-set lockstep with prerequisites.install, schema shape, and
#     orphan-leaf exemption.
# ---------------------------------------------------------------------


def test_artifact_provenance_missing_entry_fails(tmp_path, monkeypatch, capsys):
    """Drop the `git` provenance entry while prerequisites.install still
    ships a `git` install command on every OS -- the consent screen would
    silently have nothing to show for it; the gate must name it."""
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["artifactProvenance"].pop("git"))
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "missing entries for" in err
    assert "'git'" in err


def test_artifact_provenance_stale_entry_fails(tmp_path, monkeypatch, capsys):
    """A provenance entry for a tool no prerequisites.install.* map names
    an install command for anymore is stale, not merely extra -- the gate
    must flag it so a removed prerequisite doesn't leave a dangling
    consent-screen fact behind."""
    _scaffold(tmp_path)

    def _mutate(d):
        d["artifactProvenance"]["bogus-retired-tool"] = {
            "tier": "A", "source": "https://example.invalid/", "sizeBytes": None,
            "licence": "MIT",
        }

    _edit_manifest(tmp_path, _mutate)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "stale entries for" in err
    assert "'bogus-retired-tool'" in err


def test_artifact_provenance_valid_addition_passes(tmp_path, monkeypatch, capsys):
    """The mirror-image mutation: add a new prerequisite consistently (an
    install command on every OS AND a matching artifactProvenance entry) --
    proves the check isn't just permanently red once touched, it tracks
    real lockstep, not merely 'never changed'."""
    _scaffold(tmp_path)

    def _mutate(d):
        for key in ("apt",):
            d["prerequisites"]["install"]["linux"][key]["newtool"] = "sudo apt-get install -y newtool"
        d["prerequisites"]["install"]["macos"]["newtool"] = "brew install newtool"
        d["prerequisites"]["install"]["windows"]["newtool"] = "winget install -e --id New.Tool"
        d["prerequisites"]["posix"].append("newtool")
        d["prerequisites"]["macos"].append("newtool")
        d["prerequisites"]["windows"].append("newtool")
        d["artifactProvenance"]["newtool"] = {
            "tier": "A", "source": "https://example.invalid/newtool", "sizeBytes": None,
            "licence": "MIT",
        }

    _edit_manifest(tmp_path, _mutate)
    # bootstrap.sh/.ps1 hardcode REQUIRED_BINS/$Prereqs and the PREREQ_HINT_*
    # tables independently of artifactProvenance -- adding a prerequisite
    # for real would also need those updated, which is out of scope for
    # this test (it exercises _check_artifact_provenance in isolation).
    _point_gate_at(tmp_path, monkeypatch)
    problems = gate._check_artifact_provenance(
        json.loads((tmp_path / "metadata/bootstrap.json").read_text(encoding="utf-8"))
    )
    assert problems == [], problems


def test_artifact_provenance_missing_required_field_fails_schema(tmp_path, monkeypatch, capsys):
    """Every entry needs tier/source/sizeBytes/licence at minimum (issue
    #1574's own wording) -- dropping one must fail schema validation, not
    silently validate a partial fact."""
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["artifactProvenance"]["git"].pop("tier"))
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "artifactProvenance" in err
    assert "tier" in err


def test_artifact_provenance_bad_tier_value_fails_schema(tmp_path, monkeypatch, capsys):
    """`tier` is constrained to ADR 0021 §3's three tiers -- a made-up
    value must fail, not silently pass through as a string."""
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["artifactProvenance"]["git"].__setitem__("tier", "D"))
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    err = capsys.readouterr().err
    assert rv == 1
    assert "artifactProvenance" in err


def test_artifact_provenance_null_licence_and_size_are_valid(tmp_path, monkeypatch, capsys):
    """null is an explicit, schema-legal representation for sizeBytes/
    licence (issue #1574: an honest null beats a fabricated value) -- the
    real corpus already carries null for xz/7zip licence and every
    sizeBytes, and the baseline-passes tests already cover that; this
    locks in that a FRESH null (not just the pre-existing ones) also
    validates, so the schema's nullable union isn't accidentally narrowed
    later to reject null again."""
    _scaffold(tmp_path)
    _edit_manifest(tmp_path, lambda d: d["artifactProvenance"]["wget"].__setitem__("licence", None))
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out


def test_artifact_provenance_leaf_is_gate_asserted_not_orphaned(tmp_path, monkeypatch, capsys):
    """`artifactProvenance.*` has no reader in bootstrap.sh/bootstrap.ps1 by
    design (producer-only data for a future IDE/tan consumer) -- it must
    NOT trip the generic per-leaf orphan scan the way an ordinary unread
    leaf would."""
    _scaffold(tmp_path)
    _point_gate_at(tmp_path, monkeypatch)
    rv = gate.main()
    out = capsys.readouterr().out
    assert rv == 0, out
    assert "is not read by" not in out


def test_artifact_provenance_unknown_key_without_check_would_have_failed_known_keys(tmp_path, monkeypatch, capsys):
    """Fixture-assumption guard: `artifactProvenance` is in KNOWN_KEYS (if
    this ever drifts back out, `_check_known_keys` -- proven by the
    pre-existing `test_unknown_top_level_key_fails_with_known_keys_guidance`
    -- is what would catch it)."""
    assert "artifactProvenance" in gate.KNOWN_KEYS
