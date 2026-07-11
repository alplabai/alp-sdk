#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build-plan emission -- the Wave C consumer contract.

`emit_build_plan` renders the machine-readable JSON build plan the `alp` CLI
materialises; the shared helpers `_slice_build_dir` / `_slice_config_artefact` /
`_shared_artefacts` are the single source the Orchestrator's materialise path and
the plan MUST agree on byte-for-byte (the CLI reads what the Orchestrator writes).
Extracted as the #285 build-plan emit seam. The per-slice config emitters come
from kconfig.py, the header/secure artefacts from headers.py / secure.py; the
orchestrator-side slice-command bits (_slice_command, STOCK_SHIM_APP) are
lazy-imported from the package (they stay inline until orchestrator.py).
"""

from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path
from typing import Any, Optional

from .headers import emit_dts_partitions, emit_dts_reservations, emit_ipc_contract_h
from .kconfig import (
    _resolve_console,
    _slice_alp_conf,
    _slice_cmake_args,
    _slice_local_conf,
)
from .models import BoardProject, Slice
from .paths import REPO
from .secure import emit_sysbuild_conf, emit_tfm_sysbuild_conf


def _slice_build_dir(build_root: Path, slice_: Slice) -> Path:
    """Per-slice build directory: build/<core>-<os>/."""
    return Path(build_root) / f"{slice_.core_id}-{slice_.os}"


def _slice_config_artefact(
    project: BoardProject,
    slice_: Slice,
) -> Optional[tuple[str, str]]:
    """(filename, contents) of the slice's config artefact, or None
    when the os has none.

    Single source for both the Orchestrator's materialise step and
    `emit_build_plan` -- the two MUST agree byte-for-byte (the CLI
    consumer byte-writes the plan's contents and trusts them to match
    what we'd write ourselves).
    """
    if slice_.os == "zephyr":
        return ("alp.conf", _slice_alp_conf(project, slice_))
    if slice_.os == "yocto":
        return ("local.conf", _slice_local_conf(project, slice_))
    if slice_.os == "baremetal":
        return ("cmake-args.txt", _slice_cmake_args(project, slice_))
    return None


def _shared_artefacts(
    project: BoardProject,
    build_root: Path,
) -> list[tuple[Path, str]]:
    """(path, contents) of every shared generated artefact.

    Single source for `_materialise_shared` and `emit_build_plan`
    (same byte-parity contract as `_slice_config_artefact`).
    Conditional artefacts (sysbuild / TF-M) follow absence-emits-
    nothing: they only appear when their emit is non-empty.
    """
    build_root = Path(build_root)
    gen = build_root / "generated"
    out: list[tuple[Path, str]] = [
        # `<alp/system_ipc.h>` is the canonical include path consumers
        # use (see include/alp/rpc.h §usage and the per-slice main.c
        # references) -- the header sits in an `alp/` subdir so slice
        # CMakeLists add `generated/` straight to the include path.
        (gen / "alp" / "system_ipc.h", emit_ipc_contract_h(project)),
        (gen / "dts-reservations.dtsi", emit_dts_reservations(project)),
        # Apps that don't declare storage[] still get a stub file with
        # a "nothing to emit" comment so downstream #include resolves.
        (gen / "dts-partitions.dtsi", emit_dts_partitions(project)),
    ]
    sysbuild_conf = emit_sysbuild_conf(project)
    if sysbuild_conf:
        out.append((build_root / "alp_sysbuild.conf", sysbuild_conf))
    tfm_conf = emit_tfm_sysbuild_conf(project)
    if tfm_conf:
        out.append((build_root / "sysbuild" / "tfm" / "tfm.conf",
                    tfm_conf))
    return out


def _slice_toolchain(slice_: Slice) -> dict[str, Optional[str]]:
    """This slice's compiler identity: `{target_triple, compiler, sysroot, id}`
    (#610 §4 per-slice tooling index).

    Grounded in the SoM preset's `topology.<core>.toolchain` -- the same
    field `Slice.to_manifest_entry` already surfaces in
    `system-manifest.yaml` -- never invented.  For a Zephyr slice this
    value (e.g. `arm-zephyr-eabi`) IS the real Zephyr SDK toolchain
    directory name AND its GCC target triple, so `target_triple` /
    `compiler` derive straight from it.  A Yocto slice's toolchain tag
    (`poky-glibc`) is a *category* (C-library flavour), not a literal
    GCC triple -- the real triple depends on the Yocto build's own
    TUNE_FEATURES/TCLIBC (outside board.yaml / SoM metadata), so
    `target_triple` / `compiler` / `sysroot` stay null rather than
    guess an ABI suffix (e.g. `gnueabi` vs `gnueabihf`).  Zephyr has no
    conventional cross-compile sysroot either (the SDK bundles its own
    libc per architecture) -- `sysroot` is null for every runtime today.
    """
    target_triple: Optional[str] = None
    compiler: Optional[str] = None
    if slice_.os == "zephyr" and slice_.toolchain:
        target_triple = slice_.toolchain
        compiler = f"{slice_.toolchain}-gcc"
    return {
        "target_triple": target_triple,
        "compiler":      compiler,
        "sysroot":       None,
        "id":            slice_.toolchain,
    }


def _slice_artifacts(build_dir: Path, slice_: Slice) -> dict[str, Optional[str]]:
    """Deterministic OUTPUT paths under `build_dir` (#610 §4) -- the
    WHERE, not a promise the files already exist (they don't until the
    slice is actually built).

    Zephyr's own CMake layout fixes these names: `cmake/modules/
    kernel.cmake` (`PROJECT_BINARY_DIR = CMAKE_CURRENT_BINARY_DIR/
    zephyr`, `KERNEL_ELF_NAME`/`_BIN_NAME`/`_MAP_NAME`/`_SYMBOLS_NAME`/
    `_STAT_NAME`) always lands `zephyr.elf` / `.bin` / `.map` /
    `.symbols` / `.stat` in a `zephyr/` subdirectory of the build dir
    (`.symbols` / `.stat` are gated behind the opt-in `CONFIG_
    OUTPUT_SYMBOLS` / `CONFIG_OUTPUT_STAT`, same "doesn't exist until
    built/enabled" caveat as the rest); the top-level `CMakeLists.txt`
    forces `CMAKE_EXPORT_COMPILE_COMMANDS` unconditionally, always to
    the build dir root, not the `zephyr/` subdirectory.  A Yocto
    slice's real output (the wic/ext4 image) lands under the *Yocto
    build tree's* own deploy dir -- outside this slice's `build_dir`,
    which only ever carries the `local.conf` fragment -- so there is
    no honest path to report; same for `baremetal`, whose executable
    name is the app's own `CMakeLists.txt` to pick, not an SDK
    convention this emitter can predict.
    """
    if slice_.os == "zephyr":
        zdir = build_dir / "zephyr"
        return {
            "elf":              (zdir / "zephyr.elf").as_posix(),
            "map":              (zdir / "zephyr.map").as_posix(),
            "bin":              (zdir / "zephyr.bin").as_posix(),
            "size_report":      (zdir / "zephyr.stat").as_posix(),
            "symbols":          (zdir / "zephyr.symbols").as_posix(),
            "compile_commands": (build_dir / "compile_commands.json").as_posix(),
        }
    return {
        "elf":              None,
        "map":              None,
        "bin":              None,
        "size_report":      None,
        "symbols":          None,
        "compile_commands": None,
    }


def _slice_debug(
    project: BoardProject,
    slice_: Slice,
    flash_method: Optional[str],
    flash_args: Optional[dict[str, Any]],
) -> dict[str, Optional[str]]:
    """Headless monitor/debug selectors for this slice: `{console, probe}`
    (#610 §4).

    `console` reuses `_resolve_console` -- the same OS-derived /
    `diagnostics.console:`-overridable selector `_slice_alp_conf` /
    `_slice_local_conf` already emit Kconfig from.  `"none"` means
    "inherit whatever the board provides" -- not a concrete selector a
    headless consumer can act on -- so it maps to null here.  `probe`
    reuses `_slice_flash_recipe`'s already-resolved runner (the same
    fact `west alp-flash` dispatches on, computed once per slice by the
    caller and passed in): only the Zephyr `zephyr_west_flash` recipe
    names an actual debug-probe runner (`openocd`); the Yocto image-
    flash recipe and the baremetal cmake recipe don't identify a live
    debug probe, so `probe` stays null there.
    """
    console_sel = _resolve_console(
        project.diagnostics.get("console"), slice_.os)
    console = None if console_sel == "none" else console_sel
    probe: Optional[str] = None
    if flash_method == "zephyr_west_flash":
        probe = (flash_args or {}).get("runner")
    return {"console": console, "probe": probe}


def emit_build_plan(
    project: BoardProject,
    *,
    board_yaml: Path,
    build_root: Path,
) -> str:
    """Emit the machine-readable build plan as JSON (Wave C contract).

    Consumed by the `alp` CLI (alp-sdk-vscode), which materialises the
    plan's files, runs each slice's command, and owns scheduling /
    caching / progress UX on top -- instead of re-implementing this
    planner.  Agreed 2026-06-04 with the alp-sdk-vscode team; their
    docs/PROPOSAL-alp-build-core.md records the settlement.

    Contract notes (locked with the consumer -- bump `schemaVersion`
    and flag in the CHANGELOG before changing the shape):

      * camelCase keys; `schemaVersion` is independent of board.yaml's
        schema version.
      * Every artefact carries its `contents` so the consumer's
        materialise step stays pure IO.  `_shared_artefacts` /
        `_slice_config_artefact` are the single sources both this emit
        and the Orchestrator's own materialise step read, so the two
        cannot drift.
      * No `inputHash` (the consumer computes its own cache key over
        the plan) and no `sequential` (parallelism policy belongs to
        the consumer's scheduler).
      * One slice per non-`off` core, sorted by coreId.  A slice this
        script cannot build yet (e.g. no `app:`) is carried with
        `command: null` plus a `no-command` warning -- never dropped,
        so the consumer can still report the core.
      * Write-free: nothing is created on disk.  (Command resolution
        stats the app dir to pick the CMakeLists.txt convention --
        read-only, same as the build itself.)
      * Per-slice tooling index (#610 §4, additive to schemaVersion 1 --
        never renamed/removed, see `metadata/schemas/build-plan-v1.
        schema.json`): `toolchain` (compiler identity --
        `_slice_toolchain`), `artifacts` (deterministic OUTPUT paths
        under `buildDir`, not a promise they exist yet -- `_slice_
        artifacts`), and `debug` (headless console/probe selectors --
        `_slice_debug`).  A field genuinely not derivable for a
        runtime (e.g. a Yocto slice's exact GCC triple) is null, never
        guessed.
    """
    # Orchestrator-side (stay inline until orchestrator.py); lazy to avoid
    # a buildplan<->package import cycle.
    from .orchestrator import (
        STOCK_IMAGE_APP,
        _resolve_app_path,
        _slice_command,
        _slice_flash_recipe,
    )
    build_root = Path(build_root)
    # Anchor every slice's relative `app:` on the board.yaml's own
    # directory, never the emitting process's CWD -- the plan must be
    # byte-identical no matter where `--emit build-plan` is invoked from
    # (issue #596).
    base_dir = Path(board_yaml).resolve().parent
    slices_out: list[dict[str, Any]] = []
    warnings: list[dict[str, Any]] = []

    for slice_ in sorted(project.cores.values(),
                         key=lambda s: s.core_id):
        if slice_.os == "off":
            continue
        build_dir = _slice_build_dir(build_root, slice_)
        # `replace` keeps this emit side-effect free: _slice_command
        # reads `build_dir` off the slice (baremetal -B), and the
        # project's own Slice objects must stay untouched.
        cmd = _slice_command(
            project, replace(slice_, build_dir=build_dir),
            base_dir=base_dir)
        if cmd is None:
            if (slice_.os == "yocto" and slice_.app
                    and not slice_.image and not slice_.recipe):
                # An app-only Yocto slice with no `recipe:` has no valid
                # bitbake target -- `app:` is a source directory, not a
                # recipe name (issue #597).  Block the plan explicitly
                # instead of ever emitting `bitbake <path>`.
                warnings.append({
                    "code":    "yocto-recipe-missing",
                    "coreId":  slice_.core_id,
                    "message": (f"core '{slice_.core_id}' has app: "
                                f"'{slice_.app}' but no recipe: -- add "
                                f"the bitbake recipe name that packages "
                                f"this app source, or set image: to "
                                f"build a stock image instead"),
                })
            else:
                warnings.append({
                    "code":    "no-command",
                    "coreId":  slice_.core_id,
                    "message": (f"no build command for core "
                                f"'{slice_.core_id}' (os: {slice_.os}) "
                                f"-- missing app/board/image"),
                })
        config_artefacts: list[dict[str, str]] = []
        artefact = _slice_config_artefact(project, slice_)
        if artefact is not None:
            name, contents = artefact
            config_artefacts.append({
                "path":     (build_dir / name).as_posix(),
                "contents": contents,
            })
        # `appDir` retains the resolved source directory independent of
        # `command` -- tooling that wants the app source (e.g. to watch
        # it for incremental rebuilds) doesn't have to reverse-engineer
        # it out of a yocto/zephyr/baremetal-shaped command (issue #597).
        # `alp-image-edge` is the A-core stock-image token, not a source
        # path -- there is no app dir to report for it.
        app_dir = (_resolve_app_path(slice_.app, base_dir).as_posix()
                   if slice_.app and slice_.app != STOCK_IMAGE_APP else None)
        # Same flash-recipe fact `Slice.to_manifest_entry` surfaces to
        # `west alp-flash` -- reused here (not re-derived) so `debug.probe`
        # can never drift from the manifest's own `flash_method`/`flash_args`.
        flash_method, flash_args = _slice_flash_recipe(slice_)
        slices_out.append({
            "coreId":          slice_.core_id,
            "backend":         slice_.os,
            "buildDir":        build_dir.as_posix(),
            "appDir":          app_dir,
            "configArtefacts": config_artefacts,
            "toolchain":       _slice_toolchain(slice_),
            "artifacts":       _slice_artifacts(build_dir, slice_),
            "debug":           _slice_debug(
                project, slice_, flash_method, flash_args),
            "command": None if cmd is None else {
                "tool": cmd[0],
                "args": cmd[1:],
                "cwd":  build_dir.as_posix(),
            },
            # Native host-path form: the value is handed to the slice
            # subprocess environment verbatim.
            "env": {"ALP_SDK_ROOT": str(REPO)},
        })

    plan: dict[str, Any] = {
        "schemaVersion":   1,
        "generatedBy":     "scripts/alp_orchestrate.py",
        "boardYaml":       Path(board_yaml).as_posix(),
        "sku":             project.sku,
        "buildRoot":       build_root.as_posix(),
        "slices":          slices_out,
        "sharedArtefacts": [
            {"path": p.as_posix(), "contents": c}
            for p, c in _shared_artefacts(project, build_root)
        ],
        "warnings":        warnings,
    }
    return json.dumps(plan, indent=2) + "\n"
