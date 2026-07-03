"""Unit tests for the ADR 0018 library-compatibility block of
scripts/gen_portability_matrix.py.

Covers determinism + committed-file sync of the generated library table,
the new BEGIN/END marker handling (hand prose survives regeneration), and
the (library × SKU) cell classification.  The classification tests drive
`gpm.library_cell` with hand-built BoardProjects + manifest dicts so a
compatible / incompatible / n-a verdict can be asserted regardless of
whether the real SKU set happens to contain an incompatible cell today --
the logic under test is the rendering of the shared resolver's answer, not
the answer for one committed SoM.
"""

import sys
from pathlib import Path

import gen_portability_matrix as gpm  # noqa: E402  (scripts/ on sys.path via conftest)

from alp_orchestrate.models import BoardProject, Slice

REPO = Path(__file__).resolve().parents[2]
DOC = REPO / "docs" / "portability-matrix.md"


def _project(*, soc_cores, slices, som_preset=None) -> BoardProject:
    """A BoardProject carrying just the fields the library resolver reads.

    `soc_cores` is [(id, type)] for soc_spec.cores[]; `slices` is
    [(core_id, os)] for the effective per-core OS set.
    """
    return BoardProject(
        sku="E1M-TST001", hw_rev=None, board_name=None, board_hw_rev=None,
        cores={cid: Slice(core_id=cid, os=os_) for cid, os_ in slices},
        ipc=[],
        soc_spec={
            "cores": [{"id": cid, "type": ctype} for cid, ctype in soc_cores],
            "soc_ram_kb": 4096,
        },
        som_preset=som_preset or {},
        board_preset=None,
    )


# ---------------------------------------------------------------------
# Determinism + committed-file sync (the CI gate contract for this block)
# ---------------------------------------------------------------------


def test_generate_is_deterministic():
    assert gpm.generate() == gpm.generate()


def test_committed_doc_matches_generator():
    assert DOC.read_text(encoding="utf-8") == gpm.generate()


def test_library_block_is_byte_stable_across_two_sweeps():
    presets = gpm.load_presets()
    first = gpm.render_library_block(*gpm.library_sweep(presets))
    second = gpm.render_library_block(*gpm.library_sweep(presets))
    assert first == second


# ---------------------------------------------------------------------
# Marker handling for the new library block
# ---------------------------------------------------------------------


def test_library_markers_are_distinct_from_matrix_markers():
    assert gpm.LIB_BEGIN_MARK != gpm.BEGIN_MARK
    assert gpm.LIB_END_MARK != gpm.END_MARK


def test_library_block_preserves_surrounding_prose():
    doc = ("before prose\n"
           f"{gpm.LIB_BEGIN_MARK}\nstale\n{gpm.LIB_END_MARK}\n"
           "after prose\n")
    block = f"{gpm.LIB_BEGIN_MARK}\nFRESH\n{gpm.LIB_END_MARK}"
    out = gpm.replace_block(doc, block, gpm.LIB_BEGIN_MARK, gpm.LIB_END_MARK)
    assert "before prose" in out
    assert "after prose" in out
    assert "FRESH" in out
    assert "stale" not in out
    # Idempotent re-splice.
    assert gpm.replace_block(out, block, gpm.LIB_BEGIN_MARK,
                             gpm.LIB_END_MARK) == out


def test_generated_doc_keeps_library_section_prose_outside_markers():
    text = gpm.generate()
    begin = text.index(gpm.LIB_BEGIN_MARK)
    end = text.index(gpm.LIB_END_MARK)
    prose = text[:begin] + text[end + len(gpm.LIB_END_MARK):]
    # The hand-written section heading + explanation live outside the block.
    assert "## Curated library compatibility" in prose
    # And the matrix block above is untouched by the library splice.
    assert gpm.BEGIN_MARK in prose and gpm.END_MARK in prose


# ---------------------------------------------------------------------
# Auto-discovery: every manifest under metadata/libraries/ grows a row
# ---------------------------------------------------------------------


def test_every_manifest_has_a_row():
    from alp_orchestrate.libraries import available_libraries
    text = gpm.generate()
    for name in available_libraries(gpm.METADATA):
        assert f"| `{name}` |" in text, f"no row for library {name}"


def test_manifest_metadata_columns_render_from_the_manifest():
    text = gpm.generate()
    # lvgl's pinned version + MIT licence are transcribed straight from
    # metadata/libraries/lvgl.yaml -- never hand-typed here.
    lvgl_row = next(ln for ln in text.splitlines()
                    if ln.startswith("| `lvgl` |"))
    assert "`9.5.0`" in lvgl_row
    assert "MIT" in lvgl_row


# ---------------------------------------------------------------------
# Known committed cell: lvgl is compatible on a RAM-ample Cortex-M SoM
# ---------------------------------------------------------------------


def _lib_cell(text: str, library: str, sku: str) -> str:
    lines = text.splitlines()
    header = None
    for ln in lines:
        if ln.startswith("| Library | Tier |"):
            header = [c.strip() for c in ln.strip("|").split("|")]
        if ln.startswith(f"| `{library}` |"):
            assert header is not None
            cells = [c.strip() for c in ln.strip("|").split("|")]
            if sku in header:
                return cells[header.index(sku)]
    raise AssertionError(f"no cell for {library} x {sku}")


def test_lvgl_compatible_on_ram_ample_m_som():
    text = gpm.generate()
    # E1M-AEN801 (Alif E8, Cortex-M55, ample RAM) satisfies lvgl's 64 KiB
    # RAM floor and wires via its Zephyr integration.
    assert _lib_cell(text, "lvgl", "E1M-AEN801") == gpm.PASS


# ---------------------------------------------------------------------
# Cell classification (reuses the emitter's resolver via gpm.library_cell)
# ---------------------------------------------------------------------


def test_cell_compatible_when_requires_met_and_wireable():
    manifest = {"integration": {"zephyr": {"kconfig": ["CONFIG_X=y"]}}}
    project = _project(soc_cores=[("m0", "cortex-m55")],
                       slices=[("m0", "zephyr")])
    assert gpm.library_cell("x", manifest, project) == gpm.PASS


def test_cell_incompatible_names_the_failing_core_class():
    # cmsis-nn-style: requires a Cortex-M core, target is A-only.
    manifest = {"requires": {"core_class": "m"},
                "integration": {"zephyr": {"kconfig": ["CONFIG_X=y"]}}}
    project = _project(soc_cores=[("a0", "cortex-a55")],
                       slices=[("a0", "yocto")])
    cell = gpm.library_cell("x", manifest, project)
    assert cell.startswith(gpm.FAIL)
    assert "core_class" in cell
    # The SoM name (already the column header) is stripped from the reason.
    assert "E1M-TST001" not in cell


def test_cell_incompatible_on_ram_floor():
    manifest = {"requires": {"min_ram_kib": 999999},
                "integration": {"zephyr": {"kconfig": ["CONFIG_X=y"]}}}
    project = _project(soc_cores=[("m0", "cortex-m55")],
                       slices=[("m0", "zephyr")])
    cell = gpm.library_cell("x", manifest, project)
    assert cell.startswith(gpm.FAIL)
    assert "min_ram_kib" in cell


def test_cell_na_when_no_integration_for_any_running_os():
    # requires: met (none), but the manifest only wires Yocto while the
    # target runs a bare Zephyr M-core -> not applicable, not incompatible.
    manifest = {"integration": {"yocto": {"image_install": ["x"]}}}
    project = _project(soc_cores=[("m0", "cortex-m55")],
                       slices=[("m0", "zephyr")])
    assert gpm.library_cell("x", manifest, project) == gpm.NA


# ---------------------------------------------------------------------
# The reason compaction is a pure reformat of the resolver's message
# ---------------------------------------------------------------------


def test_requirement_reason_extracts_the_constraint():
    assert gpm._requirement_reason(
        "library `x` requires core_class `m`, but SoM `E1M-Y` has "
        "core class(es) ['a']") == "core_class `m`"
    assert gpm._requirement_reason(
        "library `x` requires capability `gpu2d`, which SoM `E1M-Y` does "
        "not provide") == "capability `gpu2d`"
    assert gpm._requirement_reason(
        "library `x` requires min_ram_kib 64, but SoC for SoM `E1M-Y` has "
        "32 KB RAM") == "min_ram_kib 64"


if __name__ == "__main__":
    sys.exit(__import__("pytest").main([__file__, "-q"]))
