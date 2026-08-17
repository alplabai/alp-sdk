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
"""

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]


def _load_vm():
    spec = importlib.util.spec_from_file_location(
        "vm_jlink_flash_device", REPO / "scripts/validate_metadata.py"
    )
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    return vm


def _write_fixture(name: str, body: str) -> Path:
    # _check_soc_jlink_flash_device_declared() reports paths relative to
    # REPO, so the fixture must live inside the checkout.
    p = REPO / "metadata" / "socs" / "alif" / "ensemble" / f".test-{name}.json"
    p.write_text(body, encoding="utf-8")
    return p


_ALIF_ENSEMBLE_HEADER = (
    '{ "vendor": "Alif Semiconductor", "family": "Ensemble", '
)


def test_rejects_variant_missing_the_key_entirely():
    vm = _load_vm()
    p = _write_fixture(
        "missing-key",
        _ALIF_ENSEMBLE_HEADER
        + '"variants": [ { "order_code": "AE999X", '
        '"debug": { "pyocd_target": "AE999X" } } ] }',
    )
    try:
        failures = vm._check_soc_jlink_flash_device_declared([p])
    finally:
        p.unlink(missing_ok=True)
    assert failures
    assert "AE999X" in failures[0][1][0]
    assert "debug.jlink_flash_device is absent" in failures[0][1][0]


def test_accepts_declared_null():
    vm = _load_vm()
    p = _write_fixture(
        "declared-null",
        _ALIF_ENSEMBLE_HEADER
        + '"variants": [ { "order_code": "AE999X", '
        '"debug": { "pyocd_target": "AE999X", "jlink_flash_device": null } } ] }',
    )
    try:
        failures = vm._check_soc_jlink_flash_device_declared([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_accepts_populated_string():
    vm = _load_vm()
    p = _write_fixture(
        "populated-string",
        _ALIF_ENSEMBLE_HEADER
        + '"variants": [ { "order_code": "AE999X", '
        '"debug": { "pyocd_target": "AE999X", "jlink_flash_device": "AE999X_HE" } } ] }',
    )
    try:
        failures = vm._check_soc_jlink_flash_device_declared([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_ignores_non_alif_ensemble_socs():
    vm = _load_vm()
    p = _write_fixture(
        "non-alif",
        '{ "vendor": "Renesas", "family": "RZ/V2N", '
        '"variants": [ { "order_code": "R9A09G077", "debug": {} } ] }',
    )
    try:
        failures = vm._check_soc_jlink_flash_device_declared([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures


def test_real_alif_ensemble_socs_resolve_clean():
    """The eight real shipped Alif Ensemble variants this rule actually
    guards -- must stay clean against the live checkout, not just a
    synthetic fixture."""
    vm = _load_vm()
    ensemble_dir = REPO / "metadata" / "socs" / "alif" / "ensemble"
    real = sorted(ensemble_dir.glob("e*.json"))
    assert len(real) == 6
    failures = vm._check_soc_jlink_flash_device_declared(real)
    assert not failures


def test_non_object_variants_entry_does_not_crash_the_gate():
    """`variants[]` entries are schema-typed objects, but the schema pass
    that would reject a non-object entry runs separately and is not
    guaranteed to have run first. A bare string in the list used to reach a
    bare `.get()` and raise `AttributeError` here, hiding the schema FAIL
    line that already explains the real problem. Filtered to dicts, this doc
    must return cleanly -- no real variant survives the filter."""
    vm = _load_vm()
    p = _write_fixture(
        "non-object-variant",
        _ALIF_ENSEMBLE_HEADER + '"variants": [ "not-a-dict" ] }',
    )
    try:
        failures = vm._check_soc_jlink_flash_device_declared([p])
    finally:
        p.unlink(missing_ok=True)
    assert not failures  # must not raise


def test_non_object_debug_block_does_not_crash_the_gate():
    """`variants[].debug` is schema-typed as an object, but a malformed
    document can still carry a NON-STRING scalar there (e.g. a bare
    integer, which is truthy) -- that used to reach `"jlink_flash_device"
    not in debug` and raise `TypeError: argument of type 'int' is not
    iterable` (a string would merely do a substring check and not crash,
    which is why this needs its own scalar type). Normalised to `{}`, the
    variant is treated as carrying no debug facts -- so the key is absent
    and the check correctly still fails, just without crashing."""
    vm = _load_vm()
    p = _write_fixture(
        "non-object-debug",
        _ALIF_ENSEMBLE_HEADER
        + '"variants": [ { "order_code": "AE999X", "debug": 5 } ] }',
    )
    try:
        failures = vm._check_soc_jlink_flash_device_declared([p])
    finally:
        p.unlink(missing_ok=True)
    assert failures  # must not raise; still correctly flags the absent key
    assert "debug.jlink_flash_device is absent" in failures[0][1][0]


def test_removing_the_key_from_a_real_soc_file_fails(tmp_path):
    """Proves the guard actually regresses: temporarily strip
    `jlink_flash_device` from a real, currently-clean variant (e5, the
    single-variant file) and confirm the check goes red, byte-for-byte
    restoring the original afterwards."""
    vm = _load_vm()
    real = REPO / "metadata" / "socs" / "alif" / "ensemble" / "e5.json"
    original = real.read_bytes()
    assert b'"jlink_flash_device": "AE512F80F55D5_HE"' in original
    mutated = original.replace(
        b',\n        "jlink_flash_device": "AE512F80F55D5_HE"', b""
    )
    assert mutated != original
    try:
        real.write_bytes(mutated)
        failures = vm._check_soc_jlink_flash_device_declared([real])
        assert failures
        assert "AE512F80F55D5LS" in failures[0][1][0]
        assert "debug.jlink_flash_device is absent" in failures[0][1][0]
    finally:
        real.write_bytes(original)
    assert real.read_bytes() == original
