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
from typing import TYPE_CHECKING, Any

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
    """
    t = (core_type or "").lower()
    if t.startswith("cortex-a"):
        return "yocto"
    if t.startswith("cortex-m"):
        return "zephyr"
    return "off"


@functools.lru_cache(maxsize=None)
def _core_os_choices() -> tuple[str, ...]:
    """The runtimes a core's `os:` may resolve to, read from the board
    schema's `$defs/core_entry/properties/os` enum.

    Derived (not re-typed) so the value-set has exactly one source of truth
    and cannot drift between the schema and the code.  `off` skips the core
    (no slice is built).
    """
    # Lazy import: BOARD_SCHEMA is a shared schema-path root defined in the
    # package __init__; importing it at call time (not module load) keeps
    # topology.py free of an import cycle back into the package.
    from . import BOARD_SCHEMA
    schema = json.loads(BOARD_SCHEMA.read_text(encoding="utf-8"))
    return tuple(schema["$defs"]["core_entry"]["properties"]["os"]["enum"])


# The two class-determined OS runtimes (issue #95): Cortex-A -> Yocto (Linux),
# Cortex-M -> Zephyr (RTOS).  These follow the core class and are NOT
# user-selectable (see _default_os_from_core_type); `baremetal` (no-OS) and
# `off` (disabled) are the only values a board.yaml may set explicitly.
CLASS_RUNTIMES = ("yocto", "zephyr")


def _runtime_class(core_type: str) -> str:
    """`linux` for Cortex-A, `rtos` for Cortex-M, else `other`."""
    t = (core_type or "").lower()
    if t.startswith("cortex-a"):
        return "linux"
    if t.startswith("cortex-m"):
        return "rtos"
    return "other"


def _cross_class_os(core_type: str) -> set[str]:
    """The class runtime a core may NOT be set to -- the OS of the *other*
    class.  A Cortex-A can't run Zephyr; a Cortex-M can't run Yocto."""
    return set(CLASS_RUNTIMES) - {_default_os_from_core_type(core_type)}


def _allowed_os_for_core(core_type: str) -> list[str]:
    """The os: values valid for this core: every runtime minus the other
    class's OS -- e.g. Cortex-A -> [yocto, baremetal, off], Cortex-M ->
    [zephyr, baremetal, off]."""
    cross = _cross_class_os(core_type)
    return [o for o in _core_os_choices() if o not in cross]


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
    soc_types = {
        c["id"]: c.get("type", "")
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
            "allowed_os":    _allowed_os_for_core(core_type),
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
