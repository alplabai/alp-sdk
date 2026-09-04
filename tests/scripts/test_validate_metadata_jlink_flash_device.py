# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#1295: an ABSENT `variants[].debug.jlink_flash_device` makes tan's
`flow_d_available()` false, which SILENTLY downgrades Flow D (the J-Link
MRAM loader) to the SE-UART Flow A path with no diagnostic -- the AEN
runbook's #1 trap. Only two of the eight Alif Ensemble variants shipped the
key at all before this fix; the rest were an invisible gap because
`validate_metadata.py` never checked for it.

`_check_soc_jlink_flash_device_declared` in `scripts/validate_metadata.py`
is the semantic guard: every variant of an Alif Semiconductor / Ensemble SoC
JSON must publish the key, as a non-empty string OR explicit `null` (a
declared known-unknown) -- never omit it outright. Non-Alif-Ensemble SoCs
(Renesas, NXP, DEEPX) are untouched; the schema's `oneOf` already permits
string-or-null where the key IS present, this check only guards presence.

Synthetic fixtures below live under pytest's `tmp_path`, not the real
`metadata/socs/alif/ensemble/` tree: `Path.rglob("*.json")` (what
`validate_metadata.py`'s own `SOCS.rglob("*.json")` walk uses) matches
dot-prefixed files too, so a fixture dropped into the real tree is picked up
by every other test/gate that walks it. `REPO` is monkeypatched to
`tmp_path` so the checked function's own `path.relative_to(REPO)` still
resolves.
"""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _load_vm(tmp_path, monkeypatch):
    spec = importlib.util.spec_from_file_location(
        "vm_jlink_flash_device", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    monkeypatch.setattr(vm, "REPO", tmp_path)
    return vm


def _write_fixture(tmp_path: Path, name: str, body: str) -> Path:
    p = tmp_path / f"{name}.json"
    p.write_text(body, encoding="utf-8")
    return p


_ALIF_ENSEMBLE_HEADER = (
    '{ "vendor": "Alif Semiconductor", "family": "Ensemble", '
)


def test_rejects_variant_missing_the_key_entirely(tmp_path, monkeypatch):
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "missing-key",
        _ALIF_ENSEMBLE_HEADER
        + '"variants": [ { "order_code": "AE999X", '
        '"debug": { "pyocd_target": "AE999X" } } ] }',
    )
    failures = vm._check_soc_jlink_flash_device_declared([p])
    assert failures
    assert "AE999X" in failures[0][1][0]
    assert "debug.jlink_flash_device is absent" in failures[0][1][0]


def test_accepts_declared_null(tmp_path, monkeypatch):
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "declared-null",
        _ALIF_ENSEMBLE_HEADER
        + '"variants": [ { "order_code": "AE999X", '
        '"debug": { "pyocd_target": "AE999X", "jlink_flash_device": null } } ] }',
    )
    failures = vm._check_soc_jlink_flash_device_declared([p])
    assert not failures


def test_accepts_populated_string(tmp_path, monkeypatch):
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "populated-string",
        _ALIF_ENSEMBLE_HEADER
        + '"variants": [ { "order_code": "AE999X", '
        '"debug": { "pyocd_target": "AE999X", "jlink_flash_device": "AE999X_HE" } } ] }',
    )
    failures = vm._check_soc_jlink_flash_device_declared([p])
    assert not failures


def test_ignores_non_alif_ensemble_socs(tmp_path, monkeypatch):
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "non-alif",
        '{ "vendor": "Renesas", "family": "RZ/V2N", '
        '"variants": [ { "order_code": "R9A09G077", "debug": {} } ] }',
    )
    failures = vm._check_soc_jlink_flash_device_declared([p])
    assert not failures


def test_non_object_variant_entry_does_not_crash_the_gate():
    """`variants[]` entries are schema-typed objects, but the schema pass
    that would reject a non-object entry runs separately and is not
    guaranteed to have run first. A bare string used to reach a bare
    `v.get("debug")` and raise `AttributeError` here, hiding the schema FAIL
    line that already explains the real problem. Filtered to dicts, this doc
    must return cleanly -- no real variant survives the filter, so there is
    nothing left to check for the key."""
    vm = _load_vm()
    p = _write_fixture(
        "non-object-variant",
        _ALIF_ENSEMBLE_HEADER + '"variants": [ "not-a-dict" ] }',
    )
    try:
        failures = vm._check_soc_jlink_flash_device_declared([p])  # must not raise
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_real_alif_ensemble_socs_resolve_clean():
    """The eight real shipped Alif Ensemble variants this rule actually
    guards -- must stay clean against the live checkout, not just a
    synthetic fixture."""
    spec = importlib.util.spec_from_file_location(
        "vm_jlink_flash_device_real", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    ensemble_dir = REPO / "metadata" / "socs" / "alif" / "ensemble"
    real = sorted(ensemble_dir.glob("e*.json"))
    assert len(real) == 6
    failures = vm._check_soc_jlink_flash_device_declared(real)
    assert not failures


def test_non_object_variants_entry_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`variants[]` entries are schema-typed objects, but the schema pass
    that would reject a non-object entry runs separately and is not
    guaranteed to have run first. A bare string in the list used to reach a
    bare `.get()` and raise `AttributeError` here, hiding the schema FAIL
    line that already explains the real problem. Filtered to dicts, this doc
    must return cleanly -- no real variant survives the filter."""
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "non-object-variant",
        _ALIF_ENSEMBLE_HEADER + '"variants": [ "not-a-dict" ] }',
    )
    failures = vm._check_soc_jlink_flash_device_declared([p])
    assert not failures  # must not raise


def test_non_object_debug_block_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`variants[].debug` is schema-typed as an object, but a malformed
    document can still carry a NON-STRING scalar there (e.g. a bare
    integer, which is truthy) -- that used to reach `"jlink_flash_device"
    not in debug` and raise `TypeError: argument of type 'int' is not
    iterable` (a string would merely do a substring check and not crash,
    which is why this needs its own scalar type). Normalised to `{}`, the
    variant is treated as carrying no debug facts -- so the key is absent
    and the check correctly still fails, just without crashing."""
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(
        tmp_path, "non-object-debug",
        _ALIF_ENSEMBLE_HEADER
        + '"variants": [ { "order_code": "AE999X", "debug": 5 } ] }',
    )
    failures = vm._check_soc_jlink_flash_device_declared([p])
    assert failures  # must not raise; still correctly flags the absent key
    assert "debug.jlink_flash_device is absent" in failures[0][1][0]


def test_non_object_top_level_does_not_crash_the_gate(tmp_path, monkeypatch):
    """The SoC doc's top level is schema-typed as an object, but a
    malformed file could parse to a bare JSON array -- `doc.get("vendor")`
    used to raise `AttributeError: 'list' object has no attribute 'get'`
    here, aborting the whole gate mid-run instead of leaving the schema
    FAIL line (which already flags the type mismatch) to explain the real
    problem."""
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(tmp_path, "non-object-doc", "[]")
    failures = vm._check_soc_jlink_flash_device_declared([p])
    assert failures == []  # must not raise


def test_non_list_variants_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`variants` itself is schema-typed as an array, but a malformed
    document can carry a non-list scalar there (e.g. the bare int `5`,
    which is truthy) -- iterating the unfiltered value used to raise
    `TypeError: 'int' object is not iterable` here, aborting the whole gate
    mid-run instead of leaving the schema FAIL line (which already flags
    the type mismatch) to explain the real problem."""
    vm = _load_vm(tmp_path, monkeypatch)
    p = _write_fixture(tmp_path, "non-list-variants", _ALIF_ENSEMBLE_HEADER + '"variants": 5 }')
    failures = vm._check_soc_jlink_flash_device_declared([p])
    assert failures == []  # must not raise


def test_removing_the_key_from_a_real_soc_file_fails(tmp_path, monkeypatch):
    """Proves the guard actually regresses: strip `jlink_flash_device`
    from a COPY of a real, currently-clean variant (e5, the
    single-variant file) and confirm the check goes red.

    Mutates a copy under `tmp_path`, never the real tracked file -- a
    prior version of this test mutated `metadata/socs/.../e5.json`
    in-process with a `try`/`finally` restore, and that restore does not
    survive a SIGKILL (this host has been killing runs), which would
    leave the real tree dirty with a stripped SoC file. Same `tmp_path`
    copy idiom `test_hw_rev_table_unreadable.py::_metadata_copy` uses.
    """
    spec = importlib.util.spec_from_file_location(
        "vm_jlink_flash_device_mutate", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    monkeypatch.setattr(vm, "REPO", tmp_path)
    real = REPO / "metadata" / "socs" / "alif" / "ensemble" / "e5.json"
    original = real.read_bytes()
    assert b'"jlink_flash_device": "AE512F80F55D5_HE"' in original
    mutated = original.replace(
        b',\n        "jlink_flash_device": "AE512F80F55D5_HE"', b""
    )
    assert mutated != original
    copy = tmp_path / "e5.json"
    copy.write_bytes(mutated)
    failures = vm._check_soc_jlink_flash_device_declared([copy])
    assert failures
    assert "AE512F80F55D5LS" in failures[0][1][0]
    assert "debug.jlink_flash_device is absent" in failures[0][1][0]
    # The real tracked file must be byte-for-byte untouched.
    assert real.read_bytes() == original
