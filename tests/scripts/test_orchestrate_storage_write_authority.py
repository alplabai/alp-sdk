# SPDX-License-Identifier: Apache-2.0
"""`_resolve_flash_device()` must not place a runtime mount on a
`memory_map` region whose `write_authority` forbids it (alp-sdk#2088).

Mirrors `check_atoc_reservation._check_top_write_authority()`
(alp-sdk#2086): same vocabulary (`customer_runtime` is the only value a
runtime mount/carve-out may land on, absent means unresolved and stays
legal today per the deferred-to-required field, alp-sdk#2024), opposite
direction -- that gate refuses a row that IS `customer_runtime` at the
ATOC band; this refuses a row that is NOT `customer_runtime` (and not the
`composite` whole-device-alias exemption) anywhere it is named directly as
a `flash_device:`.

Every case here is exercised directly against `_resolve_flash_device()`
with a hand-built `som_preset` -- `resolve_memory_map()` returns a
preset's own `memory_map:` verbatim (alp_project_loader.py:533-536)
without touching `metadata_root`, so no real SoM YAML or SoC JSON needs to
exist on disk for this file to run standalone.

Run locally:

    python -m pytest tests/scripts/test_orchestrate_storage_write_authority.py -v
"""

from __future__ import annotations

from alp_orchestrate.paths import METADATA_ROOT
from alp_orchestrate.partition import _resolve_flash_device


def _preset(region: dict) -> dict:
    return {"sku": "TEST-SOM", "memory_map": [region]}


class TestRefusesWrongAuthority:
    def test_secure_enclave_row_is_refused(self):
        """The literal #2088 repro shape: a mount named directly at a
        row the Secure Enclave owns, not the application."""
        descriptor, reason = _resolve_flash_device(
            "atoc", _preset({
                "name": "atoc", "base": 0x80578000, "size_kib": 32,
                "write_authority": "secure_enclave",
            }),
            METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "atoc" in reason
        assert "write_authority: 'secure_enclave'" in reason
        assert "is not customer-writable at runtime" in reason
        assert "write_authority: 'customer_runtime'" in reason, reason
        assert "#2088" in reason, reason

    def test_vendor_image_row_is_refused(self):
        """A factory-provisioned image (mcuboot's own tag) is equally
        off-limits -- the guard is not special-cased to 'atoc'."""
        descriptor, reason = _resolve_flash_device(
            "mcuboot", _preset({
                "name": "mcuboot", "base": 0x80000000, "size_kib": 64,
                "write_authority": "vendor_image",
            }),
            METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "write_authority: 'vendor_image'" in reason, reason

    def test_none_authority_row_is_refused(self):
        """`none` ("nobody writes it") is an explicit no-writer, not an
        invitation to mount a filesystem there."""
        descriptor, reason = _resolve_flash_device(
            "reserved", _preset({
                "name": "reserved", "base": 0x80550000, "size_kib": 64,
                "write_authority": "none",
            }),
            METADATA_ROOT)
        assert descriptor is None, descriptor
        assert "write_authority: 'none'" in reason, reason


class TestAllowsCustomerRuntime:
    def test_customer_runtime_row_resolves(self):
        descriptor, reason = _resolve_flash_device(
            "storage", _preset({
                "name": "storage", "base": 0x80560000, "size_kib": 96,
                "write_authority": "customer_runtime",
            }),
            METADATA_ROOT)
        assert reason is None, reason
        assert descriptor is not None
        assert descriptor["name"] == "storage"

    def test_composite_whole_device_alias_still_resolves(self):
        """`composite` ("consult the contained rows", the schema's own
        words) is NOT the #2088 hazard by itself -- it is the documented
        shape a whole-device alias carries (`mram_main` on every AEN
        preset; examples/connectivity/production-deployment/board.yaml
        names `flash_device: mram_main` directly). Refusing it here would
        break that example and every AEN storage test that targets
        mram_main, for a row that defers rather than claims runtime-write
        for itself; per-placement safety inside it is `_reserved_spans()`'s
        job, not this guard's."""
        descriptor, reason = _resolve_flash_device(
            "mram_main", _preset({
                "name": "mram_main", "base": 0x80000000, "size_kib": 5632,
                "write_authority": "composite",
            }),
            METADATA_ROOT)
        assert reason is None, reason
        assert descriptor is not None


class TestMissingWriteAuthorityStaysLegal:
    def test_absent_write_authority_still_resolves(self):
        """#2024: `write_authority` is deferred-to-required for som-preset
        v1 -- a customer preset authored before the field existed must not
        be hard-refused by this guard. Absent is "unresolved" per the
        schema, not "customer_runtime", but it is legal today, only
        flagged (#2088 requirements: decide deliberately, don't hard-fail
        every existing preset)."""
        descriptor, reason = _resolve_flash_device(
            "storage", _preset({
                "name": "storage", "base": 0x80560000, "size_kib": 96,
            }),
            METADATA_ROOT)
        assert reason is None, reason
        assert descriptor is not None
        assert descriptor["name"] == "storage"

    def test_absent_write_authority_warns_on_stderr(self, capsys):
        """Legal is not silent: the schema says a consumer must "say so"
        for an absent value (ADR-0034 clause 4, quoted verbatim in
        check_atoc_reservation.py's own absent-value branch)."""
        _resolve_flash_device(
            "storage", _preset({
                "name": "storage", "base": 0x80560000, "size_kib": 96,
            }),
            METADATA_ROOT)
        captured = capsys.readouterr()
        assert "no write_authority" in captured.err, captured.err
        assert "storage" in captured.err, captured.err
