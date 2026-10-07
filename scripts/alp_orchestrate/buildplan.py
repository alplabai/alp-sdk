#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build-plan emission -- the Wave C consumer contract.

`emit_build_plan` renders alp-sdk's reference machine-readable JSON build plan.
Python Tan normally creates the equivalent plan in process and can consume a
saved SDK plan through `--plan-from`; the shared helpers `_slice_build_dir` /
`_slice_config_artefact` / `_shared_artefacts` are the single source the
Orchestrator's materialise path and the plan MUST agree on byte-for-byte.
Extracted as the #285 build-plan emit seam. The per-slice config emitters come
from kconfig.py, the header/secure artefacts from headers.py / secure.py; the
orchestrator-side slice-command bits (_slice_command, STOCK_SHIM_APP) are
lazy-imported from the package (they stay inline until orchestrator.py).
"""

from __future__ import annotations

import json
import re
import subprocess
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
from .models import BoardProject, OrchestratorError, Slice
from .ownership import project_m33_overlay
from .paths import REPO
from .secure import emit_sysbuild_conf, emit_tfm_sysbuild_conf

# The skip-vs-fail policy a slice dispatcher MUST apply, published verbatim
# in the plan envelope so a consumer (tan-cli) stops hand-porting it: an
# unknown `backend` fails the slice, a missing tool on PATH or a null
# `command` skip it.
_EXECUTION_POLICY = {
    "unknownBackend": "fail",
    "missingTool":    "skip",
    "nullCommand":    "skip",
}

# The `${NAME}` tokens a plan consumer substitutes in a `planPathMode:
# tokened` plan.  This emitter writes the first three; tan-cli also resolves
# TOOLCHAIN_ROOT, so a board.yaml placeholder must not reuse that name either.
PLAN_PATH_TOKENS = frozenset({"SDK_ROOT", "PROJECT_ROOT", "PYTHON", "TOOLCHAIN_ROOT"})

# Any `${...}` in a plan field, and the names `deferredPlaceholders` may carry
# (the schema's pattern; tan-cli matches the same).
_PLAN_REF_RE = re.compile(r"\$\{([^}]*)\}")
_DEFERRED_NAME_RE = re.compile(r"[A-Z][A-Z0-9_]*")


def _deferred_placeholders(
    slices_out: list[dict[str, Any]],
    shared: list[dict[str, str]],
    board_yaml: Path,
) -> list[str]:
    """Names of the `${NAME}` placeholders this plan leaves unresolved on
    purpose (issue #2696), sorted.

    A deferred placeholder is a value the user wrote in board.yaml as
    `${NAME}` and the planner copied verbatim into a slice config artefact,
    for the build host or the device to fill
    (`examples/connectivity/iot-fleet-ota`'s `ota.server.tenant`).  Who fills
    it, and how, is outside the plan; the list only says the consumer must
    leave it alone.  Listing
    them lets a consumer tell them from a plan path token it must substitute,
    instead of refusing the whole plan as an unresolved token.

    Every other `${...}` is refused here, because a consumer cannot handle it
    correctly:
      * a plan path token in a config artefact -- a user value named like
        `${SDK_ROOT}` would be silently replaced by a checkout path;
      * a name not written as `${NAME}` in the project's board.yaml -- the
        planner invented it, and tan would refuse it anyway;
      * a name outside `[A-Z][A-Z0-9_]*`, the contract's name pattern;
      * a placeholder in an artefact that is not a `.conf` file -- CMake
        would expand it itself (the same rule tan-cli#1306 applies);
      * any non-token `${...}` outside `configArtefacts` (command, env,
        envAppendPath, appDir, postCommands, sharedArtefacts).
    """
    board_text = Path(board_yaml).read_text(encoding="utf-8")
    deferred: set[str] = set()

    def refuse(where: str, ref: str, why: str) -> None:
        raise OrchestratorError(
            f"build plan: `${{{ref}}}` in {where} {why} (issue #2696)")

    for slice_ in slices_out:
        core = slice_["coreId"]
        for artefact in slice_["configArtefacts"]:
            where = f"core '{core}' config artefact `{artefact['path']}`"
            for ref in _PLAN_REF_RE.findall(artefact["contents"]):
                if not artefact["path"].endswith(".conf"):
                    refuse(where, ref, "is in a non-`.conf` artefact, which "
                                       "its reader (e.g. CMake) would expand "
                                       "itself -- a placeholder may only "
                                       "reach a Kconfig fragment or local.conf")
                if ref in PLAN_PATH_TOKENS:
                    refuse(where, ref, "is a plan path token: a consumer would "
                                       "replace it with a checkout path")
                if not _DEFERRED_NAME_RE.fullmatch(ref):
                    refuse(where, ref, "is not a valid placeholder name -- "
                                       "use upper-case letters, digits and "
                                       "`_`, starting with a letter")
                if f"${{{ref}}}" not in board_text:
                    refuse(where, ref, "does not come from board.yaml, so the "
                                       "planner produced it -- an unresolved "
                                       "token")
                deferred.add(ref)
        rest = {k: slice_[k] for k in ("command", "env", "envAppendPath",
                                       "appDir", "postCommands")}
        for ref in _PLAN_REF_RE.findall(json.dumps(rest)):
            if ref not in PLAN_PATH_TOKENS:
                refuse(f"core '{core}' command/env/appDir", ref,
                       "is not a plan path token -- a placeholder may only "
                       "appear in a config artefact")
    for artefact in shared:
        for ref in _PLAN_REF_RE.findall(artefact["contents"]):
            refuse(f"shared artefact `{artefact['path']}`", ref,
                   "cannot be resolved -- a placeholder may only appear in a "
                   "slice config artefact")
    return sorted(deferred)


def _slice_build_dir(build_root: Path, slice_: Slice) -> Path:
    """Per-slice build directory: build/<core>-<os>/."""
    return Path(build_root) / f"{slice_.core_id}-{slice_.os}"


def _slice_config_artefact(
    project: BoardProject,
    slice_: Slice,
) -> Optional[tuple[str, str]]:
    """(filename, contents) of the slice's config artefact, or None
    when the os has none.

    Single source for both a consumer's materialise step and
    `emit_build_plan` -- the two MUST agree byte-for-byte (the CLI
    consumer byte-writes the plan's contents and trusts them to match
    what we'd write ourselves).

    `baremetal` carries `alp-baremetal.cmake` -- and ONLY when the slice
    actually has compile-time guards to carry (absence-emits-nothing).
    This is NOT the old `cmake-args.txt` coming back: that file was
    removed in 2026-08 (#1278) precisely because no build command ever
    read it, and the test below is exactly the condition it failed --
    `_slice_command`'s baremetal branch pulls this file in with
    `-DCMAKE_PROJECT_INCLUDE=<abs path>`, so a slice that stops writing
    it stops compiling with its `ALP_BOARD_<SLUG>` / `ALP_SOM_<SKU>`
    guards, loudly (alplabai/tan-cli#551). The `=`-bearing cache entries
    from the same source ride the configure command line directly and are
    NOT duplicated here. The full human-readable `-D` listing remains
    also published as the plan's `cmake-args.txt` artefact
    (`_slice_cmake_args_artefact`) -- see docs/board-config-emit.md.
    """
    if slice_.os == "zephyr":
        return ("alp.conf", _slice_alp_conf(project, slice_))
    if slice_.os == "yocto":
        return ("local.conf", _slice_local_conf(project, slice_))
    if slice_.os == "baremetal":
        # Lazy, same buildplan<->orchestrator cycle avoidance
        # `emit_build_plan` uses for the slice-command helpers.
        from .orchestrator import (
            BAREMETAL_PROJECT_INCLUDE,
            _baremetal_project_include,
        )
        contents = _baremetal_project_include(project, slice_)
        if contents is None:
            return None
        return (BAREMETAL_PROJECT_INCLUDE, contents)
    return None


#: The os classes a slice's `alp.overlay` artefact is emitted for: the
#: classes `alp_project.py --emit dts-overlay` unions over. A yocto slice has
#: none. `cmake-args.txt` is emitted for the same two (`--emit cmake-args`).
_DTS_OVERLAY_OS = ("zephyr", "baremetal")

#: Filename of the rendered DTS overlay config artefact (under `buildDir`).
DTS_OVERLAY_ARTEFACT = "alp.overlay"

#: Filename of the rendered full `-D` listing config artefact.
CMAKE_ARGS_ARTEFACT = "cmake-args.txt"


class DtsOverlayUnavailable(OrchestratorError):
    """The board has nothing to render a DTS overlay from: no header under
    `include/alp/boards/`, or a SKU outside the production families. The one
    failure `emit_build_plan` downgrades to a `dts-overlay-unavailable`
    warning; every other `OrchestratorError` still fails the plan."""


def _v1_shaped_project(project: BoardProject) -> dict[str, Any]:
    """The legacy `board:`-wrapper dict the project-wide emitters read.

    The public board.yaml schema no longer uses this wrapper, but the
    in-file emitters (dts-overlay, hw-info-h, west-libraries) still consume
    it. Shared by `alp_project.py` and `_slice_dts_overlay`.
    """
    return {
        "som": {
            "sku":    project.sku,
            "hw_rev": project.hw_rev,
        },
        "pins": list(project.raw.get("pins") or []),
        "board": ({
            "name":   project.board_name,
            "hw_rev": project.board_hw_rev,
        } if project.board_name else None),
    }


def _slice_dts_overlay(project: BoardProject, slice_: Slice) -> str:
    """The slice's DTS overlay text -- exactly what
    `alp_project.py --emit dts-overlay --core <id>` prints.

    Single source for that standalone emit and the build plan's
    `alp.overlay` config artefact (ADR-0026 §D: `tan` consumes these bytes
    instead of re-rendering them). The overlay is shaped by the board
    header (bus aliases + `alp,pin-array`), a SoM-mounting fact; the slice
    contributes its peripheral list and, per-product, the assignable nodes
    board.yaml `ownership:` gave the M33.
    """
    # Same lazy sys.path dance `kconfig._slice_alp_conf` does: the
    # `alp_project_emit` package lives beside this one under scripts/ and a
    # top-level import would cycle.
    import sys as _sys
    _scripts = Path(__file__).resolve().parent.parent
    if str(_scripts) not in _sys.path:
        _sys.path.insert(0, str(_scripts))
    from alp_project_emit.dts import _emit_dts_overlay  # type: ignore

    from alp_project_loader import _sku_family  # type: ignore

    # Only the two facts the emitter itself cannot render are "unavailable"
    # (the plan degrades to a warning); everything else propagates.
    try:
        _sku_family(project.sku)
    except ValueError as exc:
        raise DtsOverlayUnavailable(str(exc)) from None
    try:
        out = _emit_dts_overlay(
            _v1_shaped_project(project), project.som_preset,
            project.board_preset,
            v2_peripherals=sorted(set(slice_.peripherals)),
            v2_core_id=slice_.core_id,
            v2_core_os=slice_.os,
            v2_core_ids=[slice_.core_id],
        )
    except SystemExit as exc:
        # The emitter `sys.exit`s when the board names no header under
        # include/alp/boards/. The standalone emit prints this and exits 1.
        msg = str(exc.code)
        raise DtsOverlayUnavailable(
            msg.removeprefix("alp_project: ")) from None
    # Outside the degrade path above: an M33 ownership defect must still
    # fail the plan, not turn into a missing artefact.
    own_dts, _ = project_m33_overlay(project, slice_.core_id)
    if own_dts:
        out += ("\n/* Assignable peripherals owned by the M33 "
                "(board.yaml `ownership:`). */\n" + "\n".join(own_dts) + "\n")
    return out


def _slice_dts_overlay_artefact(
    project: BoardProject,
    slice_: Slice,
) -> Optional[tuple[str, str]]:
    """(filename, contents) of the slice's rendered DTS overlay, or None for
    an os that has none (see `_DTS_OVERLAY_OS`)."""
    if slice_.os not in _DTS_OVERLAY_OS:
        return None
    return (DTS_OVERLAY_ARTEFACT, _slice_dts_overlay(project, slice_))


def _slice_cmake_args_artefact(
    project: BoardProject,
    slice_: Slice,
) -> Optional[tuple[str, str]]:
    """(filename, contents) of the slice's full `-D` listing -- exactly what
    `alp_project.py --emit cmake-args --core <id>` prints after its
    `# --- core ---` marker -- or None for an os that has none.

    A rendered REFERENCE listing for consumers (ADR-0026 §D). The
    `=`-bearing cache entries already ride the baremetal configure line and
    the bare guards arrive via `alp-baremetal.cmake`; this file is not read
    by any build command and so does not resurrect the dead `cmake-args.txt`
    #1278 removed -- it is the same text, now published in the plan so `tan`
    stops re-rendering it.
    """
    if slice_.os not in _DTS_OVERLAY_OS:
        return None
    return (CMAKE_ARGS_ARTEFACT, _slice_cmake_args(project, slice_))


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
    """This slice's compiler identity: `{targetTriple, compiler, sysroot, id}`
    (#610 §4 per-slice tooling index).

    Grounded in the SoM preset's `topology.<core>.toolchain` default, or a
    project's `cores.<core>.toolchain` override (issue #964) -- the same
    field `Slice.to_manifest_entry` already surfaces in
    `system-manifest.yaml` -- never invented.  For a Zephyr slice this
    value (e.g. `arm-zephyr-eabi`) IS the real Zephyr SDK toolchain
    directory name AND its GCC target triple, so `targetTriple` /
    `compiler` derive straight from it.  A Yocto slice's toolchain tag
    (`poky-glibc`) is a *category* (C-library flavour), not a literal
    GCC triple -- the real triple depends on the Yocto build's own
    TUNE_FEATURES/TCLIBC (outside board.yaml / SoM metadata), so
    `targetTriple` / `compiler` / `sysroot` stay null rather than
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
        "targetTriple": target_triple,
        "compiler":     compiler,
        "sysroot":      None,
        "id":           slice_.toolchain,
    }


#: An artifact block that claims nothing.  Shared by the runtimes with
#: no honest path to report (yocto) and by any slice whose `command` was
#: blocked, so the two can never drift apart.
_NULL_ARTIFACTS: dict[str, Optional[str]] = {
    "elf":             None,
    "map":             None,
    "bin":             None,
    "sizeReport":      None,
    "symbols":         None,
    "compileCommands": None,
    "outputDir":       None,
}


def _slice_artifacts(build_dir: Path, slice_: Slice,
                     has_command: bool = True) -> dict[str, Optional[str]]:
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
    the build dir root, not the `zephyr/` subdirectory.

    That build dir root is `<buildDir>/build/`, NOT `<buildDir>` --
    `_slice_command` deliberately emits no `-d` (finding M14: west
    resolves a relative `-d` against its own cwd, already `<buildDir>`,
    so `-d <buildDir>` would double-nest), and west's default output is
    `<cwd>/build`.  Every zephyr path below therefore carries that
    `build/` level.  It used to be missing (issue #1360): the plan named
    `<buildDir>/zephyr/zephyr.elf`, a path west never creates, and every
    consumer had to re-derive the real one from `buildDir` instead of
    reading the block it was given -- `tan renode` looked for
    `build/m55_he-zephyr/zephyr/zephyr.elf` and found nothing.  The
    `build/` level is west's, not this planner's, which is exactly why
    it belongs in the reported path and not in each reader's head.

    A Yocto slice's real output (the wic/ext4 image) lands under the
    *Yocto build tree's* own deploy dir -- outside this slice's
    `build_dir`, which only ever carries the `local.conf` fragment --
    so there is no honest path to report.

    `baremetal` reports the ONE path its configure line GUARANTEES
    (tan-cli#550 -- the whole block used to be null, so a slice that
    produced no binary at all was indistinguishable from one that built
    fine): `outputDir`.  `_slice_command` passes
    `-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$<1:<buildDir>/output>`, so every
    RUNTIME artifact the app links -- i.e. every `add_executable()`
    target -- lands there, on single- and multi-config generators alike
    (the `$<1:...>` generator expression is what suppresses the
    per-config subdirectory a multi-config generator would otherwise
    append).

    What `outputDir` does NOT license is the inverse reading.  An empty
    or absent `output/` after the build step does NOT prove the slice
    produced nothing: `CMAKE_RUNTIME_OUTPUT_DIRECTORY` governs
    `add_executable` targets ONLY, so a firmware app built as
    `add_library(fwcore STATIC ...)` plus a custom link/objcopy target
    builds cleanly (rc 0, `libfwcore.a` + `main.c.o` + `fw.elf`
    produced) and never creates `output/` at all -- measured, not
    assumed.  It is a deterministic PLACE TO LOOK, not a
    build-succeeded oracle.

    `compileCommands` stays null for baremetal even though the
    configure passes `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`: CMake
    implements that variable "only by Makefile Generators and Ninja
    Generators.  It is ignored on other generators" (CMake's own docs
    for the variable), and this planner does not choose the generator.
    On Windows, whose default generator is Visual Studio, the file is
    never written -- so naming the path here would be exactly the
    artifacts-lie tan-cli#550 is about, pointed the other way.  A zephyr
    slice's `compileCommands` is a different case and stays: `west
    build` drives Ninja and Zephyr's own top-level `CMakeLists.txt`
    forces the variable on.

    `elf` / `bin` / `map` / `sizeReport` / `symbols` stay null for
    baremetal: the executable's NAME is the app's own `CMakeLists.txt`
    to pick, not an SDK convention this emitter may invent -- pinning
    the DIRECTORY is the strongest honest claim, which is exactly what
    `outputDir` carries.

    `has_command` is False for a slice whose `command` was BLOCKED
    (`command-unrooted` / `board-tree-missing` / `no-command`): nothing
    will ever configure that build dir, so every path below would be a
    dangling promise pinned by a configure that never runs.  Such a
    slice reports an all-null block.
    """
    if not has_command:
        return dict(_NULL_ARTIFACTS)
    if slice_.os == "zephyr":
        # west's own build tree: `<buildDir>/build/`, because the command
        # runs with cwd=<buildDir> and no `-d` (see the docstring above
        # and `_slice_command`'s M14 note).
        wdir = build_dir / "build"
        zdir = wdir / "zephyr"
        return {
            "elf":             (zdir / "zephyr.elf").as_posix(),
            "map":             (zdir / "zephyr.map").as_posix(),
            "bin":             (zdir / "zephyr.bin").as_posix(),
            "sizeReport":      (zdir / "zephyr.stat").as_posix(),
            "symbols":         (zdir / "zephyr.symbols").as_posix(),
            "compileCommands": (wdir / "compile_commands.json").as_posix(),
            # Zephyr's own tree lands at `<buildDir>/build/` and the six
            # named paths above all index it -- no separate output
            # directory to report.
            "outputDir":       None,
        }
    if slice_.os == "baremetal":
        return dict(_NULL_ARTIFACTS,
                    outputDir=(build_dir / "output").as_posix())
    return dict(_NULL_ARTIFACTS)


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
    fact `tan flash` dispatches on, computed once per slice by the
    caller and passed in): `_slice_flash_recipe` no longer forces a
    runner for `zephyr_west_flash` slices (not every in-tree board
    registers `openocd`), so `probe` is the explicitly-configured
    runner when one is set, else null -- meaning "board.cmake default,
    not independently knowable here".  The Yocto image-flash recipe and
    the baremetal cmake recipe don't identify a live debug probe
    either, so `probe` stays null there too.
    """
    console_sel = _resolve_console(
        project.diagnostics.get("console"), slice_.os, slice_.hw_console)
    console = None if console_sel == "none" else console_sel
    probe: Optional[str] = None
    if flash_method == "zephyr_west_flash":
        probe = (flash_args or {}).get("runner")
    return {"console": console, "probe": probe}


def _sdk_version() -> Optional[str]:
    """The `version:` field out of `metadata/sdk_version.yaml` -- the single
    source `scripts/bump_version.py` bumps and `check_version_doc_sync.py`
    pins every other copy against.  Same read-and-strip idiom as
    `alp_cli._version` / `check_version_doc_sync.declared_version`, kept
    inline here rather than imported: neither of those lives on an import
    path this package can reach without a sys.path hack (`scripts/` itself
    carries no `__init__.py`).
    """
    sdk_version_yaml = REPO / "metadata" / "sdk_version.yaml"
    try:
        text = sdk_version_yaml.read_text(encoding="utf-8")
    except OSError:
        # No adjacent metadata/ tree (e.g. packaged as a wheel) -- provenance
        # is best-effort, never a reason to fail the emit.  Mirrors the
        # OSError guard in `alp_cli._version` and `_sdk_commit` below.
        return None
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("version:"):
            return stripped.split(":", 1)[1].split("#", 1)[0].strip()
    return None


def _sdk_commit() -> Optional[str]:
    """Short git commit of this checkout (`git rev-parse --short HEAD`), or
    `None` when git -- or a `.git` dir -- isn't available.  Mirrors the
    robustness of `scripts/build_receipt.py::_git_rev` (try/except over both
    `CalledProcessError` and a missing `git` binary) rather than importing
    it: that module isn't reachable from this package without the same
    sys.path workaround noted on `_sdk_version` above, and receipt's variant
    also resolves the FULL rev plus a dirty-tree flag this envelope doesn't
    need.
    """
    try:
        result = subprocess.run(
            ["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"],
            capture_output=True, text=True, encoding="utf-8", check=True)
    except (subprocess.CalledProcessError, OSError):
        return None
    commit = result.stdout.strip()
    return commit or None


def emit_build_plan(
    project: BoardProject,
    *,
    board_yaml: Path,
    build_root: Path,
) -> str:
    """Emit the machine-readable build plan as JSON (Wave C contract).

    This remains the reference/parity producer and the explicit input to
    Python Tan's `--plan-from` path. Tan's normal path uses its relocated
    in-process planner, then materialises files, runs commands, and owns
    scheduling/caching/progress UX. The
    Wave C contract was settled 2026-06-04 with the alp-sdk-vscode team
    (docs/PROPOSAL-alp-build-core.md records that settlement); the real
    parser lives in tan-cli, not the alp-sdk-vscode extension itself.

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
        so the consumer can still report the core.  Same treatment for
        a zephyr slice whose `board:` target has no tree under
        `zephyr/boards/alp/` (`board-tree-missing`, issue #999): the
        plan never carries a `west build -b <board>` command that is
        guaranteed to fail Zephyr's own board lookup.
      * Write-free: nothing is created on disk.  (Command resolution
        stats the app dir to pick the CMakeLists.txt convention --
        read-only, same as the build itself.)
      * Per-slice tooling index (#610 §4, additive to schemaVersion 1 --
        never renamed/removed, see `metadata/schemas/build-plan-v1.
        schema.json`): `toolchain` (compiler identity --
        `_slice_toolchain`, keys `targetTriple`/`compiler`/`sysroot`/`id`),
        `artifacts` (deterministic OUTPUT paths under `buildDir`, not a
        promise they exist yet -- `_slice_artifacts`, keys `elf`/`map`/
        `bin`/`sizeReport`/`symbols`/`compileCommands`), and `debug`
        (headless console/probe selectors -- `_slice_debug`).  A field
        genuinely not derivable for a runtime (e.g. a Yocto slice's exact
        GCC triple) is null, never guessed.  (These three sub-objects'
        keys were corrected from an accidental snake_case to camelCase
        to match the rest of this contract -- see the CHANGELOG.)
      * Envelope provenance, additive to schemaVersion 1: `sdkVersion`
        (`metadata/sdk_version.yaml`'s `version:`, via `_sdk_version`)
        and `sdkCommit` (`git rev-parse --short HEAD` of this checkout,
        via `_sdk_commit`; `null` when git/`.git` isn't available -- never
        raises) so a cached/materialised plan can be traced back to the
        planner that produced it.
    """
    # Orchestrator-side (stay inline until orchestrator.py); lazy to avoid
    # a buildplan<->package import cycle.
    from .orchestrator import (
        STOCK_IMAGE_APP,
        UnbuildableYoctoMachineError,
        UnknownBoardTargetError,
        UnrootedPathError,
        _resolve_app_path,
        _slice_command,
        _slice_flash_recipe,
        _slice_post_commands,
        _tokenize,
        iter_buildable_slices,
    )
    build_root = Path(build_root)
    # Anchor every slice's relative `app:` on the board.yaml's own
    # directory, never the emitting process's CWD -- the plan must be
    # byte-identical no matter where `--emit build-plan` is invoked from
    # (issue #596).
    base_dir = Path(board_yaml).resolve().parent
    slices_out: list[dict[str, Any]] = []
    warnings: list[dict[str, Any]] = []

    for slice_ in iter_buildable_slices(project):
        build_dir = _slice_build_dir(build_root, slice_)
        # `replace` keeps this emit side-effect free: _slice_command
        # reads `build_dir` off the slice (baremetal -B), and the
        # project's own Slice objects must stay untouched.
        try:
            cmd = _slice_command(
                project, replace(slice_, build_dir=build_dir),
                base_dir=base_dir)
        except UnrootedPathError as e:
            # One of _slice_command's five `_tokenize` call sites (west
            # build's app-dir arg, -DSB_CONF_FILE's two paths,
            # -DEXTRA_CONF_FILE, cmake -S) hit a path outside both
            # ${PROJECT_ROOT} and ${SDK_ROOT} (issue #865). Block the
            # command rather than ever let a bare absolute path reach
            # `command.args` on a plan tagged `planPathMode: tokened` --
            # same "carry the slice, never emit a broken/non-hermetic
            # command" convention as `no-command` below.
            cmd = None
            warnings.append({
                "code":    "command-unrooted",
                "coreId":  slice_.core_id,
                "message": (f"core '{slice_.core_id}' build command would "
                            f"embed a path outside both the project "
                            f"({base_dir}) and the SDK checkout ({REPO}): "
                            f"'{e}' -- blocked rather than emit a "
                            f"non-hermetic command"),
            })
        except UnknownBoardTargetError as e:
            # The SoM preset's topology named a Zephyr board with no
            # tree under zephyr/boards/alp/ (issue #999 one layer down
            # from the declaration gate, at emit time): block the
            # command rather than ever hand a consumer (tan, or a
            # customer's own `west build`) a `-b <board>` argument that
            # is guaranteed to die with Zephyr's own "No board named
            # ... Invalid BOARD" error -- same "carry the slice, never
            # emit a broken command" convention as `no-command` below.
            cmd = None
            warnings.append({
                "code":    "board-tree-missing",
                "coreId":  slice_.core_id,
                "message": str(e),
            })
        except UnbuildableYoctoMachineError as e:
            # The slice's `machine:` is a known-non-buildable Yocto MACHINE
            # (issue #1982 -- the two AEN A32-cluster carriers, unbuildable
            # for related but distinct reasons: see
            # `orchestrator.YOCTO_MACHINE_UNBUILDABLE`'s own comment,
            # issues #1968 / #1971): block the command rather than ever
            # hand a consumer a `bitbake` target that cannot succeed --
            # same "carry the slice, never emit a broken command"
            # convention as `no-command` below.
            cmd = None
            warnings.append({
                "code":    "yocto-machine-unbuildable",
                "coreId":  slice_.core_id,
                "message": str(e),
            })
        else:
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
        # A baremetal slice's `alp-baremetal.cmake` has exactly ONE reader:
        # the `-DCMAKE_PROJECT_INCLUDE=` arg on this slice's own configure.
        # If the command was blocked there is no reader, so materialising
        # the file would leave a consumer holding a path nothing will ever
        # open. (A zephyr/yocto artefact is a plain config fragment a human
        # can still read, so those are emitted either way.)
        artefact = (None if (cmd is None and slice_.os == "baremetal")
                    else _slice_config_artefact(project, slice_))
        if artefact is not None:
            name, contents = artefact
            config_artefacts.append({
                "path":     (build_dir / name).as_posix(),
                "contents": contents,
            })
        # Additive rendered-text artefacts (ADR-0026 §D, tan-cli#1216): the
        # DTS overlay and the full `-D` listing, byte-identical to the
        # standalone `--emit dts-overlay --core` / `--emit cmake-args`
        # renders. Always AFTER the slice's primary config artefact, so a
        # consumer that reads `configArtefacts[0]` is unaffected.
        extras: list[Optional[tuple[str, str]]] = []
        # Same dangling-path rule as `alp-baremetal.cmake` above: a baremetal
        # slice whose command was blocked will never configure its build dir,
        # so promise it nothing.
        if not (cmd is None and slice_.os == "baremetal"):
            try:
                extras.append(_slice_dts_overlay_artefact(project, slice_))
            except DtsOverlayUnavailable as exc:
                # Additive artefact: a board with no header (or an
                # unrecognised SKU) must not stop a plan that emitted fine
                # before. The consumer sees the warning and falls back.
                # Any other OrchestratorError still fails the plan.
                warnings.append({
                    "code":    "dts-overlay-unavailable",
                    "coreId":  slice_.core_id,
                    "message": (f"core '{slice_.core_id}': no `alp.overlay` "
                                f"artefact -- {exc}"),
                })
            extras.append(_slice_cmake_args_artefact(project, slice_))
        for extra in extras:
            if extra is not None:
                config_artefacts.append({
                    "path":     (build_dir / extra[0]).as_posix(),
                    "contents": extra[1],
                })
        # `appDir` retains the resolved source directory independent of
        # `command` -- tooling that wants the app source (e.g. to watch
        # it for incremental rebuilds) doesn't have to reverse-engineer
        # it out of a yocto/zephyr/baremetal-shaped command (issue #597).
        # `alp-image-edge` is the A-core stock-image token, not a source
        # path -- there is no app dir to report for it.
        # Tokenized (issue #865): almost every `app:` resolves under the
        # project (`${PROJECT_ROOT}/...`); the SDK-owned stock M-core shim
        # (STOCK_SHIM_APP) resolves under the SDK checkout instead
        # (`${SDK_ROOT}/...`). `_tokenize` raises `UnrootedPathError` when
        # it is under neither -- fall back to the absolute path (still
        # useful for tooling) but flag it rather than let it silently
        # mis-root on the consumer side.
        app_dir = None
        if slice_.app and slice_.app != STOCK_IMAGE_APP:
            resolved_app = _resolve_app_path(slice_.app, base_dir)
            try:
                app_dir = _tokenize(resolved_app, base_dir, REPO)
            except UnrootedPathError:
                app_dir = resolved_app.as_posix()
                warnings.append({
                    "code":    "appdir-unrooted",
                    "coreId":  slice_.core_id,
                    "message": (f"core '{slice_.core_id}' app path "
                                f"'{app_dir}' is outside both the project "
                                f"({base_dir}) and the SDK checkout "
                                f"({REPO}) -- emitted absolute; a consumer "
                                f"on a different checkout cannot re-root "
                                f"it"),
                })
        # Same flash-recipe fact `Slice.to_manifest_entry` surfaces to
        # `tan flash` -- reused here (not re-derived) so `debug.probe`
        # can never drift from the manifest's own `flash_method`/`flash_args`.
        flash_method, flash_args = _slice_flash_recipe(slice_)
        slices_out.append({
            "coreId":          slice_.core_id,
            "backend":         slice_.os,
            "buildDir":        build_dir.as_posix(),
            "appDir":          app_dir,
            "configArtefacts": config_artefacts,
            "toolchain":       _slice_toolchain(slice_),
            "artifacts":       _slice_artifacts(build_dir, slice_,
                                                has_command=cmd is not None),
            "debug":           _slice_debug(
                project, slice_, flash_method, flash_args),
            "command": None if cmd is None else {
                "tool": cmd[0],
                "args": cmd[1:],
                "cwd":  build_dir.as_posix(),
            },
            # Steps the executor MUST run, in order, after `command`
            # succeeds -- empty for the runtimes whose single tool
            # invocation already configures AND builds (`west build`,
            # `bitbake`). A baremetal slice's `cmake -S ... -B .` only
            # CONFIGURES, so its `cmake --build .` lives here; without it
            # the slice exited 0 having produced no object file, archive
            # or executable at all (tan-cli#550). Empty whenever
            # `command` is null: there is nothing to build on top of a
            # slice that was never configured.
            "postCommands": [] if cmd is None else [
                {
                    "tool": step[0],
                    "args": step[1:],
                    "cwd":  build_dir.as_posix(),
                }
                for step in _slice_post_commands(slice_)
            ],
            # Tokened (issue #865), not the native host-path form: tan-cli
            # (PR #24) substitutes ${SDK_ROOT} with its own checkout root
            # before handing this to the slice subprocess environment, so
            # a cached/materialised plan never carries THIS run's absolute
            # path onto a different machine/checkout.
            "env": {"ALP_SDK_ROOT": "${SDK_ROOT}"},
            # SDK-owned values the consumer APPENDS to its own env, distinct
            # from `env` above (set-verbatim). The join separator is
            # PER-KEY, not uniformly os.pathsep: EXTRA_ZEPHYR_MODULES is a
            # CMake list Zephyr's zephyr_module.py splits on `;` on every
            # platform (not an OS path list), while PYTHONPATH is a real
            # OS-native path list (os.pathsep). Mirrors the append
            # `_alp_common.env_with_sdk` / `_workspace.subprocess_env` do
            # for a real `west build` (ADR-0020 item 3). Tokened same as
            # `env` above (issue #865).
            "envAppendPath": {
                "EXTRA_ZEPHYR_MODULES": ["${SDK_ROOT}"],
                "PYTHONPATH":           ["${SDK_ROOT}/scripts"],
            },
        })

    shared_out = [
        {"path": p.as_posix(), "contents": c}
        for p, c in _shared_artefacts(project, build_root)
    ]
    plan: dict[str, Any] = {
        "schemaVersion":   1,
        # Additive to schemaVersion 1 (issue #865): every path in this plan
        # that anchors on THIS checkout or THIS project is now emitted as
        # a `${SDK_ROOT}`/`${PROJECT_ROOT}`/`${PYTHON}` token rather than a
        # baked-in absolute path -- tan-cli's plan reader requires this literal
        # value before it will substitute them. `boardYaml` is deliberately
        # NOT tokenized (kept repo-relative as-passed) -- it is the anchor
        # both this plan's own comparator and a plan consumer use to locate
        # PROJECT_ROOT in the first place.
        "planPathMode":    "tokened",
        # Additive (issue #2696): the `${NAME}` values left unresolved on
        # purpose, so a consumer never mistakes one for a plan path token
        # above. Always present, `[]` when there are none, so a consumer can
        # tell "none" from a pre-#2696 plan that does not carry the field.
        "deferredPlaceholders": _deferred_placeholders(
            slices_out, shared_out, board_yaml),
        "generatedBy":     "scripts/alp_orchestrate.py",
        # Additive provenance (ADR 0014's additive rule -- no schemaVersion
        # bump): traces a cached/materialised plan back to the planner that
        # produced it. `sdkCommit` is null, never a crash, when git/`.git`
        # isn't available (e.g. a wheel-installed CLI with no checkout).
        "sdkVersion":      _sdk_version(),
        "sdkCommit":       _sdk_commit(),
        "boardYaml":       Path(board_yaml).as_posix(),
        "sku":             project.sku,
        "buildRoot":       build_root.as_posix(),
        "executionPolicy": _EXECUTION_POLICY,
        "slices":          slices_out,
        "sharedArtefacts": shared_out,
        "warnings":        warnings,
    }
    return json.dumps(plan, indent=2) + "\n"
