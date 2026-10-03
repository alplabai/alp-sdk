#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Per-product core ownership: the ONE resolver.

`metadata/e1m_modules/<family>/core-ownership.yaml` keeps FIXED facts under
`core_ownership:` and per-product choices under `assignable:` (keyed by E1M
instance, each with `candidates`, a `default`, and references to
metadata/pinmux rows).  A board.yaml `ownership: {<instance>: <core>}` block
overrides a default; `resolve_ownership` rejects an unknown instance or a core
outside `candidates`.  The resolved map is what `--emit system-manifest`
projects as `ownership:`.  Fixed rows are never touched by an override.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Optional

import yaml

from .models import OrchestratorError

# Core-ownership tokens -> the SoC-JSON `cores[].type` they name.
CORE_TOKEN_TYPES = {"a55": "cortex-a55", "m33": "cortex-m33"}


def load_ownership_doc(metadata_root: Path, family_dir: Optional[str]) -> Optional[dict]:
    """The family's core-ownership.yaml, or None.  The V2M family
    (`v2n-m1`) shares the V2N file."""
    if not family_dir:
        return None
    for fam in (family_dir, "v2n" if family_dir.startswith("v2n") else None):
        if fam is None:
            continue
        p = metadata_root / "e1m_modules" / fam / "core-ownership.yaml"
        if p.is_file():
            return yaml.safe_load(p.read_text(encoding="utf-8"))
    return None


def resolve_ownership(doc: Optional[dict],
                      overrides: Optional[dict[str, str]] = None,
                      declared_core_types: Optional[set[str]] = None) -> dict[str, str]:
    """{instance: core} = assignable defaults + validated overrides.

    Fixed `core_ownership` rows are not in the result and cannot be
    overridden (they are not instances).  `declared_core_types` (SoC core
    types of the project's `cores:` keys; None = skip) rejects an override
    naming a core the project does not declare.  Raises OrchestratorError.
    """
    assignable = (doc or {}).get("assignable") or {}
    out = {inst: e["default"] for inst, e in assignable.items()}
    for inst, core in (overrides or {}).items():
        if inst not in assignable:
            raise OrchestratorError(
                f"board.yaml ownership: unknown instance {inst!r}; "
                f"assignable instances: {sorted(assignable) or 'none for this SoM'}")
        cands = assignable[inst]["candidates"]
        if core not in cands:
            raise OrchestratorError(
                f"board.yaml ownership: {inst} cannot be owned by {core!r}; "
                f"allowed cores: {cands}")
        if (declared_core_types is not None
                and CORE_TOKEN_TYPES[core] not in declared_core_types):
            raise OrchestratorError(
                f"board.yaml ownership: {inst} is assigned to {core!r} but "
                f"board.yaml `cores:` does not declare a {CORE_TOKEN_TYPES[core]} core")
        out[inst] = core
    return out


def pad_pfc(pad: str, func: int) -> tuple[str, int, int]:
    """("P50", 1) -> ("PORT_05", 0, 1): the RZ/V2N PFC (port, pin, func) of a
    `P<port><pin>` pad.  The function code is a silicon fact from the SoC
    JSON `linux_dt[soc_instance].pinmux`; only the pad spelling is parsed."""
    if len(pad) != 3 or pad[0] != "P" or not pad[2].isdigit():
        raise OrchestratorError(f"pad {pad!r} is not a P<port><pin> pad")
    return f"PORT_0{pad[1]}", int(pad[2]), func


def instance_pfc(soc: dict, entry: dict) -> list[tuple[dict, Optional[tuple[str, int, int]]]]:
    """[(row, (port, pin, func) | None)] for an assignable entry; None where
    the SoC `linux_dt` carries no function code for the row (never guessed)."""
    pinmux = ((soc.get("linux_dt") or {}).get(entry.get("soc_instance")) or {}).get("pinmux") or {}
    return [(r, pad_pfc(r["pad"], pinmux[r["peripheral"]]) if r["peripheral"] in pinmux else None)
            for r in entry["rows"]]


def validate_assignable(doc: dict, pinmux_pairs: set[tuple[str, str]],
                        soc_core_types: set[str],
                        linux_dt: Optional[dict] = None) -> list[str]:
    """Metadata cross-checks: rows exist in pinmux, are not also FIXED rows,
    default is a candidate, candidates exist on the SoM, `soc_instance`
    resolves in the SoC `linux_dt` (when given), an `m33:` block has a PFC
    function for every row."""
    msgs: list[str] = []
    fixed = {(r["peripheral"], r["pad"]) for r in doc.get("core_ownership") or []}
    seen: dict[tuple[str, str], str] = {}
    for inst, e in (doc.get("assignable") or {}).items():
        if e["default"] not in e["candidates"]:
            msgs.append(f"assignable.{inst}: default {e['default']!r} not in candidates {e['candidates']}")
        for c in e["candidates"]:
            if CORE_TOKEN_TYPES.get(c) not in soc_core_types:
                msgs.append(f"assignable.{inst}: candidate {c!r} is not a core of this SoM")
        for r in e["rows"]:
            k = (r["peripheral"], r["pad"])
            if k not in pinmux_pairs:
                msgs.append(f"assignable.{inst}: row {k} matches no owner=renesas row in metadata/pinmux")
            if k in fixed:
                msgs.append(f"assignable.{inst}: row {k} is also a FIXED core_ownership row")
            if k in seen:
                msgs.append(f"assignable.{inst}: row {k} already assigned to {seen[k]}")
            seen[k] = inst
        if linux_dt is not None:
            if e.get("soc_instance") not in linux_dt:
                msgs.append(f"assignable.{inst}: soc_instance {e.get('soc_instance')!r} is not a key of the SoC linux_dt")
            elif "m33" in e:
                missing = [r["peripheral"] for r, p in instance_pfc({"linux_dt": linux_dt}, e) if p is None]
                if missing:
                    msgs.append(f"assignable.{inst}: has an m33: block but linux_dt.{e['soc_instance']}.pinmux has no function for {missing}")
    return msgs


def m33_overlay(doc: Optional[dict], ownership: dict[str, str]) -> tuple[list[str], list[str]]:
    """(dts lines, Kconfig lines) enabling every assignable instance this
    project assigned to `m33`.  The board tree (gen_zephyr_board.py) carries
    those nodes `disabled`; only an owning project turns them on.  An
    m33-owned instance with no `m33:` devicetree block is an error, not a
    silent no-op."""
    assignable = (doc or {}).get("assignable") or {}
    dts: list[str] = []
    kconfig: list[str] = []
    for inst, core in sorted(ownership.items()):
        if core != "m33":
            continue
        blocked = assignable[inst].get("hw_blocked")
        if blocked:
            raise OrchestratorError(
                f"board.yaml ownership: {inst} is hardware-blocked on every core: {blocked['reason']}")
        b = assignable[inst].get("m33")
        if not b:
            raise OrchestratorError(
                f"board.yaml ownership: {inst} is assigned to 'm33' but core-ownership.yaml "
                f"carries no `m33:` devicetree block for it (the CM33 node, pinctrl and PFC "
                f"data are not in metadata yet)")
        dts += [f"&{b['dt_label']} {{", '	status = "okay";', "};",
                "/ {", "	aliases {", f"		{b['alias']} = &{b['dt_label']};", "	};", "};"]
        kconfig += b.get("kconfig") or []
    return dts, kconfig


def project_m33_overlay(project, core_id: Optional[str]) -> tuple[list[str], list[str]]:
    """`m33_overlay` for a loaded project, scoped to a Cortex-M33 core id
    (`core_id` None = any M33 core of the project)."""
    from .loader import _sku_family_dir
    if not project.ownership or not (core_id or "m33").startswith("m33"):
        return [], []
    doc = load_ownership_doc(project.effective_metadata_root(), _sku_family_dir(project.sku))
    return m33_overlay(doc, project.ownership)
