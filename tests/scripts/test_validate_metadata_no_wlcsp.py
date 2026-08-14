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
"""

import importlib.util
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ENSEMBLE = REPO / "metadata" / "socs" / "alif" / "ensemble"


def _load_vm():
    spec = importlib.util.spec_from_file_location(
        "vm_no_wlcsp", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    return vm


def _write_fixture(name: str, body: str) -> Path:
    # The check reports paths relative to REPO, so the fixture must live
    # inside the checkout.
    p = ENSEMBLE / f".test-wlcsp-{name}.json"
    p.write_text(body, encoding="utf-8")
    return p


_HEADER = '{ "vendor": "Alif Semiconductor", "family": "Ensemble", '


def test_rejects_a_wlcsp_variant():
    vm = _load_vm()
    p = _write_fixture(
        "reject",
        _HEADER + '"variants": [ { "order_code": "AE999XAS", '
        '"package": "WLCSP208" } ] }',
    )
    try:
        failures = vm._check_soc_no_wlcsp_variants([p])
    finally:
        p.unlink(missing_ok=True)
    assert failures
    message = failures[0][1][0]
    assert "AE999XAS" in message, message
    assert "WLCSP208" in message, message
    # The reader is told what to do, not merely that something is wrong.
    assert "Drop the variant" in message, message


def test_accepts_a_bga_variant():
    vm = _load_vm()
    p = _write_fixture(
        "accept",
        _HEADER + '"variants": [ { "order_code": "AE999XLS", '
        '"package": "FBGA194" } ] }',
    )
    try:
        failures = vm._check_soc_no_wlcsp_variants([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_a_variant_with_no_package_is_not_refused():
    """`package` is optional in soc-spec-v1. An undeclared package is not a
    WLCSP claim, and this guard must not turn absence into a failure."""
    vm = _load_vm()
    p = _write_fixture(
        "no-package",
        _HEADER + '"variants": [ { "order_code": "AE999X" } ] }',
    )
    try:
        failures = vm._check_soc_no_wlcsp_variants([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_ignores_non_alif_ensemble_socs():
    """Another vendor's package choices are not this rule's business."""
    vm = _load_vm()
    p = _write_fixture(
        "non-alif",
        '{ "vendor": "Renesas", "family": "RZ/V2N", "variants": '
        '[ { "order_code": "R9A09G077", "package": "WLCSP208" } ] }',
    )
    try:
        failures = vm._check_soc_no_wlcsp_variants([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_the_real_corpus_is_clean():
    real = sorted(ENSEMBLE.glob("e*.json"))
    assert len(real) == 6
    assert not _load_vm()._check_soc_no_wlcsp_variants(real)


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
