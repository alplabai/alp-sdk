#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The Orchestrator -- fan-out, per-slice build dispatch, and materialise.

The `Orchestrator` class runs a board.yaml project: fans it into per-core build
slices, dispatches each slice's build command, and materialises the shared +
per-slice artefacts (byte-parity with `emit_build_plan`, which it reads from
buildplan.py). Plus the slice-command cluster it owns: `_slice_command`,
`_slice_flash_recipe`, `_resolve_app_path`, `_zephyr_app_dir`, the `_TOOL_FOR_OS`
table, and the `STOCK_SHIM_APP` token. The #285 orchestrator seam -- the finale;
after it `__init__` is a pure re-export surface.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional

from .buildplan import _shared_artefacts, _slice_build_dir, _slice_config_artefact
from .carveout import resolve_carve_outs
from .manifest import _helper_mcus, emit_system_manifest
from .models import BoardProject, OrchestratorError, Slice, SystemManifest
from .paths import REPO
from .secure import emit_sysbuild_conf, emit_tfm_sysbuild_conf


# Tool table used to decide whether a slice can actually be built on
# this host.  Each os maps to the executable the slice's build dispatch
# needs; missing tools land the slice in `status: skipped`.
_TOOL_FOR_OS: dict[str, str] = {
    "zephyr":    "west",
    "yocto":     "bitbake",
    "baremetal": "cmake",
    # 'off' never reaches the dispatcher.
}


class Orchestrator:
    """Fans out one build sub-process per non-off slice.

    Phase 2 ships the dispatch + manifest assembly; the per-os build
    invocations are stubbed where a tool isn't present so the
    orchestrator completes end-to-end on Windows / non-Yocto hosts.
    """

    def __init__(
        self,
        project: BoardProject,
        build_root: Path,
    ) -> None:
        self.project = project
        self.build_root = Path(build_root)
        self.state_path = self.build_root / ".alp-build-state.json"
        self._state: dict[str, Any] = self._load_state()

    # ---- state cache ----

    def _load_state(self) -> dict[str, Any]:
        if not self.state_path.is_file():
            return {}
        try:
            return json.loads(self.state_path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            return {}

    def _save_state(self) -> None:
        try:
            self.state_path.parent.mkdir(parents=True, exist_ok=True)
            self.state_path.write_text(
                json.dumps(self._state, indent=2, sort_keys=True),
                encoding="utf-8")
        except OSError as e:
            print(f"alp-orchestrate: warning: failed to write "
                  f"{self.state_path}: {e}", file=sys.stderr)

    def _slice_hash(self, slice_: Slice) -> str:
        """Hash the inputs that determine a slice's output."""
        import hashlib
        m = hashlib.sha256()
        m.update(self.project.sku.encode("utf-8"))
        m.update(slice_.os.encode("utf-8"))
        m.update((slice_.app or "").encode("utf-8"))
        m.update((slice_.image or "").encode("utf-8"))
        m.update((slice_.board or "").encode("utf-8"))
        m.update((slice_.machine or "").encode("utf-8"))
        m.update(",".join(sorted(slice_.peripherals)).encode("utf-8"))
        m.update(",".join(sorted(slice_.libraries)).encode("utf-8"))
        m.update(json.dumps(slice_.inference, sort_keys=True)
                 .encode("utf-8"))
        m.update(json.dumps(slice_.iot, sort_keys=True)
                 .encode("utf-8"))
        for entry in sorted(self.project.ipc, key=lambda e: e.name):
            m.update(entry.name.encode("utf-8"))
            m.update(entry.kind.encode("utf-8"))
            m.update(",".join(sorted(entry.endpoints)).encode("utf-8"))
            m.update(str(entry.carve_out_kb).encode("utf-8"))
        return m.hexdigest()[:16]

    # ---- materialisation ----

    def _materialise_slice_config(self, slice_: Slice) -> Path:
        """Write per-core config artefacts under
        build/<core>-<os>/.

        The artefact itself comes from `_slice_config_artefact` -- the
        same source `emit_build_plan` reads -- so what we write and
        what the plan promises cannot drift.
        """
        slice_dir = _slice_build_dir(self.build_root, slice_)
        slice_dir.mkdir(parents=True, exist_ok=True)
        slice_.build_dir = slice_dir
        slice_.log_path = slice_dir / "build.log"
        artefact = _slice_config_artefact(self.project, slice_)
        if artefact is not None:
            name, contents = artefact
            (slice_dir / name).write_text(contents, encoding="utf-8")
        return slice_dir

    def _materialise_shared(self) -> Path:
        """Write the shared generated artefacts.

        The (path, contents) set comes from `_shared_artefacts` -- the
        same source `emit_build_plan` reads -- so what we write and
        what the plan promises cannot drift.  Conditional artefacts
        (sysbuild / TF-M) are absent from the list when empty, so
        their directories are never created spuriously.
        """
        gen = self.build_root / "generated"
        for path, contents in _shared_artefacts(self.project,
                                                self.build_root):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        return gen

    # ---- dispatch ----

    def _dispatch_slice(self, slice_: Slice) -> Slice:
        """Run the per-slice build sub-process (or skip if its tool
        isn't on PATH)."""
        if slice_.os == "off":
            slice_.status = "skipped"
            slice_.reason = "os: off"
            return slice_

        tool = _TOOL_FOR_OS.get(slice_.os)
        if tool is None:
            slice_.status = "failed"
            slice_.reason = f"unknown os '{slice_.os}'"
            return slice_

        if shutil.which(tool) is None:
            slice_.status = "skipped"
            slice_.reason = (f"{tool} not found in PATH; this is normal "
                             f"on non-{slice_.os} dev hosts")
            return slice_

        cmd = _slice_command(self.project, slice_)
        if cmd is None:
            slice_.status = "skipped"
            slice_.reason = ("no command resolver implemented yet for "
                             f"os: {slice_.os} -- Phase 3 wires this up")
            return slice_

        # Slice subprocess: scoped env + dedicated log file.
        env = os.environ.copy()
        env["ALP_SDK_ROOT"] = str(REPO)
        log_path = slice_.log_path or (slice_.build_dir / "build.log")
        start = time.time()
        try:
            with open(log_path, "w", encoding="utf-8") as logf:
                logf.write(f"# alp_orchestrate.py slice command: "
                           f"{' '.join(cmd)}\n")
                logf.flush()
                proc = subprocess.run(
                    cmd, cwd=str(slice_.build_dir),
                    env=env, stdout=logf, stderr=subprocess.STDOUT,
                    check=False)
            slice_.duration_s = time.time() - start
            if proc.returncode == 0:
                slice_.status = "ok"
            else:
                slice_.status = "failed"
                slice_.reason = (f"{tool} exited rc={proc.returncode}; "
                                 f"see {log_path}")
        except (OSError, subprocess.SubprocessError) as e:
            slice_.duration_s = time.time() - start
            slice_.status = "failed"
            slice_.reason = f"slice subprocess raised: {e}"
        return slice_

    # ---- public API ----

    def fan_out(
        self,
        only_core: Optional[str] = None,
        parallel: bool = True,
    ) -> SystemManifest:
        """Run every non-off slice, write the manifest, return it."""
        # #603: an explicit `--core` that doesn't name a real core of this
        # project used to silently skip every slice (each iteration's
        # `cid != only_core` check tripped for every core), write an
        # all-skipped manifest, and exit 0 -- a typo'd `--core` looked
        # exactly like a successful, deliberately-scoped build.  Fail
        # fast instead, before any artefact is written.
        if only_core is not None and only_core not in self.project.cores:
            raise OrchestratorError(
                f"--core {only_core!r} is not a core of this project. "
                f"Known core IDs: {sorted(self.project.cores)}")

        self.build_root.mkdir(parents=True, exist_ok=True)
        # 1. Shared artefacts.
        self._materialise_shared()

        # 2. Per-slice config materialisation.
        targets: list[Slice] = []
        for cid, slice_ in self.project.cores.items():
            if slice_.os == "off":
                slice_.status = "skipped"
                slice_.reason = "os: off"
                continue
            if only_core is not None and cid != only_core:
                slice_.status = "skipped"
                slice_.reason = f"--core {only_core} selected; this slice not in scope"
                continue
            self._materialise_slice_config(slice_)
            targets.append(slice_)

        # 3. Caching: skip slices whose inputs hash matches the last
        #    successful run AND whose build dir still exists.
        skip_targets: set[str] = set()
        for slice_ in targets:
            h = self._slice_hash(slice_)
            cached = self._state.get(slice_.core_id) or {}
            if (cached.get("hash") == h
                    and cached.get("status") == "ok"
                    and slice_.build_dir and slice_.build_dir.is_dir()):
                slice_.status = "ok"
                slice_.reason = "cache-hit (inputs unchanged since last successful build)"
                slice_.output_artefact = cached.get("output_artefact")
                skip_targets.add(slice_.core_id)

        runnable = [s for s in targets if s.core_id not in skip_targets]

        # 4. Dispatch.  Use ProcessPoolExecutor for parallel, but the
        # cheap reality on Phase 2 is most slices skip (missing tool)
        # so the sequential path is fine when parallel=False.
        if parallel and len(runnable) > 1:
            try:
                from concurrent.futures import ProcessPoolExecutor, as_completed
                with ProcessPoolExecutor(max_workers=len(runnable)) as ex:
                    futures = {
                        ex.submit(self._dispatch_slice, s): s
                        for s in runnable
                    }
                    for fut in as_completed(futures):
                        result = fut.result()
                        # Mutate the original Slice in place.
                        original = futures[fut]
                        original.status = result.status
                        original.reason = result.reason
                        original.duration_s = result.duration_s
                        original.output_artefact = result.output_artefact
            except (OSError, RuntimeError) as e:
                # ProcessPool on Windows in some envs fails; degrade to
                # sequential.
                print(f"alp-orchestrate: ProcessPoolExecutor unusable "
                      f"({e}); falling back to sequential", file=sys.stderr)
                for slice_ in runnable:
                    self._dispatch_slice(slice_)
        else:
            for slice_ in runnable:
                self._dispatch_slice(slice_)

        # 5. Persist cache state.
        for slice_ in self.project.cores.values():
            if slice_.status == "ok":
                self._state[slice_.core_id] = {
                    "hash":           self._slice_hash(slice_),
                    "status":         slice_.status,
                    "output_artefact": slice_.output_artefact,
                }
        self._save_state()

        # 6. Manifest.
        ordered = sorted(self.project.cores.values(),
                         key=lambda s: s.core_id)
        manifest = SystemManifest(
            project=self.project,
            slices=ordered,
            carve_outs=resolve_carve_outs(self.project),
            boot_order=list(self.project.som_preset.get("boot_order") or []),
            helper_mcus=_helper_mcus(self.project),
        )

        out = self.build_root / "system-manifest.yaml"
        out.write_text(emit_system_manifest(
            self.project, slices=ordered), encoding="utf-8")

        return manifest


def _slice_flash_recipe(
    slice_: Slice,
) -> tuple[Optional[str], Optional[dict[str, Any]]]:
    """Per-runtime default flash backend + args for a slice.

    Used by `Slice.to_manifest_entry` to record how `west alp-flash`
    should program the slice's output artefact.  The actual backend
    implementations land in subsequent PRs alongside `alp_flash.py`
    -- this Phase 3 wiring is just data plumbing.

    Returns ``(None, None)`` for `os: off` slices (skipped at flash
    time) and unknown `os:` values; the manifest emitter drops keys
    with None values, so off slices stay tidy.
    """
    if slice_.os == "yocto":
        return ("yocto_wic_to_sd_or_emmc",
                {"target": slice_.machine or ""})
    if slice_.os == "zephyr":
        # OpenOCD is the canonical Zephyr runner for the SoCs we
        # ship; vendor-specific runners (jlink for AEN, segger for
        # NX) are picked up via the slice's toolchain when set.
        runner = "openocd"
        return ("zephyr_west_flash", {"runner": runner})
    if slice_.os == "baremetal":
        return ("baremetal_cmake_flash", {})
    return (None, None)


# M-core "stock shim" app token (Zephyr side).  Accepted by the SoM-preset
# schema and defaulted into M-core slots (AEN m55_hp/he, V2N m33_sm,
# NX91 m33).  The token resolves to the SDK-owned app below rather than a
# project-local path.
STOCK_SHIM_APP = "alp-stock-shim"
STOCK_SHIM_DIR = REPO / "firmware" / "alp-stock-shim"


def _slice_command(
    project: BoardProject,
    slice_: Slice,
) -> Optional[list[str]]:
    """Resolve the build command for a slice.  Returns None when there is no
    buildable command yet -- the caller carries the slice as `skipped` /
    `no-command`, never dropped."""
    if slice_.os == "zephyr":
        if not slice_.app or not slice_.board:
            return None
        cmd = [
            "west", "build",
            "-b", slice_.board,
            str(_zephyr_app_dir(slice_.app)),
        ]
        # ADR 0014 Phase-3 conf->build: wire the generated sysbuild
        # overlays into the build command itself.  `_shared_artefacts`
        # emits the top-level overlay at build_root/alp_sysbuild.conf and
        # the TF-M child overlay at build_root/sysbuild/tfm/tfm.conf; the
        # command runs with cwd=build_dir (build/<core>-<os>), so the
        # top-level overlay is one directory up.  Pass --sysbuild whenever
        # a sysbuild child image is configured (a `boot:` or `security.psa:`
        # block), and --sysbuild-config only when the top-level overlay is
        # non-empty (the TF-M overlay is picked up by sysbuild convention
        # from its sysbuild/tfm/ path).  Absent both, the stock per-family
        # sysbuild defaults apply and no flag is added.
        if emit_sysbuild_conf(project) or emit_tfm_sysbuild_conf(project):
            cmd.append("--sysbuild")
            if emit_sysbuild_conf(project):
                cmd += ["--sysbuild-config", "../alp_sysbuild.conf"]
        return cmd
    if slice_.os == "yocto":
        target = slice_.image or slice_.app
        if not target:
            return None
        return ["bitbake", str(target)]
    if slice_.os == "baremetal":
        if not slice_.app:
            return None
        return ["cmake", "-S", str(_resolve_app_path(slice_.app)),
                "-B", str(slice_.build_dir)]
    return None


def _resolve_app_path(app: str) -> Path:
    """Resolve `./linux` or absolute paths from a slice.app."""
    if app == STOCK_SHIM_APP:
        return STOCK_SHIM_DIR
    p = Path(app)
    if p.is_absolute():
        return p
    return (Path.cwd() / p).resolve()


def _zephyr_app_dir(app: str) -> Path:
    """Resolve a Zephyr slice's `app:` to the directory holding the
    application `CMakeLists.txt` (what `west build` needs).

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
    p = _resolve_app_path(app)
    if (p / "CMakeLists.txt").is_file():
        return p
    if (p.parent / "CMakeLists.txt").is_file():
        return p.parent
    return p
