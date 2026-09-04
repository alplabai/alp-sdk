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
    nothing left to pair.

    `npus` also carries ONE real dict entry (paired to the one real `cores`
    dict entry) so the function does not short-circuit at `if not npus:
    continue` before ever reaching the `cores[]` filter this test exists to
    exercise -- a prior version of this fixture (`npus: ["not-a-dict"]`
    only) never actually reached that line, so reverting its guard alone
    would not have reddened this test."""
    doc = {
        "cores": ["not-a-dict", {"id": "m55_hp"}],
        "npus": ["not-a-dict", {"type": "ethos-u55", "subtype": "high-perf",
                                 "mac_per_cycle": 256, "paired_core": "m55_hp"}],
    }
    assert _run(tmp_path, monkeypatch, doc) == 0  # must not raise


def test_non_object_top_level_does_not_crash_the_gate(tmp_path, monkeypatch):
    """The SoC doc's top level is schema-typed as an object, but a
    malformed file could parse to a bare JSON array -- `doc.get("npus")`
    used to raise `AttributeError: 'list' object has no attribute 'get'`
    here, aborting the whole gate mid-run instead of leaving the schema
    FAIL line (which already flags the type mismatch) to explain the real
    problem."""
    monkeypatch.setattr(V, "REPO", tmp_path)
    p = tmp_path / "soc.json"
    p.write_text(json.dumps([]))
    assert V._check_soc_npu_pairing([p]) == []  # must not raise


def test_non_list_npus_and_cores_do_not_crash_the_gate(tmp_path, monkeypatch):
    """`npus`/`cores` are themselves schema-typed as arrays, but a malformed
    document can carry a non-list scalar there (e.g. the bare int `5`,
    which is truthy) -- iterating the unfiltered value used to raise
    `TypeError: 'int' object is not iterable`, aborting the whole gate
    mid-run instead of leaving the schema FAIL line (which already flags
    the type mismatch) to explain the real problem."""
    doc = {"cores": 5, "npus": 5}
    assert _run(tmp_path, monkeypatch, doc) == 0  # must not raise


def test_non_string_core_id_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`cores[].id` is schema-typed as a string, but a malformed doc can
    carry a dict/list there -- the unfiltered `core_ids` set comprehension
    used to raise `TypeError: unhashable type: 'dict'` building the set.
    A real dict-typed `id` alongside a real string `id` (paired to the one
    real npu) proves the filter, not just an early-continue."""
    doc = {
        "cores": [{"id": {"nested": "dict"}}, {"id": "m55_he"}],
        "npus": [{"type": "ethos-u55", "subtype": "high-efficiency",
                   "mac_per_cycle": 128, "paired_core": "m55_he"}],
    }
    assert _run(tmp_path, monkeypatch, doc) == 0  # must not raise


def test_non_string_paired_core_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`npus[].paired_core` is schema-typed as a string, but a malformed
    doc can carry a dict/list there -- the unfiltered `pc not in core_ids`
    membership test used to raise `TypeError: unhashable type: 'dict'`.
    A non-string `paired_core` is reported as a mismatch, not skipped."""
    doc = {
        "cores": [{"id": "m55_he"}],
        "npus": [{"type": "ethos-u55", "subtype": "high-efficiency",
                   "mac_per_cycle": 128, "paired_core": {"nested": "dict"}}],
    }
    assert _run(tmp_path, monkeypatch, doc) == 1  # must not raise; reported as a mismatch


def test_non_int_mac_per_cycle_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`npus[].mac_per_cycle` is schema-typed as an integer, but a malformed
    doc can carry a dict/list there -- the unfiltered `macs` set
    comprehension used to raise `TypeError: unhashable type: 'dict'`
    building the set (and a mixed str/int set would separately raise on
    the `sorted(macs)` message below it)."""
    doc = {
        "cores": [{"id": "m55_hp"}, {"id": "m55_he"}],
        "npus": [
            {"type": "ethos-u55", "subtype": "high-perf",
             "mac_per_cycle": {"nested": "dict"}, "paired_core": "m55_hp"},
            {"type": "ethos-u55", "subtype": "high-efficiency",
             "mac_per_cycle": 128, "paired_core": "m55_he"},
        ],
    }
    assert _run(tmp_path, monkeypatch, doc) == 0  # must not raise


def test_no_shipped_metadata_or_schema_claims_an_hg_subsystem():
    """The Alif Ensemble E8 has exactly three subsystems -- RTSS-HE, RTSS-HP,
    and the A32 APSS.  "HG" is the Ethos-U85 NPU block's OWN instance name
    (`NPU_HG`, Alif's internal codename ZAPHOD -- see `ZAPHOD_CKOR` in
    `SHMEM_CLK_CTRL`), not a fourth subsystem: `NPU_HG_BASE 0x49042000` is
    byte-identical in both M55 cores' generated CMSIS headers, unlike the two
    U55s, which alias one local address (0x400E1000) under per-core names
    (NPU_HP_BASE / NPU_HE_BASE).  This pins the fix to THREE code surfaces:
    every shipped document under `metadata/**` (schema descriptions,
    including `paired_core`'s, and every SoC spec), every Python source
    under `scripts/**` (which is where `_check_soc_npu_pairing`'s own
    docstring lives, in `scripts/validate_metadata.py` -- a walk scoped to
    `metadata/**` alone cannot see that file), and every Zephyr devicetree
    source under `zephyr/dts/**` -- `zephyr/dts/alif/ensemble_e8_peripherals.dtsi`
    carries hand-written prose comments about this exact block (the
    `ethosu85@49042000` node) and is exactly the kind of shipped text this
    invented-subsystem claim could resurface in, yet neither of the other
    two globs would ever see it. `tests/scripts/` is deliberately NOT
    walked: it is where this very assertion message quotes the phrase it
    forbids, so scanning it would make the gate fail on itself. Stops any
    of the three surfaces from reintroducing the invented subsystem."""
    hits = []
    needles = ("HG subsystem", "HG-subsystem", "HG_subsystem")
    for pattern in ("metadata/**/*", "scripts/**/*.py", "zephyr/dts/**/*"):
        for p in sorted(V.REPO.glob(pattern)):
            if not p.is_file():
                continue
            try:
                text = p.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for needle in needles:
                if needle in text:
                    hits.append(f"{p.relative_to(V.REPO)}: {needle!r}")
    assert hits == [], (
        "shipped metadata/schema/zephyr-dts text or scripts/ source must not "
        "claim an 'HG subsystem' -- the E8 has exactly three subsystems "
        "(RTSS-HE, RTSS-HP, A32 APSS); NPU_HG is the U85 NPU block's own "
        "instance name, not a fourth subsystem: " + "; ".join(hits)
    )


def test_no_shipped_metadata_or_schema_claims_a32_npu_reach_is_unverified():
    """Whether the A32 cluster can reach the E8's Ethos-U85 (NPU_HG,
    0x49042000) used to be hedged as unverified in both `paired_core`'s
    schema description and `_check_soc_npu_pairing`'s own docstring -- "there
    is no A32 SVD view or board devicetree NPU node to confirm either way".
    It is now class-1 vendor-sourced: the Alif E8 Hardware Reference Manual
    (AHRM0012NDA v0.3) Table 10-2 places NPU_HG's 0x49042000 inside the
    Shared Peripherals region (A32/M55-HP/M55-HE all Y, unlike the
    M55-local-peripherals row above it), Table 10-6 lists NPU-HG there by
    address, and Table 4-13 fans its interrupt to all three cores
    (GIC400_IRQS[355] on the A32). Same three-surface walk as the
    HG-subsystem pin above (`metadata/**`, `scripts/**/*.py`,
    `zephyr/dts/**`), and for the same reasons: `metadata/**` alone cannot
    see `_check_soc_npu_pairing`'s docstring in
    `scripts/validate_metadata.py`, and neither of those two globs can see
    `zephyr/dts/alif/ensemble_e8_peripherals.dtsi`'s own prose comments
    about this exact NPU_HG node -- the retired hedge phrase names "a board
    devicetree NPU node" directly, making that dtsi's comment block a
    plausible place for the hedge to resurface. `tests/scripts/` is
    deliberately NOT walked -- this docstring itself quotes the forbidden
    phrase. Stops any of the three surfaces from reintroducing the
    now-superseded hedge."""
    hits = []
    needle = "unverified -- there is no A32 SVD view or board devicetree NPU node"
    for pattern in ("metadata/**/*", "scripts/**/*.py", "zephyr/dts/**/*"):
        for p in sorted(V.REPO.glob(pattern)):
            if not p.is_file():
                continue
            try:
                text = p.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            if needle in text:
                hits.append(f"{p.relative_to(V.REPO)}")
    assert hits == [], (
        "shipped metadata/schema/zephyr-dts text or scripts/ source must "
        "not reassert A32-vs-U85 reachability as unverified -- the Alif E8 "
        "HWRM (AHRM0012NDA v0.3) Tables 10-2/10-6/4-13 settle it: " + "; ".join(hits)
    )
