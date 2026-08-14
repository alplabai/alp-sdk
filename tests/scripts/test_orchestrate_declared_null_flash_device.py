# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""A DECLARED `jlink_flash_device: null` must reach `flash_args` (tan-cli#734).

`_slice_flash_recipe` gated the key on TRUTHINESS, so a schema-declared
`jlink_flash_device: null` was dropped and arrived downstream as an ABSENT
key. Those mean opposite things:

  * ABSENT -- the SoC variant says nothing; the Flow A default stands.
  * NULL   -- the variant publishes "no known J-Link flash profile", and
              soc-spec-v1.schema.json's description says a consumer must
              refuse rather than silently pick another transport.

Collapsing null into absent makes tan's presence-based `flow_d_available()`
(`flash_plan._fa_has_key`) see nothing and silently downgrade Flow D to the
SE-UART Flow A path -- which is Linux-only, so a Windows operator's flash
fails later somewhere else with the real cause already discarded. This is the
same defect tan-cli#734 fixed on tan's side; it lived here too, so the null
was destroyed at the source and tan's fix could never see it.

Driven through `_slice_flash_recipe` with a constructed `Slice` rather than a
board fixture: the distinction under test is the emitter's, and no shipped SoC
variant on this branch declares a null to exercise it with.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from alp_orchestrate.loader import _enforce_flow_d_preflight_pair  # noqa: E402
from alp_orchestrate.models import OrchestratorError, Slice  # noqa: E402
from alp_orchestrate.orchestrator import _slice_flash_recipe  # noqa: E402


def _args(**kwargs) -> dict:
    slice_ = Slice(core_id="m55_hp", os="zephyr", **kwargs)
    _method, args = _slice_flash_recipe(slice_)
    assert args is not None, "a zephyr slice must produce flash_args"
    return args


def test_a_declared_null_lands_as_a_present_null():
    """The fix. Asserted on KEY PRESENCE, not on the value -- absent and
    declared-null both read as `None`, so a value assertion cannot tell them
    apart and would pass against the bug."""
    args = _args(jlink_flash_device=None, jlink_flash_device_declared=True)
    assert "jlink_flash_device" in args, args
    assert args["jlink_flash_device"] is None, args


def test_an_undeclared_device_still_emits_nothing():
    """The other half of the contract, and the reason this is not simply
    `is not None`: a variant that says nothing must keep today's behaviour and
    stay out of `flash_args` entirely."""
    args = _args(jlink_flash_device=None, jlink_flash_device_declared=False)
    assert "jlink_flash_device" not in args, args


def test_a_real_profile_is_unchanged():
    """Regression guard for the common case."""
    args = _args(
        jlink_flash_device="AE822FA0E5597LS0_M55_HE",
        jlink_flash_device_declared=True,
    )
    assert args["jlink_flash_device"] == "AE822FA0E5597LS0_M55_HE", args


@pytest.mark.parametrize("os_", ["off", "yocto"])
def test_non_zephyr_slices_are_untouched(os_):
    """The emitter's other branches must not gain the key."""
    _method, args = _slice_flash_recipe(
        Slice(core_id="m55_hp", os=os_,
              jlink_flash_device=None, jlink_flash_device_declared=True))
    assert args is None or "jlink_flash_device" not in args, args


# ---------------------------------------------------------------------
# The emitter was not the only truthiness gate. Two more read the same
# field, and both were found by diffing against tan's relocated copy of
# this module (tan-cli#734/#735), which had already fixed all three --
# the SDK side was the one lagging, not the port.
# ---------------------------------------------------------------------


def _preflight(**kwargs) -> None:
    """`_enforce_flow_d_preflight_pair` with a variant that publishes an
    `expect_dpidr` but no `jlink_device` for this core -- the #1355 gap it
    exists to refuse. It raises IF it decides the slice is Flow-D-armed, so
    a raise means the guard ran and a silent return means it skipped."""
    _enforce_flow_d_preflight_pair(
        Slice(core_id="m55_hp", os="zephyr", **kwargs),
        {"expect_dpidr": "0x4C013477"},
        "E1M-AENTEST",
    )


def test_a_declared_null_still_gets_the_preflight_pair_check():
    """The gate read `not slice_.jlink_flash_device`, so a declared null
    skipped the #1355 half-armed-pair refusal entirely -- the variant most
    in need of a diagnostic got the fewest."""
    with pytest.raises(OrchestratorError) as excinfo:
        _preflight(jlink_flash_device=None, jlink_flash_device_declared=True)
    assert "expect_dpidr" in str(excinfo.value)
    assert "0x4C013477" in str(excinfo.value)


def test_an_undeclared_device_still_skips_the_preflight_pair_check():
    """The other half: a variant that says nothing is not a Flow D target,
    and must keep skipping. Without this, the fix above would turn every
    non-Flow-D core into a hard error."""
    _preflight(jlink_flash_device=None, jlink_flash_device_declared=False)


def test_a_real_profile_still_gets_the_preflight_pair_check():
    with pytest.raises(OrchestratorError):
        _preflight(jlink_flash_device="AE822FA0E5597LS0_M55_HE",
                   jlink_flash_device_declared=True)


@pytest.mark.parametrize("os_", ["off", "yocto"])
def test_a_non_zephyr_slice_skips_the_preflight_pair_check(os_):
    _enforce_flow_d_preflight_pair(
        Slice(core_id="m55_hp", os=os_,
              jlink_flash_device=None, jlink_flash_device_declared=True),
        {"expect_dpidr": "0x4C013477"},
        "E1M-AENTEST",
    )
