# SPDX-License-Identifier: Apache-2.0
"""Resolve a project's `cameras: [{connector, module}]` into build inputs.

One resolver for both OS lanes, so the same board.yaml line picks the camera
on every SoM:

  * Zephyr slice -> ONE `-DSHIELD=<carrier shields> <module shield>` define
    (carrier half from the board's `camera_connectors.<CAMn>.zephyr_shields`,
    camera half from `metadata/camera_modules/<module>.yaml: zephyr_shield`).
  * Yocto slice  -> `ALP_CAMERA_CAM<n> = "<module_id>"` in local.conf, the
    variable the kernel bbappend keys the sensor devicetree include on.

Connector `CAMn` is camera index `n` (`alp-camera<n>` in Zephyr DT, the `n` in
`ALP_CAMERA_CAM<n>`).
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

import yaml

from .models import BoardProject, OrchestratorError, Slice
from .paths import REPO

# firmware/alp-stock-shim: the SDK's own M-core shim, never a camera owner.
# (Same literal as orchestrator.STOCK_SHIM_APP; not imported -- cycle.)
_STOCK_SHIM_APP = "alp-stock-shim"


class CameraSelectError(OrchestratorError):
    """A `cameras:` entry cannot be turned into a build input for a slice."""


def _connectors(project: BoardProject) -> dict:
    src = project.board_preset if project.board_preset else project.raw
    return (src or {}).get("camera_connectors") or {}


def _module(project: BoardProject, module_id: str) -> dict:
    p = project.effective_metadata_root() / "camera_modules" / f"{module_id}.yaml"
    try:
        return yaml.safe_load(p.read_text(encoding="utf-8")) or {}
    except OSError as e:
        raise CameraSelectError(f"cameras: unknown module '{module_id}' ({p})") from e


def _cameras(project: BoardProject) -> list[tuple[str, str]]:
    """(connector, module_id) pairs in CAM-index order."""
    pairs = [(c["connector"], c["module"])
             for c in project.raw.get("cameras") or []]
    return sorted(pairs, key=lambda p: int(p[0][3:]))


def _has_overlay(shield: str, board: str, repo: Path) -> bool:
    """Zephyr applies `boards/<board>_<qualifiers>.overlay`, else `<board>.overlay`."""
    name, *quals = board.split()[0].split("/")
    d = repo / "zephyr" / "boards" / "shields" / shield / "boards"
    return any((d / f"{n}.overlay").is_file()
               for n in ("_".join([name, *quals]), name))


def _owns_camera(s: Slice) -> bool:
    """A Zephyr slice running a customer app (not the stock shim)."""
    return (s.os == "zephyr" and bool(s.app) and bool(s.board)
            and s.app != _STOCK_SHIM_APP)


def zephyr_shield_define(project: BoardProject, slice_: Slice,
                         repo: Path = REPO) -> Optional[str]:
    """`SHIELD=<...>` value for a Zephyr slice, or None when the project
    declares no cameras (or the slice is not the camera-owning one).

    Only Zephyr slices running a customer app qualify.  Each qualifying slice
    with a board overlay for every carrier shield gets the define; if NONE
    does, every qualifying slice raises -- never silently emitting nothing.
    """
    cams = _cameras(project)
    if not cams or not _owns_camera(slice_):
        return None
    connectors = _connectors(project)
    shields: list[str] = []
    for connector, module_id in cams:
        shield = _module(project, module_id).get("zephyr_shield")
        if not shield:
            raise CameraSelectError(
                f"cameras: module '{module_id}' on {connector} has no "
                f"`zephyr_shield:` in metadata/camera_modules/{module_id}.yaml "
                f"-- it cannot be selected on a Zephyr core (Linux-only module)")
        carrier = (connectors.get(connector) or {}).get("zephyr_shields") or []
        if not carrier:
            raise CameraSelectError(
                f"cameras: connector {connector} declares no `zephyr_shields:` "
                f"-- no Zephyr carrier shield exists for it")
        shields += [*carrier, shield]
    shields = list(dict.fromkeys(shields))  # carrier first, deduped, ordered

    def covered(s: Slice) -> bool:
        return bool(s.board) and all(
            _has_overlay(sh, s.board, repo)
            for conn, _m in cams
            for sh in connectors[conn]["zephyr_shields"])

    if not covered(slice_):
        peers = [s for s in project.cores.values() if _owns_camera(s)]
        if any(covered(s) for s in peers):
            return None
        raise CameraSelectError(
            f"cameras: no Zephyr shield overlay for board target "
            f"'{slice_.board}' (core '{slice_.core_id}'): the carrier shield(s) "
            f"have no zephyr/boards/shields/<shield>/boards/<board>.overlay "
            f"for it -- add one for that SoM target")
    return "SHIELD=" + " ".join(shields)


def yocto_camera_lines(project: BoardProject) -> list[str]:
    """local.conf lines selecting each camera's sensor DT include."""
    return [f'ALP_CAMERA_CAM{c[3:]} = "{m}"' for c, m in _cameras(project)]
