#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Carrier route / netlist / BOM emission
(`--emit composed-route-table` and `--emit carrier-netlist`).

Split out of the former flat `alp_project_emit.py` module (issue #673
Phase 1) -- see `scripts/alp_project_emit/__init__.py` for the package-
level contract.  Structural split only, no behaviour change;
`check_emit_snapshots.py` pins the byte-identical output.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from alp_project_loader import (
    _compose_route,
    _hwrev_pad_route_overrides,
    _load_yaml,
    _resolve_pad_routes,
    _resolve_silicon_variant,
)


# ---------------------------------------------------------------------
# Carrier route / netlist emitters
# ---------------------------------------------------------------------
#
# `composed-route-table` stays as the original debug surface.  The
# route-row helper below is shared by the production `carrier-netlist`
# contract so the two views cannot drift on hw_rev pad-route overrides.


def _composed_route_rows(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> tuple[list[dict[str, Any]], str | None, str | None]:
    """Return composed route rows plus the selected hw_rev / variant.

    Rows cover every E1M pad named by the board and every SoM-only pad
    that has a dispatch route.  Board-defined rows preserve YAML order;
    SoM-only rows are sorted by E1M ID for deterministic output.
    """
    pad_routes = _resolve_pad_routes(sku_preset)

    # Apply the selected board revision's pad-route overrides on top of the
    # base (production-rev) pad_routes, so the composed table -- and thus
    # `--emit composed-route-table` -- differs by hw_rev.  The rev comes from
    # the board's `som.hw_rev`, falling back to the SoM's `default_hw_rev`.
    hw_rev = ((project.get("som") or {}).get("hw_rev")
              or sku_preset.get("default_hw_rev"))
    for ov in _hwrev_pad_route_overrides(project["som"]["sku"], hw_rev,
                                         metadata_root):
        pad_routes[ov["e1m"]] = ov

    # Resolve silicon variant order_code for the top-level summary field.
    variant = _resolve_silicon_variant(sku_preset, metadata_root)
    silicon_variant_str = variant["order_code"] if variant else None

    # Collect board-side entries, preserving the sub-category name.
    # Build a mapping: e1m_id -> (category, entry_dict).
    # When the same E1M pad appears multiple times (e.g. E1M_PWM1 maps to
    # both EVK_PWM_LED_BLUE and EVK_ARD_PWM1 in the EVK YAML) we emit one
    # row per board entry so no information is lost.
    board_entries: list[tuple[str, dict[str, Any]]] = []
    seen_from_board: set[str] = set()
    if board_preset is not None:
        e1m_routes = board_preset.get("e1m_routes") or {}
        for category, entries in e1m_routes.items():
            if not isinstance(entries, list):
                continue
            for entry in entries:
                if not isinstance(entry, dict):
                    continue
                e1m = entry.get("e1m")
                if not isinstance(e1m, str):
                    continue
                board_entries.append((category, entry))
                seen_from_board.add(e1m)

    # Also include SoM-only pads (in pad_routes but not in board).
    som_only_pads = sorted(set(pad_routes.keys()) - seen_from_board)

    routes: list[dict[str, Any]] = []

    # Board-defined entries first (preserves YAML order).
    for category, c_entry in board_entries:
        e1m = c_entry["e1m"]
        composed = _compose_route(e1m, c_entry, pad_routes)
        row: dict[str, Any] = {"e1m": e1m, "board_category": category}
        row["board_macro"] = composed.get("board_macro")
        row["board_role"] = composed.get("board_role")
        if "board_doc" in composed:
            row["board_doc"] = composed["board_doc"]
        # active_low is a board-side flag, not surfaced by _compose_route;
        # read it directly from the board entry.
        active_low = c_entry.get("active_low")
        if active_low is not None:
            row["active_low"] = bool(active_low)
        row["dispatch"] = composed.get("dispatch", "direct")
        if "dispatch_pin" in composed:
            row["dispatch_pin"] = composed["dispatch_pin"]
        if "som_doc" in composed:
            row["som_doc"] = composed["som_doc"]
        routes.append(row)

    # SoM-only pads (not assigned a board role in this board YAML).
    for e1m in som_only_pads:
        composed = _compose_route(e1m, None, pad_routes)
        row = {
            "e1m": e1m,
            "board_category": None,
            "board_macro": None,
            "board_role": None,
            "dispatch": composed.get("dispatch", "direct"),
        }
        if "dispatch_pin" in composed:
            row["dispatch_pin"] = composed["dispatch_pin"]
        if "som_doc" in composed:
            row["som_doc"] = composed["som_doc"]
        routes.append(row)

    return routes, hw_rev, silicon_variant_str


def _emit_composed_route_table(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> str:
    """Emit a JSON summary of the fully-composed pad route table for
    the current (board x SoM) pair.

    The table is derived by calling _resolve_pad_routes() (SoM side) and
    _compose_route() (join with board side) for every E1M pad that
    appears in either the board's e1m_routes: block or the SoM's
    pad_routes: block.

    Pads that only appear in the SoM's pad_routes: block (i.e. no
    board-side role assigned) are included with null board_* fields
    so the table is complete for the SoM-standalone scenario.
    """
    routes, hw_rev, silicon_variant_str = _composed_route_rows(
        project, sku_preset, board_preset, metadata_root)
    board_name = (board_preset or {}).get("name") or project.get("name")
    result: dict[str, Any] = {
        "board": board_name,
        "som": project["som"]["sku"],
        "hw_rev": hw_rev,
        "silicon_variant": silicon_variant_str,
        "routes": routes,
    }
    return json.dumps(result, indent=2) + "\n"


def _manifest_path(kind: str, item_id: str, metadata_root: Path) -> Path:
    return metadata_root / kind / f"{item_id}.yaml"


def _load_optional_manifest(kind: str, item_id: str,
                            metadata_root: Path) -> dict[str, Any] | None:
    path = _manifest_path(kind, item_id, metadata_root)
    if not path.is_file():
        return None
    return _load_yaml(path)


def _passive_rows(passives: list[dict[str, Any]] | None) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for passive in passives or []:
        if not isinstance(passive, dict):
            continue
        row: dict[str, Any] = {
            "role": passive.get("role"),
            "value": passive.get("value"),
            "net": passive.get("net"),
            "refdes_prefix": passive.get("refdes_prefix"),
        }
        rows.append({k: v for k, v in row.items() if v is not None})
    return rows


def _chip_bom_row(item_id: str, manifest: dict[str, Any],
                  manifest_relpath: str) -> dict[str, Any]:
    physical = manifest.get("physical") or {}
    caveats: list[str] = []
    if not physical:
        caveats.append("missing_physical")
    elif physical.get("visibility") == "internal":
        caveats.append("physical_detail_internal")
    if len(manifest.get("mpn_population") or []) > 1:
        caveats.append("mpn_population_candidates")

    row: dict[str, Any] = {
        "item_id": item_id,
        "kind": "chip",
        "scope": "carrier",
        "source": manifest_relpath,
        "display_name": manifest.get("display_name"),
        "vendor": manifest.get("vendor"),
        "mpn_population": manifest.get("mpn_population") or [],
        "bus": manifest.get("bus"),
        "quantity": None,
        "physical": {
            "refdes_prefix": physical.get("refdes_prefix"),
            "package": physical.get("package"),
            "footprint": physical.get("footprint"),
            "visibility": physical.get("visibility"),
            "provenance": physical.get("provenance"),
        },
        "passives": _passive_rows(physical.get("passives")),
    }
    row["physical"] = {k: v for k, v in row["physical"].items()
                       if v is not None}
    if caveats:
        row["caveats"] = caveats
    return row


def _block_bom_row(item_id: str, manifest: dict[str, Any],
                   manifest_relpath: str) -> dict[str, Any]:
    realizations = [
        r for r in manifest.get("realizations") or []
        if isinstance(r, dict)
    ]
    realization = realizations[0] if realizations else {}
    caveats: list[str] = []
    if not realizations:
        caveats.append("missing_realization")
    elif len(realizations) > 1:
        caveats.append("multiple_realizations")
    if realization.get("visibility") == "internal":
        caveats.append("physical_detail_internal")
    if not realization.get("parts"):
        caveats.append("no_concrete_parts")

    row: dict[str, Any] = {
        "item_id": item_id,
        "kind": "block",
        "scope": "carrier",
        "source": manifest_relpath,
        "display_name": manifest.get("display_name"),
        "quantity": None,
        "realization": {
            "id": realization.get("id"),
            "physical_form": realization.get("physical_form"),
            "visibility": realization.get("visibility"),
        },
        "parts": realization.get("parts") or [],
        "passives": _passive_rows(realization.get("passives")),
    }
    row["realization"] = {k: v for k, v in row["realization"].items()
                          if v is not None}
    if caveats:
        row["caveats"] = caveats
    return row


def _carrier_bom_rows(
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> list[dict[str, Any]]:
    """Build carrier BOM rows from board `populated: true`.

    `populated:` is a logical population map, not a line-item BOM with
    refdes or count, so rows deliberately leave `quantity` null unless a
    future metadata field makes it authoritative.
    """
    rows: list[dict[str, Any]] = []
    if board_preset is None:
        return rows

    populated = board_preset.get("populated") or {}
    for item_id in sorted(k for k, v in populated.items() if v is True):
        chip = _load_optional_manifest("chips", item_id, metadata_root)
        if chip is not None:
            rows.append(_chip_bom_row(
                item_id, chip, f"metadata/chips/{item_id}.yaml"))
            continue

        block = _load_optional_manifest("blocks", item_id, metadata_root)
        if block is not None:
            rows.append(_block_bom_row(
                item_id, block, f"metadata/blocks/{item_id}.yaml"))
            continue

        rows.append({
            "item_id": item_id,
            "kind": "unknown",
            "scope": "carrier",
            "source": None,
            "quantity": None,
            "caveats": ["missing_manifest"],
        })
    return rows


def _route_to_net(row: dict[str, Any]) -> dict[str, Any]:
    net_id = row.get("board_macro") or row["e1m"]
    endpoints: list[dict[str, Any]] = [
        {"kind": "e1m", "ref": row["e1m"]},
    ]
    if row.get("board_macro"):
        endpoints.append({"kind": "board-macro", "ref": row["board_macro"]})
    if row.get("dispatch") and row["dispatch"] != "direct":
        endpoint: dict[str, Any] = {
            "kind": "som-dispatch",
            "ref": row["dispatch"],
        }
        if "dispatch_pin" in row:
            endpoint["pin"] = row["dispatch_pin"]
        endpoints.append(endpoint)

    net: dict[str, Any] = {
        "net_id": net_id,
        "e1m": row["e1m"],
        "board_category": row.get("board_category"),
        "board_macro": row.get("board_macro"),
        "board_role": row.get("board_role"),
        "dispatch": row.get("dispatch", "direct"),
        "endpoints": endpoints,
    }
    for key in ("board_doc", "active_low", "dispatch_pin", "som_doc"):
        if key in row:
            net[key] = row[key]
    caveats = []
    if row.get("board_macro") is None:
        caveats.append("som_only_no_carrier_role")
    if caveats:
        net["caveats"] = caveats
    return net


def _emit_carrier_netlist(
    project: dict[str, Any],
    sku_preset: dict[str, Any],
    board_preset: dict[str, Any] | None,
    metadata_root: Path,
) -> str:
    """Emit the Studio-facing carrier netlist + BOM handoff contract.

    This is intentionally not a KiCad, Gerber, or layout artifact.  It
    exposes only public carrier-facing facts derivable from board.yaml,
    board presets, chip/block manifests, and SoM pad dispatch metadata.
    """
    routes, hw_rev, silicon_variant_str = _composed_route_rows(
        project, sku_preset, board_preset, metadata_root)
    board_name = (board_preset or {}).get("name") or project.get("name")
    result: dict[str, Any] = {
        "schema_version": 1,
        "kind": "alp.carrier_netlist",
        "generated_by": "scripts/alp_project.py --emit carrier-netlist",
        "board": board_name,
        "som": project["som"]["sku"],
        "hw_rev": hw_rev,
        "silicon_variant": silicon_variant_str,
        "nets": [_route_to_net(row) for row in routes],
        "bom": {
            "carrier": _carrier_bom_rows(board_preset, metadata_root),
        },
        "caveats": [
            "carrier_handoff_not_pcb_layout",
            "no_kicad_or_gerber_output",
            "som_internals_excluded",
            "quantity_null_when_board_populated_has_no_count",
        ],
    }
    return json.dumps(result, indent=2) + "\n"
