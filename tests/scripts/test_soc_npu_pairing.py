# SPDX-License-Identifier: Apache-2.0
"""
`validate_metadata._check_soc_npu_pairing` -- the semantic gate that makes
`npus[].paired_core` the enforced single source of the NPU->core pairing
(#909 follow-up).  JSON Schema can't express the cross-reference, so this
gate does: (1) every paired_core names a real cores[].id, and (2) an NPU
`type` that appears with >1 distinct mac_per_cycle must pair every instance.

Run locally:

    python -m pytest tests/scripts/test_soc_npu_pairing.py -v
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
    return len(V._check_soc_npu_pairing([p]))


_E3_LIKE = {
    "cores": [{"id": "m55_hp"}, {"id": "m55_he"}],
    "npus": [
        {"type": "ethos-u55", "subtype": "high-perf", "mac_per_cycle": 256, "paired_core": "m55_hp"},
        {"type": "ethos-u55", "subtype": "high-efficiency", "mac_per_cycle": 128, "paired_core": "m55_he"},
    ],
}


def test_valid_paired_soc_passes(tmp_path, monkeypatch):
    assert _run(tmp_path, monkeypatch, copy.deepcopy(_E3_LIKE)) == 0


def test_paired_core_referencing_unknown_core_fails(tmp_path, monkeypatch):
    doc = copy.deepcopy(_E3_LIKE)
    doc["npus"][0]["paired_core"] = "m55_nope"
    assert _run(tmp_path, monkeypatch, doc) == 1


def test_multi_mac_variant_with_unpaired_instance_fails(tmp_path, monkeypatch):
    doc = copy.deepcopy(_E3_LIKE)
    for n in doc["npus"]:
        n.pop("paired_core", None)
    assert _run(tmp_path, monkeypatch, doc) == 1


def test_single_mac_variant_may_omit_paired_core(tmp_path, monkeypatch):
    # One U55 only -> unambiguous, paired_core not required.
    doc = {"cores": [{"id": "m55_he"}],
           "npus": [{"type": "ethos-u55", "subtype": "high-efficiency", "mac_per_cycle": 128}]}
    assert _run(tmp_path, monkeypatch, doc) == 0


def test_shared_u85_may_omit_paired_core(tmp_path, monkeypatch):
    # E8-like: the U85 on the HG subsystem is legitimately unpaired; the two
    # U55s are paired.  Distinct-MAC rule is per-type, so U85 (single 256) and
    # the paired U55s all pass.
    doc = {
        "cores": [{"id": "m55_hp"}, {"id": "m55_he"}],
        "npus": [
            {"type": "ethos-u85", "subtype": "generative", "mac_per_cycle": 256},
            {"type": "ethos-u55", "subtype": "high-perf", "mac_per_cycle": 256, "paired_core": "m55_hp"},
            {"type": "ethos-u55", "subtype": "high-efficiency", "mac_per_cycle": 128, "paired_core": "m55_he"},
        ],
    }
    assert _run(tmp_path, monkeypatch, doc) == 0


def test_real_alif_socs_pass_the_gate():
    # The shipped e3-e8 SoC specs must satisfy the gate as-is.
    socs = sorted((V.SOCS / "alif" / "ensemble").glob("e*.json"))
    assert socs, "no Alif ensemble SoC specs found"
    assert V._check_soc_npu_pairing(socs) == []
