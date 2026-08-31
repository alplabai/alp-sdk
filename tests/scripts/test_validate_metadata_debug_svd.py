# SPDX-License-Identifier: Apache-2.0
"""`variants[].debug.svd` shape + conditional-existence gate (#948).

The key names the CMSIS-SVD file cortex-debug passes as `svdFile`. It resolves
in two places -- the repository directory first, then `ALP_SVD_DIR` -- so the
gate deliberately does two different things:

  * SHAPE is checked always. Absolute paths, `..`, and URLs are refused
    wherever the file would come from.
  * EXISTENCE is checked only for a value that CLAIMS to be in-repo, i.e. one
    under `metadata/svd/` (the subtree ADR 0032 designates). Anything else is
    the customer-supplied case, which no gate on a build host can see.

The discriminator is the declared prefix, not the filesystem. An earlier draft
keyed it on "does the parent directory exist", which inverted the check: a
value naming a subtree nobody had created yet -- exactly the first mistake a
vendoring PR would make -- passed silently.
`test_missing_file_under_the_vendored_prefix_is_refused` is that regression.

No SoC declares `svd` today; the gate ships ahead of the data on purpose, so
the first value to land is checked by an already-reviewed rule.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import validate_metadata as vm  # noqa: E402

_ABSENT = object()


def _run(tmp_path: Path, monkeypatch, svd, *, create: str | None = None) -> list:
    """Run the gate over one synthetic SoC file rooted at *tmp_path*.

    `vm.REPO` is monkeypatched so the gate's `relative_to(REPO)` and its
    in-repo existence probe both land in the tmp tree -- the real repository
    is never written to.
    """
    monkeypatch.setattr(vm, "REPO", tmp_path)
    if create is not None:
        target = tmp_path / create
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text("<device/>", encoding="utf-8")
    variant: dict = {"order_code": "AE822FA0E5597LS0"}
    if svd is _ABSENT:
        variant["debug"] = {"pyocd_target": "AE822FA0E5597LS0"}
    else:
        variant["debug"] = {"svd": svd}
    soc = tmp_path / "socs" / "probe.json"
    soc.parent.mkdir(parents=True, exist_ok=True)
    soc.write_text(json.dumps({"variants": [variant]}), encoding="utf-8")
    return vm._check_soc_debug_svd_shape([soc])


@pytest.mark.parametrize(
    ("svd", "fragment"),
    [
        ("/opt/alif/AE822_HE.svd", "absolute path"),
        ("C:/alif/AE822_HE.svd", "absolute path"),
        ("../outside/AE822_HE.svd", "`..`"),
        ("https://vendor.example/AE822_HE.svd", "URL"),
        ("", "non-empty string"),
    ],
)
def test_bad_shapes_are_refused_wherever_the_file_would_come_from(
    tmp_path: Path, monkeypatch, svd: str, fragment: str
) -> None:
    failures = _run(tmp_path, monkeypatch, svd)
    assert failures, f"{svd!r} should have been refused"
    (_, msgs) = failures[0]
    assert any(fragment in m for m in msgs), msgs
    # The message must name the offending variant, or a multi-variant SoC
    # leaves the reader hunting for which one.
    assert any("AE822FA0E5597LS0" in m for m in msgs), msgs


def test_missing_file_under_the_vendored_prefix_is_refused(
    tmp_path: Path, monkeypatch
) -> None:
    """The regression the first draft of this gate let through.

    `metadata/svd/alif/` does not exist, and that is precisely why the value
    is wrong: it asserts the repository carries a file it does not.
    """
    failures = _run(tmp_path, monkeypatch, "metadata/svd/alif/AE822_HE.svd")
    assert failures
    (_, msgs) = failures[0]
    assert any("not present" in m for m in msgs), msgs


def test_present_file_under_the_vendored_prefix_passes(
    tmp_path: Path, monkeypatch
) -> None:
    rel = "metadata/svd/alif/AE822FA0E5597LS0_CM55_HE_View.svd"
    assert _run(tmp_path, monkeypatch, rel, create=rel) == []


@pytest.mark.parametrize(
    "svd",
    [
        "vendor-supplied/AE822FA0E5597LS0_CM55_HE_View.svd",
        "Debug/SVD/AE822FA0E5597LS0_CM55_HE_View.svd",
        "AE822FA0E5597LS0_CM55_HP_View.svd",
    ],
)
def test_customer_supplied_paths_pass_without_existing(
    tmp_path: Path, monkeypatch, svd: str
) -> None:
    """The `ALP_SVD_DIR` case names a file on the customer's machine.

    Failing these would break a correctly-configured project on any build
    host that simply has no vendor SDK installed -- the opposite of what the
    gate is for.
    """
    assert _run(tmp_path, monkeypatch, svd) == []


def test_absent_key_is_not_a_defect(tmp_path: Path, monkeypatch) -> None:
    """`debug`'s house rule: an absent key is a published 'unknown'."""
    assert _run(tmp_path, monkeypatch, _ABSENT) == []


def test_no_soc_declares_svd_today() -> None:
    """Pins the deliberate absence, and fails loudly the day it changes.

    Whether Alp Lab redistributes a vendor's SVD under that vendor's terms is
    a per-vendor maintainer decision (ADR 0032 records the mechanism, not an
    authorisation). If a `debug.svd` value ever lands, this test should be
    updated in the same change that argues for it -- not quietly deleted.
    """
    declared = []
    for soc in sorted((REPO / "metadata" / "socs").rglob("*.json")):
        doc = json.loads(soc.read_text(encoding="utf-8"))
        for v in doc.get("variants") or []:
            if isinstance(v, dict) and isinstance(v.get("debug"), dict) \
                    and "svd" in v["debug"]:
                declared.append(f"{soc.relative_to(REPO)}:{v.get('order_code')}")
    assert declared == [], (
        "a SoC now declares `debug.svd`: " + ", ".join(declared) + ". That is "
        "a licensing decision, not a metadata edit -- see ADR 0032 and #948.")
