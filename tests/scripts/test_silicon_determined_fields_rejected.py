# SPDX-License-Identifier: Apache-2.0
"""
Regression tests for commit a3cd4fd "schema: silicon-determined
fields out of board.yaml v2".

Locks in the post-cleanup schema behaviour:

  1. `cores.<id>.inference.backend` is REJECTED for every removed
     value (auto / cpu / ethos_u / drpai / deepx_dx).  The dispatcher
     set is silicon-determined from the SoM preset's `capabilities:`
     block; apps pick per-handle at runtime via
     `alp_inference_open(.backend=...)`.

  2. v1's top-level `os:` is REJECTED (the v2 schema's
     `not: { required: [os] }` clause).  Per-core runtime lives
     under `cores.<id>.os:` now.

  3. v1's top-level `peripherals:` / `libraries:` / `iot:` /
     `inference:` blocks are REJECTED.  All moved per-core in v2.

  4. POSITIVE CONTROL: `cores.<id>.inference: { default_arena_kib: N }`
     is ACCEPTED -- arena tuning is an app-level concern, not a
     silicon fact.

If any of these regress (e.g. someone re-adds `backend:` to the
schema's inference block), this file's tests catch it before the
schema lands in main.

Run locally:

    python -m pytest tests/scripts/test_silicon_determined_fields_rejected.py -v
"""

from __future__ import annotations

import sys
import textwrap
from pathlib import Path

import pytest


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import (                       # noqa: E402
    OrchestratorError,
    load_board_yaml,
)


# ---------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------


def _write_board(tmp: Path, body: str) -> Path:
    """Write a board.yaml in `tmp` with a dedented body."""
    path = tmp / "board.yaml"
    path.write_text(textwrap.dedent(body).lstrip("\n"), encoding="utf-8")
    return path


def _expect_schema_reject(tmp: Path, body: str, *, marker: str) -> str:
    """Load `body`, expect OrchestratorError, return the error message.
    `marker` is the substring the error message should contain (so we
    catch regressions where the schema goes vague)."""
    path = _write_board(tmp, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert marker.lower() in msg.lower(), (
        f"expected '{marker}' in error message; got:\n{msg}"
    )
    return msg


# ---------------------------------------------------------------------
# 1. inference.backend rejected for every removed value
# ---------------------------------------------------------------------


@pytest.mark.parametrize(
    "backend_value",
    ["auto", "cpu", "ethos_u", "drpai", "deepx_dx"],
)
def test_inference_backend_rejected(
    tmp_path: Path, backend_value: str,
) -> None:
    """Every value the removed `backend:` field used to accept must
    now trip the schema's `additionalProperties: false` rule."""
    body = f"""
        som:
          sku: E1M-AEN701

        cores:
          m55_hp:
            app: ./src
            inference:
              backend: {backend_value}
    """
    msg = _expect_schema_reject(tmp_path, body, marker="backend")
    # Confirm the error points at the right JSON-pointer location so a
    # regression that *moves* the failure (schema accepts at cores
    # scope, fails elsewhere) is caught.
    assert "inference" in msg.lower()


def test_inference_arena_only_accepted(tmp_path: Path) -> None:
    """POSITIVE CONTROL: `inference: { default_arena_kib: N }` (the
    one knob the schema still carries) must load cleanly."""
    body = """
        som:
          sku: E1M-AEN701

        cores:
          m55_hp:
            app: ./src
            inference:
              default_arena_kib: 256
    """
    path = _write_board(tmp_path, body)
    # Must not raise.
    project = load_board_yaml(path)
    assert project.sku == "E1M-AEN701"


def test_inference_arena_below_minimum_rejected(tmp_path: Path) -> None:
    """Schema bounds-check on the arena: `minimum: 16` means a 15-KiB
    request must fail (catches a regression where the bound is dropped)."""
    body = """
        som:
          sku: E1M-AEN701

        cores:
          m55_hp:
            app: ./src
            inference:
              default_arena_kib: 15
    """
    _expect_schema_reject(tmp_path, body, marker="15")


# ---------------------------------------------------------------------
# 2. v1 top-level `os:` rejected
# ---------------------------------------------------------------------


def test_top_level_os_rejected(tmp_path: Path) -> None:
    """The v2 schema's `not: { required: [os] }` clause must reject
    any board.yaml that still uses the v1 top-level `os:` field."""
    body = """
        som:
          sku: E1M-AEN701

        os: zephyr

        cores:
          m55_hp:
            app: ./src
    """
    msg = _expect_schema_reject(tmp_path, body, marker="os")
    # The schema rejection should mention either `not` or the
    # `os` key name explicitly so the customer can fix it.
    assert "os" in msg.lower()


def test_top_level_inference_rejected(tmp_path: Path) -> None:
    """V1 also accepted a top-level `inference:` block.  V2 forces it
    per-core, so a top-level instance violates additionalProperties."""
    body = """
        som:
          sku: E1M-AEN701

        inference:
          default_arena_kib: 256

        cores:
          m55_hp:
            app: ./src
    """
    _expect_schema_reject(tmp_path, body, marker="inference")


def test_top_level_peripherals_rejected(tmp_path: Path) -> None:
    """Same for top-level `peripherals:` -- v1 shape, v2 forbids it."""
    body = """
        som:
          sku: E1M-AEN701

        peripherals:
          - i2c
          - gpio

        cores:
          m55_hp:
            app: ./src
    """
    _expect_schema_reject(tmp_path, body, marker="peripherals")


def test_top_level_libraries_rejected(tmp_path: Path) -> None:
    """And top-level `libraries:`."""
    body = """
        som:
          sku: E1M-AEN701

        libraries:
          - lvgl
          - mbedtls

        cores:
          m55_hp:
            app: ./src
    """
    _expect_schema_reject(tmp_path, body, marker="libraries")


def test_top_level_iot_rejected(tmp_path: Path) -> None:
    """And top-level `iot:`."""
    body = """
        som:
          sku: E1M-AEN701

        iot:
          wifi: true

        cores:
          m55_hp:
            app: ./src
    """
    _expect_schema_reject(tmp_path, body, marker="iot")


# ---------------------------------------------------------------------
# 3. Per-core os enum is the closed set {zephyr, yocto, baremetal, off}
# ---------------------------------------------------------------------


@pytest.mark.parametrize(
    "bad_os",
    ["auto", "linux", "freertos", "rtos", "any"],
)
def test_per_core_os_enum_closed(tmp_path: Path, bad_os: str) -> None:
    """Any string outside the documented enum must be rejected so a
    future maintainer can't quietly extend the runtime set without
    going through the schema review."""
    body = f"""
        som:
          sku: E1M-V2N101

        cores:
          m33_sm:
            os: {bad_os}
            app: ./src
    """
    _expect_schema_reject(tmp_path, body, marker="os")


# ---------------------------------------------------------------------
# 4. cores.<id>.inference is the closed shape {default_arena_kib}
# ---------------------------------------------------------------------


@pytest.mark.parametrize(
    "field_name",
    ["preferred_backend", "model_path", "ethos_u_variant",
     "npu_population", "auto", "backend_list"],
)
def test_inference_block_rejects_unknown_fields(
    tmp_path: Path, field_name: str,
) -> None:
    """`inference:` has `additionalProperties: false`.  Any field
    other than `default_arena_kib` must fail -- catches regressions
    where someone re-leaks SoM-preset fields into the customer
    schema."""
    body = f"""
        som:
          sku: E1M-AEN701

        cores:
          m55_hp:
            app: ./src
            inference:
              {field_name}: whatever
    """
    msg = _expect_schema_reject(tmp_path, body, marker=field_name)
    assert "inference" in msg.lower()
