# SPDX-License-Identifier: Apache-2.0
"""#1365 split A: `_check_som_write_authority_present` in
`scripts/validate_metadata.py` is the semantic gate for
`memory_region.write_authority` that `metadata/schemas/som-preset-v1.
schema.json` defers (the field is deliberately NOT `required` in v1, so
a customer preset that authored `memory_map:` rows before this field
existed does not break at `tan validate` on upgrade).

Absence must never be read as `customer_runtime` (ADR-0034 clause 4) --
a region an author forgot to annotate must fail loudly here instead of
silently defaulting to the most permissive value.
"""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _load_vm():
    spec = importlib.util.spec_from_file_location(
        "vm_memory_authority", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    return vm


def _write_fixture(name: str, body: str) -> Path:
    # _check_som_write_authority_present() reports paths relative to
    # REPO, so the fixture must live inside the checkout.
    p = REPO / "metadata" / "e1m_modules" / f".test-{name}.yaml"
    p.write_text(body, encoding="utf-8")
    return p


def test_rejects_region_missing_write_authority():
    vm = _load_vm()
    p = _write_fixture(
        "authority-missing",
        "memory_map:\n"
        "  - { name: mcuboot, base: 0x80000000, size_kib: 64, "
        "accessible_from: [m55_he], cacheable: true, carveout: false, "
        "write_authority: vendor_image }\n"
        "  - { name: storage, base: 0x80560000, size_kib: 96, "
        "accessible_from: [m55_he], carveout: false }\n",
    )
    try:
        failures = vm._check_som_write_authority_present([p])
    finally:
        p.unlink(missing_ok=True)
    assert failures
    assert "storage" in failures[0][1][0]
    assert "UNRESOLVED" in failures[0][1][0]
    assert "customer_runtime" in failures[0][1][0]


def test_absence_is_never_read_as_customer_runtime():
    """A region missing `write_authority` must be flagged regardless of
    what its NAME suggests the value should be -- the check does not
    infer a default from the region's own name."""
    vm = _load_vm()
    p = _write_fixture(
        "authority-missing-storage-named",
        "memory_map:\n"
        "  - { name: storage, base: 0x80560000, size_kib: 96, "
        "accessible_from: [m55_he], carveout: false }\n",
    )
    try:
        failures = vm._check_som_write_authority_present([p])
    finally:
        p.unlink(missing_ok=True)
    assert failures
    msg = failures[0][1][0]
    assert "storage" in msg
    assert "never `customer_runtime`" in msg or "UNRESOLVED" in msg


def test_accepts_region_with_write_authority():
    vm = _load_vm()
    p = _write_fixture(
        "authority-present",
        "memory_map:\n"
        "  - { name: atoc, base: 0x80578000, size_kib: 32, "
        "accessible_from: [m55_he], carveout: false, "
        "write_authority: secure_enclave }\n",
    )
    try:
        failures = vm._check_som_write_authority_present([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_accepts_preset_with_no_memory_map():
    vm = _load_vm()
    p = _write_fixture("no-memory-map", "sku: E1M-TST001\n")
    try:
        failures = vm._check_som_write_authority_present([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_real_aen_presets_all_carry_write_authority():
    """Every real shipped AEN preset this rule guards -- all must stay
    clean against the live checkout.

    Asserts the PROPERTY over however many AEN presets ship, not a fixed
    count: `dev` added E1M-AEN803 while #1365 was in flight, and a
    hardcoded `== 6` turned a new SoM into a red test instead of the
    coverage it should have been.  The named six are pinned as a floor so
    a preset going missing still fails.
    """
    vm = _load_vm()
    aen_presets = sorted(
        (REPO / "metadata" / "e1m_modules").glob("E1M-AEN*.yaml"))
    known = {f"E1M-AEN{n}.yaml" for n in (301, 401, 501, 601, 701, 801)}
    assert known <= {p.name for p in aen_presets}
    failures = vm._check_som_write_authority_present(aen_presets)
    assert not failures


def test_real_non_aen_presets_are_skipped_not_flagged():
    """E1M-V2N101/102, E1M-V2M101/102, E1M-NX9101 author no `memory_map:`
    at all -- this check must not fire on them."""
    vm = _load_vm()
    non_aen = [
        REPO / "metadata" / "e1m_modules" / f"{sku}.yaml"
        for sku in ("E1M-V2N101", "E1M-V2N102", "E1M-V2M101",
                    "E1M-V2M102", "E1M-NX9101")
    ]
    for p in non_aen:
        assert p.is_file()
    failures = vm._check_som_write_authority_present(non_aen)
    assert not failures
