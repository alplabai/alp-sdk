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

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, _write_board   # noqa: E402

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


def test_ipc_and_storage_join_memory_by_name(tmp_path: Path) -> None:
    """`ipc[].carve_out_region` and `storage[].flash_device` each name a
    `memory[].name` -- partial by construction, since an
    `on_module.ospi_memories:` key is a legal `flash_device` target that
    carries a `capacity_mbit` and no base, so it gets no row."""
    project = load_board_yaml(_write_board(tmp_path, AEN_BOARD))
    parsed = yaml.safe_load(emit_system_manifest(project))
    names = {r["name"] for r in parsed.get("memory", [])}
    ospi = set((project.som_preset.get("on_module") or {}).get("ospi_memories") or {})

    for link in parsed.get("ipc", []):
        region = link.get("carve_out_region")
        if region is not None:
            assert region in names, f"ipc[] names {region}, absent from memory[]"
    for part in parsed.get("storage", []):
        device = part.get("flash_device")
        if device is not None and device not in ospi:
            assert device in names, f"storage[] names {device}, absent from memory[]"


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
