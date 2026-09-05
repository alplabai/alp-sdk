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
    """A crashed/`-x`-aborted run must never leave a fixture behind in
    tracked `metadata/socs/` -- unlike `metadata/e1m_modules/` and
    `metadata/chips/`, that tree has no `.test-*` precedent, and a
    stray file there bricks `validate_metadata.py` repo-wide. Point
    `cr.SOCS` at a scratch `tmp_path` instead of writing into the real
    checkout."""
    cr = _load_cr()
    monkeypatch.setattr(cr, "SOCS", tmp_path)
    bad_dir = tmp_path / "alif" / "ensemble"
    bad_dir.mkdir(parents=True)
    bad_path = bad_dir / ".test-bad-aperture.json"
    bad_path.write_text(
        '{"soc_spec_version": 1, "ref": "alif:ensemble:xx", '
        '"soc_flash_base": 2147483647}',
        encoding="utf-8",
    )
    failures = cr._check_aperture_cross_check()
    assert failures
    joined = "\n".join(failures)
    assert ".test-bad-aperture.json" in joined
    assert "_AEN_MRAM_BASE" in joined


def test_every_aen_preset_resolves_a_non_none_aperture():
    """#1365 split A MAJOR 1: coverage must not depend on the new
    `_check_aperture_declared()` gate alone -- pin directly that every
    shipped AEN preset still resolves a concrete aperture (base, top),
    not None.

    Asserts the PROPERTY over however many AEN presets ship, not a fixed
    count: `dev` added E1M-AEN803 while #1365 was in flight, and a
    hardcoded `== 6` turned a new SoM into a red test instead of the
    coverage it should have been.  The named six are pinned as a floor so
    a preset going missing still fails.
    """
    cr = _load_cr()
    aen_presets = sorted(cr.PRESETS.glob("E1M-AEN*.yaml"))
    known = {f"E1M-AEN{n}.yaml" for n in (301, 401, 501, 601, 701, 801)}
    assert known <= {p.name for p in aen_presets}
    for path in aen_presets:
        doc = cr.yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        aperture = cr._resolve_aperture(doc)
        assert aperture is not None, f"{path.name} resolved no aperture"


def test_aperture_declared_gate_passes_on_the_real_tree():
    cr = _load_cr()
    assert not cr._check_aperture_declared()


def test_aperture_declared_gate_fails_when_an_ensemble_soc_omits_the_field(
    tmp_path, monkeypatch
):
    """MAJOR 1: reproduces the reviewer's exact mutation -- an Alif
    Ensemble SoC that declares no `soc_flash_base` -- against a scratch
    `SOCS` tree, so this doesn't depend on e3-e8.json never regressing.
    Deleting `soc_flash_base` from e3-e7.json got rc=0 from every other
    signal (validate_metadata.py, gen_catalog.py, 21 other new tests);
    this is the one check that catches it."""
    cr = _load_cr()
    monkeypatch.setattr(cr, "SOCS", tmp_path)
    ensemble = tmp_path / "alif" / "ensemble"
    ensemble.mkdir(parents=True)
    (ensemble / "e3.json").write_text(
        '{"soc_spec_version": 1, "ref": "alif:ensemble:e3", '
        '"family": "Ensemble", "part": "E3"}',
        encoding="utf-8",
    )
    failures = cr._check_aperture_declared()
    assert failures
    joined = "\n".join(failures)
    assert "e3.json" in joined
    assert "soc_flash_base" in joined


def test_aperture_declared_gate_passes_when_the_field_is_present(tmp_path, monkeypatch):
    cr = _load_cr()
    monkeypatch.setattr(cr, "SOCS", tmp_path)
    ensemble = tmp_path / "alif" / "ensemble"
    ensemble.mkdir(parents=True)
    (ensemble / "e3.json").write_text(
        '{"soc_spec_version": 1, "ref": "alif:ensemble:e3", '
        '"family": "Ensemble", "part": "E3", "soc_flash_base": 2147483648}',
        encoding="utf-8",
    )
    assert not cr._check_aperture_declared()


def test_aperture_uses_variant_mram_mb_not_soc_flash_mb():
    """MAJOR 3: e3.json's 1.5 MB order codes (AE302F80C1557LE,
    AE302F40C1537LE) carry `variants[].mram_mb == 1.5` against the
    SoC's own `soc_flash_mb == 5.5` -- the one real-tree pair where the
    two disagree, so this is what makes `variant.get("mram_mb")` a live
    discriminator against `soc_spec.get("soc_flash_mb")`. Before this
    test, every AEN preset's variant happened to carry `mram_mb` equal
    to its SoC's `soc_flash_mb`, so swapping the two source fields in
    `_resolve_aperture()` passed all 30 other tests."""
    cr = _load_cr()
    aperture = cr._resolve_aperture({
        "silicon": "alif:ensemble:e3",
        "silicon_variant": "AE302F80C1557LE",
    })
    assert aperture is not None
    base, top = aperture
    assert base == 0x80000000
    # base + 1.5 MiB (the VARIANT's mram_mb), NOT base + soc_flash_mb's 5.5 MiB.
    assert top == 0x80180000


def test_unresolved_base_row_emits_a_skip_note_not_silence(capsys):
    """MAJOR 2: a region with an unresolved `base:` inside an aperture
    that DID resolve must be SAID, not just silently `continue`d --
    every real AEN preset carries exactly one such row (`mram_main`,
    `base: "TBD"`), and the gate's own summary line never mentioned
    that a row went unevaluated."""
    cr = _load_cr()
    p = _write_fixture(
        cr, "aperture-skip-note",
        _SILICON_HEADER + "memory_map:\n"
        "  - { name: mram_main, base: \"TBD\", size_kib: 5632, "
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
    assert not failures  # an unresolved-base skip must never fail the gate
    printed = capsys.readouterr().out
    assert "mram_main" in printed
    assert "SKIP" in printed


def test_row_straddling_the_aperture_floor_gets_a_distinct_message():
    """MINOR 5: a row with `lo < full_lo < hi` was wrongly reported as
    overlapping a preceding contained region -- there is none, since
    it's the first row. The real defect is that the row crosses the
    aperture floor, symmetric to the existing top-overflow message."""
    cr = _load_cr()
    p = _write_fixture(
        cr, "aperture-floor-straddle",
        _SILICON_HEADER + "memory_map:\n"
        "  - { name: mcuboot, base: 0x7fff0000, size_kib: 80,   "
        "accessible_from: [m55_he], carveout: false, write_authority: vendor_image }\n"
        "  - { name: storage, base: 0x80004000, size_kib: 5584, "
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
    assert "floor" in joined
    assert "overlaps the preceding contained region" not in joined
