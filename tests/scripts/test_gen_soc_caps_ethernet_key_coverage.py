# SPDX-License-Identifier: Apache-2.0
"""#1240: ETHERNET_COUNT must consume every Ethernet-flavoured peripherals key.

`gen_soc_caps.py`'s ETHERNET_COUNT lambda used to read only the `ethernet`
key; `metadata/socs/renesas/rzv2n/n44.json` publishes `ethernet_1g` instead,
so `ALP_SOC_ETHERNET_COUNT` (and the `ALP_CAP_HW_ETHERNET` macro derived from
it) silently read 0 on the V2N/V2M family -- the family with the most
Ethernet on it.

This test probes the REAL lambda out of `gen_soc_caps.CAPS` rather than
comparing against a copy of the key list.  That distinction is the whole
point: a test carrying its own frozen key set stays green when someone edits
only the generator, which reproduces #1240 with a passing test sitting next
to it.  Feeding each key to the actual lambda means the guard fails in both
drift directions -- a new SoC key the generator ignores, and a generator that
stops consuming a key already in the tree.

If this test fails, the offending SoC JSON + key name are in the assertion
message: either fold the new key into the ETHERNET_COUNT lambda in
scripts/gen_soc_caps.py, or, if the key is not actually an Ethernet
peripheral count, rename it so it does not look like one.
"""
from __future__ import annotations

import json
from pathlib import Path

import gen_soc_caps as gsc  # scripts/ on sys.path via conftest

REPO = Path(__file__).resolve().parents[2]
SOCS_DIR = REPO / "metadata" / "socs"

ETHERNET_COUNT = dict(gsc.CAPS)["ETHERNET_COUNT"]


def test_ethernet_count_consumes_every_ethernet_key_in_the_tree():
    offenders = []
    for soc_json in sorted(SOCS_DIR.rglob("*.json")):
        doc = json.loads(soc_json.read_text(encoding="utf-8"))
        for key in doc.get("peripherals", {}):
            # Case-insensitive: metadata/schemas/soc-spec-v1.schema.json
            # constrains peripherals values to integers but puts no pattern
            # on the key, so `Ethernet_2_5G` validates and would break the
            # generator's exact-match get() while a case-sensitive probe
            # waved it through.
            if "ethernet" not in key.lower():
                continue
            # A key the lambda consumes returns the value it was given.
            if ETHERNET_COUNT({key: 1}) != 1:
                offenders.append(f"{soc_json.relative_to(REPO)}: {key!r}")

    assert not offenders, (
        "SoC JSON declares an Ethernet-flavoured peripherals key that "
        "gen_soc_caps.py's ETHERNET_COUNT lambda does not consume, so "
        "ALP_SOC_ETHERNET_COUNT / ALP_CAP_HW_ETHERNET will silently read 0 "
        "for it (the exact #1240 defect):\n  " + "\n  ".join(offenders)
    )
