# SPDX-License-Identifier: Apache-2.0
"""`_check_tier_a_library_ci` in `scripts/validate_metadata.py` cross-checks
`metadata/registries/tier-a-library-ci.json`'s `familyMatrix[]` cells
against each cell's SoM preset `topology.<core>`.

`topology` is schema-typed as an object in som-preset-v1, but
`doc.get("topology") or {}` does not protect a non-empty scalar (e.g. a bare
string, which is truthy) -- a malformed SoM preset used to reach
`topology.get(core)` and raise `AttributeError`, aborting the whole gate
mid-run instead of producing a clean FAIL line (same class of bug as
`_check_board_targets`'s `topology.items()`, #see propagating-code-changes
audit).
"""

import importlib.util
import json
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _load_vm(tmp_path, monkeypatch, name):
    spec = importlib.util.spec_from_file_location(
        name, REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    monkeypatch.setattr(vm, "REPO", tmp_path)
    # Point the registry at a tmp fixture and disable schema validation so
    # this test exercises only the topology cross-check, not the schema
    # pass (which is covered elsewhere and would require a full-shape doc).
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_SCHEMA", tmp_path / "does-not-exist.json")
    return vm


def test_non_object_topology_does_not_crash_the_gate(tmp_path, monkeypatch):
    """A SoM preset publishing `topology: m33_sm` (a bare string) instead of
    a mapping used to crash when a familyMatrix cell's `core` happened to be
    a substring of that string (here, identical to it) -- `"m33_sm" not in
    "m33_sm"` is False, so the buggy code fell through to
    `topology.get(core)` and raised `AttributeError`. Normalised to `{}`,
    the cell is instead reported as a clean FAIL (`core` not found), never a
    crash."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_topology_crash")

    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps({
        "hostBuild": {"libraries": [], "excludedLibraries": {}},
        "familyMatrix": [{"family": "aen", "som": "E1M-TST001", "core": "m33_sm"}],
        "excludedFamilies": {},
    }))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)

    som = tmp_path / "E1M-TST001.yaml"
    som.write_text("sku: E1M-TST001\nfamily: aen\ntopology: m33_sm\n")

    failures = vm._check_tier_a_library_ci([], [som])  # must not raise
    assert failures
    assert "m33_sm" in failures[0][1][0]
    assert "is not a topology core" in failures[0][1][0]


def test_object_topology_with_matching_core_passes(tmp_path, monkeypatch):
    """Control: a well-formed `topology:` mapping with a Zephyr slice for
    the matrix cell's core resolves clean."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_topology_ok")

    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps({
        "hostBuild": {"libraries": [], "excludedLibraries": {}},
        "familyMatrix": [{"family": "aen", "som": "E1M-TST001", "core": "m33_sm"}],
        "excludedFamilies": {},
    }))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)

    som = tmp_path / "E1M-TST001.yaml"
    som.write_text(
        "sku: E1M-TST001\nfamily: aen\n"
        "topology:\n  m33_sm:\n    board: alp_e1m_tst001_m33_sm\n"
    )

    failures = vm._check_tier_a_library_ci([], [som])
    assert not failures


def test_real_tier_a_library_ci_registry_resolves_clean():
    """The one real shipped registry this rule actually guards -- must stay
    clean against the live checkout, not just a synthetic fixture."""
    import validate_metadata as vm

    library_files = sorted(vm.LIBRARIES.glob("*.yaml"))
    som_files = sorted(vm.SOM_PRESETS.glob("E1M-*.yaml"))
    assert library_files and som_files
    failures = vm._check_tier_a_library_ci(library_files, som_files)
    assert not failures
