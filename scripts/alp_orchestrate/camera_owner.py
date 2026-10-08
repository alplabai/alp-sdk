# SPDX-License-Identifier: Apache-2.0
"""Which core owns a board.yaml `cameras:` entry -- the ONE ownership rule.

A dependency-free leaf (like `slugs.py`): plain dicts in, plain data out, so
the CLI validator (`alp_cli/validator.py`, lazy import) and the build planner
(`cameras.py`) cannot disagree.

A camera is owned by exactly one core, and ONLY that core's build gets the
camera (`-DSHIELD` for Zephyr, `ALP_CAMERA_CAM<n>` for Yocto).

  * `cameras[].core` names the owner explicitly; it must be a candidate.
  * Otherwise the candidates are the cores whose OS the camera's CONNECTOR
    supports: a Zephyr core running a customer app (not `alp-stock-shim`)
    needs the connector's `zephyr_shields` and the module's `zephyr_shield`; a
    Yocto core needs the connector's `linux: true`.  Exactly one -> it owns
    the camera; none or several is an error (several: set `cameras[].core`).

`cores` maps core id -> {"os": ..., "app": ..., "board": ...}, already merged
from the board.yaml `cores:` over the SoM preset's `topology:`.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

#: The SDK's own M-core shim (firmware/alp-stock-shim): never a camera owner.
STOCK_SHIM_APP = "alp-stock-shim"


@dataclass
class CameraPlan:
    connector: str
    module: str
    owner: Optional[str]
    errors: list[str] = field(default_factory=list)
    #: With no owner: the cores whose build this camera's errors block -- the
    #: ones that could own it (usable, or of an OS the connector supports),
    #: never an unrelated core (e.g. the AEN A32 for a Zephyr-only connector).
    blocks: list[str] = field(default_factory=list)


def resolve_cores(project_cores: Any, topology: Any) -> dict[str, dict[str, Any]]:
    """Merge board.yaml `cores:` over the SoM `topology:` into
    {id: {os, app, board}}.  `os` falls back the way the loader's class rule
    does for the shipped SoMs: a Zephyr `board:` -> zephyr, a Yocto
    `machine:` -> yocto, else off."""
    topo = topology if isinstance(topology, dict) else {}
    proj = project_cores if isinstance(project_cores, dict) else {}
    out: dict[str, dict[str, Any]] = {}
    for cid, base in topo.items():
        entry = dict(base) if isinstance(base, dict) else {}
        if isinstance(proj.get(cid), dict):
            entry.update(proj[cid])
        os_ = entry.get("os") or (
            "zephyr" if entry.get("board")
            else "yocto" if entry.get("machine") else "off")
        out[cid] = {"os": os_, "app": entry.get("app"), "board": entry.get("board")}
    return out


def candidates(cores: dict[str, dict[str, Any]]) -> list[str]:
    return [cid for cid, c in cores.items()
            if c["os"] == "yocto"
            or (c["os"] == "zephyr" and c.get("app")
                and c["app"] != STOCK_SHIM_APP)]


def _usable(core: dict[str, Any], connector: dict[str, Any],
            module_doc: Optional[dict[str, Any]]) -> bool:
    """Can a core of this OS drive a camera of this module on this connector?"""
    if core["os"] == "yocto":
        return bool(connector.get("linux"))
    return bool(connector.get("zephyr_shields")) and (
        module_doc is None or bool(module_doc.get("zephyr_shield")))


def camera_owner(cores: dict[str, dict[str, Any]], explicit: Optional[str],
                 conn: str, connector: dict[str, Any], module: str,
                 module_doc: Optional[dict[str, Any]]
                 ) -> tuple[Optional[str], Optional[str]]:
    """(owner core id, None) or (None, error message).

    An explicit `core:` must be a structural candidate (the OS-specific
    problems are reported by `plan_cameras`).  Otherwise the candidates are
    the structural ones whose OS the connector (and, for Zephyr, the module)
    supports: one -> owner; none or several -> error."""
    cands = candidates(cores)
    if explicit is not None:
        if explicit in cands:
            return explicit, None
        return None, (f"cameras: core '{explicit}' is not a Zephyr core running a "
                      f"customer app or a Yocto core of this project "
                      f"(candidates: {', '.join(cands) or 'none'})")
    usable = [c for c in cands if _usable(cores[c], connector, module_doc)]
    if len(usable) == 1:
        return usable[0], None
    if usable:
        return None, (f"cameras: ambiguous camera owner for {conn} "
                      f"({', '.join(usable)}): set `cameras[].core`")
    supports = [n for n, ok in (("Zephyr", connector.get("zephyr_shields")),
                                ("Linux (Yocto)", connector.get("linux"))) if ok]
    if not supports:
        return None, (f"cameras: connector {conn} declares neither "
                      f"`zephyr_shields:` nor `linux: true`, so no core can own "
                      f"its camera")
    why = ""
    if module_doc is not None and not module_doc.get("zephyr_shield"):
        why = (f"; module '{module}' has no `zephyr_shield:` in "
               f"metadata/camera_modules/{module}.yaml, so Zephyr cannot use it")
    return None, (f"cameras: no core can own the camera on {conn}: the connector "
                  f"supports {' and '.join(supports)} only, and the project has "
                  f"no matching core (Zephyr customer app / Yocto){why}")


def has_overlay(shield: str, board: str, repo: Path) -> bool:
    """Zephyr applies `boards/<board>_<qualifiers>.overlay`, else `<board>.overlay`."""
    name, *quals = board.split()[0].split("/")
    d = repo / "zephyr" / "boards" / "shields" / shield / "boards"
    return any((d / f"{n}.overlay").is_file()
               for n in ("_".join([name, *quals]), name))


def plan_cameras(cameras: Any, connectors: Any, cores: dict[str, dict[str, Any]],
                 modules: dict[str, Any], repo: Path) -> list[CameraPlan]:
    """One CameraPlan per `cameras:` entry: its owner and every static
    problem (empty `errors` = buildable).  `modules` maps module id -> its
    camera-module YAML (absent id: reported elsewhere, not here)."""
    connectors = connectors if isinstance(connectors, dict) else {}
    plans: list[CameraPlan] = []
    seen: dict[tuple[str, str], str] = {}  # (owner, module shield) -> connector
    for entry in cameras if isinstance(cameras, list) else []:
        if not isinstance(entry, dict):
            continue
        conn, mod = entry.get("connector"), entry.get("module")
        if not isinstance(conn, str) or not isinstance(mod, str):
            continue
        if conn not in connectors:  # reported by the validator / planner
            plans.append(CameraPlan(conn, mod, None))
            continue
        connector = connectors[conn] if isinstance(connectors[conn], dict) else {}
        owner, err = camera_owner(cores, entry.get("core"), conn, connector,
                                  mod, modules.get(mod))
        plan = CameraPlan(conn, mod, owner, [err] if err else [])
        plans.append(plan)
        if owner is None:
            cands = candidates(cores)
            usable = [c for c in cands
                      if _usable(cores[c], connector, modules.get(mod))]
            by_os = [c for c in cands
                     if connector.get("linux" if cores[c]["os"] == "yocto"
                                      else "zephyr_shields")]
            plan.blocks = usable if len(usable) > 1 else (by_os or cands)
        if owner is not None and cores[owner]["os"] == "yocto"                 and not connector.get("linux"):
            plan.errors.append(
                f"cameras: connector {conn} does not declare Linux support "
                f"(`linux: true`), so Yocto core '{owner}' cannot own its camera")
        if owner is None or cores[owner]["os"] != "zephyr":
            continue
        shield = (modules.get(mod) or {}).get("zephyr_shield")
        carrier = connector.get("zephyr_shields") or []
        if mod in modules and not shield:
            plan.errors.append(
                f"cameras: module '{mod}' on {conn} has no `zephyr_shield:` in "
                f"metadata/camera_modules/{mod}.yaml -- it cannot be selected on "
                f"Zephyr core '{owner}'; use a module with one, or give the "
                f"camera to a Yocto core with `cameras[].core`")
        if not carrier:
            plan.errors.append(
                f"cameras: connector {conn} declares no `zephyr_shields:` -- no "
                f"Zephyr carrier shield exists for it, so Zephyr core '{owner}' "
                f"cannot own its camera")
        board = cores[owner].get("board")
        if board:
            for sh in carrier:
                if not has_overlay(sh, board, repo):
                    plan.errors.append(
                        f"cameras: no Zephyr shield overlay for board target "
                        f"'{board}' (core '{owner}'): shield '{sh}' has no "
                        f"zephyr/boards/shields/{sh}/boards/<board>.overlay for "
                        f"it -- add one for that SoM target")
        if shield:
            # A module shield instantiates ONE camera node; two connectors
            # naming the same module can't both be expressed in one -DSHIELD.
            if (owner, shield) in seen:
                plan.errors.append(
                    f"cameras: module shield '{shield}' is used by both "
                    f"{seen[(owner, shield)]} and {conn} on core '{owner}'; a "
                    f"Zephyr shield is a single instance")
            seen.setdefault((owner, shield), conn)
    return plans
