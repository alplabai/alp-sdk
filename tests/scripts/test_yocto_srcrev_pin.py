# SPDX-License-Identifier: Apache-2.0
"""Every meta-alp-sdk recipe that builds from this repository fetches ONE
pinned commit of branch=main (alp-sdk-version.inc), never a floating
${AUTOREV} and never another branch (#2962)."""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
META = REPO / "meta-alp-sdk"
VERSION_INC = META / "recipes-core/alp-sdk/alp-sdk-version.inc"
SELF_URI = "alplabai/alp-sdk.git"


def _self_recipes() -> list[Path]:
    return [p for p in META.rglob("*.bb") if SELF_URI in p.read_text(encoding="utf-8")]


def test_self_recipes_are_found():
    assert len(_self_recipes()) >= 9


def test_nothing_in_the_layer_floats_on_autorev():
    offenders = [str(p.relative_to(META)) for p in META.rglob("*")
                 if p.suffix in {".bb", ".inc", ".bbappend", ".conf"}
                 and re.search(r'^\s*SRCREV\S*\s*[?:]*=\s*"\$\{AUTOREV\}"', p.read_text(encoding="utf-8"), re.M)]
    assert not offenders


def test_the_shared_pin_is_a_full_commit_hash():
    text = VERSION_INC.read_text(encoding="utf-8")
    assert re.search(r'^SRCREV\s*=\s*"[0-9a-f]{40}"$', text, re.M)


def test_self_recipes_take_the_pin_and_the_branch_from_one_place():
    for p in _self_recipes():
        text = p.read_text(encoding="utf-8")
        assert "require recipes-core/alp-sdk/alp-sdk-version.inc" in text, p
        assert not re.search(r"^SRCREV", text, re.M), f"{p}: SRCREV belongs in alp-sdk-version.inc"
        uri = re.search(r'^SRC_URI = "git://github.com/alplabai/alp-sdk\.git[^"]*"', text, re.M)
        assert uri and ";branch=main" in uri.group(0), p


def test_the_pin_is_the_tag_commit_of_pv():
    """SRCREV is the commit v<PV> points at; skipped where the tag is not
    fetched (shallow clones, or PV bumped before the tag exists)."""
    import subprocess

    import pytest

    text = VERSION_INC.read_text(encoding="utf-8")
    pv = re.search(r'^PV\s*=\s*"([^"]+)"', text, re.M).group(1).replace("~", "-")
    pin = re.search(r'^SRCREV\s*=\s*"([0-9a-f]{40})"', text, re.M).group(1)
    tag = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"v{pv}^{{commit}}"],
                         cwd=REPO, capture_output=True, text=True, encoding="utf-8")
    if tag.returncode != 0:
        pytest.skip(f"tag v{pv} is not in this clone")
    assert tag.stdout.strip() == pin
