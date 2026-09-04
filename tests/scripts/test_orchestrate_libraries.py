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

import shutil
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _orchestrate_support import REPO, _write_board  # noqa: E402

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
    # The open-set `profile:` escape hatch takes an arbitrary, user-supplied
    # hw-backends.yaml-style file.  Stand up a self-contained stub whose HW
    # accelerator entries are `status: planned` (so the walker skips them) plus
    # an sw_fallback that always matches.
    profile = tmp_path / "mylib-hw-backends.yaml"
    profile.write_text(
        "schema_version: 1\n"
        "library: mylib_profile\n"
        "class: crypto\n"
        "accelerators:\n"
        "  - class: crypto\n"
        "    priority:\n"
        "      - { requires_cap: optiga_trust_m, backend: optiga, "
        "kconfig: CONFIG_ALP_MBEDTLS_OPTIGA=y, status: planned }\n"
        "      - { requires_cap: cau, backend: cau, "
        "kconfig: CONFIG_ALP_MBEDTLS_CAU=y, status: planned }\n"
        "sw_fallback:\n"
        "  kconfig: CONFIG_ALP_MBEDTLS_PURE_C=y\n",
        encoding="utf-8")
    body = _v2n_with_extra(
        "    extra_libraries:\n"
        "      - name: mylib_profile\n"
        f"        profile: {profile}\n")
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


# ---------------------------------------------------------------------
# #1485 follow-up -- the ADR-0018 library layer (scripts/alp_orchestrate/
# libraries.py) is a SECOND resolver family the original #1485 fix missed:
# `resolve_selection` / `zephyr_kconfig_lines` / `yocto_unwireable` /
# `yocto_image_install` / `baremetal_cmake_args` all take a `metadata_root:
# Path = METADATA_ROOT` DEFAULT ARGUMENT, so a kconfig.py call site that
# passes `project` + `slice_` but omits the third positional arg silently
# binds the module-global default instead of `project.effective_
# metadata_root()` -- the exact two-trees-in-one-artifact shape #1485
# describes, just one layer further out than the fixed loader/carveout/
# partition call sites.
# ---------------------------------------------------------------------


def test_curated_library_kconfig_resolves_against_explicit_metadata_root(
    tmp_path: Path,
) -> None:
    """#1485 follow-up: a project-wide `libraries: [cmsis-dsp]` Kconfig
    line must come from the manifest at `metadata_root`, not the SDK's
    own in-tree `metadata/libraries/cmsis-dsp.yaml` -- pre-fix,
    `kconfig.py`'s `_library_layer.zephyr_kconfig_lines(project, slice_)`
    (and four siblings) called the ADR-0018 layer without its third
    `metadata_root` argument, so they always read the in-tree manifest
    even under `load_board_yaml(..., metadata_root=<scratch>)`.

    Mutates the scratch root's cmsis-dsp manifest exactly as the
    adversarial review's manual repro did: swap
    `CONFIG_CMSIS_DSP_TRANSFORM=y` for a scratch-only marker line. The
    emitted alp.conf must carry the marker and must NOT carry the
    in-tree-only symbol.
    """
    meta = tmp_path / "metadata"
    shutil.copytree(REPO / "metadata", meta)
    manifest = meta / "libraries" / "cmsis-dsp.yaml"
    text = manifest.read_text(encoding="utf-8")
    assert "CONFIG_CMSIS_DSP_TRANSFORM=y" in text, (
        "cmsis-dsp.yaml no longer declares CONFIG_CMSIS_DSP_TRANSFORM=y")
    manifest.write_text(
        text.replace("CONFIG_CMSIS_DSP_TRANSFORM=y",
                     "CONFIG_SCRATCH_ONLY_MARKER=y"),
        encoding="utf-8")

    body = """
name: test-v2n-metadata-root-libs
som:
  sku: E1M-V2N101
  hw_rev: r1

libraries:
  - cmsis-dsp

cores:
  m33_sm:
    os: zephyr
    app: ./m33
"""
    path = _write_board(tmp_path, body)
    project = load_board_yaml(path, metadata_root=meta)
    out = _slice_alp_conf(project, project.cores["m33_sm"])
    assert "CONFIG_SCRATCH_ONLY_MARKER=y" in out
    assert "CONFIG_CMSIS_DSP_TRANSFORM=y" not in out

    # Same board, no override: must resolve against the real in-tree
    # manifest and emit the REAL symbol, not the scratch marker.
    project_intree = load_board_yaml(path)
    out_intree = _slice_alp_conf(project_intree, project_intree.cores["m33_sm"])
    assert "CONFIG_CMSIS_DSP_TRANSFORM=y" in out_intree
    assert "CONFIG_SCRATCH_ONLY_MARKER=y" not in out_intree


