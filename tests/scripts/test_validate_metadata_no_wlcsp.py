# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#1444: no Alif Ensemble `variants[]` row may declare a WLCSP package.

Alp Lab modules are BGA only, so a WLCSP row describes a part this corpus
will never ship. That is a product decision, not a fact about the silicon --
Alif really does offer WLCSP208/WLCSP216 across several subfamilies, and each
file's own `packages` list still records that.

It is a gate rather than a note because the natural way to extend
`variants[]` is to walk the vendor pack's device list, and the pack draws no
line between the packages we buy and the ones we do not. Nine WLCSP rows
accumulated that way, each carrying a `debug` block -- a pyocd target and a
J-Link flash profile -- for hardware that does not exist here.

`_check_soc_no_wlcsp_variants` in `scripts/validate_metadata.py` is the
guard. Non-Alif-Ensemble SoCs (Renesas, NXP, DEEPX) are untouched: their
package choices are not ours to rule on.

Synthetic fixtures below live under pytest's `tmp_path`, not the real
`metadata/socs/alif/ensemble/` tree: `Path.rglob("*.json")` (what
`validate_metadata.py`'s own `SOCS.rglob("*.json")` walk uses) matches
dot-prefixed files too, so a fixture dropped into the real tree is picked up
by every other test/gate that walks it (`test_validate_metadata_passes_on_
real_tree`, a hand-run `python3 scripts/validate_metadata.py`, ...). `REPO`
is monkeypatched to `tmp_path` so the checked function's own
`path.relative_to(REPO)` still resolves.
"""

import importlib.util
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ENSEMBLE = REPO / "metadata" / "socs" / "alif" / "ensemble"


def _load_vm(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location(
        "vm_no_wlcsp", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    monkeypatch.setattr(vm, "REPO", tmp_path)
    return vm


def _write_fixture(tmp_path: Path, name: str, body: str) -> Path:
    p = tmp_path / f"{name}.json"
    p.write_text(body, encoding="utf-8")
    return p


_HEADER = '{ "vendor": "Alif Semiconductor", "family": "Ensemble", '


def test_rejects_a_wlcsp_variant(tmp_path, monkeypatch):
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "reject",
        _HEADER + '"variants": [ { "order_code": "AE999XAS", '
        '"package": "WLCSP208" } ] }',
    )
    failures = vm._check_soc_no_wlcsp_variants([p])
    assert failures
    message = failures[0][1][0]
    assert "AE999XAS" in message, message
    assert "WLCSP208" in message, message
    # The reader is told what to do, not merely that something is wrong.
    assert "Drop the variant" in message, message


def test_accepts_a_bga_variant(tmp_path, monkeypatch):
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "accept",
        _HEADER + '"variants": [ { "order_code": "AE999XLS", '
        '"package": "FBGA194" } ] }',
    )
    failures = vm._check_soc_no_wlcsp_variants([p])
    assert not failures


def test_a_variant_with_no_package_is_not_refused(tmp_path, monkeypatch):
    """`package` is optional in soc-spec-v1. An undeclared package is not a
    WLCSP claim, and this guard must not turn absence into a failure."""
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "no-package",
        _HEADER + '"variants": [ { "order_code": "AE999X" } ] }',
    )
    failures = vm._check_soc_no_wlcsp_variants([p])
    assert not failures


def test_ignores_non_alif_ensemble_socs(tmp_path, monkeypatch):
    """Another vendor's package choices are not this rule's business."""
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "non-alif",
        '{ "vendor": "Renesas", "family": "RZ/V2N", "variants": '
        '[ { "order_code": "R9A09G077", "package": "WLCSP208" } ] }',
    )
    failures = vm._check_soc_no_wlcsp_variants([p])
    assert not failures


def test_non_object_variants_entry_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`variants[]` entries are schema-typed objects, but the schema pass
    that would reject a non-object entry runs separately and is not
    guaranteed to have run first. A bare string in the list used to reach a
    bare `.get()` and raise `AttributeError` here, hiding the schema FAIL
    line that already explains the real problem. Filtered to dicts, this doc
    must return cleanly -- no real variant survives the filter."""
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "non-object-variant",
        _HEADER + '"variants": [ "not-a-dict" ] }',
    )
    failures = vm._check_soc_no_wlcsp_variants([p])
    assert not failures  # must not raise


def test_non_string_package_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`package` is schema-typed as a string, but a malformed document can
    carry a non-string truthy value there (e.g. the bare int `208`, seen on
    a real Alif Ensemble part file) -- `package.upper()` used to raise
    `AttributeError: 'int' object has no attribute 'upper'` here, aborting
    the whole gate mid-run instead of leaving the schema FAIL line (which
    already flags the type mismatch) to explain the real problem."""
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "non-string-package",
        _HEADER + '"variants": [ { "order_code": "AE999X", "package": 208 } ] }',
    )
    failures = vm._check_soc_no_wlcsp_variants([p])
    assert failures == []  # must not raise; a non-string package is not a WLCSP claim


def test_non_object_top_level_does_not_crash_the_gate(tmp_path, monkeypatch):
    """The SoC doc's top level is schema-typed as an object, but a
    malformed file could parse to a bare JSON array -- `doc.get("vendor")`
    used to raise `AttributeError: 'list' object has no attribute 'get'`
    here, aborting the whole gate mid-run instead of leaving the schema
    FAIL line (which already flags the type mismatch) to explain the real
    problem."""
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(tmp_path, "non-object-doc", "[]")
    failures = vm._check_soc_no_wlcsp_variants([p])
    assert failures == []  # must not raise


def test_non_list_variants_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`variants` itself is schema-typed as an array, but a malformed
    document can carry a non-list scalar there (e.g. the bare int `5`,
    which is truthy) -- `for i, v in enumerate(variants)` over the
    unfiltered value used to raise `TypeError: 'int' object is not
    iterable` here, aborting the whole gate mid-run instead of leaving the
    schema FAIL line (which already flags the type mismatch) to explain
    the real problem."""
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(tmp_path, "non-list-variants", _HEADER + '"variants": 5 }')
    failures = vm._check_soc_no_wlcsp_variants([p])
    assert failures == []  # must not raise


def test_the_real_corpus_is_clean():
    real = sorted(ENSEMBLE.glob("e*.json"))
    assert len(real) == 6
    spec = importlib.util.spec_from_file_location(
        "vm_no_wlcsp_real", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    assert not vm._check_soc_no_wlcsp_variants(real)


def test_every_shipped_variant_is_bga():
    """The corpus assertion behind the gate, stated positively.

    The guard refuses the string WLCSP; this pins the other side -- every
    row that ships is an FBGA part -- so a third package spelling could not
    slip through by being neither.
    """
    packages = {
        v["order_code"]: v.get("package")
        for path in sorted(ENSEMBLE.glob("e*.json"))
        for v in json.loads(path.read_text(encoding="utf-8"))["variants"]
    }
    assert packages, "no Alif Ensemble variants found"
    non_bga = {k: v for k, v in packages.items()
               if not (v or "").startswith("FBGA")}
    assert not non_bga, non_bga
