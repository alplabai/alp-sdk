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

import re
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


# test_new_som_scaffold_still_writes_under_output_root() lived here through
# #1367/#1368's own PR: it spawned `python -m alp_cli new-som ...`, a
# subprocess argv STRING an AST import scan (the audit #1367/#1368 relied on)
# structurally cannot see -- the retirement deleted scripts/alp_cli/new_som.py
# and __main__.py out from under it, so it failed hard
# (`No module named alp_cli.__main__`) rather than skipping. The next
# retirement of a scripts/alp_cli/** module must grep subprocess argv strings
# too, not just `import` statements.
#
# It is deleted rather than re-pointed at `tan new-som`: alp-sdk owns no
# new-som scaffolding logic any more (it moved to tan-cli's in-process
# `python/tan/commands/new_som_cmd.py`, released at tan-cli v0.6.0, which
# carries its own coverage there) and this repo's own test-all.sh /
# tests/scripts/ stage never puts a `tan` binary on PATH -- see
# scripts/bootstrap.sh:689 ("Tan is installed separately"). A subprocess call
# to `tan new-som` here would just trade one hard failure for another on
# every host that runs this suite without a separate tan-cli install.
