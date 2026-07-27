# SPDX-License-Identifier: Apache-2.0
"""
`validate_metadata._check_soc_debug_probe_identity` -- the semantic gate
that makes `variants[].debug.jlink_device` keys real `cores[].id` values
(#987).  JSON Schema can express that `jlink_device` is an object of
string values but not that its *keys* name a real core on *this* SoC, so
this gate does that cross-reference.

Run locally:

    python -m pytest tests/scripts/test_soc_debug_probe_identity.py -v
"""

from __future__ import annotations

import copy
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import validate_metadata as V  # noqa: E402


def _run(tmp_path, monkeypatch, doc) -> int:
    """Run the gate on one synthetic SoC doc; return the failure count."""
    monkeypatch.setattr(V, "REPO", tmp_path)  # so relative_to(REPO) resolves
    p = tmp_path / "soc.json"
    p.write_text(json.dumps(doc))
    return len(V._check_soc_debug_probe_identity([p]))


_E8_LIKE = {
    "cores": [{"id": "m55_hp"}, {"id": "m55_he"}],
    "variants": [
        {
            "order_code": "AE822FA0E5597LS0",
            "debug": {
                "pyocd_target": "AE822FA0E5597LS0",
                "jlink_device": {
                    "m55_hp": "AE822FA0E5597LS0_HP",
                    "m55_he": "AE822FA0E5597LS0_HE",
                },
            },
        }
    ],
}


def test_valid_jlink_device_keys_pass(tmp_path, monkeypatch):
    assert _run(tmp_path, monkeypatch, copy.deepcopy(_E8_LIKE)) == 0


def test_jlink_device_key_referencing_unknown_core_fails(tmp_path, monkeypatch):
    doc = copy.deepcopy(_E8_LIKE)
    doc["variants"][0]["debug"]["jlink_device"]["m55_nope"] = "AE822FA0E5597LS0_XX"
    assert _run(tmp_path, monkeypatch, doc) == 1


def test_variant_with_no_debug_block_passes(tmp_path, monkeypatch):
    # Absence is a valid, publishable state -- the cardinal rule of #987.
    doc = {"cores": [{"id": "m55_hp"}], "variants": [{"order_code": "AE302F80F55D5AE"}]}
    assert _run(tmp_path, monkeypatch, doc) == 0


def test_pyocd_target_only_variant_passes(tmp_path, monkeypatch):
    # E3/E5/E7: pyocd_target with no jlink_device at all -- nothing to cross-ref.
    doc = {
        "cores": [{"id": "m55_hp"}, {"id": "m55_he"}],
        "variants": [{"order_code": "AE512F80F55D5LS", "debug": {"pyocd_target": "AE512F80F55D5LS"}}],
    }
    assert _run(tmp_path, monkeypatch, doc) == 0


def test_real_alif_socs_pass_the_gate():
    # The shipped e3-e8 SoC specs must satisfy the gate as-is.
    socs = sorted((V.SOCS / "alif" / "ensemble").glob("e*.json"))
    assert socs, "no Alif ensemble SoC specs found"
    assert V._check_soc_debug_probe_identity(socs) == []
