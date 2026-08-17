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
    # E8-like: the U85 is a shared, SoC-level NPU (Alif block name NPU_HG,
    # not wired to one core) and is legitimately unpaired; the two U55s are
    # paired.  Distinct-MAC rule is per-type, so U85 (single 256) and the
    # paired U55s all pass.
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


def test_non_object_npus_and_cores_entries_do_not_crash_the_gate(tmp_path, monkeypatch):
    """`npus[]`/`cores[]` entries are schema-typed objects, but the schema
    pass that would reject a non-object entry runs separately and is not
    guaranteed to have run first. A bare string in either list used to reach
    a bare `.get()` and raise `AttributeError` here, hiding the schema FAIL
    line that already explains the real problem. Filtered to dicts, this doc
    must return cleanly -- no real npu survives the filter, so there is
    nothing left to pair."""
    doc = {"cores": ["not-a-dict"], "npus": ["not-a-dict"]}
    assert _run(tmp_path, monkeypatch, doc) == 0  # must not raise


def test_no_shipped_metadata_or_schema_claims_an_hg_subsystem():
    """The Alif Ensemble E8 has exactly three subsystems -- RTSS-HE, RTSS-HP,
    and the A32 APSS.  "HG" is the Ethos-U85 NPU block's OWN instance name
    (`NPU_HG`, Alif's internal codename ZAPHOD -- see `ZAPHOD_CKOR` in
    `SHMEM_CLK_CTRL`), not a fourth subsystem: `NPU_HG_BASE 0x49042000` is
    byte-identical in both M55 cores' generated CMSIS headers, unlike the two
    U55s, which alias one local address (0x400E1000) under per-core names
    (NPU_HP_BASE / NPU_HE_BASE).  This pins the fix to `paired_core`'s schema
    description and to `_check_soc_npu_pairing`'s docstring, and stops any
    new metadata/schema text from reintroducing the invented subsystem."""
    hits = []
    for p in sorted(V.REPO.glob("metadata/**/*")):
        if not p.is_file():
            continue
        try:
            text = p.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for needle in ("HG subsystem", "HG-subsystem", "HG_subsystem"):
            if needle in text:
                hits.append(f"{p.relative_to(V.REPO)}: {needle!r}")
    assert hits == [], (
        "shipped metadata/schema text must not claim an 'HG subsystem' -- "
        "the E8 has exactly three subsystems (RTSS-HE, RTSS-HP, A32 APSS); "
        "NPU_HG is the U85 NPU block's own instance name, not a fourth "
        "subsystem: " + "; ".join(hits)
    )
