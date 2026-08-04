# SPDX-License-Identifier: Apache-2.0
"""#1170: `soc_peripheral_instances[].instance` slugs must be unique within
one SoM preset.  JSON Schema `uniqueItems` alone can't catch this -- it only
rejects two byte-identical array entries, so two entries naming the same
`instance` with a different `class`/`driver_status` validate cleanly.
`_check_som_peripheral_instance_uniqueness` in `scripts/validate_metadata.py`
is the semantic cross-check that closes that gap; #655's DT/pinctrl
generation binds nodes by this slug, so a silent duplicate is a real
generation hazard, not an editorial nit.
"""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _load_vm():
    spec = importlib.util.spec_from_file_location(
        "vm_instance_uniqueness", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    return vm


def _write_fixture(name: str, body: str) -> Path:
    # _check_som_peripheral_instance_uniqueness() reports paths relative to
    # REPO, so the fixture must live inside the checkout.
    p = REPO / "metadata" / "e1m_modules" / f".test-{name}.yaml"
    p.write_text(body, encoding="utf-8")
    return p


def test_rejects_duplicate_instance_slug():
    vm = _load_vm()
    p = _write_fixture(
        "dup-instance",
        "soc_peripheral_instances:\n"
        "  - { instance: uart1, class: uart, silicon_peripheral: \"UART1_RXD1/TXD1\", driver_status: none }\n"
        "  - { instance: uart1, class: spi,  silicon_peripheral: \"SOMETHING_ELSE\",   driver_status: planned }\n",
    )
    try:
        failures = vm._check_som_peripheral_instance_uniqueness([p])
    finally:
        p.unlink(missing_ok=True)
    assert failures
    assert "instance `uart1` declared 2 times" in failures[0][1][0]


def test_accepts_unique_instance_slugs():
    vm = _load_vm()
    p = _write_fixture(
        "unique-instances",
        "soc_peripheral_instances:\n"
        "  - { instance: uart1, class: uart, silicon_peripheral: \"UART1_RXD1/TXD1\", driver_status: none }\n"
        "  - { instance: i2s0,  class: i2s,  silicon_peripheral: \"SSIU1_SSI1_*\",     driver_status: none }\n",
    )
    try:
        failures = vm._check_som_peripheral_instance_uniqueness([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures
