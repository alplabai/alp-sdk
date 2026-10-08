# SPDX-License-Identifier: Apache-2.0
"""Which core owns a board.yaml `cameras:` entry -- the ONE ownership rule.

A dependency-free leaf (like `slugs.py`): plain dicts in, plain data out, so
the CLI validator (`alp_cli/validator.py`, lazy import) and the build planner
(`cameras.py`) cannot disagree.

A camera is owned by exactly one core, and ONLY that core's build gets the
camera (`-DSHIELD` for Zephyr, `ALP_CAMERA_CAM<n>` for Yocto).

  * `cameras[].core` names the owner explicitly; it must be a candidate.
  * Otherwise the candidates are the Zephyr cores running a customer app (not
    `alp-stock-shim`) plus the Yocto cores.  Exactly one -> it owns the
    camera; none or several is an error (several: set `cameras[].core`).

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


def camera_owner(cores: dict[str, dict[str, Any]],
                 explicit: Optional[str]) -> tuple[Optional[str], Optional[str]]:
    """(owner core id, None) or (None, error message)."""
    cands = candidates(cores)
    if explicit is not None:
        if explicit in cands:
            return explicit, None
        return None, (f"cameras: core '{explicit}' is not a Zephyr core running a "
                      f"customer app or a Yocto core of this project "
                      f"(candidates: {', '.join(cands) or 'none'})")
    if len(cands) == 1:
        return cands[0], None
    if not cands:
        return None, ("cameras: no core can own the camera (needs a Zephyr core "
                      "running a customer app, or a Yocto core)")
    return None, (f"cameras: ambiguous camera owner ({', '.join(cands)}): "
                  f"set `cameras[].core`")


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
        owner, err = camera_owner(cores, entry.get("core"))
        plan = CameraPlan(conn, mod, owner, [err] if err else [])
        plans.append(plan)
        if owner is None or cores[owner]["os"] != "zephyr":
            continue
        shield = (modules.get(mod) or {}).get("zephyr_shield")
        carrier = (connectors.get(conn) or {}).get("zephyr_shields") or []
        if mod in modules and not shield:
            plan.errors.append(
                f"cameras: module '{mod}' on {conn} has no `zephyr_shield:` in "
                f"metadata/camera_modules/{mod}.yaml -- it cannot be selected on "
                f"Zephyr core '{owner}'; use a module with one, or give the "
                f"camera to a Yocto core with `cameras[].core`")
        if conn in connectors and not carrier:
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
