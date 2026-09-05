# SPDX-License-Identifier: Apache-2.0
"""#1365 split A: `check_atoc_reservation.py`'s aperture-anchored checks --

  - `_check_aperture_cross_check()`: every Alif SoC declaring
    `soc_flash_base` must agree with `gen_zephyr_board._AEN_MRAM_BASE`,
    the single source `_aen_flash_partitions()` and
    `_resolve_slot0_load_address()` already import.
  - `_check_aperture_tiling()` (4b): the authored regions CONTAINED in
    the declared aperture must tile it exactly -- no gaps, no overlaps.
    Anchored on the SoC aperture, never on `mram_main` (whose `base` is
    the string `"TBD"`); rows outside the aperture are ignored, not
    gaps; the whole-device alias (extent == full aperture) is exempt.

These extend `check_atoc_reservation.py` rather than opening a second
script over the same 42 rows -- see that file's own docstring on why.
"""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _load_cr():
    spec = importlib.util.spec_from_file_location(
        "cr_aperture_tiling", REPO / "scripts/check_atoc_reservation.py"
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

# E8's aperture per metadata/socs/alif/ensemble/e8.json: base 0x80000000,
# variant mram_mb 5.5 -> top 0x80580000 (5632 KiB).


def test_real_tree_has_no_aperture_tiling_failures():
    cr = _load_cr()
    failures = []
    for path in sorted(cr.PRESETS.glob("*.yaml")):
        failures += cr._check_preset(path)
    assert not failures


def test_gap_between_regions_fails_naming_the_region():
    cr = _load_cr()
    p = _write_fixture(
        cr, "aperture-gap",
        _SILICON_HEADER + "memory_map:\n"
        "  - { name: mcuboot, base: 0x80000000, size_kib: 64,   "
        "accessible_from: [m55_he], carveout: false, write_authority: vendor_image }\n"
        # 64 KiB gap: 0x80010000 - 0x80020000 is undeclared.
        "  - { name: storage, base: 0x80020000, size_kib: 5472, "
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
    assert "gap" in joined
    assert "storage" in joined


def test_overlapping_regions_fail_naming_the_region():
    cr = _load_cr()
    p = _write_fixture(
        cr, "aperture-overlap",
        _SILICON_HEADER + "memory_map:\n"
        "  - { name: mcuboot, base: 0x80000000, size_kib: 64,   "
        "accessible_from: [m55_he], carveout: false, write_authority: vendor_image }\n"
        # Overlaps mcuboot by 32 KiB (starts before mcuboot ends).
        "  - { name: storage, base: 0x80008000, size_kib: 5536, "
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
    assert "overlaps" in joined
    assert "storage" in joined


def test_row_outside_aperture_not_reported_as_gap():
    cr = _load_cr()
    p = _write_fixture(
        cr, "aperture-outside-row",
        _SILICON_HEADER + "memory_map:\n"
        "  - { name: mcuboot,  base: 0x80000000, size_kib: 64,   "
        "accessible_from: [m55_he], carveout: false, write_authority: vendor_image }\n"
        "  - { name: storage,  base: 0x80010000, size_kib: 5536, "
        "accessible_from: [m55_he], carveout: false, write_authority: customer_runtime }\n"
        "  - { name: atoc,     base: 0x80578000, size_kib: 32,   "
        "accessible_from: [m55_he], carveout: false, write_authority: secure_enclave }\n"
        # Entirely outside the aperture -- must not be treated as a gap.
        "  - { name: ospi_xip, base: 0x70000000, size_kib: 1024, "
        "accessible_from: [m55_he], carveout: false, write_authority: none }\n",
    )
    try:
        failures = cr._check_preset(p)
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_whole_device_alias_spanning_full_aperture_not_reported_as_overlap():
    cr = _load_cr()
    p = _write_fixture(
        cr, "aperture-whole-device-alias",
        _SILICON_HEADER + "memory_map:\n"
        # Spans the FULL aperture -- exempt as the whole-device alias,
        # not counted as overlapping the regions it aliases.
        "  - { name: mram_main, base: 0x80000000, size_kib: 5632, "
        "accessible_from: [m55_he], carveout: false, write_authority: composite }\n"
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


def test_no_aperture_declared_skips_tiling_entirely():
    """A preset naming no `silicon:` (or a non-Alif one) resolves no
    aperture -- 4b must not fire at all, gap or no gap."""
    cr = _load_cr()
    p = _write_fixture(
        cr, "no-aperture",
        "sku: E1M-TST002\n"
        "memory_map:\n"
        "  - { name: mcuboot, base: 0x80000000, size_kib: 64, "
        "accessible_from: [m55_he], carveout: false, write_authority: vendor_image }\n"
        # A huge gap that WOULD fail 4b if an aperture resolved. Named
        # 'atoc' so the pre-existing (unrelated) ATOC-top-of-window
        # check does not fire and this test isolates 4b alone.
        "  - { name: atoc, base: 0x90000000, size_kib: 64, "
        "accessible_from: [m55_he], carveout: false, write_authority: secure_enclave }\n",
    )
    try:
        failures = cr._check_preset(p)
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_aperture_cross_check_passes_on_real_socs():
    cr = _load_cr()
    assert not cr._check_aperture_cross_check()


def test_aperture_cross_check_fails_on_disagreeing_base(tmp_path, monkeypatch):
    cr = _load_cr()
    bad_dir = REPO / "metadata" / "socs" / "alif" / "ensemble"
    bad_path = bad_dir / ".test-bad-aperture.json"
    bad_path.write_text(
        '{"soc_spec_version": 1, "ref": "alif:ensemble:xx", '
        '"soc_flash_base": 2147483647}',
        encoding="utf-8",
    )
    try:
        failures = cr._check_aperture_cross_check()
    finally:
        bad_path.unlink(missing_ok=True)
    assert failures
    joined = "\n".join(failures)
    assert ".test-bad-aperture.json" in joined
    assert "_AEN_MRAM_BASE" in joined
