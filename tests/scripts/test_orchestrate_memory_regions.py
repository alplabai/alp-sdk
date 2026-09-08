# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for the system manifest's `memory[]` pane -- #1365 item 3.

`resolve_memory_regions()` projects the SoM's effective memory-region
table (`alp_project.resolve_memory_map`'s all-or-nothing derivation) into
the resolved, name-joined view `system-manifest-v1` declares, and
`emit_system_manifest()` carries it.

The vocabulary under test is the SHIPPED one, not the one #1365's issue
body proposes: `kind` is `aperture.classify_region()`'s own four verdicts
(`flash` / `ram` / `unclassified` / `unresolved`), the authority field is
`write_authority` with som-preset-v1's six values (not a 3-value `owner`),
and `status` is `ok` / `unresolved` -- the word the schema's own items
description already made normative (ADR-0034 clause 4), not `ipc[]`'s
`ok` / `blocked`.

Run locally:

    python -m pytest tests/scripts/test_orchestrate_memory_regions.py -v
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, _write_board   # noqa: E402

from alp_orchestrate import memory                      # noqa: E402
from alp_orchestrate import (                          # noqa: E402
    emit_system_manifest,
    load_board_yaml,
    resolve_memory_regions,
)

SCHEMA = REPO / "metadata" / "schemas" / "system-manifest-v1.schema.json"

# E1M-AEN301 authors a `memory_map:` -- seven rows, six with a resolved
# base and `mram_main` deliberately carrying `base: "TBD"`.  Its aperture
# is [0x80000000, 0x80580000) (soc_flash_base + 5.5 MiB).
AEN_BOARD = """
name: test-aen-memory
som:
  sku: E1M-AEN301
  hw_rev: r1

cores:
  m55_hp:
    os: zephyr
    app: ./m55_hp

storage:
  - { name: settings, size_kib: 64, fs: littlefs, flash_device: ospi0, mount: /lfs/settings }
"""

# E1M-V2N101 declares no on-die flash aperture (`soc_flash_base` is
# deliberately omitted for RZ/V2N) and authors no `memory_map:`, so its
# rows come from the SoC side.
V2N_BOARD = """
name: test-v2n-memory
som:
  sku: E1M-V2N101
  hw_rev: r1

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
"""


def _memory(board_text: str, tmp_path: Path) -> list[dict]:
    """Emit a manifest for `board_text` and return its `memory[]` rows."""
    project = load_board_yaml(_write_board(tmp_path, board_text))
    return yaml.safe_load(emit_system_manifest(project)).get("memory", [])


def _row(rows: list[dict], name: str) -> dict:
    matches = [r for r in rows if r["name"] == name]
    assert matches, f"no memory[] row named {name!r} in {[r['name'] for r in rows]}"
    return matches[0]


# ---------------------------------------------------------------------
# The pane lands, with the SoM's own regions
# ---------------------------------------------------------------------


def test_emit_system_manifest_carries_memory_for_a_preset_authored_som(
        tmp_path: Path) -> None:
    """The AEN preset's seven authored regions each become one row."""
    rows = _memory(AEN_BOARD, tmp_path)

    assert sorted(r["name"] for r in rows) == [
        "atoc", "he_slot0", "hp_slot0", "mcuboot", "mram_main",
        "reserved", "storage",
    ]


def test_every_row_carries_the_four_required_fields(tmp_path: Path) -> None:
    """`name`, `source`, `kind` and `status` are derivable for every row,
    so every row carries all four -- they are the schema's `required`."""
    for row in _memory(AEN_BOARD, tmp_path):
        for key in ("name", "source", "kind", "status"):
            assert key in row, f"{row['name']} is missing {key}"


# ---------------------------------------------------------------------
# source -- the provenance the IDE derives editability from
# ---------------------------------------------------------------------


def test_preset_authored_regions_report_source_som_preset(
        tmp_path: Path) -> None:
    """A SoM that authors `memory_map:` owns every row in the table --
    `resolve_memory_map`'s precedence is all-or-nothing, so provenance is
    uniform across the pane rather than per row."""
    rows = _memory(AEN_BOARD, tmp_path)

    assert {r["source"] for r in rows} == {"som_preset"}


def test_regions_the_loader_derives_report_source_soc_derived(
        tmp_path: Path) -> None:
    """A SoM with no `memory_map:` override gets its rows from the SoC
    side -- either its fixed `memory_regions` table or the silicon-variant
    derivation -- and both are `soc_derived`."""
    rows = _memory(V2N_BOARD, tmp_path)

    assert rows, "V2N101 resolves a non-empty memory map"
    assert {r["source"] for r in rows} == {"soc_derived"}


# ---------------------------------------------------------------------
# kind -- classify_region()'s own verdicts, not a lossy 2-value collapse
# ---------------------------------------------------------------------


def test_a_region_contained_in_the_aperture_is_flash(tmp_path: Path) -> None:
    """`mcuboot` is [0x80000000, 0x80010000), flush with the aperture's
    low edge and strictly inside it."""
    assert _row(_memory(AEN_BOARD, tmp_path), "mcuboot")["kind"] == "flash"


def test_a_region_whose_base_does_not_resolve_is_kind_unresolved(
        tmp_path: Path) -> None:
    """`mram_main` carries `base: "TBD"`, so there is no extent to test
    against the aperture."""
    assert _row(_memory(AEN_BOARD, tmp_path), "mram_main")["kind"] == "unresolved"


def test_a_som_with_no_declared_aperture_classifies_every_row_unresolved(
        tmp_path: Path) -> None:
    """RZ/V2N omits `soc_flash_base` deliberately, so `classify_region()`
    has nothing to compare against and says so rather than guessing `ram`
    -- the emitter reports that verdict verbatim instead of inventing a
    class the deriver never returned."""
    assert {r["kind"] for r in _memory(V2N_BOARD, tmp_path)} == {"unresolved"}


# ---------------------------------------------------------------------
# status / reason -- ADR-0034 clause 4, never a guessed base
# ---------------------------------------------------------------------


def test_an_unresolved_base_carries_status_and_reason_but_no_base(
        tmp_path: Path) -> None:
    """The schema's items description is normative: a region whose base
    does not resolve carries no `base` and says why."""
    row = _row(_memory(AEN_BOARD, tmp_path), "mram_main")

    assert row["status"] == "unresolved"
    assert "base" not in row
    assert row["reason"], "an unresolved row must say why"


def test_a_resolved_region_is_status_ok_and_carries_its_base(
        tmp_path: Path) -> None:
    row = _row(_memory(AEN_BOARD, tmp_path), "atoc")

    assert row["status"] == "ok"
    assert row["base"] == 0x80578000
    assert "reason" not in row


def test_size_is_emitted_even_when_the_base_is_unresolved(
        tmp_path: Path) -> None:
    """Size and base resolve independently: `mram_main`'s 5632 KiB is
    known even though its base is `"TBD"`."""
    row = _row(_memory(AEN_BOARD, tmp_path), "mram_main")

    assert row["size_bytes"] == 5632 * 1024


def test_size_bytes_is_derived_for_a_resolved_region(tmp_path: Path) -> None:
    assert _row(_memory(AEN_BOARD, tmp_path), "mcuboot")["size_bytes"] == 64 * 1024


# ---------------------------------------------------------------------
# write_authority -- passed through verbatim, never collapsed or defaulted
# ---------------------------------------------------------------------


def test_write_authority_is_passed_through_verbatim(tmp_path: Path) -> None:
    """All six som-preset-v1 values survive the projection unchanged --
    collapsing them onto a 3-value `owner` would merge `customer_image`
    with `customer_runtime` and lose the flash-time / runtime distinction
    the vocabulary exists to draw."""
    rows = _memory(AEN_BOARD, tmp_path)

    assert _row(rows, "mcuboot")["write_authority"] == "vendor_image"
    assert _row(rows, "he_slot0")["write_authority"] == "customer_image"
    assert _row(rows, "storage")["write_authority"] == "customer_runtime"
    assert _row(rows, "atoc")["write_authority"] == "secure_enclave"
    assert _row(rows, "reserved")["write_authority"] == "none"
    assert _row(rows, "mram_main")["write_authority"] == "composite"


def test_a_row_with_no_authored_write_authority_omits_the_key(
        tmp_path: Path) -> None:
    """Absent means unresolved, never `customer_runtime` (ADR-0034 clause
    4) -- a derived row carries no authority field at all rather than a
    defaulted one."""
    for row in _memory(V2N_BOARD, tmp_path):
        assert "write_authority" not in row


# ---------------------------------------------------------------------
# The pane is omitted, never emitted empty
# ---------------------------------------------------------------------


def test_the_memory_key_is_omitted_when_no_region_resolves(
        tmp_path: Path) -> None:
    """An absent `memory:` means "this producer does not emit it yet",
    so an empty list must never stand in for it -- a SoM whose
    silicon_variant cannot resolve emits no key at all."""
    project = load_board_yaml(_write_board(tmp_path, AEN_BOARD))
    project.som_preset.pop("memory_map", None)
    # No authored table AND no resolvable SoC to derive one from -- the
    # shape NX9101 is in today with `silicon_variant: TBD`.
    project.som_preset["silicon"] = "vendor:family:not-a-real-soc"
    project.som_preset.pop("silicon_variant", None)

    assert resolve_memory_regions(project) == []
    assert "memory" not in yaml.safe_load(emit_system_manifest(project))


# ---------------------------------------------------------------------
# The name join the schema promises
# ---------------------------------------------------------------------


def test_ipc_joins_memory_by_name() -> None:
    """`ipc[].carve_out_region` names a `memory[].name`.

    Uses the shipped `rpmsg-v2n` project deliberately: on every AEN SoM
    the carve-out is REFUSED (`mram_main` has an unresolved base), and a
    blocked `ipc[]` row carries no `carve_out_region` at all -- so an AEN
    fixture makes this test vacuous, which is exactly how its first
    version passed while asserting nothing. The explicit `checked` count
    is what stops it silently degrading that way again.
    """
    project = load_board_yaml(
        REPO / "examples" / "multicore" / "rpmsg-v2n" / "board.yaml")
    parsed = yaml.safe_load(emit_system_manifest(project))
    names = {r["name"] for r in parsed.get("memory", [])}

    checked = 0
    for link in parsed.get("ipc", []):
        region = link.get("carve_out_region")
        if region is None:
            continue
        checked += 1
        assert region in names, (
            f"ipc[] names carve_out_region {region!r}, absent from "
            f"memory[] {sorted(names)}")

    assert checked > 0, "fixture produced no resolved carve-out to join"


def test_the_storage_join_is_partial_by_construction(tmp_path: Path) -> None:
    """`storage[].flash_device` may name something that gets NO row.

    An `on_module.ospi_memories:` key is a legal target but is a
    controller-instance name carrying a `capacity_mbit` and no base, so
    it lies outside every aperture. Pinning the partiality is the honest
    test: no shipped project has a `flash_device` naming a `memory_map:`
    region, so asserting that join would assert nothing.
    """
    project = load_board_yaml(_write_board(tmp_path, AEN_BOARD))
    parsed = yaml.safe_load(emit_system_manifest(project))
    names = {r["name"] for r in parsed.get("memory", [])}
    ospi = set((project.som_preset.get("on_module") or {}).get("ospi_memories") or {})

    devices = {p.get("flash_device") for p in parsed.get("storage", [])}
    assert devices, "fixture produced no storage partitions"
    for device in devices:
        assert device in ospi, f"unexpected flash_device {device!r}"
        assert device not in names, (
            f"{device!r} is a controller instance and must get no memory[] row")


# ---------------------------------------------------------------------
# The schema types what the emitter writes
# ---------------------------------------------------------------------


def test_schema_declares_every_field_the_emitter_can_write(
        tmp_path: Path) -> None:
    """The prose contract stops being prose: every key the emitter
    produces is a declared property, and `additionalProperties: false`
    means a typo'd key fails the gate instead of shipping."""
    items = json.loads(SCHEMA.read_text(encoding="utf-8"))[
        "properties"]["memory"]["items"]

    assert items["additionalProperties"] is False
    assert sorted(items["required"]) == ["kind", "name", "source", "status"]

    declared = set(items["properties"])
    for row in _memory(AEN_BOARD, tmp_path) + _memory(V2N_BOARD, tmp_path):
        assert set(row) <= declared, f"{set(row) - declared} not declared"


# ---------------------------------------------------------------------
# Direct-call pins for the legs no shipped preset reaches today.
#
# `kind: ram` and `kind: unclassified` have ZERO producers across all 12
# `metadata/e1m_modules/E1M-*.yaml` presets (every AEN row that resolves
# is contained in the aperture; every non-Alif SoM has no aperture at
# all), and no preset authors a resolved base with an unresolvable size.
# Waiting for a preset to grow one of those rows is how a safety
# direction stays unpinned -- the same shape as #2022's mutants C3/C11.
# ---------------------------------------------------------------------

_AEN_APERTURE = (0x80000000, 0x80580000)


def test_a_preset_authored_region_outside_the_aperture_is_unclassified() -> None:
    """Containment is ONE-DIRECTIONAL: outside proves nothing. A future
    authored OSPI XIP window must not be reported as RAM."""
    row = memory._resolved_row(
        {"name": "ospi_xip", "base": 0x90000000, "size_kib": 64},
        _AEN_APERTURE, True, "som_preset")

    assert row["kind"] == "unclassified"


def test_a_derived_region_outside_the_aperture_is_ram() -> None:
    """The same extent, NOT preset-authored, is RAM by construction --
    this is the leg that keeps every V2N/V2M/NX9101 derivation intact."""
    row = memory._resolved_row(
        {"name": "sram0", "base": 0x90000000, "size_kib": 64},
        _AEN_APERTURE, False, "soc_derived")

    assert row["kind"] == "ram"


def test_a_resolved_base_with_an_unresolvable_size_stays_status_ok() -> None:
    """`status` keys off the BASE, not off `region_extent()`.

    The address resolved; only the size did not. An extent-keyed rule
    would report `status: unresolved` for a row whose address is known,
    and no shipped preset has this shape to catch the difference.
    """
    row = memory._resolved_row(
        {"name": "half_known", "base": 0x80600000, "size_kib": "TBD"},
        _AEN_APERTURE, True, "som_preset")

    assert row["status"] == "ok"
    assert row["base"] == 0x80600000
    assert "size_bytes" not in row
    assert "reason" not in row


def test_an_unresolved_row_never_carries_a_base() -> None:
    """ADR-0034 clause 4, at the row level."""
    row = memory._resolved_row(
        {"name": "pending", "base": "TBD", "size_kib": 64},
        _AEN_APERTURE, True, "som_preset")

    assert row["status"] == "unresolved"
    assert "base" not in row
    assert row["reason"]


# ---------------------------------------------------------------------
# The schema enforces the row invariant, rather than describing it
# ---------------------------------------------------------------------


def _item_validator():
    import jsonschema
    items = json.loads(SCHEMA.read_text(encoding="utf-8"))[
        "properties"]["memory"]["items"]
    return jsonschema.Draft202012Validator(items)


@pytest.mark.parametrize("row", [
    {"name": "a", "source": "som_preset", "kind": "flash", "status": "ok"},
    {"name": "a", "source": "som_preset", "kind": "unresolved",
     "status": "unresolved"},
    {"name": "a", "source": "som_preset", "kind": "unresolved",
     "status": "unresolved", "reason": "why", "base": 1},
])
def test_the_schema_rejects_a_row_that_breaks_the_status_invariant(row) -> None:
    """`status: ok` with no `base`, `unresolved` with no `reason`, and
    `unresolved` carrying a `base` were all schema-legal while the rule
    lived only in the items description -- which is the failure mode this
    pane was typed to end."""
    assert list(_item_validator().iter_errors(row)), f"{row} should be refused"


@pytest.mark.parametrize("row", [
    {"name": "a", "source": "som_preset", "kind": "flash", "status": "ok",
     "base": 0x80000000},
    {"name": "a", "source": "soc_derived", "kind": "unresolved",
     "status": "unresolved", "reason": "no base declared"},
])
def test_the_schema_accepts_both_legal_row_shapes(row) -> None:
    assert not list(_item_validator().iter_errors(row))


def test_schema_pins_the_shipped_vocabularies(tmp_path: Path) -> None:
    """The enums are the ones the code actually produces, not the ones
    #1365's issue body proposed before split A/B shipped."""
    props = json.loads(SCHEMA.read_text(encoding="utf-8"))[
        "properties"]["memory"]["items"]["properties"]

    assert props["kind"]["enum"] == ["flash", "ram", "unclassified", "unresolved"]
    assert props["status"]["enum"] == ["ok", "unresolved"]
    assert props["source"]["enum"] == ["som_preset", "soc_derived"]
    assert props["write_authority"]["enum"] == [
        "customer_image", "vendor_image", "customer_runtime",
        "secure_enclave", "none", "composite",
    ]
