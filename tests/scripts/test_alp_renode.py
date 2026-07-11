# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for the deterministic logic in
scripts/west_commands/alp_renode.py.

These cover only the HW-free / Renode-free parts:

  * system-manifest.yaml parsing,
  * single-Zephyr-slice ELF-path resolution,
  * SoM family -> Renode platform-descriptor mapping,
  * headless `renode` command-line construction,
  * the missing-`renode`-binary error path.

The actual Renode boot (the .repl / .resc) is NOT exercised here -- it
is proven by the advisory CI gate .github/workflows/pr-renode-aen-smoke.yml.

Run locally:

    python -m pytest tests/scripts/test_alp_renode.py -q
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
# alp_renode lives under scripts/west_commands/; alp_project (its lazy
# import) lives under scripts/.  conftest.py already adds scripts/.
sys.path.insert(0, str(REPO / "scripts" / "west_commands"))
sys.path.insert(0, str(REPO / "scripts"))

import alp_renode  # noqa: E402
from alp_renode import (  # noqa: E402
    AlpRenodeError,
    build_renode_argv,
    load_manifest,
    platform_files_for_sku,
    platform_stem_for_sku,
    resolve_renode_binary,
    zephyr_elf_from_manifest,
)


# ---------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------


def _write_manifest(build_root: Path, slices: list[dict],
                    hw_info: dict | None = None) -> Path:
    """Write a minimal system-manifest.yaml into build_root."""
    build_root.mkdir(parents=True, exist_ok=True)
    doc = {
        "schema_version": 1,
        "generated_by": "scripts/alp_orchestrate.py",
        "hw_info": hw_info if hw_info is not None else {"sku": "E1M-AEN801"},
        "slices": slices,
    }
    mpath = build_root / "system-manifest.yaml"
    mpath.write_text(yaml.safe_dump(doc, sort_keys=False), encoding="utf-8")
    return mpath


# ---------------------------------------------------------------------
# Manifest parsing
# ---------------------------------------------------------------------


def test_load_manifest_roundtrips(tmp_path):
    build_root = tmp_path / "build"
    _write_manifest(build_root, [{"core_id": "m55_hp", "os": "zephyr"}])
    data = load_manifest(build_root)
    assert data["hw_info"]["sku"] == "E1M-AEN801"
    assert data["slices"][0]["core_id"] == "m55_hp"


def test_load_manifest_missing_raises(tmp_path):
    with pytest.raises(AlpRenodeError, match="no system-manifest.yaml"):
        load_manifest(tmp_path / "build")


def test_load_manifest_non_mapping_raises(tmp_path):
    build_root = tmp_path / "build"
    build_root.mkdir(parents=True)
    (build_root / "system-manifest.yaml").write_text(
        "- just\n- a\n- list\n", encoding="utf-8")
    with pytest.raises(AlpRenodeError, match="top-level mapping"):
        load_manifest(build_root)


# ---------------------------------------------------------------------
# ELF-path resolution
# ---------------------------------------------------------------------


def test_elf_path_uses_manifest_build_dir(tmp_path):
    build_root = tmp_path / "build"
    bd = build_root / "m55_hp-zephyr"
    manifest = {
        "slices": [
            {"core_id": "m55_hp", "os": "zephyr", "build_dir": str(bd)},
        ],
    }
    elf = zephyr_elf_from_manifest(manifest, build_root)
    assert elf == bd / "zephyr" / "zephyr.elf"


def test_elf_path_reconstructs_when_build_dir_absent(tmp_path):
    build_root = tmp_path / "build"
    manifest = {"slices": [{"core_id": "m55_hp", "os": "zephyr"}]}
    elf = zephyr_elf_from_manifest(manifest, build_root)
    assert elf == build_root / "m55_hp-zephyr" / "zephyr" / "zephyr.elf"


def test_elf_path_relative_build_dir_anchored_to_build_root(tmp_path):
    build_root = tmp_path / "build"
    manifest = {
        "slices": [
            {"core_id": "m55_hp", "os": "zephyr",
             "build_dir": "m55_hp-zephyr"},
        ],
    }
    elf = zephyr_elf_from_manifest(manifest, build_root)
    assert elf == build_root / "m55_hp-zephyr" / "zephyr" / "zephyr.elf"


def test_elf_path_ignores_non_zephyr_and_blocked_slices(tmp_path):
    build_root = tmp_path / "build"
    bd = build_root / "m55_hp-zephyr"
    manifest = {
        "slices": [
            {"core_id": "a32_cluster", "os": "yocto"},
            {"core_id": "m55_he", "os": "zephyr", "status": "blocked"},
            {"core_id": "m55_hp", "os": "zephyr", "build_dir": str(bd)},
        ],
    }
    elf = zephyr_elf_from_manifest(manifest, build_root)
    assert elf == bd / "zephyr" / "zephyr.elf"


def test_elf_path_no_zephyr_slice_raises(tmp_path):
    manifest = {"slices": [{"core_id": "a32_cluster", "os": "yocto"}]}
    with pytest.raises(AlpRenodeError, match="no os: zephyr slice"):
        zephyr_elf_from_manifest(manifest, tmp_path / "build")


def test_elf_path_multiple_zephyr_slices_raises(tmp_path):
    manifest = {
        "slices": [
            {"core_id": "m55_hp", "os": "zephyr"},
            {"core_id": "m55_he", "os": "zephyr"},
        ],
    }
    with pytest.raises(AlpRenodeError, match="single-Zephyr-slice"):
        zephyr_elf_from_manifest(manifest, tmp_path / "build")


# ---------------------------------------------------------------------
# Family -> platform-file mapping
# ---------------------------------------------------------------------


def test_platform_stem_aen_resolves_to_e8():
    assert platform_stem_for_sku("E1M-AEN801") == "alif_ensemble_e8"
    # AEN701 shares the Ensemble M55 family -> same descriptor.
    assert platform_stem_for_sku("E1M-AEN701") == "alif_ensemble_e8"


def test_platform_files_paths(tmp_path):
    repl, resc = platform_files_for_sku("E1M-AEN801", tmp_path)
    assert repl == tmp_path / "metadata" / "renode" / "alif_ensemble_e8.repl"
    assert resc == tmp_path / "metadata" / "renode" / "alif_ensemble_e8.resc"


def test_platform_stem_v2n_resolves_to_renesas_rzv2n():
    # V2N family -> renesas_rzv2n, wired for the --sim-mode contract (#674).
    assert platform_stem_for_sku("E1M-V2N101") == "renesas_rzv2n"


def test_platform_stem_bad_sku_raises():
    with pytest.raises(AlpRenodeError):
        platform_stem_for_sku("NOT-A-SKU")


def test_real_descriptors_exist_for_aen():
    """The .repl/.resc this PR ships must actually be on disk for AEN."""
    repl, resc = platform_files_for_sku("E1M-AEN801", REPO)
    assert repl.is_file(), f"{repl} should exist"
    assert resc.is_file(), f"{resc} should exist"


def test_real_descriptors_exist_for_v2n():
    """The renesas_rzv2n .repl/.resc shipped for --sim-mode must be on disk."""
    repl, resc = platform_files_for_sku("E1M-V2N101", REPO)
    assert repl.is_file(), f"{repl} should exist"
    assert resc.is_file(), f"{resc} should exist"


# ---------------------------------------------------------------------
# Command-line construction
# ---------------------------------------------------------------------


def test_build_renode_argv_shape(tmp_path):
    repl = tmp_path / "p.repl"
    resc = tmp_path / "p.resc"
    elf = tmp_path / "zephyr.elf"
    argv = build_renode_argv("/usr/bin/renode", repl, resc, elf)
    assert argv[0] == "/usr/bin/renode"
    # Headless flags present.
    for flag in ("--console", "--disable-xwt", "--hide-monitor"):
        assert flag in argv
    # Variables injected before including the script, with @-prefixed paths.
    assert f"$repl=@{repl}" in argv
    assert f"$elf=@{elf}" in argv
    assert f"i @{resc}" in argv
    # The include must come after both variable assignments.
    assert argv.index(f"i @{resc}") > argv.index(f"$elf=@{elf}")
    assert argv.index(f"i @{resc}") > argv.index(f"$repl=@{repl}")


# ---------------------------------------------------------------------
# Missing-renode-binary error path
# ---------------------------------------------------------------------


def test_resolve_renode_binary_found():
    assert resolve_renode_binary(which=lambda name: "/opt/renode/renode") \
        == "/opt/renode/renode"


def test_resolve_renode_binary_missing_raises():
    with pytest.raises(AlpRenodeError, match="not found on PATH"):
        resolve_renode_binary(which=lambda name: None)


def test_resolve_renode_binary_queries_renode():
    seen = {}

    def fake_which(name):
        seen["name"] = name
        return "/usr/bin/renode"

    resolve_renode_binary(which=fake_which)
    assert seen["name"] == "renode"
