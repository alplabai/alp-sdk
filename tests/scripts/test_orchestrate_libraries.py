# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_orchestrate/ -- extra_libraries: escape
hatch (inline kconfig / profile-driven, cross-field validation, v0.6 P2.1).

Split out of the orchestrator test suite as part of issue #460 / #673
Phase 3 (module-size reduction).

Run locally:

    python -m pytest tests/scripts/test_orchestrate_libraries.py -v
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import _write_board  # noqa: E402

from alp_orchestrate import (                       # noqa: E402
    OrchestratorError,
    _slice_alp_conf,
    load_board_yaml,
)


# ---------------------------------------------------------------------
# v0.6 P2.1 -- extra_libraries: escape hatch
# ---------------------------------------------------------------------


_V2N_BASE_FOR_EXTRA = """
som:
  sku: E1M-V2N101

cores:
  m33_sm:
    os: zephyr
    app: ./m33
{extra}
"""


def _v2n_with_extra(extra_yaml: str) -> str:
    """Build a V2N101 board.yaml with the given extra_libraries block
    (indented two spaces under the m33_sm core)."""
    return _V2N_BASE_FOR_EXTRA.format(extra=extra_yaml)


def test_extra_libraries_inline_kconfig_happy(tmp_path: Path) -> None:
    """An entry with `name:` + `kconfig:` loads cleanly and lands in
    the slice's emitted alp.conf verbatim."""
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: mylib\n"
        "        include_path: third_party/mylib/include\n"
        "        kconfig:\n"
        "          - CONFIG_MYLIB=y\n"
        "          - CONFIG_MYLIB_FEATURE_X=y\n")
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    slice_ = project.cores["m33_sm"]
    assert len(slice_.extra_libraries) == 1
    assert slice_.extra_libraries[0]["name"] == "mylib"
    conf = _slice_alp_conf(project, slice_)
    assert "extra_libraries[mylib]" in conf
    assert "CONFIG_MYLIB=y" in conf
    assert "CONFIG_MYLIB_FEATURE_X=y" in conf


def test_extra_libraries_profile_happy(tmp_path: Path) -> None:
    """An entry with `name:` + `profile:` resolves the profile file
    and emits its accelerators / sw_fallback Kconfig per the same
    silicon / soc_family / requires_cap matcher used by curated libs."""
    # Profile file lives under metadata/library-profiles/ -- mbedtls
    # ships with one out of the box, and it carries an `optiga_trust_m`
    # priority entry plus an sw_fallback that always matches.  Reuse
    # it as the test stub.
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: mylib_profile\n"
        "        profile: metadata/library-profiles/mbedtls/hw-backends.yaml\n")
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path)
    slice_ = project.cores["m33_sm"]
    conf = _slice_alp_conf(project, slice_)
    assert "extra_libraries[mylib_profile]" in conf
    # V2N101 has optiga_trust_m + cau capabilities, but those mbedTLS
    # accelerator entries are `status: planned`; the profile walker
    # must skip them and emit only the sw_fallback line.
    assert "CONFIG_ALP_MBEDTLS_CAU=y" not in conf
    assert "CONFIG_ALP_MBEDTLS_OPTIGA=y" not in conf
    assert "CONFIG_ALP_MBEDTLS_PURE_C=y" in conf
    assert "sw_fallback" in conf


def test_extra_libraries_both_kconfig_and_profile_rejected(
    tmp_path: Path,
) -> None:
    """An entry declaring BOTH `kconfig:` and `profile:` must fail
    the cross-field validator (the exactly-one rule)."""
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: mylib_bad\n"
        "        kconfig: [CONFIG_X=y]\n"
        "        profile: metadata/library-profiles/mbedtls/hw-backends.yaml\n")
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "mylib_bad" in msg
    assert "exactly one" in msg
    assert "both" in msg.lower()


def test_extra_libraries_neither_kconfig_nor_profile_rejected(
    tmp_path: Path,
) -> None:
    """An entry declaring NEITHER `kconfig:` nor `profile:` must
    fail the cross-field validator (the exactly-one rule)."""
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: mylib_empty\n"
        "        include_path: third_party/foo/include\n")
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "mylib_empty" in msg
    assert "exactly one" in msg
    assert "neither" in msg.lower()


def test_extra_libraries_name_collides_with_curated(tmp_path: Path) -> None:
    """A name that matches the curated `libraries:` enum (e.g.
    `mbedtls`) must be rejected -- the escape hatch is for
    non-curated entries only."""
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: mbedtls\n"
        "        kconfig: [CONFIG_FOO=y]\n")
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "mbedtls" in msg
    assert "curated" in msg.lower()


def test_extra_libraries_name_collides_across_cores(tmp_path: Path) -> None:
    """Names must be globally unique across all cores' extra_libraries."""
    body = """
som:
  sku: E1M-V2N101

cores:
  a55_cluster:
    os: yocto
    app: ./linux
    image: alp-image-edge
    extra_libraries:
      - name: shared_slug
        kconfig: [CONFIG_X=y]
  m33_sm:
    os: zephyr
    app: ./m33
    extra_libraries:
      - name: shared_slug
        kconfig: [CONFIG_Y=y]
"""
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "shared_slug" in msg
    assert "globally unique" in msg.lower() or "collides" in msg.lower()


def test_extra_libraries_profile_file_missing(tmp_path: Path) -> None:
    """A `profile:` path that doesn't resolve to a file must fail."""
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: phantom\n"
        "        profile: metadata/library-profiles/does-not-exist/hw-backends.yaml\n")
    path = _write_board(tmp_path, body)
    with pytest.raises(OrchestratorError) as excinfo:
        load_board_yaml(path)
    msg = str(excinfo.value)
    assert "phantom" in msg
    assert "does not resolve" in msg


