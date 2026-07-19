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
from .secure import (emit_sysbuild_conf, emit_tfm_sysbuild_conf,
                      sysbuild_family_base_conf)


# metadata/sdk_version.yaml is the single source of the SDK's own
# declared version (see scripts/check_version_doc_sync.py) -- reading it
# raw (no YAML parse needed) gives the cache a token that changes on
# every version/status bump, so an SDK upgrade invalidates every slice's
# cache even when board.yaml itself is untouched (issue #591).
_SDK_VERSION_YAML = REPO / "metadata" / "sdk_version.yaml"

# Directory names never worth walking when content-hashing an app's
# source tree -- VCS metadata and prior build output the app itself
# doesn't ship.
_SOURCE_HASH_SKIP_DIRS = frozenset({".git", "build", "__pycache__"})


def _sdk_version_token() -> str:
    """Raw contents of `metadata/sdk_version.yaml` as an opaque cache
    token. Deliberately not parsed -- any byte change (version bump,
    status flip, or even a comment edit) is a legitimate reason to
    distrust a prior build, so the whole file is the input.  Falls back
    to a fixed sentinel if the file is ever unreadable so a broken
    checkout fails open (always rebuild) rather than silently caching
    forever."""
    try:
        return _SDK_VERSION_YAML.read_text(encoding="utf-8")
    except OSError:
        return "sdk-version-unreadable"


def _hash_app_sources(slice_: Slice, base_dir: Path) -> str:
    """Content-hash of every file under this slice's resolved `app:`
    directory, so editing e.g. `src/main.c` invalidates the slice's
    cache entry even though no `Slice` field changed (issue #591).

    Resolves the directory the same way `_slice_command` resolves it
    (`_zephyr_app_dir` / `_resolve_app_path`), so the hash always
    covers exactly what will actually be built.

    Hashes file *contents*, not mtimes: mtimes are not preserved by
    `git clone`/`checkout`, so hashing them would spuriously invalidate
    on a fresh checkout with zero real content change -- content
    hashing is both deterministic (no wall-clock/random input) and
    accurate.

    Yocto slices built purely from `image:` (no `app:`) have no
    filesystem app dir to hash -- their inputs are the bitbake layers,
    already tracked by Yocto's own dependency graph, not this cache.
    """
    if not slice_.app:
        return "no-app-dir"
    if slice_.os == "zephyr":
        app_dir = _zephyr_app_dir(slice_.app, base_dir)
    elif slice_.os == "baremetal":
        app_dir = _resolve_app_path(slice_.app, base_dir)
    elif slice_.os == "yocto" and slice_.app != STOCK_IMAGE_APP:
        app_dir = _resolve_app_path(slice_.app, base_dir)
    else:
        return "no-app-dir"

    if not app_dir.is_dir():
        # Missing app dir is itself a distinct, deterministic state --
        # never silently collapses onto a real prior hash.
        return f"missing-app-dir:{app_dir.as_posix()}"

    import hashlib
    m = hashlib.sha256()
    for path in sorted(
        p for p in app_dir.rglob("*")
        if p.is_file()
        and not _SOURCE_HASH_SKIP_DIRS.intersection(
            p.relative_to(app_dir).parts)
    ):
        m.update(path.relative_to(app_dir).as_posix().encode("utf-8"))
        try:
            m.update(path.read_bytes())
        except OSError:
            m.update(b"<unreadable>")
    return m.hexdigest()


# Tool table used to decide whether a slice can actually be built on
# this host.  Each os maps to the executable the slice's build dispatch
# needs; missing tools land the slice in `status: skipped`.
_TOOL_FOR_OS: dict[str, str] = {
    "zephyr":    "west",
    "yocto":     "bitbake",
    "baremetal": "cmake",
    # 'off' never reaches the dispatcher.
}


def iter_buildable_slices(project: BoardProject):
    """Yield every non-`off` core's `Slice`, sorted by `core_id`.

    ADR-0020 Phase 1: the SINGLE source of WHICH cores build and in WHAT
    ORDER, so `emit_build_plan()` (the plan the `alp` CLI reads) and
    `Orchestrator.fan_out()` (the real build) can never disagree.  Before
    this seam, `emit_build_plan` iterated `sorted(project.cores.values())`
    while `fan_out` iterated `project.cores.items()` (dict/insertion
    order) -- for a project whose cores don't happen to already be
    declared in sorted order, the plan's slice order and the real build's
    dispatch order silently diverged.
    """
    for core_id in sorted(project.cores):
        slice_ = project.cores[core_id]
        if slice_.os == "off":
            continue
        yield slice_


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
        board_yaml: Path,
    ) -> None:
        self.project = project
        self.build_root = Path(build_root)
        # `board_yaml` anchors every slice's relative `app:` path (issue
        # #596) -- pass the project's actual board.yaml so build commands
        # resolve identically regardless of the caller's CWD.  Required
        # (ADR-0020 Phase 1): the old `Path.cwd()` fallback was a live
        # "same board.yaml, two answers" divergence between callers that
        # passed it and callers that didn't -- every real caller (cli.py)
        # already has the path, so there is no legitimate caller left to
        # fall back for.
        self.base_dir = Path(board_yaml).resolve().parent
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
        """Hash every input that determines a slice's build output.

        Issue #591: the previous hash covered only a handful of
        resolved `board.yaml` fields, so it stayed unchanged after an
        application source edit, an SDK upgrade, or a generated-config
        change -- the slice reported `cache-hit` and the builder never
        ran, leaving a stale artefact in place.  This hash now covers,
        in addition to the resolved slice fields:

          * the slice's application source tree content (`app:` dir,
            resolved exactly as `_slice_command` resolves it) --
            catches `src/main.c` edits;
          * the SDK's own declared version -- catches an SDK upgrade
            landing under an otherwise-unchanged board.yaml;
          * the materialised per-slice config artefact (`alp.conf` /
            `local.conf` / `cmake-args.txt`) and every shared generated
            artefact (IPC header, dts reservations/partitions, sysbuild
            overlays) -- catches a config-only change that resolves
            differently without touching any `Slice` field directly.

        Deliberately excludes orchestrator-populated bookkeeping
        (`build_dir`, `status`, `duration_s`, ...) and does NOT walk the
        whole SDK tree or the process environment -- only the inputs
        that actually reach this slice's build command.
        """
        import dataclasses
        import hashlib
        m = hashlib.sha256()

        # 1. Resolved slice fields -- everything the loader assigned to
        #    this slice (board, peripherals, libraries, memory/power
        #    tuning, extra_libraries, toolchain, recipe, ...), minus the
        #    fields the orchestrator itself populates during/after a run.
        slice_fields = dataclasses.asdict(slice_)
        for bookkeeping in ("build_dir", "output_artefact", "status",
                            "reason", "log_path", "duration_s"):
            slice_fields.pop(bookkeeping, None)
        m.update(json.dumps(slice_fields, sort_keys=True, default=str)
                 .encode("utf-8"))

        # 2. Project-level fields every slice's generated header/config
        #    is derived from.
        m.update(self.project.sku.encode("utf-8"))
        for entry in sorted(self.project.ipc, key=lambda e: e.name):
            m.update(entry.name.encode("utf-8"))
            m.update(entry.kind.encode("utf-8"))
            m.update(",".join(sorted(entry.endpoints)).encode("utf-8"))
            m.update(str(entry.carve_out_kb).encode("utf-8"))

        # 3. The materialised per-slice config artefact -- pure/
        #    deterministic, same source `_materialise_slice_config` writes.
        artefact = _slice_config_artefact(self.project, slice_)
        if artefact is not None:
            name, contents = artefact
            m.update(name.encode("utf-8"))
            m.update(contents.encode("utf-8"))

        # 4. Shared generated artefacts -- every slice builds against
        #    these (e.g. the IPC header), so a project-level config
        #    change (boot/security/diagnostics) that only shows up here
        #    must still invalidate every slice, not just the one whose
        #    own fields changed.
        for path, contents in _shared_artefacts(self.project, self.build_root):
            m.update(path.relative_to(self.build_root).as_posix()
                     .encode("utf-8"))
            m.update(contents.encode("utf-8"))

        # 5. Application source tree -- the actual files the build
        #    command compiles.
        m.update(_hash_app_sources(slice_, self.base_dir).encode("utf-8"))

        # 6. SDK revision -- an SDK upgrade must invalidate every
        #    slice's cache even when board.yaml is untouched.
        m.update(_sdk_version_token().encode("utf-8"))

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

        cmd = _slice_command(self.project, slice_, base_dir=self.base_dir)
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

        # 2. Off-core / `--core`-filtered status bookkeeping.  Runs over
        #    EVERY core (not just the buildable ones) because the
        #    manifest (step 6) reports every core, including the ones
        #    this run never touches.  Order doesn't matter here -- only
        #    which slices get a status, not what order they build in.
        for cid, slice_ in self.project.cores.items():
            if slice_.os == "off":
                slice_.status = "skipped"
                slice_.reason = "os: off"
            elif only_core is not None and cid != only_core:
                slice_.status = "skipped"
                slice_.reason = f"--core {only_core} selected; this slice not in scope"

        # 3. Per-slice config materialisation.  Build targets is drawn
        #    from `iter_buildable_slices` -- the SAME enumeration
        #    `emit_build_plan` reads -- so the plan and the real build
        #    can never disagree on which cores build or in what order
        #    (ADR-0020 Phase 1).
        targets: list[Slice] = []
        for slice_ in iter_buildable_slices(self.project):
            if only_core is not None and slice_.core_id != only_core:
                continue
            self._materialise_slice_config(slice_)
            targets.append(slice_)

        # 4. Caching: skip slices whose inputs hash matches the last
        #    successful run AND whose build actually produced evidence
        #    of running (its build.log).  `slice_.build_dir` is created
        #    unconditionally by `_materialise_slice_config` just above
        #    for every target -- including ones that have never been
        #    built -- so `build_dir.is_dir()` alone is never a useful
        #    gate (issue #591: it was always true here and so never
        #    actually blocked a cache-hit). `log_path` is written only
        #    by a real `_dispatch_slice` run, so its presence is real
        #    evidence a build happened; its absence (output missing)
        #    can never produce a cache-hit even if the hash matches.
        skip_targets: set[str] = set()
        for slice_ in targets:
            h = self._slice_hash(slice_)
            cached = self._state.get(slice_.core_id) or {}
            log_path = slice_.log_path
            if (cached.get("hash") == h
                    and cached.get("status") == "ok"
                    and log_path is not None and log_path.is_file()):
                slice_.status = "ok"
                slice_.reason = "cache-hit (inputs unchanged since last successful build)"
                slice_.output_artefact = cached.get("output_artefact")
                skip_targets.add(slice_.core_id)

        runnable = [s for s in targets if s.core_id not in skip_targets]

        # 5. Dispatch.  Use ProcessPoolExecutor for parallel, but the
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

        # 6. Persist cache state.
        for slice_ in self.project.cores.values():
            if slice_.status == "ok":
                self._state[slice_.core_id] = {
                    "hash":           self._slice_hash(slice_),
                    "status":         slice_.status,
                    "output_artefact": slice_.output_artefact,
                }
        self._save_state()

        # 7. Manifest.
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
    matter where `west` / `alp-orchestrate` happens to be invoked from
    (issue #596).
    """
    if slice_.os == "zephyr":
        if not slice_.app or not slice_.board:
            return None
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
        if emit_sysbuild_conf(project) or emit_tfm_sysbuild_conf(project):
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
                sb_conf = (Path(slice_.build_dir).parent
                           / "alp_sysbuild.conf").resolve()
                # LAYER, don't replace: SB_CONF_FILE accepts a `;`-joined
                # list, and sysbuild merges every listed file in order
                # (later files win on a repeated symbol).  When the SoM
                # family ships a curated zephyr/sysbuild/<family>/
                # sysbuild.conf, put it FIRST so the customer's `boot:`
                # overlay lands as deltas on top of the curated base
                # instead of forking family boot policy into two
                # divergent places (issue #807).
                family_base = sysbuild_family_base_conf(project)
                sb_conf_files = (
                    [str(family_base), str(sb_conf)] if family_base
                    else [str(sb_conf)]
                )
                defines.append(f"-DSB_CONF_FILE={';'.join(sb_conf_files)}")
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
