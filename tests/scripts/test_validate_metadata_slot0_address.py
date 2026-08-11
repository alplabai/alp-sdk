# SPDX-License-Identifier: Apache-2.0
"""tan-cli#353: alp-sdk emitted no `flash_args.slot0_load_address` at all,
so tan correctly routed an AEN slice to Flow D and then refused outright
(`flash_args.slot0_load_address is required to auto-sign via SETOOLS`),
forcing a customer to hand-edit `system-manifest.yaml` to flash an AEN.

`_check_som_slot0_address_resolved` in `scripts/validate_metadata.py` is
the semantic guard that keeps this from regressing at the metadata layer:
a `memory_map:` region that spells a `*_slot0` name (declaring an MRAM
slot0 path) but whose `base:` isn't a resolved integer address is refused,
rather than silently falling through
`alp_orchestrate.loader._resolve_slot0_load_address` /
`gen_zephyr_board._aen_role_slot0_map` to the wrong default (or None).
"""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _load_vm():
    spec = importlib.util.spec_from_file_location(
        "vm_slot0_address", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    return vm


def _write_fixture(name: str, body: str) -> Path:
    # _check_som_slot0_address_resolved() reports paths relative to REPO,
    # so the fixture must live inside the checkout.
    p = REPO / "metadata" / "e1m_modules" / f".test-{name}.yaml"
    p.write_text(body, encoding="utf-8")
    return p


def test_rejects_slot0_region_with_tbd_base():
    vm = _load_vm()
    p = _write_fixture(
        "slot0-tbd-base",
        "memory_map:\n"
        "  - { name: mcuboot,  base: 0x80000000, size_kib: 64,   "
        "accessible_from: [m55_he], cacheable: true, carveout: false }\n"
        "  - { name: he_slot0, base: \"TBD\",      size_kib: 2688, "
        "accessible_from: [m55_he], cacheable: true, carveout: false }\n",
    )
    try:
        failures = vm._check_som_slot0_address_resolved([p])
    finally:
        p.unlink(missing_ok=True)
    assert failures
    assert "he_slot0" in failures[0][1][0]
    assert "declares an MRAM slot0 path" in failures[0][1][0]


def test_rejects_slot0_region_with_missing_base():
    vm = _load_vm()
    p = _write_fixture(
        "slot0-missing-base",
        "memory_map:\n"
        "  - { name: hp_slot0, size_kib: 2688, "
        "accessible_from: [m55_hp], cacheable: true, carveout: false }\n",
    )
    try:
        failures = vm._check_som_slot0_address_resolved([p])
    finally:
        p.unlink(missing_ok=True)
    assert failures
    assert "hp_slot0" in failures[0][1][0]


def test_accepts_slot0_region_with_resolved_base():
    vm = _load_vm()
    p = _write_fixture(
        "slot0-resolved-base",
        "memory_map:\n"
        "  - { name: he_slot0, base: 0x80010000, size_kib: 2688, "
        "accessible_from: [m55_he], cacheable: true, carveout: false }\n"
        "  - { name: hp_slot0, base: 0x802b0000, size_kib: 2688, "
        "accessible_from: [m55_hp], cacheable: true, carveout: false }\n",
    )
    try:
        failures = vm._check_som_slot0_address_resolved([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_accepts_preset_with_no_memory_map():
    vm = _load_vm()
    p = _write_fixture("no-memory-map", "sku: E1M-TST001\n")
    try:
        failures = vm._check_som_slot0_address_resolved([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_real_e1m_aen801_preset_resolves_clean():
    """The one real shipped preset this rule actually guards -- must stay
    clean against the live checkout, not just a synthetic fixture."""
    vm = _load_vm()
    real = REPO / "metadata" / "e1m_modules" / "E1M-AEN801.yaml"
    assert real.is_file()
    failures = vm._check_som_slot0_address_resolved([real])
    assert not failures
