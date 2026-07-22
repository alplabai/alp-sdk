#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
ALP heterogeneous-OS build orchestrator (Phase 2 of the 2026-05-15 design).

A board.yaml declares per-core runtimes; this module loads the
project, resolves topology defaults from the SoM preset, allocates
IPC carve-outs from the SoM's memory_map, and fans out into one
build slice per non-`off` core.

Public API:

    load_board_yaml(path)          -> BoardProject
    resolve_carve_outs(project)    -> list[ResolvedCarveOut]
    emit_system_manifest(project, slices=...) -> str
    emit_dts_reservations(project) -> str
    emit_ipc_contract_h(project)   -> str
    emit_build_plan(project, board_yaml=..., build_root=...) -> str
    iter_buildable_slices(project) -> Iterator[Slice]

    BoardProject, Slice, ResolvedCarveOut, SystemManifest, OrchestratorError

ADR-0020 Phase 4 (preview) retired the SDK-side executor (`Orchestrator`/
`fan_out`) -- this module is planner/emit-only; execution is an external
consumer's job.

Reference: docs/superpowers/specs/2026-05-15-heterogeneous-os-orchestration-design.md
"""

from __future__ import annotations

# The filesystem roots now live in paths.py (the #285 paths seam) -- a leaf both
# __init__ and topology.py import, so topology no longer lazy-imports BOARD_SCHEMA
# back through the package. Re-exported here so `from alp_orchestrate import REPO`
# (and friends) keeps working unchanged.
from .paths import (  # noqa: E402
    BOARD_PRESET_SCHEMA,  # noqa: F401  (re-export for the public surface)
    BOARD_SCHEMA,  # noqa: F401  (re-export for the public surface)
    METADATA_ROOT,  # noqa: F401  (re-export for the public surface)
    REPO,  # noqa: F401  (re-export for the public surface)
)


# The per-core OS-class taxonomy + topology view now live in topology.py (the
# #285 topology seam). Re-export the names referenced outside the module:
# `_default_os_from_core_type` (test_silicon_variant_and_os_inference) + the public
# emit surface `core_os_topology` / `emit_os_topology` (alp_project.py + the
# test_emit_os_topology tests). The
# remaining helpers (CLASS_RUNTIMES, _allowed_os_for_core, _runtime_class) stay
# topology-private -- nothing outside topology.py references them.
from .topology import (  # noqa: E402
    _default_os_from_core_type,  # noqa: F401  (re-export: test_silicon_variant_and_os_inference)
    core_os_topology,  # noqa: F401  (re-export: test_emit_os_topology)
    emit_os_topology,  # noqa: F401  (re-export: alp_project.py + tests)
)

# The orchestrator data model now lives in alp_orchestrate_models (the first
# #285 modularization seam); re-exported here so `from alp_orchestrate import
# Slice` (and friends) keeps working unchanged for callers + tests.
from .models import (  # noqa: E402
    BoardProject,  # noqa: F401  (re-export: public model surface)
    IpcEntry,  # noqa: F401  (re-export: public model surface; consumed by loader.py)
    OrchestratorError,  # noqa: F401  (re-export: alp_project's lazy imports + tests)
    ResolvedCarveOut,  # noqa: F401  (re-export; consumed by carveout.py now, not __init__)
    ResolvedPartition,  # noqa: F401  (re-export; consumed by partition.py + headers.py now)
    Slice,  # noqa: F401  (re-export: public model surface)
    StorageEntry,  # noqa: F401  (re-export: public model surface; consumed by loader.py)
    SystemManifest,  # noqa: F401  (re-export: public model surface)
)


# ---------------------------------------------------------------------
# Loader
# ---------------------------------------------------------------------
# The board.yaml loader now lives in loader.py (the #285 loader seam).
# Re-export load_board_yaml -- the public parse entry (cli, west alp-build via
# the module form, alp_project's --emit dispatch, the tests + fuzzer).
from .loader import load_board_yaml  # noqa: E402,F401  (re-export: cli + west + alp_project + tests)


# ---------------------------------------------------------------------
# Carve-out resolver
# ---------------------------------------------------------------------
# resolve_carve_outs lives in carveout.py (the #285 carve-out seam); re-exported
# so the public surface (orchestrator + emitters + tests) stays unchanged.
from .carveout import resolve_carve_outs  # noqa: E402,F401  (re-export: tests)


# ---------------------------------------------------------------------
# Storage partition resolver
# ---------------------------------------------------------------------
# The storage-partition resolver now lives in partition.py (the #285 partition
# seam). Re-export resolve_storage_partitions (emitters + orchestrator + tests)
# and _known_flash_devices (the loader's eager flash-device cross-check above).
from .partition import resolve_storage_partitions  # noqa: E402,F401  (re-export for tests)


# ---------------------------------------------------------------------
# Emitters
# ---------------------------------------------------------------------


# The header / DTS / mount-table emitters now live in headers.py (the #285
# headers emit seam). Re-exported so cli.py + alp_project.py + the build-plan
# artefact table + tests keep importing them unchanged.
from .headers import (  # noqa: E402
    emit_dts_partitions,  # noqa: F401  (re-export: cli + tests)
    emit_dts_reservations,  # noqa: F401  (re-export: cli + alp_project + tests)
    emit_ipc_contract_h,  # noqa: F401  (re-export: cli + alp_project + tests)
    emit_storage_mounts_c,  # noqa: F401  (re-export: cli + alp_project + tests; not used in __init__)
)


# emit_system_manifest now lives in manifest.py (the #285 manifest emit seam);
# re-exported for cli + alp_project + tests. (_helper_mcus + the shared build-plan
# helpers are imported directly by orchestrator.py now, not back through here.)
from .manifest import emit_system_manifest  # noqa: E402,F401  (re-export: cli + alp_project + tests)


# ---------------------------------------------------------------------
# Build-plan emission (the Wave C consumer contract)
# ---------------------------------------------------------------------


# The build-plan emitter now lives in buildplan.py (the #285 build-plan seam);
# re-exported for cli + tests. (The shared materialise helpers are imported
# directly by orchestrator.py now, not back through here.)
from .buildplan import emit_build_plan  # noqa: E402,F401  (re-export: cli + tests)


# ---------------------------------------------------------------------
# Slice-command resolution (planner-side; the executor was retired)
# ---------------------------------------------------------------------
# The slice-command / flash-recipe cluster lives in orchestrator.py.
# Re-export _slice_command (tests) and iter_buildable_slices (ADR-0020
# Phase 1 -- tests + buildplan.py).
from .orchestrator import (  # noqa: E402,F401
    _slice_command,
    iter_buildable_slices,  # noqa: F401  (re-export: ADR-0020 Phase 1 -- tests + buildplan.py)
)


# ---------------------------------------------------------------------
# Per-slice config emitters (consumed by both Orchestrator and the
# new --core <id> --emit zephyr-conf / yocto-conf modes in alp_project.py).
# ---------------------------------------------------------------------


# The shared slug / peripheral-Kconfig helpers now live in slugs.py (the #285
# slug leaf). Re-exported for the per-slice config emitters below + the tests.
from .slugs import (  # noqa: E402
    _slugs_from_helper_firmware,  # noqa: F401  (re-export for tests)
    _slugs_from_on_module,  # noqa: F401  (re-export for tests)
)


# The per-slice Kconfig (alp.conf) emitter now lives in kconfig.py (the #285
# kconfig emit seam); it pulls the slug tables from slugs.py. Re-exported for
# the build-plan _slice_config_artefact helper (inline) + alp_project + tests.
from .kconfig import (  # noqa: E402
    _emit_extra_library_profile,  # noqa: F401  (re-export for tests)
    _slice_alp_conf,  # noqa: F401  (re-export: alp_project + tests)
    _slice_cmake_args,  # noqa: F401  (re-export: alp_project + tests)
    _slice_local_conf,  # noqa: F401  (re-export: alp_project + tests)
)


# The sysbuild / TF-M secure-boot config emitters now live in secure.py (the
# #285 secure emit seam). Re-exported so cli.py + tests + the build-plan artefact
# table / slice-command helpers (still inline) keep importing them unchanged.
from .secure import emit_sysbuild_conf, emit_tfm_sysbuild_conf  # noqa: E402,F401  (re-export: cli + tests)


# ---------------------------------------------------------------------
# CLI (thin wrapper for ad-hoc invocation)
# ---------------------------------------------------------------------


# Re-export the CLI entry: main() lives in __main__ (so `python -m
# alp_orchestrate` is the invocation), but `from alp_orchestrate import main`
# stays valid for callers + the test-suite.  Placed at module end so __main__'s
# `from alp_orchestrate import ...` sees a fully-populated package.
from .cli import main  # noqa: F401,E402  (intentional re-export)
