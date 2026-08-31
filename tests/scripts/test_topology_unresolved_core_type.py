# SPDX-License-Identifier: Apache-2.0
"""An UNRESOLVED `cores[].type` yields unresolved, never a guess (#1852).

alp-sdk and tan carry the same class->runtime rule in two places, and they had
diverged on the one input neither schema constrains: a `cores[].type` that is
absent, empty, or not a string at all.

    alp-sdk  _allowed_os_for_core("")       -> ["baremetal", "off"]
    alp-sdk  _default_os_from_core_type(5)  -> AttributeError
    tan      allowed_os_for_core("")        -> []
    tan      allowed_os_for_core(5)         -> []

tan took the fix (tan-cli#914 / tan-cli#957) and alp-sdk did not, in the one
direction `test_planner_relocation_freshness.py` cannot see -- tan's copy lives
at `python/tan/core/os_class.py`, outside `tan/planner/` and outside
`HAND_PORT_SOURCES`. These tests pin alp-sdk to tan's answers so the two cannot
drift back apart on this input.

The distinction the tests are really guarding is UNRESOLVED vs UNCLASSIFIED:

  * unresolved -- no usable type was read at all. Advertise nothing (`[]`).
  * unclassified -- a real type string was read, it just matches neither
    `cortex-a*` nor `cortex-m*`. That still resolves to `off` with
    `["baremetal", "off"]` allowed, in both repos.

Collapsing those two is what produced the plausible-but-wrong answer.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate.topology import (  # noqa: E402
    _allowed_os_for_core,
    _cross_class_os,
    _default_os_from_core_type,
)

METADATA_ROOT = REPO / "metadata"

#: Every shape a `cores[].type` can arrive in that carries no usable class.
#: `""` and `None` were already handled by the old `(core_type or "")`; the
#: truthy non-strings are the ones that used to raise.
UNRESOLVED = ["", None, 5, True, ["cortex-m55"], {"type": "cortex-m55"}, 0.0]


@pytest.mark.parametrize("core_type", UNRESOLVED)
def test_default_os_never_raises_on_an_unresolved_type(core_type) -> None:
    """A truthy non-string used to reach `.lower()` and raise AttributeError.

    A crash here is worse than a wrong answer: it surfaces as a bare traceback
    from the loader rather than as a diagnostic, and it happens on a path a
    schema-invalid `--metadata-root` can reach.
    """
    assert _default_os_from_core_type(core_type) == "off"


@pytest.mark.parametrize("core_type", UNRESOLVED)
def test_allowed_os_is_empty_for_an_unresolved_type(core_type) -> None:
    """`[]`, not `["baremetal", "off"]` -- do not offer runtimes for a core
    whose class nobody established. This is the exact answer tan returns."""
    assert _allowed_os_for_core(core_type, METADATA_ROOT) == []


@pytest.mark.parametrize(
    ("core_type", "expected_os", "expected_allowed"),
    [
        ("cortex-m55", "zephyr", ["zephyr", "baremetal", "off"]),
        ("cortex-m33", "zephyr", ["zephyr", "baremetal", "off"]),
        ("cortex-a32", "yocto", ["yocto", "baremetal", "off"]),
        ("cortex-a55", "yocto", ["yocto", "baremetal", "off"]),
        ("CORTEX-M55", "zephyr", ["zephyr", "baremetal", "off"]),
    ],
)
def test_classified_cores_are_untouched(
    core_type: str, expected_os: str, expected_allowed: list[str]
) -> None:
    """The guard must not change a single answer for a real core type --
    including the case-folded spelling, since the function lowercases."""
    assert _default_os_from_core_type(core_type) == expected_os
    assert _allowed_os_for_core(core_type, METADATA_ROOT) == expected_allowed


@pytest.mark.parametrize("core_type", ["riscv-x", "starfive-u74", "gd32-m4"])
def test_unclassified_but_resolved_types_still_get_the_no_class_answer(
    core_type: str,
) -> None:
    """A real type string that matches neither prefix is UNCLASSIFIED, not
    unresolved: it resolves to `off`, and `baremetal`/`off` stay on offer.

    Widening the guard to `startswith`-misses as well would silently stop
    offering `baremetal` on a future non-ARM companion core, so the guard is
    deliberately keyed on "is there a string at all", exactly as tan's is.
    """
    assert _default_os_from_core_type(core_type) == "off"
    assert _allowed_os_for_core(core_type, METADATA_ROOT) == ["baremetal", "off"]


class _StubSlice:
    def __init__(self, os_: str) -> None:
        self.os = os_


class _StubProject:
    """The four attributes `core_os_topology` actually reads.

    Deliberately a stub rather than a scratch metadata root: the assertion is
    about one comprehension in `core_os_topology`, and a real SoC JSON cannot
    carry a non-string `type` without also failing `soc-spec-v1.schema.json`
    -- which is the point, since the unvalidated path is exactly what a
    schema-invalid `--metadata-root` reaches.
    """

    def __init__(self, core_type) -> None:
        self.sku = "STUB-SKU"
        self.soc_spec = {"cores": [{"id": "c0", "type": core_type}]}
        self.cores = {"c0": _StubSlice("zephyr")}

    def effective_metadata_root(self) -> Path:
        return METADATA_ROOT


@pytest.mark.parametrize("core_type", [5, True, ["cortex-m55"], {"a": 1}, 0.0])
def test_core_os_topology_does_not_leak_a_non_string_type_into_the_document(
    core_type,
) -> None:
    """The second failure mode, and the quieter one.

    `core_type` is written verbatim into the emitted `os-topology` document,
    so before the normalisation a non-string `type` either aborted the emit
    with a bare `AttributeError` or shipped a non-string into the document.
    It now becomes the same `""` unresolved sentinel a missing `type`
    produces, and every derived field agrees with that.
    """
    from alp_orchestrate.topology import core_os_topology  # noqa: PLC0415

    doc = core_os_topology(_StubProject(core_type))
    (row,) = doc["cores"]
    assert row["core_type"] == ""
    assert isinstance(row["core_type"], str)
    assert row["runtime_class"] == "other"
    assert row["default_os"] == "off"
    assert row["allowed_os"] == []


def test_core_os_topology_keeps_a_real_type_verbatim() -> None:
    """The normalisation must not touch a well-formed SoC spec."""
    from alp_orchestrate.topology import core_os_topology  # noqa: PLC0415

    doc = core_os_topology(_StubProject("cortex-m55"))
    (row,) = doc["cores"]
    assert row["core_type"] == "cortex-m55"
    assert row["runtime_class"] == "rtos"
    assert row["default_os"] == "zephyr"
    assert row["allowed_os"] == ["zephyr", "baremetal", "off"]


def test_cross_class_os_is_deliberately_left_unguarded() -> None:
    """`_cross_class_os` keeps refusing BOTH real runtimes for an unresolved
    type, matching tan's `cross_class_os`, which has no guard either.

    It feeds `validate.py::_enforce_os_matches_core_class`, a REFUSAL -- and
    refusing both is the conservative answer when the class is unknown. Only
    the advertised `allowed_os` set was wrong. If someone ever "fixes" this
    function for symmetry, that turns a refusal into a permission on both
    sides, so the asymmetry is pinned here rather than left to be rediscovered.
    """
    assert _cross_class_os("") == {"yocto", "zephyr"}
    assert _cross_class_os("cortex-m55") == {"yocto"}
    assert _cross_class_os("cortex-a32") == {"zephyr"}
