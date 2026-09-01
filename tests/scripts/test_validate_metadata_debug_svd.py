# SPDX-License-Identifier: Apache-2.0
"""`variants[].debug.svd` shape + conditional-existence gate (#948).

The key maps a `cores[].id` to the CMSIS-SVD file cortex-debug passes as
`svdFile`. It is a MAP, not one string per variant, because an SVD is one
core's register view: the Alif DFP ships `<order_code>_CM55_HE_View.svd` and
`<order_code>_CM55_HP_View.svd` as separate files, and the sibling
`debug.jlink_device` in the same object is already keyed the same way.
`test_a_per_variant_string_is_refused` pins that, because the shape was
originally a single string and the first vendoring PR would have had to change
the schema and this gate again to land real data (#1890 review).

The value resolves in two places -- the repository directory first, then
`ALP_SVD_DIR` -- so the gate deliberately does two different things:

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

Shape is judged with `PureWindowsPath`, which treats both `/` and `\\` as
separators. The `windows_separator` cases are the second regression: under
`PurePosixPath` every one of them PASSED while its POSIX twin was refused,
because `..\\outside\\x.svd` reads as a single filename.

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

#: Mirrors a real Alif Ensemble E8 topology: two M55s that have SVD views and
#: an A-class cluster that does not.
_CORES = [
    {"id": "a32_cluster", "type": "cortex-a32", "count": 2},
    {"id": "m55_hp", "type": "cortex-m55", "count": 1},
    {"id": "m55_he", "type": "cortex-m55", "count": 1},
]


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
    soc.write_text(
        json.dumps({"cores": _CORES, "variants": [variant]}), encoding="utf-8")
    return vm._check_soc_debug_svd_shape([soc])


def _one(svd_value: str) -> dict:
    """The common single-core map, so a shape case reads as one value."""
    return {"m55_he": svd_value}


# --- shape: refused wherever the file would come from ----------------------

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
    failures = _run(tmp_path, monkeypatch, _one(svd))
    assert failures, f"{svd!r} should have been refused"
    (_, msgs) = failures[0]
    assert any(fragment in m for m in msgs), msgs
    # The message must name the offending variant AND core, or a multi-variant
    # SoC leaves the reader hunting for which one.
    assert any("AE822FA0E5597LS0" in m for m in msgs), msgs
    assert any("m55_he" in m for m in msgs), msgs


@pytest.mark.parametrize(
    ("svd", "fragment"),
    [
        (r"..\outside\AE822_HE.svd", "`..`"),
        (r"sub\..\..\AE822_HE.svd", "`..`"),
        (r"\alif\AE822_HE.svd", "absolute path"),
        (r"\\server\share\AE822_HE.svd", "absolute path"),
        (r"C:\alif\AE822_HE.svd", "absolute path"),
    ],
)
def test_windows_separator_spellings_are_refused_like_their_posix_twins(
    tmp_path: Path, monkeypatch, svd: str, fragment: str
) -> None:
    """The `PurePosixPath` hole (#1890 review).

    Under the original implementation every one of these PASSED while the
    identical POSIX spelling was refused, because `PurePosixPath` does not
    treat a backslash as a separator -- it read the whole value as one
    filename. A backslash is the likeliest real mistake on a Windows
    maintainer host, and `..\\` escapes whichever root resolved it just as
    `../` does.
    """
    failures = _run(tmp_path, monkeypatch, _one(svd))
    assert failures, f"{svd!r} should have been refused"
    (_, msgs) = failures[0]
    assert any(fragment in m for m in msgs), msgs


def test_a_windows_spelled_vendored_prefix_cannot_dodge_the_existence_check(
    tmp_path: Path, monkeypatch
) -> None:
    """A backslash-spelled `metadata/svd/...` claims the in-repo prefix.

    Under `PurePosixPath` its `parts[:2]` never equalled
    `('metadata', 'svd')`, so it skipped the existence half entirely while
    the POSIX spelling was held to it.
    """
    failures = _run(
        tmp_path, monkeypatch, _one(r"metadata\svd\alif\AE822_HE.svd"))
    assert failures
    (_, msgs) = failures[0]
    assert any("not present" in m for m in msgs), msgs


# --- the map shape itself --------------------------------------------------

def test_a_per_variant_string_is_refused(tmp_path: Path, monkeypatch) -> None:
    """The original shape, pinned as wrong.

    One string per variant cannot say which core's view it names, and the
    consequence is the one the key's own description calls worse than shipping
    nothing: an HE register map attached to an HP session reads plausibly.
    """
    failures = _run(
        tmp_path, monkeypatch, "AE822FA0E5597LS0_CM55_HE_View.svd")
    assert failures
    (_, msgs) = failures[0]
    assert any("keyed" in m and "cores[].id" in m for m in msgs), msgs


def test_an_unknown_core_key_is_refused(tmp_path: Path, monkeypatch) -> None:
    """A typo'd core id would silently attach to nothing."""
    failures = _run(tmp_path, monkeypatch, {"m55_typo": "AE822_HE.svd"})
    assert failures
    (_, msgs) = failures[0]
    assert any("not a cores[].id" in m for m in msgs), msgs
    # The message lists the real ids, or the reader cannot see the typo.
    assert any("m55_he" in m for m in msgs), msgs


def test_an_empty_map_is_refused(tmp_path: Path, monkeypatch) -> None:
    """An empty object declares the key while carrying nothing."""
    failures = _run(tmp_path, monkeypatch, {})
    assert failures
    (_, msgs) = failures[0]
    assert any("non-empty object" in m for m in msgs), msgs


def test_a_sparse_map_is_fine(tmp_path: Path, monkeypatch) -> None:
    """Declaring only the cores whose view you have is the expected case.

    The A-class cluster typically has no SVD; demanding coverage of every
    core would refuse the realistic document.
    """
    assert _run(tmp_path, monkeypatch, {"m55_he": "AE822_HE.svd"}) == []


# --- existence, only under the vendored prefix -----------------------------

def test_missing_file_under_the_vendored_prefix_is_refused(
    tmp_path: Path, monkeypatch
) -> None:
    """The regression the first draft of this gate let through.

    `metadata/svd/alif/` does not exist, and that is precisely why the value
    is wrong: it asserts the repository carries a file it does not.
    """
    failures = _run(
        tmp_path, monkeypatch, _one("metadata/svd/alif/AE822_HE.svd"))
    assert failures
    (_, msgs) = failures[0]
    assert any("not present" in m for m in msgs), msgs


def test_present_file_under_the_vendored_prefix_passes(
    tmp_path: Path, monkeypatch
) -> None:
    rel = "metadata/svd/alif/AE822FA0E5597LS0_CM55_HE_View.svd"
    assert _run(tmp_path, monkeypatch, _one(rel), create=rel) == []


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
    assert _run(tmp_path, monkeypatch, _one(svd)) == []


def test_absent_key_is_not_a_defect(tmp_path: Path, monkeypatch) -> None:
    """`debug`'s house rule: an absent key is a published 'unknown'."""
    assert _run(tmp_path, monkeypatch, _ABSENT) == []


# --- the diagnostics have to actually reach the operator -------------------

def test_the_failure_is_printed_not_only_returned(
    tmp_path: Path, monkeypatch, capsys
) -> None:
    """#1890 review: the gate returned its messages and printed nothing.

    Every sibling checker in this file prints `FAIL <rel>` and a bulleted
    reason; `main()` only sums the failure counts. So a vendoring PR that
    tripped this gate got a red build naming no file, no core and no rule,
    and had to bisect its own metadata to find out which of six checks
    fired -- the repo's own silent-failure red flag, applied to this gate's
    whole deliverable. Asserting on the returned list alone cannot catch
    that, which is why this test reads stdout.
    """
    failures = _run(tmp_path, monkeypatch, _one("/opt/alif/AE822_HE.svd"))
    assert failures
    out = capsys.readouterr().out
    assert "FAIL " in out, out
    assert "socs/probe.json" in out, out
    assert "AE822FA0E5597LS0" in out, out
    assert "m55_he" in out, out
    assert "absolute path" in out, out


def test_the_failure_entry_is_a_str_like_every_sibling(
    tmp_path: Path, monkeypatch
) -> None:
    """`_check_files()`-shaped, so a caller can join the two lists.

    The first draft appended a `Path` where every sibling appends the
    `str` relative path.
    """
    failures = _run(tmp_path, monkeypatch, _one("/opt/alif/AE822_HE.svd"))
    assert failures
    assert isinstance(failures[0][0], str), type(failures[0][0])


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
