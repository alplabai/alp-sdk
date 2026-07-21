#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Slice-command resolution -- the planner-side helpers `buildplan.py`'s
`emit_build_plan` reads to describe (never run) each slice's build command.

ADR-0020 Phase 4 (preview) retired the SDK-side executor -- the
`Orchestrator` class that fanned build sub-processes out and materialised
artefacts to disk.  What remains here is pure, side-effect-free: resolving
what a slice's build command WOULD be, so `emit_build_plan` can describe it
to an external consumer (`tan`, alplabai/tan-cli, / alp-studio) that owns
execution.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import Any, Optional

from .models import BoardProject, Slice
from .paths import REPO
from .secure import (emit_sysbuild_conf, emit_tfm_sysbuild_conf,
                      sysbuild_family_base_conf)


def iter_buildable_slices(project: BoardProject):
    """Yield every non-`off` core's `Slice`, sorted by `core_id`.

    ADR-0020 Phase 1: the SINGLE source of WHICH cores build and in WHAT
    ORDER, so `emit_build_plan()` (the plan `tan` reads) always
    enumerates slices the same way.
    """
    for core_id in sorted(project.cores):
        slice_ = project.cores[core_id]
        if slice_.os == "off":
            continue
        yield slice_


def _slice_flash_recipe(
    slice_: Slice,
) -> tuple[Optional[str], Optional[dict[str, Any]]]:
    """Per-runtime default flash backend + args for a slice.

    Used by `Slice.to_manifest_entry` to record how an external flash
    step should program the slice's output artefact.

    Returns ``(None, None)`` for `os: off` slices (skipped at flash
    time) and unknown `os:` values; the manifest emitter drops keys
    with None values, so off slices stay tidy.
    """
    if slice_.os == "yocto":
        return ("yocto_wic_to_sd_or_emmc",
                {"target": slice_.machine or ""})
    if slice_.os == "zephyr":
        # No runner is forced here: not every in-tree board registers
        # an openocd runner (e.g. AEN's board.cmake sets
        # flash-runner: alif_flash), so `west flash --runner openocd`
        # FATAL-errors on those boards. Emit no runner and let `west
        # flash` fall back to the board.cmake default; an explicit
        # runner can still be set on flash_args downstream when one is
        # actually known.
        return ("zephyr_west_flash", {})
    if slice_.os == "baremetal":
        return ("baremetal_cmake_flash", {})
    return (None, None)


# M-core "stock shim" app token (Zephyr side).  Accepted by the SoM-preset
# schema and defaulted into M-core slots (AEN m55_hp/he, V2N m33_sm,
# NX91 m33).  The token resolves to the SDK-owned app below rather than a
# project-local path.
STOCK_SHIM_APP = "alp-stock-shim"
STOCK_SHIM_DIR = REPO / "firmware" / "alp-stock-shim"

# A-core "stock image" app token (Yocto side).  Every shipped SoM preset's
# topology.<a-core-id> defaults `app:` to this value (see
# metadata/e1m_modules/*.yaml) -- unlike a customer's own `app:` (a
# filesystem path to their app source), this token IS already the real
# bitbake recipe name for the stock alp-image-edge image, so it is exempt
# from the `recipe:` requirement `_slice_command` enforces for a
# project-supplied app-only Yocto slice (issue #597).
STOCK_IMAGE_APP = "alp-image-edge"


def _slice_command(
    project: BoardProject,
    slice_: Slice,
    base_dir: Path,
) -> Optional[list[str]]:
    """Resolve the build command for a slice.  Returns None when there is no
    buildable command yet -- the caller carries the slice as `skipped` /
    `no-command`, never dropped.

    `base_dir` anchors every relative `app:` path -- the directory holding
    the project's `board.yaml` (or an equivalent explicit root), NEVER the
    caller's process CWD.  A relative `app:` means "relative to the project
    file that named it", so the same board.yaml must resolve identically no
    matter where the emitting process happens to be invoked from
    (issue #596).
    """
    if slice_.os == "zephyr":
        if not slice_.app or not slice_.board:
            return None
        # NB: no explicit `-d`. west's default output is <cwd>/build (a
        # subdirectory of the command's cwd = buildDir), so the tree lands
        # at <buildDir>/build/; the consumer (tan) reconciles that nested
        # layout when it resolves artefacts. Adding `-d <buildDir>` here
        # would double-nest (west resolves a relative -d against its cwd,
        # already = buildDir) -- see finding M14.
        cmd = [
            "west", "build",
            "-b", slice_.board,
            str(_zephyr_app_dir(slice_.app, base_dir)),
        ]
        # ADR 0014 Phase-3 conf->build: wire the generated sysbuild
        # overlays into the build command itself.  `_shared_artefacts`
        # emits the top-level overlay at build_root/alp_sysbuild.conf and
        # the TF-M child overlay at build_root/sysbuild/tfm/tfm.conf.
        # Pass --sysbuild whenever a sysbuild child image is configured (a
        # `boot:` or `security.psa:` block), and point sysbuild at the
        # top-level overlay only when it is non-empty (the TF-M overlay is
        # picked up by sysbuild convention from its sysbuild/tfm/ path).
        # Absent both, the stock per-family sysbuild defaults apply.
        #
        # Zephyr normally derives Python3_EXECUTABLE from the interpreter
        # that launched west (WEST_PYTHON). A pre-existing CMake cache can
        # already define Python3_EXECUTABLE, however, which prevents that
        # hand-off and can select a host Python without the west package.
        # The orchestrator itself runs under the intended workspace Python,
        # so pin it as an explicit CMake cache override (issue #787).
        # Forward slashes: on Windows sys.executable is a backslash path
        # (C:\Users\...), and CMake parses `\U`/`\N` etc. in the resulting
        # cache-override string as invalid character escapes ("Invalid
        # character escape '\U'") when Zephyr expands it into a custom-target
        # command. Posix slashes are valid on every host and a no-op on
        # non-Windows (found via the tan<->alp-sdk e2e build).
        py_exe = sys.executable.replace("\\", "/")
        defines = [f"-DPython3_EXECUTABLE={py_exe}"]
        is_sysbuild = emit_sysbuild_conf(project) or emit_tfm_sysbuild_conf(project)
        if is_sysbuild:
            cmd.append("--sysbuild")
            if emit_sysbuild_conf(project):
                # SB_CONF_FILE is the only supported way to name a
                # non-default top-level sysbuild overlay: `west build` is a
                # ZEPHYR extension command, and no Zephyr has ever had a
                # `--sysbuild-config` flag -- west forwarded the unknown
                # argument to CMake, which failed the configure step with
                # `Unknown argument --sysbuild-config` (issue #805).
                #
                # The path must be ABSOLUTE.  sysbuild resolves a relative
                # SB_CONF_FILE against APP_DIR, not the command's cwd
                # (share/sysbuild/cmake/modules/sysbuild_kconfig.cmake),
                # so the cwd-relative form this used to emit would silently
                # look for the overlay under the application's source dir.
                #
                # Anchor on `base_dir` (the board.yaml directory), never the
                # emitting process's CWD: `slice_.build_dir`'s parent is the
                # build root, which may itself be a relative path, and
                # resolving a relative path bare falls back to
                # `Path.cwd()` -- the same #596 CWD-dependence bug class
                # `_resolve_app_path` guards against for `app:`.
                build_root_dir = Path(slice_.build_dir).parent
                if not build_root_dir.is_absolute():
                    build_root_dir = base_dir / build_root_dir
                sb_conf = (build_root_dir.resolve() / "alp_sysbuild.conf")
                # LAYER, don't replace: SB_CONF_FILE accepts a `;`-joined
                # list, and sysbuild merges every listed file in order
                # (later files win on a repeated symbol).  When the SoM
                # family ships a curated zephyr/sysbuild/<family>/
                # sysbuild.conf, put it FIRST so the customer's `boot:`
                # overlay lands as deltas on top of the curated base
                # instead of forking family boot policy into two
                # divergent places (issue #807).
                family_base = sysbuild_family_base_conf(project)
                # Forward slashes: CMake's `cmake_path()` (which
                # sysbuild_kconfig.cmake uses to split the `;`-joined
                # SB_CONF_FILE list) only recognises `/` -- a native
                # Windows backslash path here dies the configure step
                # with "File ... not found", the same class of bug
                # #849 fixed for -DPython3_EXECUTABLE.
                sb_conf_files = (
                    [family_base.as_posix(), sb_conf.as_posix()]
                    if family_base else [sb_conf.as_posix()]
                )
                defines.append(f"-DSB_CONF_FILE={';'.join(sb_conf_files)}")
        # Wire the slice's materialised per-core Kconfig fragment
        # (`_slice_config_artefact` -> build_dir/alp.conf, carried in the
        # plan's `configArtefacts`) into the build command via
        # EXTRA_CONF_FILE -- Zephyr's supported extra-fragment merge point
        # (layered on prj.conf). The path is ABSOLUTE and anchored on
        # `base_dir` (issue #596), never Path.cwd(), so the plan is
        # byte-identical wherever it is emitted.
        #
        # NOT on a --sysbuild build: a bare -DEXTRA_CONF_FILE there lands
        # on the SYSBUILD image, not the default application image
        # (sysbuild scopes per-image as -D<image>_VAR), so it would NOT
        # reach the app -- silently dropping the per-core alp.conf on
        # boot:/OTA projects. The app-image name is not derivable from
        # board.yaml (it is the app CMakeLists `project()` name), so the
        # image-prefixed form cannot be emitted here. Sysbuild slices
        # still get the per-core alp.conf via the app's own --core-scoped
        # CMakeLists.txt bridge (#870); a plan-native per-image sysbuild
        # wiring is the remaining half of #866.
        if not is_sysbuild:
            alp_conf = Path(slice_.build_dir) / "alp.conf"
            if not alp_conf.is_absolute():
                alp_conf = Path(base_dir) / alp_conf
            alp_conf = alp_conf.resolve()
            defines.append(f"-DEXTRA_CONF_FILE={alp_conf.as_posix()}")
        cmd += ["--", *defines]
        return cmd
    if slice_.os == "yocto":
        # `image:` always names a real recipe (e.g. `alp-image-edge`) --
        # safe to hand straight to bitbake.  `app:` is a filesystem path to
        # the app's source directory (mirrors the zephyr/baremetal `app:`
        # convention), NOT a recipe name -- `bitbake <path>` is never a
        # valid target (issue #597), so an app-only slice needs an explicit
        # `recipe:` naming the bitbake recipe that packages that source.
        if slice_.image:
            return ["bitbake", str(slice_.image)]
        if slice_.app == STOCK_IMAGE_APP:
            return ["bitbake", slice_.app]
        if slice_.app:
            if not slice_.recipe:
                return None
            return ["bitbake", str(slice_.recipe)]
        return None
    if slice_.os == "baremetal":
        if not slice_.app:
            return None
        return ["cmake", "-S", str(_resolve_app_path(slice_.app, base_dir)),
                "-B", str(slice_.build_dir)]
    return None


def _resolve_app_path(app: str, base_dir: Path) -> Path:
    """Resolve `./linux` or absolute paths from a slice.app.

    Relative paths resolve against `base_dir` (the project's board.yaml
    directory) -- never the process's current working directory, so the
    result is identical regardless of the caller's CWD (issue #596).
    """
    if app == STOCK_SHIM_APP:
        return STOCK_SHIM_DIR
    p = Path(app)
    if p.is_absolute():
        return p
    return (Path(base_dir) / p).resolve()


def _zephyr_app_dir(app: str, base_dir: Path) -> Path:
    """Resolve a Zephyr slice's `app:` to the directory holding the
    application `CMakeLists.txt` (what `west build` needs).

    `base_dir` anchors relative paths -- see `_resolve_app_path`.

    Two example conventions are supported:

      * multicore examples point `app:` straight at a self-contained
        Zephyr app directory (e.g. ``./m33_sm`` -- carries its own
        CMakeLists.txt + prj.conf); used verbatim.
      * single-core examples keep one CMakeLists.txt at the example
        root and point `app:` at the sources subdir (e.g. ``./src`` with
        ``target_sources(app PRIVATE src/main.c)``).  The sources dir has
        no CMakeLists.txt of its own, so fall back to its parent (the
        example root) which does.
    """
    p = _resolve_app_path(app, base_dir)
    if (p / "CMakeLists.txt").is_file():
        return p
    if (p.parent / "CMakeLists.txt").is_file():
        return p.parent
    return p
