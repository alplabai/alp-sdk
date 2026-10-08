# SPDX-License-Identifier: Apache-2.0
"""Resolve a project's `cameras: [{connector, module, core?}]` into build inputs.

One resolver for both OS lanes, so the same board.yaml line picks the camera
on every SoM.  Ownership (which core gets the camera) is `camera_owner.py`,
shared with the CLI validator; ONLY the owner core's build is touched (the owner is the core whose OS the
connector supports, see there):

  * Zephyr owner -> ONE `-DSHIELD=<carrier shields> <module shield>` define
    (carrier half from the board's `camera_connectors.<CAMn>.zephyr_shields`,
    camera half from `metadata/camera_modules/<module>.yaml: zephyr_shield`).
    On a sysbuild it is `-D<image>_SHIELD=...` so MCUboot does not get it.
    The space-joined value is ONE argv element (a shell would need it quoted:
    `-DSHIELD="a b"`).
  * Yocto owner  -> `ALP_CAMERA_CAM<n> = "<module_id>"` in local.conf.  Once
    the camera-DT work (#2736) lands, the kernel bbappend keys the sensor DT
    include on that variable; it consumes CAM0 only.

Connector `CAMn` is camera index `n` (`alp-camera<n>` in Zephyr DT, the `n`
in `ALP_CAMERA_CAM<n>`).
"""

from __future__ import annotations

from pathlib import Path
from typing import Optional

import yaml

from .camera_owner import CameraPlan, candidates, plan_cameras
from .models import BoardProject, OrchestratorError, Slice
from .paths import REPO


class CameraSelectError(OrchestratorError):
    """A `cameras:` entry cannot be turned into a build input for a slice."""


def _connectors(project: BoardProject) -> dict:
    # A `preset:` board carries them; an inline board's live in board.yaml.
    return ((project.board_preset or {}).get("camera_connectors")
            or project.raw.get("camera_connectors") or {})


def _module(project: BoardProject, module_id: str) -> Optional[dict]:
    p = project.effective_metadata_root() / "camera_modules" / f"{module_id}.yaml"
    try:
        return yaml.safe_load(p.read_text(encoding="utf-8")) or {}
    except OSError:
        return None


def _cores(project: BoardProject) -> dict:
    return {cid: {"os": s.os, "app": s.app, "board": s.board}
            for cid, s in project.cores.items()}


def _plans(project: BoardProject, repo: Path = REPO) -> list[CameraPlan]:
    cams = sorted(project.raw.get("cameras") or [],
                  key=lambda c: int(c["connector"][3:]))
    if not cams:
        return []
    modules = {c["module"]: _module(project, c["module"]) for c in cams}
    plans = plan_cameras(cams, _connectors(project), _cores(project),
                         {m: d for m, d in modules.items() if d is not None}, repo)
    connectors = _connectors(project)
    for p in plans:
        if modules.get(p.module) is None:
            p.errors.append(f"cameras: unknown module '{p.module}' "
                            f"(no metadata/camera_modules/{p.module}.yaml)")
        if p.connector not in connectors:
            p.errors.append(f"cameras: connector '{p.connector}' is not a camera "
                            f"connector of this board")
    return plans


def check(project: BoardProject, slice_: Slice,
          repo: Path = REPO) -> list[CameraPlan]:
    """The cameras this slice owns.  Raises CameraSelectError when they are
    unbuildable for it (the backstop for what `tan validate` reports as
    ALP-B003).  An ownerless error (none / ambiguous) blocks every
    candidate core."""
    plans = _plans(project, repo)
    mine = [p for p in plans if p.owner == slice_.core_id]
    is_candidate = slice_.core_id in candidates(_cores(project))
    errs = [e for p in plans
            if p.owner == slice_.core_id or (p.owner is None and is_candidate)
            for e in p.errors]
    if errs:
        raise CameraSelectError("; ".join(dict.fromkeys(errs)))
    return mine


def zephyr_shield_define(project: BoardProject, slice_: Slice,
                         image: Optional[str] = None,
                         repo: Path = REPO) -> Optional[str]:
    """`SHIELD=<...>` (`<image>_SHIELD=<...>` on a sysbuild) for the Zephyr
    slice that owns cameras, else None."""
    mine = check(project, slice_, repo)
    if not mine or slice_.os != "zephyr":
        return None
    connectors = _connectors(project)
    shields: list[str] = []
    for p in mine:
        shields += connectors[p.connector]["zephyr_shields"]
        shields.append(_module(project, p.module)["zephyr_shield"])
    shields = list(dict.fromkeys(shields))  # a carrier shield is shared
    return f"{image + '_' if image else ''}SHIELD=" + " ".join(shields)


def shield_define_for_build(project: BoardProject, slice_: Slice,
                            base_dir: Optional[Path] = None) -> Optional[str]:
    """`zephyr_shield_define` with the sysbuild image prefix resolved the way
    `_slice_command` names `<image>_EXTRA_CONF_FILE` (#866)."""
    if not check(project, slice_) or slice_.os != "zephyr":
        return None
    from .orchestrator import _zephyr_app_dir
    from .secure import emit_sysbuild_conf, emit_tfm_sysbuild_conf
    image = None
    if emit_sysbuild_conf(project) or emit_tfm_sysbuild_conf(project):
        image = _zephyr_app_dir(
            slice_.app, base_dir or project.source_dir or Path.cwd()).name
    return zephyr_shield_define(project, slice_, image)


def yocto_camera_lines(project: BoardProject, slice_: Slice) -> list[str]:
    """local.conf lines for the cameras this Yocto slice owns.  Never raises:
    `tan validate` and the command path report the problems."""
    if slice_.os != "yocto":
        return []
    return [f'ALP_CAMERA_CAM{p.connector[3:]} = "{p.module}"'
            for p in _plans(project) if p.owner == slice_.core_id]
