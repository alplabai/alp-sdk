# SPDX-License-Identifier: Apache-2.0
"""`_check_tier_a_library_ci` in `scripts/validate_metadata.py` cross-checks
`metadata/registries/tier-a-library-ci.json`'s `familyMatrix[]` cells
against each cell's SoM preset `topology.<core>`.

`topology` is schema-typed as an object in som-preset-v1, but
`doc.get("topology") or {}` does not protect a non-empty scalar (e.g. a bare
string, which is truthy) -- a malformed SoM preset used to reach
`topology.get(core)` and raise `AttributeError`, aborting the whole gate
mid-run instead of producing a clean FAIL line (same class of bug as
`_check_board_targets`'s `topology.items()`).
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


def test_excluded_libraries_as_a_list_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`hostBuild.excludedLibraries` is schema-typed as an object (name ->
    reason), but a malformed registry can carry a list there instead --
    `.keys()` on that used to raise `AttributeError: 'list' object has no
    attribute 'keys'` here, aborting the whole gate mid-run instead of
    leaving the schema FAIL line (which already flags the type mismatch) to
    explain the real problem."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_excluded_libs_list")
    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps({"hostBuild": {"excludedLibraries": ["foo"]}}))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)
    # `_as_dict()` normalises the malformed (list, not object) container to
    # `{}` -- same as a non-dict `capabilities:`/`hostBuild:` elsewhere in
    # this file -- so `foo` is not carried through as a name to check
    # against; the point pinned here is that this must not raise.
    failures = vm._check_tier_a_library_ci([], [])  # must not raise
    assert isinstance(failures, list)


def test_excluded_families_as_a_list_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`excludedFamilies` is schema-typed as an object (family -> reason),
    but a malformed registry can carry a list there instead -- `.items()` on
    that used to raise `AttributeError: 'list' object has no attribute
    'items'` here, aborting the whole gate mid-run instead of leaving the
    schema FAIL line (which already flags the type mismatch) to explain the
    real problem."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_excluded_families_list")
    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps({"excludedFamilies": ["foo"]}))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)
    failures = vm._check_tier_a_library_ci([], [])  # must not raise
    assert failures == []  # a list `excludedFamilies` iterates to nothing meaningful to check


def test_non_object_top_level_does_not_crash_the_gate(tmp_path, monkeypatch):
    """The registry's top level is schema-typed as an object, but a
    malformed file could parse to a bare JSON array -- `data.get("hostBuild",
    {})` used to raise `AttributeError: 'list' object has no attribute
    'get'` here, aborting the whole gate mid-run instead of leaving the
    schema FAIL line (which already flags the type mismatch) to explain the
    real problem."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_non_object_top")
    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps([]))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)
    failures = vm._check_tier_a_library_ci([], [])  # must not raise
    assert failures


def test_non_list_family_matrix_and_non_list_host_libraries_do_not_crash_the_gate(
    tmp_path, monkeypatch
):
    """`familyMatrix` and `hostBuild.libraries` are themselves schema-typed
    as arrays, but a malformed registry can carry a non-list scalar there
    (e.g. the bare int `5`, which is truthy) -- `enumerate(data.get(
    "familyMatrix") or [])` and `set(host.get("libraries") or [])` over the
    unfiltered value used to raise `TypeError: 'int' object is not
    iterable`, aborting the whole gate mid-run instead of leaving the schema
    FAIL line (which already flags the type mismatch) to explain the real
    problem."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_non_list_fields")
    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps({
        "hostBuild": {"libraries": 5, "excludedLibraries": {}},
        "familyMatrix": 5,
        "excludedFamilies": {},
    }))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)
    vm._check_tier_a_library_ci([], [])  # must not raise


def test_non_string_host_library_entry_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`hostBuild.libraries[]` entries are schema-typed as strings, but a
    malformed registry can carry a dict/list entry there -- the unfiltered
    `set(_as_list(host.get("libraries")))` used to raise `TypeError:
    unhashable type: 'dict'` building the set."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_nonstring_lib")
    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps({
        "hostBuild": {"libraries": [{"nested": "dict"}], "excludedLibraries": {}},
        "familyMatrix": [],
        "excludedFamilies": {},
    }))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)
    failures = vm._check_tier_a_library_ci([], [])  # must not raise
    assert isinstance(failures, list)


def test_non_string_family_matrix_som_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`familyMatrix[].som` is schema-typed as a string, but a malformed
    registry can carry a dict/list there -- the unfiltered
    `som_docs.get(som)` used to raise `TypeError: unhashable type: 'dict'`."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_nonstring_som")
    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps({
        "hostBuild": {"libraries": [], "excludedLibraries": {}},
        "familyMatrix": [{"family": "aen", "som": {"nested": "dict"}, "core": "m33_sm"}],
        "excludedFamilies": {},
    }))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)
    failures = vm._check_tier_a_library_ci([], [])  # must not raise
    assert isinstance(failures, list)


def test_non_string_family_matrix_core_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`familyMatrix[].core` is schema-typed as a string, but a malformed
    registry can carry a dict/list there -- the unfiltered `core not in
    topology` membership test used to raise `TypeError: unhashable type:
    'dict'`."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_nonstring_core")
    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps({
        "hostBuild": {"libraries": [], "excludedLibraries": {}},
        "familyMatrix": [{"family": "aen", "som": "E1M-TST001", "core": {"nested": "dict"}}],
        "excludedFamilies": {},
    }))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)
    som = tmp_path / "E1M-TST001.yaml"
    som.write_text(
        "sku: E1M-TST001\nfamily: aen\n"
        "topology:\n  m33_sm:\n    board: alp_e1m_tst001_m33_sm\n"
    )
    failures = vm._check_tier_a_library_ci([], [som])  # must not raise
    assert isinstance(failures, list)


def test_non_string_topology_keys_do_not_crash_the_gate(tmp_path, monkeypatch):
    """`topology`'s own KEYS are schema-typed as strings (core ids), but
    YAML -- unlike JSON -- permits int/float/bool/null mapping keys. When
    `core` is not found in `topology`, the unfiltered
    `sorted(topology)` used to raise `TypeError: '<' not supported
    between instances of 'str' and 'int'` building the `available:` list
    (mixed str/int keys)."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_int_topology_key")
    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps({
        "hostBuild": {"libraries": [], "excludedLibraries": {}},
        "familyMatrix": [{"family": "aen", "som": "E1M-TST001", "core": "m33_sm"}],
        "excludedFamilies": {},
    }))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)
    som = tmp_path / "E1M-TST001.yaml"
    # `99:` is a YAML integer key, sitting alongside the string key
    # `a55_cluster`; `core` ("m33_sm") is in neither, so the `core not in
    # topology` branch's `sorted(topology)` call over the mixed-type key
    # set is exercised.
    som.write_text(
        "sku: E1M-TST001\nfamily: aen\n"
        "topology:\n  a55_cluster:\n    board: x\n  99:\n    board: y\n"
    )
    failures = vm._check_tier_a_library_ci([], [som])  # must not raise
    assert failures
    assert "a55_cluster" in failures[0][1][0]
    assert "is not a topology core" in failures[0][1][0]


def test_all_non_string_topology_keys_do_not_crash_the_gate(tmp_path, monkeypatch):
    """The all-non-string-key variant of the same class: with every
    `topology` key a YAML int, the unfiltered `sorted(topology)` reaches
    the `", ".join(...)` and raises `TypeError: sequence item 0: expected
    str instance, int found` instead."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_all_int_topology_keys")
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
        "topology:\n  99:\n    board: y\n"
    )
    failures = vm._check_tier_a_library_ci([], [som])  # must not raise
    assert failures
    assert "available: <none>" in failures[0][1][0]


def test_null_som_reports_diagnostic_not_silently_dropped(tmp_path, monkeypatch):
    """A JSON `null` `som` is hashable (unlike a dict/list) and safe for
    `som_docs.get(som)` -- an overbroad `not isinstance(som, str)` skip
    (rather than one scoped to the actual unhashable-type hazard) would
    silently drop the `has no SoM preset` diagnostic instead of reporting
    it."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_null_som")
    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps({
        "hostBuild": {"libraries": [], "excludedLibraries": {}},
        "familyMatrix": [{"family": "aen", "som": None, "core": "m33_sm"}],
        "excludedFamilies": {},
    }))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)
    failures = vm._check_tier_a_library_ci([], [])  # must not raise
    assert failures
    assert "familyMatrix[0]/som: `None` has no SoM preset" in failures[0][1][0]


def test_null_core_reports_diagnostic_not_silently_dropped(tmp_path, monkeypatch):
    """Same reasoning as the `som` case above, for `core`: a JSON `null`
    is hashable and safe for `core not in topology`, and must still
    surface the `is not a topology core` diagnostic."""
    vm = _load_vm(tmp_path, monkeypatch, "vm_tier_a_null_core")
    registry = tmp_path / "tier-a-library-ci.json"
    registry.write_text(json.dumps({
        "hostBuild": {"libraries": [], "excludedLibraries": {}},
        "familyMatrix": [{"family": "aen", "som": "E1M-TST001", "core": None}],
        "excludedFamilies": {},
    }))
    monkeypatch.setattr(vm, "TIER_A_LIBRARY_CI_REGISTRY", registry)
    som = tmp_path / "E1M-TST001.yaml"
    som.write_text(
        "sku: E1M-TST001\nfamily: aen\n"
        "topology:\n  m33_sm:\n    board: x\n"
    )
    failures = vm._check_tier_a_library_ci([], [som])  # must not raise
    assert failures
    assert ("familyMatrix[0]/core: `None` is not a topology core on "
            "E1M-TST001 (available: m33_sm)") in failures[0][1][0]


def test_real_tier_a_library_ci_registry_resolves_clean():
    """The one real shipped registry this rule actually guards -- must stay
    clean against the live checkout, not just a synthetic fixture."""
    import validate_metadata as vm

    library_files = sorted(vm.LIBRARIES.glob("*.yaml"))
    som_files = sorted(vm.SOM_PRESETS.glob("E1M-*.yaml"))
    assert library_files and som_files
    failures = vm._check_tier_a_library_ci(library_files, som_files)
    assert not failures
