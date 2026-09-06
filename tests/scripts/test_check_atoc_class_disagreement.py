# SPDX-License-Identifier: Apache-2.0
"""#1365 split A: `check_atoc_reservation.py::_check_class_disagreement`
(4c) -- derives the flash/RAM class by containment against the declared
MRAM aperture and compares it with the authored `carveout`. Containment
is a ONE-DIRECTIONAL test: inside the aperture proves flash (so
`carveout` must be exactly `False`); outside proves NOTHING, because
Ensemble's OSPI XIP windows are flash outside the aperture and the same
OSPI0 controller also carries the W958D8NBYA5I HyperRAM on
`chip_select: 1` -- a RAM row with `carveout: false` outside the
aperture is a legitimate hardware secure-enclave reservation, not a
defect. A region with an unresolved base is skipped, never classified
(ADR-0034 clause 4)."""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _load_cr():
    spec = importlib.util.spec_from_file_location(
        "cr_class_disagreement", REPO / "scripts/check_atoc_reservation.py"
    )
    cr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cr)
    return cr


def _write_fixture(cr, name: str, body: str) -> Path:
    p = cr.PRESETS / f".test-{name}.yaml"
    p.write_text(body, encoding="utf-8")
    return p


_SILICON_HEADER = (
    "sku: E1M-TST001\n"
    "silicon: alif:ensemble:e8\n"
    "silicon_variant: AE822FA0E5597LS0\n"
)


def test_real_tree_has_no_class_disagreement_failures():
    cr = _load_cr()
    failures = []
    for path in sorted(cr.PRESETS.glob("*.yaml")):
        failures += cr._check_preset(path)
    assert not failures


def test_region_inside_aperture_with_carveout_true_fails():
    cr = _load_cr()
    p = _write_fixture(
        cr, "class-disagreement",
        _SILICON_HEADER + "memory_map:\n"
        # Contained in the aperture but wrongly claims carveout-eligible.
        "  - { name: mcuboot, base: 0x80000000, size_kib: 64,   "
        "accessible_from: [m55_he], carveout: true, write_authority: vendor_image }\n"
        "  - { name: storage, base: 0x80010000, size_kib: 5536, "
        "accessible_from: [m55_he], carveout: false, write_authority: customer_runtime }\n"
        "  - { name: atoc,    base: 0x80578000, size_kib: 32,   "
        "accessible_from: [m55_he], carveout: false, write_authority: secure_enclave }\n",
    )
    try:
        failures = cr._check_preset(p)
    finally:
        p.unlink(missing_ok=True)
    assert failures
    joined = "\n".join(failures)
    assert "mcuboot" in joined
    assert "0x80000000" in joined


def test_region_inside_aperture_with_carveout_absent_fails():
    """`carveout` defaults to `true` (eligible) per the schema, so an
    authored region inside the aperture with NO `carveout` key at all
    must still fail -- absence is not a free pass."""
    cr = _load_cr()
    p = _write_fixture(
        cr, "class-disagreement-absent",
        _SILICON_HEADER + "memory_map:\n"
        "  - { name: mcuboot, base: 0x80000000, size_kib: 64,   "
        "accessible_from: [m55_he], write_authority: vendor_image }\n"
        "  - { name: storage, base: 0x80010000, size_kib: 5536, "
        "accessible_from: [m55_he], carveout: false, write_authority: customer_runtime }\n"
        "  - { name: atoc,    base: 0x80578000, size_kib: 32,   "
        "accessible_from: [m55_he], carveout: false, write_authority: secure_enclave }\n",
    )
    try:
        failures = cr._check_preset(p)
    finally:
        p.unlink(missing_ok=True)
    assert failures
    assert "mcuboot" in "\n".join(failures)


def test_unresolved_base_is_skipped_not_classified():
    """A region with `base: "TBD"` inside what WOULD be the aperture,
    carrying a `carveout` that would fail if classified, must produce
    NO failure -- skipped, never guessed at (ADR-0034 clause 4)."""
    cr = _load_cr()
    p = _write_fixture(
        cr, "class-unresolved-base",
        _SILICON_HEADER + "memory_map:\n"
        "  - { name: he_slot0, base: \"TBD\", size_kib: 2688, "
        "accessible_from: [m55_he], carveout: true, write_authority: customer_image }\n"
        "  - { name: mcuboot,  base: 0x80000000, size_kib: 64,   "
        "accessible_from: [m55_he], carveout: false, write_authority: vendor_image }\n"
        "  - { name: storage,  base: 0x80010000, size_kib: 5536, "
        "accessible_from: [m55_he], carveout: false, write_authority: customer_runtime }\n"
        "  - { name: atoc,     base: 0x80578000, size_kib: 32,   "
        "accessible_from: [m55_he], carveout: false, write_authority: secure_enclave }\n",
    )
    try:
        failures = cr._check_preset(p)
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_row_outside_aperture_with_carveout_false_passes():
    """The HyperRAM-on-shared-OSPI0-controller case: a RAM row entirely
    outside the aperture legitimately carries `carveout: false` (e.g.
    SRAM reserved for a hardware secure enclave) -- the one-directional
    rule means this must NOT be flagged."""
    cr = _load_cr()
    p = _write_fixture(
        cr, "class-outside-false",
        _SILICON_HEADER + "memory_map:\n"
        "  - { name: mcuboot,   base: 0x80000000, size_kib: 64,   "
        "accessible_from: [m55_he], carveout: false, write_authority: vendor_image }\n"
        "  - { name: storage,   base: 0x80010000, size_kib: 5536, "
        "accessible_from: [m55_he], carveout: false, write_authority: customer_runtime }\n"
        "  - { name: atoc,      base: 0x80578000, size_kib: 32,   "
        "accessible_from: [m55_he], carveout: false, write_authority: secure_enclave }\n"
        # Outside the aperture, RAM reserved for a secure enclave.
        "  - { name: hyperram_secure, base: 0x60000000, size_kib: 512, "
        "accessible_from: [m55_he], carveout: false, write_authority: none }\n",
    )
    try:
        failures = cr._check_preset(p)
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_row_outside_aperture_with_carveout_true_also_passes():
    """Symmetrically, outside the aperture proves nothing in EITHER
    direction -- a row there with carveout left eligible is also not
    classified/asserted against."""
    cr = _load_cr()
    p = _write_fixture(
        cr, "class-outside-true",
        _SILICON_HEADER + "memory_map:\n"
        "  - { name: mcuboot, base: 0x80000000, size_kib: 64,   "
        "accessible_from: [m55_he], carveout: false, write_authority: vendor_image }\n"
        "  - { name: storage, base: 0x80010000, size_kib: 5536, "
        "accessible_from: [m55_he], carveout: false, write_authority: customer_runtime }\n"
        "  - { name: atoc,    base: 0x80578000, size_kib: 32,   "
        "accessible_from: [m55_he], carveout: false, write_authority: secure_enclave }\n"
        "  - { name: ospi_xip, base: 0x70000000, size_kib: 1024, "
        "accessible_from: [m55_he], carveout: true, write_authority: customer_image }\n",
    )
    try:
        failures = cr._check_preset(p)
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_whole_device_alias_exempt_regardless_of_carveout():
    cr = _load_cr()
    p = _write_fixture(
        cr, "class-whole-device-alias",
        _SILICON_HEADER + "memory_map:\n"
        "  - { name: mram_main, base: 0x80000000, size_kib: 5632, "
        "accessible_from: [m55_he], carveout: true, write_authority: composite }\n"
        "  - { name: mcuboot, base: 0x80000000, size_kib: 64,   "
        "accessible_from: [m55_he], carveout: false, write_authority: vendor_image }\n"
        "  - { name: storage, base: 0x80010000, size_kib: 5536, "
        "accessible_from: [m55_he], carveout: false, write_authority: customer_runtime }\n"
        "  - { name: atoc,    base: 0x80578000, size_kib: 32,   "
        "accessible_from: [m55_he], carveout: false, write_authority: secure_enclave }\n",
    )
    try:
        failures = cr._check_preset(p)
    finally:
        p.unlink(missing_ok=True)
    assert not failures
