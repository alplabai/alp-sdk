#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Load + validate a board.yaml project config and emit the per-backend
native config it compiles down to.

Usage:

    # Emit a Zephyr Kconfig fragment from ./board.yaml to stdout:
    python3 scripts/alp_project.py

    # Same, explicit:
    python3 scripts/alp_project.py --input board.yaml --emit zephyr-conf

    # Plain-CMake -D args:
    python3 scripts/alp_project.py --emit cmake-args

    # Yocto local.conf snippet:
    python3 scripts/alp_project.py --emit yocto-conf

    # Per-core natural-vs-effective OS facts (JSON; for IDEs / tooling):
    python3 scripts/alp_project.py --emit os-topology

    # Write to a file (typical Zephyr usage: included by prj.conf):
    python3 scripts/alp_project.py --emit zephyr-conf \\
        --output build/generated/alp.conf

The loader resolves:
  - The SoM SKU preset under metadata/e1m_modules/<SKU>.yaml
  - The shared board definition under metadata/boards/<preset>.yaml
    (when board.yaml uses `preset:`), OR the inline top-level
    `populated:` + `e1m_routes:` block (when board.yaml defines
    its board inline)

Then emits the appropriate native config.  For Zephyr this is a
.conf file the build appends to prj.conf; for plain CMake a
sequence of `-D` args; for Yocto a local.conf snippet.

Errors are reported with a one-line summary + the underlying
schema / file path so failures are debuggable.

Dependencies (standard CPython 3.10+ stdlib + two well-established
pip packages):
  - PyYAML  (yaml parser)
  - jsonschema  (already used by validate_metadata.py)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_project: PyYAML is required.  Install via `pip install pyyaml`.")

try:
    import jsonschema  # type: ignore[import-untyped]
except ImportError:
    sys.exit("alp_project: jsonschema is required.  Install via `pip install jsonschema`.")

# ---------------------------------------------------------------------
# Loader + emitter modules (issue #459 split)
# ---------------------------------------------------------------------
#
# `alp_project.py` stays the CLI entry point + the byte-stable import
# surface (`import alp_project` / `from alp_project import <name>`):
# CMakeLists.txt invokes this file by path, and the west commands +
# `alp_orchestrate/` + the test suite import internals straight off the
# `alp_project` module name.  The re-exports below are that surface --
# every name a caller outside this file reaches for lives in one of the
# two seams and is re-exported here unchanged, so moving the
# implementation into `alp_project_loader.py` / `alp_project_emit.py` is
# a structural split only (no behaviour change; see check_emit_snapshots.py
# and the emit-mode tests for the byte-identical-output pin).
from alp_project_loader import (  # noqa: F401  (compat re-export)
    METADATA_ROOT,
    _compose_route,
    _load_yaml,
    _resolve_board,
    _resolve_inline_or_preset_board,
    _resolve_pad_routes,
    _resolve_silicon_variant,
    _resolve_sku,
    _sku_family,
    _validate_and_load,
    resolve_capabilities,
    resolve_memory_map,
    silicon_to_kconfig,
    som_unpopulated_capabilities,
)
from alp_project_emit import (  # noqa: F401  (compat re-export)
    _CHIP_SUBSYSTEMS,
    _LIBRARY_KCONFIG,
    _PERIPHERAL_KCONFIG,
    _SOC_FAMILY_TOKEN,
    _emit_carrier_netlist,
    _emit_composed_route_table,
    _emit_dts_overlay,
    _emit_hw_info_h,
    _emit_library_hw_backends,
    _emit_native_sim_overlay,
    _emit_west_libraries,
)


# ---------------------------------------------------------------------
# v2 emit shims
# ---------------------------------------------------------------------
#
# The orchestrator (scripts/alp_orchestrate/) owns the v2 board.yaml
# loader + carve-out resolver + system-manifest emitter.  These shims
# route the v2-only `--emit` modes (and the per-core
# `--emit zephyr-conf --core <id>`) through the orchestrator.


def _write_or_print(out: str, target: Path | None) -> int:
    if target is not None:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(out, encoding="utf-8")
        try:
            rel = target.relative_to(Path.cwd())
        except ValueError:
            rel = target
        print(f"alp_project: wrote {rel} ({len(out)} bytes)",
              file=sys.stderr)
    else:
        sys.stdout.write(out)
    return 0


def _run_v2_emit(args: argparse.Namespace) -> int:
    """Handle the three project-level v2 emit modes."""
    if args.core is not None:
        print(f"alp_project: --core is ignored for --emit {args.emit} "
              f"(project-level emit)", file=sys.stderr)
    try:
        # Imported here so the v1 path doesn't pay the import cost when
        # the orchestrator module is being modified in-tree.
        from alp_orchestrate import (
            OrchestratorError,
            emit_dts_reservations,
            emit_ipc_contract_h,
            emit_os_topology,
            emit_system_manifest,
            load_board_yaml,
        )
    except ImportError as e:
        print(f"alp_project: failed to import alp_orchestrate: {e}",
              file=sys.stderr)
        return 1

    try:
        project = load_board_yaml(args.input,
                                  metadata_root=args.metadata_root)
        if args.emit == "system-manifest":
            out = emit_system_manifest(project)
        elif args.emit == "ipc-contract-h":
            out = emit_ipc_contract_h(project)
        elif args.emit == "dts-reservations":
            out = emit_dts_reservations(project)
        elif args.emit == "os-topology":
            out = emit_os_topology(project)
        else:
            print(f"alp_project: unknown v2 emit '{args.emit}'",
                  file=sys.stderr)
            return 1
    except OrchestratorError as e:
        print(f"alp_project: {e}", file=sys.stderr)
        return 1

    return _write_or_print(out, args.output)


def _run_v2_per_core_emit(args: argparse.Namespace) -> int:
    """v2 board.yaml + per-core --emit zephyr-conf / yocto-conf, plus the
    project-wide legacy emit modes (`dts-overlay`, `hw-info-h`,
    `west-libraries`) re-fitted for the v2 schema.

    The orchestrator owns the per-slice config emitters; this shim
    delegates after resolving the requested core (or summing across
    cores when `--core` is unset).
    """
    try:
        from alp_orchestrate import (
            OrchestratorError,
            _slice_alp_conf,
            _slice_cmake_args,
            _slice_local_conf,
            load_board_yaml,
        )
    except ImportError as e:
        print(f"alp_project: failed to import alp_orchestrate: {e}",
              file=sys.stderr)
        return 1

    try:
        project = load_board_yaml(args.input,
                                  metadata_root=args.metadata_root)
    except OrchestratorError as e:
        print(f"alp_project: {e}", file=sys.stderr)
        return 1

    # Validate --core if supplied (used by every emit path).
    if args.core is not None and args.core not in project.cores:
        print(f"alp_project: --core {args.core} not present in "
              f"board.yaml (known: {sorted(project.cores.keys())})",
              file=sys.stderr)
        return 1

    # Build a dict in the legacy "board:"-wrapper shape that the
    # in-file emitters still consume internally (dts-overlay,
    # hw-info-h, west-libraries).  The public board.yaml schema no
    # longer uses this wrapper, but it's a convenient internal
    # representation for the emitters' read paths.
    project_v1_shaped: dict[str, Any] = {
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

    # --- legacy project-wide emits, v2-flavoured -------------------------
    if args.emit == "dts-overlay":
        # The DTS overlay is shaped by the board header (bus aliases +
        # alp,pin-array) which is a SoM-mounting fact, not a per-core
        # fact.  v2 contributes only the peripherals list: union across
        # Zephyr/baremetal cores (or one core when --core is set).
        if args.core is not None:
            slice_ = project.cores[args.core]
            v2_peripherals = sorted(set(slice_.peripherals))
            out = _emit_dts_overlay(
                project_v1_shaped, project.som_preset,
                project.board_preset,
                v2_peripherals=v2_peripherals,
                v2_core_id=args.core,
                v2_core_os=slice_.os,
            )
        else:
            union: set[str] = set()
            for slice_ in project.cores.values():
                if slice_.os in ("zephyr", "baremetal"):
                    union.update(slice_.peripherals)
            out = _emit_dts_overlay(
                project_v1_shaped, project.som_preset,
                project.board_preset,
                v2_peripherals=sorted(union),
            )
        return _write_or_print(out, args.output)

    if args.emit == "native-sim-overlay":
        # native_sim GPIO emulation -- board-agnostic (the E1M pad map is a
        # SoM-mounting fact), so no --core / peripheral scoping applies.
        out = _emit_native_sim_overlay(project_v1_shaped)
        return _write_or_print(out, args.output)

    if args.emit == "hw-info-h":
        # hw-info-h is a project-level emit even under v2 -- consumers
        # `#include` it from any slice.  --core picks which slice's OS
        # lands in ALP_HW_BUILD_OS; absent --core, primary-core rules apply.
        v2_cores = {cid: s.os for cid, s in project.cores.items()}
        out = _emit_hw_info_h(
            project_v1_shaped, project.som_preset,
            project.board_preset,
            v2_cores=v2_cores,
            v2_selected_core=args.core,
        )
        return _write_or_print(out, args.output)

    if args.emit == "west-libraries":
        if args.core is not None:
            slice_ = project.cores[args.core]
            v2_libraries = sorted(set(slice_.libraries))
        else:
            union_l: set[str] = set()
            for slice_ in project.cores.values():
                if slice_.os in ("zephyr", "baremetal"):
                    union_l.update(slice_.libraries)
            v2_libraries = sorted(union_l)
        out = _emit_west_libraries(
            project_v1_shaped, project.som_preset,
            project.board_preset,
            v2_libraries=v2_libraries,
            v2_project_libraries=sorted(project.libraries),
        )
        return _write_or_print(out, args.output)

    # --- per-core emits (zephyr-conf / yocto-conf / cmake-args) ----------
    #
    # If --core is unset, sum across cores.  Per spec §4.6, the new
    # per-core invocation is the canonical entry point; the unscoped
    # invocation is a sum-across-cores convenience for tools that
    # haven't moved off the v1 single-OS world yet.
    if args.core is not None:
        core_ids = [args.core]
    else:
        core_ids = sorted(project.cores.keys())

    # Resolve + compatibility-validate any top-level `libraries:` once, up
    # front, so an unknown name or a failed `requires:` constraint surfaces
    # as a clean one-line error (ADR 0018) rather than a traceback mid-emit.
    if project.libraries:
        try:
            from alp_orchestrate.libraries import resolve_selection
            resolve_selection(project, args.metadata_root)
        except OrchestratorError as e:
            print(f"alp_project: {e}", file=sys.stderr)
            return 1

    # #605: the os class(es) each per-core `--emit` mode is valid for.
    # `zephyr-conf` / `yocto-conf` are OS-SPECIFIC -- they hand a Kconfig
    # fragment / local.conf snippet to a build system that only makes
    # sense for that one runtime.  `cmake-args` is GENERIC across the two
    # runtimes that actually consume raw CMake args (baremetal + zephyr).
    # An explicit `--core` naming a core outside its emit mode's classes
    # used to warn-and-emit-anyway (zephyr-conf/yocto-conf) or emit
    # silently with no warning at all (cmake-args) -- automation reading
    # the output got valid-looking config for the wrong consumption path.
    # That is now a hard error; the unscoped (`--core` omitted) sum-
    # across-cores path keeps silently skipping incompatible cores, since
    # it is explicitly a "give me everything applicable" query.
    _EMIT_OS_CLASSES: dict[str, tuple[str, ...]] = {
        "zephyr-conf": ("zephyr",),
        "yocto-conf": ("yocto",),
        "cmake-args": ("baremetal", "zephyr"),
    }

    parts: list[str] = []
    for cid in core_ids:
        slice_ = project.cores[cid]
        if slice_.os == "off":
            continue
        allowed_os = _EMIT_OS_CLASSES.get(args.emit)
        if allowed_os is not None and slice_.os not in allowed_os:
            if args.core is None:
                continue
            print(f"alp_project: --core {cid} has os: {slice_.os}, which "
                  f"--emit {args.emit} does not support (supported os: "
                  f"{', '.join(allowed_os)})", file=sys.stderr)
            return 1
        if args.emit == "zephyr-conf":
            parts.append(f"# --- core: {cid} ({slice_.os}) ---")
            parts.append(_slice_alp_conf(project, slice_))
            hw_lines = _emit_library_hw_backends(slice_.libraries, project.sku)
            if hw_lines:
                parts.append("# §D.lib.loader -- per-library HW-accelerator wiring (auto-emitted).")
                parts.extend(hw_lines)
                parts.append("")
        elif args.emit == "yocto-conf":
            parts.append(f"# --- core: {cid} ({slice_.os}) ---")
            parts.append(_slice_local_conf(project, slice_))
        elif args.emit == "cmake-args":
            parts.append(f"# --- core: {cid} ({slice_.os}) ---")
            parts.append(_slice_cmake_args(project, slice_))
        else:
            print(f"alp_project: unknown --emit {args.emit} for v2 board.yaml",
                  file=sys.stderr)
            return 1

    out = "\n".join(parts) + ("\n" if parts and not parts[-1].endswith("\n") else "")
    return _write_or_print(out, args.output)


# ---------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description="Compile board.yaml -> per-backend native config.")
    parser.add_argument("--input", type=Path, default=Path("board.yaml"),
                        help="Path to the project's board.yaml (default: ./board.yaml).")
    parser.add_argument("--emit",
                        choices=["zephyr-conf", "cmake-args", "yocto-conf",
                                 "dts-overlay", "native-sim-overlay",
                                 "hw-info-h", "west-libraries",
                                 # v2 orchestration emits (Phase 2):
                                 "system-manifest", "dts-reservations",
                                 "ipc-contract-h",
                                 # Per-core natural-vs-effective OS facts (issue #95).
                                 "os-topology",
                                 # Carrier routing / Studio handoff JSON.
                                 "composed-route-table",
                                 "carrier-netlist"],
                        default="zephyr-conf",
                        help="Output format (default: zephyr-conf).")
    parser.add_argument("--output", type=Path, default=None,
                        help="Write to this path; default: stdout.")
    parser.add_argument("--metadata-root", type=Path, default=METADATA_ROOT,
                        help="Override the metadata search root.")
    parser.add_argument("--core", default=None,
                        help="When the project is v2, limit emits to this "
                             "core ID.  For per-core emit modes "
                             "(zephyr-conf, yocto-conf, cmake-args) this "
                             "picks the single slice to emit.  For "
                             "project-wide emit modes (dts-overlay, "
                             "hw-info-h, west-libraries) this scopes the "
                             "union calculation to a single slice (e.g. "
                             "ALP_HW_BUILD_OS reflects the selected core's "
                             "runtime).  Ignored for system-manifest, "
                             "ipc-contract-h, dts-reservations.")
    args = parser.parse_args()

    # Project-wide v2 emit modes (system-manifest, dts-reservations,
    # ipc-contract-h) route through alp_orchestrate/ directly.
    if args.emit in ("system-manifest", "dts-reservations",
                     "ipc-contract-h", "os-topology"):
        return _run_v2_emit(args)

    project = _validate_and_load(args.input, args.metadata_root)

    # composed-route-table / carrier-netlist only need the SoM + board
    # definitions; they do not require the per-core slice machinery.
    if args.emit in ("composed-route-table", "carrier-netlist"):
        sku_preset_rt = _resolve_sku(project["som"]["sku"], args.metadata_root)
        board_preset_rt = _resolve_inline_or_preset_board(
            project, args.metadata_root)
        if args.emit == "composed-route-table":
            out = _emit_composed_route_table(
                project, sku_preset_rt, board_preset_rt, args.metadata_root
            )
        else:
            out = _emit_carrier_netlist(
                project, sku_preset_rt, board_preset_rt, args.metadata_root
            )
        return _write_or_print(out, args.output)

    # board.yamls flow through the per-core / project-wide emit path
    # in _run_v2_per_core_emit.  Project-wide emits (system-manifest,
    # dts-reservations, ipc-contract-h) were already dispatched above.
    return _run_v2_per_core_emit(args)


if __name__ == "__main__":
    sys.exit(main())
