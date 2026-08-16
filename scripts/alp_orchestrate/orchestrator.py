#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Slice-command resolution -- the planner-side helpers `buildplan.py`'s
`emit_build_plan` reads to describe (never run) each slice's build command.

ADR-0020 Phase 4 (preview) retired the SDK-side executor -- the
`Orchestrator` class that fanned build sub-processes out and materialised
artefacts to disk.  What remains here is pure, side-effect-free: resolving
what a slice's build command WOULD be, so `emit_build_plan` can describe the
SDK reference plan to parity tooling, Alp Studio, or Python Tan's explicit
`--plan-from` path. Normal Tan builds use the relocated equivalent in process.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Optional

import yaml

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
        #
        # `jlink_flash_device` is added only when this slice's SoC
        # variant actually publishes one: it's the part-number J-Link
        # profile that arms Flow D's built-in Alif MRAM loader, a fact a
        # downstream consumer (tan) needs to pick that path over the
        # SETOOLS/SE-UART fallback. Absent for every non-AEN slice today,
        # so the args dict stays `{}` -- no shape change for them.
        #
        # `expect_dpidr` + `jlink_device` are the read-only SW-DP IDR
        # wrong-board preflight (#1355) and are emitted as ONE inseparable
        # pair: the expected debug-port ID, and the live-core attach profile
        # the read is performed with. Without them a real MRAM write can
        # proceed against whatever board answered -- measured on silicon
        # 2026-08-10, four transcripts, `ISSUES = []` and no guard -- because
        # `JLinkExe` selects a probe only by serial (`-SelectEmuBySN`) and
        # this bench has an OEM-cloned serial across two probes on different
        # boards. Emitting exactly one of the two is NOT a partial win: tan's
        # `validate_flow_d_preflight_args` refuses a half-armed pair at plan
        # time, so it would hard-fail every AEN flash including a dry run.
        # `loader._resolve_flow_d_preflight` guarantees the both-or-neither
        # shape upstream; the `and` here makes that guarantee locally
        # readable rather than assumed at a distance.
        #
        # `slot0_load_address` (tan-cli#353) is the AEN MRAM slot0-XIP
        # address the slice's application blob is linked at -- the fact
        # Flow D's auto-sign-via-SETOOLS path needs and, before this,
        # alp-sdk never emitted, so tan correctly armed Flow D and then
        # refused (`flash_args.slot0_load_address is required`), forcing a
        # customer to hand-edit `system-manifest.yaml`. Independent of the
        # `expect_dpidr`/`jlink_device` pair above -- see
        # `loader._resolve_slot0_load_address` for where it comes from.
        args: dict[str, Any] = {}
        # PRESENCE, not truthiness (#1295, tan-cli#734): a schema-declared
        # `jlink_flash_device: null` is a published "no known J-Link flash
        # profile" and must reach `flash_args` AS a present null, so the
        # consumer's presence-based check (tan's `flash_plan._fa_has_key`)
        # sees it and refuses loudly.  A truthiness test drops it, which
        # re-collapses declared-null into absent and silently downgrades
        # Flow D to the SE-UART Flow A path -- the exact failure the null
        # exists to prevent.  Absent still emits nothing.
        if slice_.jlink_flash_device_declared or slice_.jlink_flash_device is not None:
            args["jlink_flash_device"] = slice_.jlink_flash_device
        if slice_.expect_dpidr and slice_.jlink_device:
            args["expect_dpidr"] = slice_.expect_dpidr
            args["jlink_device"] = slice_.jlink_device
        if slice_.slot0_load_address:
            args["slot0_load_address"] = slice_.slot0_load_address
        return ("zephyr_west_flash", args)
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


class UnrootedPathError(ValueError):
    """Raised by `_tokenize` when a path resolves under neither
    `${PROJECT_ROOT}` nor `${SDK_ROOT}` (issue #865's split-brain guard).

    A single exception type raised from the ONE place paths get tokenized
    means every call site -- the five command-arg sites in `_slice_command`
    plus `buildplan.py`'s `appDir` -- is guarded uniformly by construction;
    nothing can add a sixth call site and forget the check. Callers catch
    this and turn it into a `warnings` entry (never a hard crash of the
    whole emit), matching the existing `no-command`/`yocto-recipe-missing`
    "block the one broken slice, keep going" convention.
    """


def _real_zephyr_board_names(repo: Path) -> set[str]:
    """Every Zephyr board name that actually has a tree under
    `zephyr/boards/alp/`, read from each tree's own `board.yml`
    `board: name:` field -- existence, not naming shape, is what makes a
    target buildable. Mirrors `check_board_target_tree_parity.py`'s
    `_load_real_board_names` (issue #999's own list of what's real),
    duplicated here rather than imported: that script is a CI gate over
    declared SoM presets, this is a runtime planner check over one
    resolved slice, and a ~10-line directory glob is cheaper than
    cross-importing a top-level gate script into the package.
    """
    boards_dir = repo / "zephyr" / "boards" / "alp"
    names: set[str] = set()
    if not boards_dir.is_dir():
        return names
    for board_yml in sorted(boards_dir.glob("*/board.yml")):
        doc = yaml.safe_load(board_yml.read_text(encoding="utf-8")) or {}
        name = (doc.get("board") or {}).get("name")
        if isinstance(name, str):
            names.add(name)
    return names


class UnknownBoardTargetError(ValueError):
    """Raised by `_slice_command` when a zephyr slice's `board:` target
    (resolved from the SoM preset's `topology.<core>.board:`) names no
    tree under `zephyr/boards/alp/` (issue #999 one layer down: the
    *declaration* gate `check_board_target_tree_parity.py` already
    flags this at the metadata level; this is the same fact checked at
    *emit* time, so the planner never hands a consumer -- `tan`, `west`
    itself -- a `west build -b <board>` command that is guaranteed to
    fail with Zephyr's own "No board named ... Invalid BOARD" error).

    Carries the SKU/core/board/what-exists facts so the caller can
    render them in the customer's own terms, same as `UnrootedPathError`
    above.
    """

    def __init__(self, sku: str, core_id: str, board: str,
                 real_boards: set[str]) -> None:
        self.sku = sku
        self.core_id = core_id
        self.board = board
        self.real_boards = real_boards
        super().__init__(
            f"SoM '{sku}' core '{core_id}' wants Zephyr board '{board}', "
            f"which has no tree under zephyr/boards/alp/ -- board bring-up "
            f"for this target has not happened yet."
        )


def _tokenize(path: Path, base_dir: Path, repo: Path) -> str:
    """Render an absolute path as a portable `${PROJECT_ROOT}`/`${SDK_ROOT}`
    token (issue #865) instead of baking in THIS checkout's absolute path.
    tan-cli's plan reader substitutes both tokens at materialise time, so a plan
    emitted on one machine/checkout can be materialised faithfully on
    another -- the split-brain risk a baked-in absolute path invites.

    Prefers `${PROJECT_ROOT}` (the project's own board.yaml directory) since
    most anchored paths are project-owned; falls back to `${SDK_ROOT}` for
    paths the SDK itself owns (e.g. the family sysbuild base, the stock
    M-core shim app). Raises `UnrootedPathError` for a path under neither
    root -- never returns a bare absolute path, so a non-hermetic path can
    never silently reach a `planPathMode: tokened` plan.
    """
    path = Path(path)
    for root, token in ((base_dir, "${PROJECT_ROOT}"), (repo, "${SDK_ROOT}")):
        try:
            rel = path.relative_to(root)
        except ValueError:
            continue
        return token if rel == Path(".") else f"{token}/{rel.as_posix()}"
    raise UnrootedPathError(path.as_posix())


def _slice_command(
    project: BoardProject,
    slice_: Slice,
    base_dir: Path,
) -> Optional[list[str]]:
    """Resolve the build command for a slice.  Returns None when there is no
    buildable command yet -- the caller carries the slice as `skipped` /
    `no-command`, never dropped.  Raises `UnrootedPathError` /
    `UnknownBoardTargetError` for a slice the plan must block rather than
    mis-emit.

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
        real_boards = _real_zephyr_board_names(REPO)
        # `board:` may carry Zephyr's extended `board/soc/variant`
        # qualifier (e.g. AEN801's `alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/
        # rtss_hp`) -- a tree's board.yml `name:` is always the bare board
        # only, so check existence on the bare form (same `bare =
        # raw.split()[0].split("/")[0]` normalisation
        # check_board_target_tree_parity.py uses); the full qualified
        # string still goes to `west build -b` unchanged below.
        bare_board = slice_.board.split()[0].split("/")[0]
        if bare_board not in real_boards:
            raise UnknownBoardTargetError(
                project.sku, slice_.core_id, slice_.board, real_boards)
        # NB: no explicit `-d`. west's default output is <cwd>/build (a
        # subdirectory of the command's cwd = buildDir), so the tree lands
        # at <buildDir>/build/. Adding `-d <buildDir>` here would
        # double-nest (west resolves a relative -d against its cwd,
        # already = buildDir) -- see finding M14.
        #
        # The consumer does NOT have to reconcile that nesting any more:
        # `_slice_artifacts` reports the `<buildDir>/build/...` paths west
        # actually writes (issue #1360). `-d .` was the alternative -- it
        # would move west's tree to <buildDir> itself and make the old
        # un-nested `artifacts` spelling true -- and was REJECTED: this
        # slice's `alp.conf` is materialised at <buildDir>/alp.conf and
        # handed to this very command via `-DEXTRA_CONF_FILE=`, so making
        # <buildDir> west's own build dir puts that file inside the tree
        # `west build -p` (or a `--pristine=auto` board/app change) wipes,
        # deleting the fragment the command line points at. Today it sits
        # one level ABOVE west's tree, where pristine cannot reach it.
        # Tokenized (issue #865): `_zephyr_app_dir` resolves an absolute
        # path (project-anchored for a customer app, SDK-anchored for the
        # stock M-core shim) -- see `_tokenize`.
        cmd = [
            "west", "build",
            "-b", slice_.board,
            _tokenize(_zephyr_app_dir(slice_.app, base_dir), base_dir, REPO),
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
        # ${PYTHON} token, not `sys.executable` (issue #865): this plan is
        # emitted once by whichever Python ran the planner, but may be
        # materialised on a different host/checkout than that -- baking in
        # a concrete interpreter path here would pin THIS run's Python, not
        # the consumer's. tan-cli's `--plan-from` path substitutes ${PYTHON} with its
        # own resolved interpreter (forward-slashed for the same CMake
        # backslash-escape reason issue #787/#849 fixed: CMake parses
        # `\U`/`\N` etc. in a Windows path as an invalid character escape)
        # at materialise time; the SDK never emits a bare fallback here.
        defines = ["-DPython3_EXECUTABLE=${PYTHON}"]
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
                # #849 fixed for -DPython3_EXECUTABLE. `_tokenize` (#865)
                # already emits posix-form tokens, so this holds for both:
                # `sb_conf` is project-anchored (`${PROJECT_ROOT}/...`),
                # `family_base` is SDK-anchored (`${SDK_ROOT}/...`) -- the
                # `;`-joined list correctly carries both roots.
                sb_conf_files = (
                    [_tokenize(family_base, base_dir, REPO),
                     _tokenize(sb_conf, base_dir, REPO)]
                    if family_base else [_tokenize(sb_conf, base_dir, REPO)]
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
            defines.append(
                f"-DEXTRA_CONF_FILE={_tokenize(alp_conf, base_dir, REPO)}")
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
        # `-S` tokenized (issue #865) same as the zephyr app-dir arg above.
        app_path = _tokenize(_resolve_app_path(slice_.app, base_dir),
                              base_dir, REPO)
        # `-B .`, NOT `-B <slice_.build_dir>`: the plan pins this command's
        # `cwd` to the slice's buildDir (schema: "always equal to this
        # slice's buildDir"), and cmake resolves a relative `-B` against
        # its own cwd -- so the historical `-B build/<core>-baremetal`
        # double-nested the tree at `<buildDir>/build/<core>-baremetal/`,
        # where nothing that reads `artifacts`/`buildDir` would ever find
        # it (tan-cli#550). The zephyr branch avoids that SECOND level by
        # emitting no `-d` at all (finding M14, above); the one level west
        # still adds on its own is reported honestly by `_slice_artifacts`
        # rather than left for the reader to add back (issue #1360).
        return [
            "cmake", "-S", app_path, "-B", ".",
            # Every `-D` below is set BY THIS PLANNER, not by the customer,
            # and an app is free to consume none of them -- CMake would then
            # end each configure with `CMake Warning: Manually-specified
            # variables were not used by the project: ALP_SOM_FAMILY,
            # ALP_TOOLCHAIN, ...`, a warning about the planner's own
            # behaviour that the customer can neither act on nor silence.
            "--no-warn-unused-cli",
            # A best-effort IDE convenience, NOT a promise: CMake implements
            # `CMAKE_EXPORT_COMPILE_COMMANDS` "only by Makefile Generators
            # and Ninja Generators. It is ignored on other generators"
            # (CMake's own docs for the variable). This planner does not
            # choose the generator, so `artifacts.compileCommands` stays
            # null -- see `buildplan._slice_artifacts`.
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
            # Pins WHERE the slice's linked executables land
            # (`artifacts.outputDir`). The app's own CMakeLists.txt picks
            # the target NAMES -- not an SDK convention this planner may
            # invent -- so the directory is the strongest honest claim
            # available.
            #
            # ABSOLUTE (tokened, #865) on purpose: CMake resolves a RELATIVE
            # CMAKE_RUNTIME_OUTPUT_DIRECTORY against each target's own
            # CMAKE_CURRENT_BINARY_DIR, so a relative value would scatter a
            # multi-subdirectory app's outputs instead of pinning them.
            #
            # Wrapped in the no-op generator expression `$<1:...>` on
            # purpose too: "Multi-configuration generators (Visual Studio,
            # Xcode, Ninja Multi-Config) append a per-configuration
            # subdirectory to the specified directory UNLESS A GENERATOR
            # EXPRESSION IS USED" (CMake's own RUNTIME_OUTPUT_DIRECTORY
            # docs). Without it the plan says `<buildDir>/output` while the
            # binary is at `<buildDir>/output/Debug/` on every
            # multi-config generator -- measured on Ninja Multi-Config, and
            # the default generator on Windows (Visual Studio) is one.
            f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=$<1:"
            f"{_tokenize(_baremetal_output_dir(slice_, base_dir), base_dir, REPO)}>",
            # tan-cli#551: the slice's `-DALP_SOM_SKU` / `-DALP_SOM_FAMILY`
            # / `-DALP_CORE_ID` / `-DALP_TOOLCHAIN` / NPU-dispatch cache
            # entries. They used to be rendered ONLY into a `cmake-args.txt`
            # nothing ever read, then dropped outright with that artefact
            # (#1278) -- the configure has never carried a single one of
            # them. The bare `#if defined(...)` guards from the same source
            # (`ALP_BOARD_<slug>`, `ALP_SOM_<SKU>`) CANNOT ride here (cmake
            # rejects a `-D` with no `=value`); they arrive as real compiler
            # definitions via -DCMAKE_PROJECT_INCLUDE below.
            *_baremetal_cache_args(project, slice_),
            *_baremetal_project_include_arg(project, slice_, base_dir),
        ]
    return None


def _baremetal_output_dir(slice_: Slice, base_dir: Path) -> Path:
    """Absolute directory a baremetal slice's linked executables land in.

    Anchored on `base_dir` (the board.yaml's own directory), never
    `Path.cwd()`, so the emitted plan is byte-identical wherever it is
    emitted from -- the same rule `_resolve_app_path` and the zephyr
    branch's `-DEXTRA_CONF_FILE` follow (issue #596).
    """
    out = Path(slice_.build_dir)
    if not out.is_absolute():
        out = Path(base_dir) / out
    return (out / "output").resolve()


#: Filename of the generated CMake include a baremetal slice's configure
#: pulls in via `-DCMAKE_PROJECT_INCLUDE`.  Shared by `_slice_command`
#: (which references it) and `buildplan._slice_config_artefact` (which
#: renders its contents) so the two cannot name different files.
BAREMETAL_PROJECT_INCLUDE = "alp-baremetal.cmake"


def _baremetal_alp_lines(project: BoardProject, slice_: Slice) -> list[str]:
    """The slice's `-D` lines, read from the SINGLE source that renders
    them -- `kconfig.py::_slice_cmake_args`, the same text
    `alp_project.py --emit cmake-args` prints -- so the plan's configure
    and that emit can never drift.

    That emit is a TEXT format, not an argv: it opens with a
    `# Auto-generated ...` banner, and `libraries.baremetal_cmake_args`
    contributes `# library <name>: <cmake hint>` lines that are prose FOR
    A HUMAN (no `-D` in them at all).  Only real flags survive here --
    handing cmake a `#`-prefixed line would make it a stray source-dir
    argument, not a define.
    """
    # Lazy: `kconfig` is a heavier sibling, and `buildplan` already imports
    # this module lazily for the same cycle-avoidance reason.
    from .kconfig import _slice_cmake_args

    return [line for line in _slice_cmake_args(project, slice_).splitlines()
            if line.startswith("-D")]


def _baremetal_cache_args(
    project: BoardProject,
    slice_: Slice,
) -> list[str]:
    """The `-DNAME=VALUE` subset of the slice's `-D` lines -- CMake CACHE
    entries, safe to pass on the configure command line verbatim.

    Split from the bare `-DNAME` guards on the one thing that actually
    distinguishes them: `cmake -D` REQUIRES `VAR[:type]=value` and exits
    1 with `Parse error in command line argument: ALP_BOARD_E1M_EVK /
    Should be: VAR:type=value` on anything else (measured, not assumed).
    `docs/board-config-emit.md` documents that split from the other side.
    """
    return [line for line in _baremetal_alp_lines(project, slice_)
            if "=" in line]


def _baremetal_compile_guards(
    project: BoardProject,
    slice_: Slice,
) -> list[str]:
    """The bare `-DNAME` subset of the slice's `-D` lines, `-D` stripped.

    These are compile-time `#if defined(...)` guards, NOT cache entries:
    `ALP_BOARD_<SLUG>` gates `include/alp/board.h`'s board facade and
    `ALP_SOM_<SKU>` gates the per-SKU override block in the generated
    `<alp/soc_caps.h>`.  A CMake cache variable of the same name is
    invisible to the preprocessor, so they only do their job as real
    compiler definitions -- which is what `_baremetal_project_include`
    turns them into.  Zephyr slices get the identical guards through
    `CONFIG_COMPILER_OPT="-D..."` in their alp.conf.
    """
    return [line[len("-D"):]
            for line in _baremetal_alp_lines(project, slice_)
            if "=" not in line]


def _baremetal_project_include(
    project: BoardProject,
    slice_: Slice,
) -> Optional[str]:
    """Contents of the slice's generated `alp-baremetal.cmake`, or None
    when the slice needs no compile-time guards.

    Wired into the configure as `-DCMAKE_PROJECT_INCLUDE=<abs path>`,
    which CMake includes at the end of every `project()` call (CMake
    3.15+, comfortably below the repo-wide `cmake_minimum_required(VERSION
    3.20)`).  Deliberately NOT `-DCMAKE_C_FLAGS=-DALP_BOARD_...`: setting
    that variable from the command line seeds the cache entry itself, so a
    firmware toolchain file's `CMAKE_C_FLAGS_INIT` (`-mcpu=cortex-m55
    -mfloat-abi=hard`, ...) would never be applied -- silently building the
    slice for the wrong core.  `add_compile_definitions` only ADDS, and
    cannot drop a flag.
    """
    guards = _baremetal_compile_guards(project, slice_)
    if not guards:
        return None
    lines = [
        "# Auto-generated by scripts/alp_orchestrate -- do not edit.",
        "# Pulled in by the slice's configure via -DCMAKE_PROJECT_INCLUDE.",
        "# Compile-time guards <alp/board.h> / <alp/soc_caps.h> test with",
        "# #if defined(...); a CMake cache variable would not reach the",
        "# preprocessor at all.",
        f"add_compile_definitions({' '.join(guards)})",
    ]
    return "\n".join(lines) + "\n"


def _baremetal_project_include_arg(
    project: BoardProject,
    slice_: Slice,
    base_dir: Path,
) -> list[str]:
    """`['-DCMAKE_PROJECT_INCLUDE=<abs tokened path>']`, or `[]` when the
    slice has no guards to carry (absence-emits-nothing: no dangling
    reference to a file `_slice_config_artefact` would not have written).

    ABSOLUTE and `base_dir`-anchored, never cwd-relative: CMake resolves a
    relative `CMAKE_PROJECT_INCLUDE` against the SOURCE dir of the
    `project()` that pulls it in, which is the app's tree, not the slice's
    build dir. Same anchoring rule as the zephyr branch's
    `-DEXTRA_CONF_FILE` (issue #596) and tokened the same way (#865).
    """
    if _baremetal_project_include(project, slice_) is None:
        return []
    inc = Path(slice_.build_dir)
    if not inc.is_absolute():
        inc = Path(base_dir) / inc
    inc = (inc / BAREMETAL_PROJECT_INCLUDE).resolve()
    return [f"-DCMAKE_PROJECT_INCLUDE={_tokenize(inc, base_dir, REPO)}"]


def _slice_post_commands(slice_: Slice) -> list[list[str]]:
    """Argv steps that MUST run, in order, AFTER the slice's `command`.

    `west build` (zephyr) and `bitbake` (yocto) each configure AND build in
    one invocation, so they need none. `cmake -S ... -B ...` only
    CONFIGURES: without the `cmake --build` step below, a baremetal slice
    exits 0 having produced a `CMakeCache.txt` and no object file, no
    archive and no executable -- a green build over an empty output
    directory (tan-cli#550).

    `.` (not the build dir path) for the same cwd-relative reason
    `_slice_command`'s baremetal `-B .` uses; the caller pairs every step
    with the slice's buildDir as its `cwd`.

    No `--parallel`: job-count is the consumer's scheduling policy, the
    same reason the plan carries no `sequential` key.
    """
    if slice_.os == "baremetal":
        return [["cmake", "--build", "."]]
    return []


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
