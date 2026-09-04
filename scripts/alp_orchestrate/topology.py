#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Per-core OS-class taxonomy + the topology view (issue #95).

A core's runtime is fixed by its Cortex class -- Cortex-A -> Yocto (Linux),
Cortex-M -> Zephyr (RTOS) -- not chosen freely. This module owns that taxonomy
(the default-OS rule, the runtime class, the allowed-OS set read from the board
schema) plus `core_os_topology` / `emit_os_topology`, the per-core OS facts an
IDE/tool renders. Extracted from alp_orchestrate as the #285 topology seam and
re-exported from the package __init__, so callers + alp_project.py keep importing
the same names unchanged.
"""

from __future__ import annotations

import functools
import json
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .models import OrchestratorError
from .paths import BOARD_SCHEMA

if TYPE_CHECKING:
    from .models import BoardProject


def _default_os_from_core_type(core_type: str) -> str:
    """Infer default OS from a SoC's `cores[].type`.

    Convention (codified across the SoM presets pre-2026-05-18):
        cortex-a*  ->  yocto
        cortex-m*  ->  zephyr
        anything else ->  off

    Used as the fallback when a SoM preset's `topology.<core>.os` is
    omitted (the field is now optional in som-preset-v1.schema.json --
    M-class cores default to Zephyr, A-class to Yocto).

    *core_type* is typed `str`, but the value is read straight off a SoC
    JSON that a schema-invalid `--metadata-root` tree makes no promise
    about, so the annotation is not a guarantee (#1852, porting tan-cli#957).
    `(core_type or "")` handled `None` and `""` but still reached `.lower()`
    on a TRUTHY non-string (`7`, a populated list, `True`) and raised
    `AttributeError` there -- a crash where tan, the other implementation of
    this same rule, normalises to the `""` unresolved sentinel a genuinely
    absent `type` already produces. `isinstance` here is the backstop for
    every caller that forwards a `cores[].type` without its own guard.
    """
    t = core_type.lower() if isinstance(core_type, str) else ""
    if t.startswith("cortex-a"):
        return "yocto"
    if t.startswith("cortex-m"):
        return "zephyr"
    return "off"


@functools.lru_cache(maxsize=None)
def _core_os_choices(metadata_root: Path) -> tuple[str, ...]:
    """The runtimes a core's `os:` may resolve to, read from *metadata_root*'s
    board schema's `$defs/core_entry/properties/os` enum.

    Derived (not re-typed) so the value-set has exactly one source of truth
    and cannot drift between the schema and the code.  `off` skips the core
    (no slice is built).  *metadata_root* is REQUIRED and the cache is keyed
    on it -- a fixed in-tree default here silently ignored a project's
    `--metadata-root` override (#1485).

    Falls back to the in-tree `BOARD_SCHEMA` when *metadata_root* has no
    `schemas/` of its own (e.g. a synthetic test root) -- the same fallback
    `loader._validate_board` applies, so a scratch root without a schema copy
    still resolves instead of raising a raw `FileNotFoundError`.
    """
    schema_path = Path(metadata_root) / "schemas" / "board.schema.json"
    if not schema_path.is_file():
        schema_path = BOARD_SCHEMA
    if not schema_path.is_file():
        raise OrchestratorError(f"board schema not found: {schema_path}")
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    return tuple(schema["$defs"]["core_entry"]["properties"]["os"]["enum"])


# The two class-determined OS runtimes (issue #95): Cortex-A -> Yocto (Linux),
# Cortex-M -> Zephyr (RTOS).  These follow the core class and are NOT
# user-selectable (see _default_os_from_core_type); `baremetal` (no-OS) and
# `off` (disabled) are the only values a board.yaml may set explicitly.
CLASS_RUNTIMES = ("yocto", "zephyr")


def _runtime_class(core_type: str) -> str:
    """`linux` for Cortex-A, `rtos` for Cortex-M, else `other`.

    The `isinstance` guard is defense-in-depth rather than a live bug fix:
    `core_os_topology`, the one caller, now normalises a non-string `type`
    to `""` before this is reached, so the branch is currently unreachable
    -- not currently wrong. It is kept anyway so this helper does not repeat
    the assumption ("my caller surely guarded this") that let the bare
    `(core_type or "").lower()` idiom survive unnoticed in three functions at
    once here (#1852). Mirrors tan's `python/tan/planner/topology.py`
    `_runtime_class` (tan-cli#957 / tan-cli#962).
    """
    t = core_type.lower() if isinstance(core_type, str) else ""
    if t.startswith("cortex-a"):
        return "linux"
    if t.startswith("cortex-m"):
        return "rtos"
    return "other"


def _cross_class_os(core_type: str) -> set[str]:
    """The class runtime a core may NOT be set to -- the OS of the *other*
    class.  A Cortex-A can't run Zephyr; a Cortex-M can't run Yocto."""
    return set(CLASS_RUNTIMES) - {_default_os_from_core_type(core_type)}


def _allowed_os_for_core(core_type: str, metadata_root: Path) -> list[str]:
    """The os: values valid for this core: every runtime minus the other
    class's OS -- e.g. Cortex-A -> [yocto, baremetal, off], Cortex-M ->
    [zephyr, baremetal, off].

    An UNRESOLVED *core_type* -- absent, empty, or not a string -- yields
    `[]`, not a guess (#1852, porting tan-cli#914 / tan-cli#957). Without
    this guard `_cross_class_os("")` is `{yocto, zephyr}`, so the
    comprehension handed back `["baremetal", "off"]`: an offer of two
    runtimes for a core whose class nobody established. tan's
    `allowed_os_for_core` rejects exactly that answer by name, and this
    function is the other half of the same rule, so it must agree.

    `_cross_class_os` is deliberately left unguarded, matching tan's
    `cross_class_os`: the REFUSAL path it feeds
    (`validate.py::_enforce_os_matches_core_class`) already declines both
    real runtimes for an unclassified core in both repos, which is the
    conservative answer there. Only the *advertised* set was wrong.
    """
    if not isinstance(core_type, str) or not core_type:
        return []
    cross = _cross_class_os(core_type)
    return [o for o in _core_os_choices(metadata_root) if o not in cross]


def core_os_topology(project: "BoardProject") -> dict[str, Any]:
    """Per-core OS facts for an IDE / tool (issue #95).

    The runtime is *determined by the core class* -- Cortex-A -> Yocto (Linux),
    Cortex-M -> Zephyr (RTOS) -- and is not user-selectable.  For each resolved
    core this reports its `runtime_class`, the class `default_os`, the
    `effective_os` (after a `baremetal`/`off` override -- the only board.yaml
    knobs), whether it is `enabled`, and the per-core `allowed_os` set (the
    valid dropdown).  Lets the Board Configurator show the SDK's selection +
    the legal overrides instead of guessing or offering a cross-class OS.
    """
    # `c.get("type", "")` is UNVALIDATED against `soc-spec-v1.schema.json`'s
    # own `"type": {"type": "string"}` -- a `--metadata-root` pointing at a
    # schema-invalid tree reaches here unchecked. `core_type` below feeds
    # `_runtime_class` / `_default_os_from_core_type` / `_allowed_os_for_core`
    # AND is written verbatim to the emitted `core_type` field, so a
    # non-string here is either an `AttributeError` (`--emit os-topology`
    # aborts with a bare traceback) or a wire leak into the document --
    # the same two failure modes tan closed in tan-cli#957 / tan-cli#962.
    # Normalises to the same `""` unresolved sentinel a missing `type` or a
    # missing entry already produces (#1852).
    soc_types = {
        c["id"]: c["type"] if isinstance(c.get("type"), str) else ""
        for c in (project.soc_spec.get("cores") or []) if "id" in c
    }
    rows: list[dict[str, Any]] = []
    for core_id, sl in sorted(project.cores.items()):
        core_type = soc_types.get(core_id, "")
        default_os = _default_os_from_core_type(core_type)
        rows.append({
            "core_id":       core_id,
            "core_type":     core_type,
            "runtime_class": _runtime_class(core_type),
            "default_os":    default_os,
            "effective_os":  sl.os,
            "enabled":       sl.os != "off",
            "overridden":    sl.os != default_os,
            "allowed_os":    _allowed_os_for_core(
                core_type, project.effective_metadata_root()),
        })
    return {
        "schema_version": 1,
        "sku":            project.sku,
        "cores":          rows,
    }


def emit_os_topology(project: "BoardProject") -> str:
    """JSON for `alp_project.py --emit os-topology` (see core_os_topology).

    Sorted keys + a trailing newline so the output is byte-deterministic.
    """
    return json.dumps(core_os_topology(project), indent=2) + "\n"
