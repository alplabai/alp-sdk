# SPDX-License-Identifier: Apache-2.0
"""`silicon:` ref splitting has exactly one implementation (#997/#1004/#1096).

Three rounds of consolidation each left hand-rolled copies behind, because
each round chased `resolve_soc_path()` -- a helper that returns a path
rooted at a metadata root -- while the sites that survived rooted their
result somewhere else entirely (a caller-injected `soc_dir`, an
`output_root`, or no filesystem path at all). What they actually shared was
the SPLIT, so `split_silicon_ref()` is the single source and
`resolve_soc_path()` is one rooting convenience over it.

The tests below pin two things: that the split has one implementation, and
that migrating the call sites did not quietly change the shape each one
fails with. The second matters more than it looks -- callers distinguish a
malformed ref from a well-formed ref naming a missing file, and the naive
migration collapses those two into one.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "scripts"
OWNER = SCRIPTS / "alp_project_loader.py"

from alp_project_loader import resolve_soc_path, split_silicon_ref  # noqa: E402


def test_split_silicon_ref_arity_and_falsy_handling():
    """The guard is exact-3, not 'at least 3', and falsy input is not a crash."""
    assert split_silicon_ref("alif:ensemble:e8") == ("alif", "ensemble", "e8")
    assert split_silicon_ref("alif:ensemble") is None
    assert split_silicon_ref("alif:ensemble:e8:r2") is None
    assert split_silicon_ref("") is None
    assert split_silicon_ref(None) is None


def test_resolve_soc_path_still_roots_under_socs():
    """resolve_soc_path() is a rooting convenience -- its layout is unchanged."""
    assert resolve_soc_path("alif:ensemble:e8", Path("/m")) == Path(
        "/m/socs/alif/ensemble/e8.json"
    )
    assert resolve_soc_path("alif:ensemble", Path("/m")) is None
    assert resolve_soc_path(None, Path("/m")) is None


def test_no_hand_rolled_silicon_split_outside_the_owner():
    """Fails the moment a fourth copy of the three-part split appears.

    Greps the real tree rather than trusting the migration: this is the
    check that would have caught #1096 being filed at all.
    """
    pattern = re.compile(r"\b(silicon|silicon_ref|soc_ref)\s*\.split\(")
    offenders = []
    for py in SCRIPTS.rglob("*.py"):
        if py == OWNER:
            continue
        for lineno, line in enumerate(py.read_text(encoding="utf-8").splitlines(), 1):
            if pattern.search(line):
                offenders.append(f"{py.relative_to(REPO)}:{lineno}: {line.strip()}")

    assert not offenders, (
        "hand-rolled `silicon:` ref split(s) found outside "
        f"{OWNER.relative_to(REPO)}:\n  " + "\n  ".join(offenders) + "\n"
        "Use split_silicon_ref() (and root the result yourself), or "
        "resolve_soc_path() when you want <metadata_root>/socs/... -- "
        "otherwise a future widening of the ref format has to be re-found "
        "by grep, which is exactly what #997, #1004 and #1096 each had to do."
    )


def test_owner_module_holds_the_only_split():
    """Guards the grep above: if the owner stops splitting, the test is vacuous."""
    assert 'silicon.split(":")' in OWNER.read_text(encoding="utf-8"), (
        f"{OWNER.relative_to(REPO)} no longer contains the split the rest of "
        f"the tree is forbidden from re-implementing -- either it moved (update "
        f"OWNER here) or the single-source property is gone"
    )


def test_resolve_targets_still_raises_valueerror_on_a_malformed_ref(tmp_path):
    """The migration must NOT collapse ValueError into FileNotFoundError.

    `resolve_soc_path()` returns None for both 'malformed ref' and (by not
    checking the disk) 'ref fine, file absent'. Callers of resolve_targets()
    distinguish the two, so the site re-raises ValueError itself.
    """
    from alp_model.targets import resolve_targets

    meta = tmp_path / "metadata"
    (meta / "e1m_modules").mkdir(parents=True)
    (meta / "e1m_modules" / "E1M-TEST.yaml").write_text(
        "silicon: alif:ensemble\n", encoding="utf-8"
    )
    with pytest.raises(ValueError, match="malformed silicon ref"):
        resolve_targets("E1M-TEST", metadata_root=meta)


def test_resolve_targets_still_raises_filenotfound_for_a_wellformed_missing_spec(tmp_path):
    """The other half of the pair -- a good ref naming a spec that isn't there."""
    from alp_model.targets import resolve_targets

    meta = tmp_path / "metadata"
    (meta / "e1m_modules").mkdir(parents=True)
    (meta / "e1m_modules" / "E1M-TEST.yaml").write_text(
        "silicon: alif:ensemble:nosuchpart\n", encoding="utf-8"
    )
    with pytest.raises(FileNotFoundError, match="no SoC spec for"):
        resolve_targets("E1M-TEST", metadata_root=meta)


def test_load_soc_caps_still_returns_none_on_a_malformed_ref(tmp_path):
    """validator's site soft-fails; it must not start raising."""
    from alp_cli.validator import _load_soc_caps

    assert _load_soc_caps("alif:ensemble", soc_dir=tmp_path) is None
    assert _load_soc_caps("nonsense", soc_dir=tmp_path) is None


def test_load_soc_caps_roots_at_the_injected_soc_dir_not_a_metadata_parent(tmp_path):
    """Pins why this site uses split_silicon_ref() and not resolve_soc_path().

    Rebuilding it as `resolve_soc_path(ref, soc_dir.parent)` is exact only
    while every caller passes a directory literally named `socs`. This test
    injects one that is not, so that shortcut fails here.
    """
    from alp_cli.validator import _load_soc_caps

    odd = tmp_path / "not-named-socs"
    (odd / "alif" / "ensemble").mkdir(parents=True)
    (odd / "alif" / "ensemble" / "e8.json").write_text(
        '{"peripherals": {"uart": 4}}', encoding="utf-8"
    )
    assert _load_soc_caps("alif:ensemble:e8", soc_dir=odd) == {"uart": 4}


def test_new_som_scaffold_still_writes_under_output_root(tmp_path):
    """new_som's site roots at output_root/metadata, not the repo metadata root.

    Deliberately NOT skip-on-nonzero: a skip here would enforce nothing,
    which is the same inert-gate shape this PR's sibling work keeps finding.
    Supplying --sku/--soc-ref/--family is what suppresses the interactive
    prompts; there is no --non-interactive flag. The SKU must satisfy the
    som-preset SKU pattern (#1089), hence E1M-NX9999 rather than a freeform
    name.
    """
    # sys.executable, not "python3": the CLI needs `click`, which lives in
    # whatever interpreter/venv is running pytest -- a bare "python3" picks
    # up the system one on the macOS and Linux CI legs and dies with
    # ModuleNotFoundError. Likewise inherit os.environ and override only
    # PYTHONPATH; replacing the whole environment strips the venv's
    # site-packages resolution along with it.
    env = dict(os.environ)
    env["PYTHONPATH"] = str(SCRIPTS)
    result = subprocess.run(
        [sys.executable, "-m", "alp_cli", "new-som",
         "--sku", "E1M-NX9999", "--soc-ref", "testvendor:testfam:testpart",
         "--family", "testfam", "--output-root", str(tmp_path)],
        cwd=REPO, capture_output=True, text=True, env=env,
    )
    assert result.returncode == 0, (
        f"alp new-som failed (rc={result.returncode}):\n"
        f"stdout: {result.stdout[-800:]}\nstderr: {result.stderr[-800:]}"
    )
    assert (tmp_path / "metadata" / "socs" / "testvendor" / "testfam"
            / "testpart.json").is_file(), (
        "scaffold did not land under output_root -- the resolve_soc_path() "
        "migration must keep rooting at output_root/metadata, not the repo's"
    )
    assert (tmp_path / "metadata" / "e1m_modules" / "E1M-NX9999.yaml").is_file()
    # The preset banner cites the repo-relative SoC path built by the
    # str-not-Path site (#1096) -- pin its shape, since a Path leaking in
    # there would render backslashes on Windows.
    banner = (tmp_path / "metadata" / "e1m_modules" / "E1M-NX9999.yaml").read_text(
        encoding="utf-8"
    )
    assert "metadata/socs/testvendor/testfam/testpart.json" in banner
