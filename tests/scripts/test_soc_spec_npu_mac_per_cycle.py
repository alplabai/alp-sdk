# SPDX-License-Identifier: Apache-2.0
"""`$defs/npu`'s conditional `mac_per_cycle` requirement (#1849).

A consumer builds the vela `--accelerator-config` string as
`<type>-<mac_per_cycle>` (e.g. `ethos-u55-256`). Before this gate, a document
could be schema-valid and still omit the field, because `$defs/npu` required
only `type` plus `anyOf(gops, tops)`:

    { "type": "ethos-u55", "gops": 100 }

validated cleanly. Three in-tree consumers then disagree about what that
means, which is the reason the refusal belongs in the schema rather than in
any one of them:

  * `alp_model.targets._accel_config` raises `KeyError` -- deliberately, per
    its own docstring: the truncated `ethos-u55-` must never be emitted.
  * `alp_orchestrate.kconfig._emit_inference` filters the entry out and falls
    back to `mac = 256`, which on a 128-MAC part emits `CONFIG_ETHOS_U55_256=y`
    -- a legal `ETHOS_U_NPU_CONFIG` member, so no downstream guard catches it.
  * tan-cli skips the NPU (its `isinstance` guard, tan-cli#965).

Crash, silent mis-size, silent skip. None is wrong on its own; what is wrong
is that the document was ever accepted. This moves the refusal to authoring
time, where there is exactly one answer.

The requirement is CONDITIONAL on purpose: `drp-ai` and `deepx-dx-m1` are not
compiled through vela and legitimately carry no `mac_per_cycle`. Making the
field unconditionally required would refuse two committed documents.
`test_a_non_ethos_npu_without_mac_per_cycle_still_validates` is the mutation
that proves the condition is actually a condition -- without it, an over-tight
`"required": ["mac_per_cycle"]` would pass every other test in this file.

Run locally:

    python -m pytest tests/scripts/test_soc_spec_npu_mac_per_cycle.py -v
"""

from __future__ import annotations

import copy
import json
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCHEMA = REPO / "metadata" / "schemas" / "soc-spec-v1.schema.json"
SOCS = REPO / "metadata" / "socs"


def _validator():
    jsonschema = pytest.importorskip("jsonschema")
    return jsonschema.Draft202012Validator(
        json.loads(SCHEMA.read_text(encoding="utf-8")))


def _doc(rel: str) -> dict:
    return json.loads((SOCS / rel).read_text(encoding="utf-8"))


def test_every_committed_soc_doc_still_validates() -> None:
    """The tightening must cost zero metadata churn.

    All 16 `ethos-u*` entries across the tree already declare
    `mac_per_cycle`; this pins that, so the day one does not, it fails here
    rather than in whichever consumer reaches it first.
    """
    validator = _validator()
    failures = []
    for soc in sorted(SOCS.rglob("*.json")):
        errors = list(validator.iter_errors(
            json.loads(soc.read_text(encoding="utf-8"))))
        if errors:
            failures.append(
                f"{soc.relative_to(REPO)}: "
                + "; ".join(e.message for e in errors[:3]))
    assert failures == [], "\n".join(failures)


@pytest.mark.parametrize(
    ("rel", "index", "expected_type"),
    [
        ("alif/ensemble/e8.json", 0, "ethos-u85"),
        ("alif/ensemble/e8.json", 1, "ethos-u55"),
        ("nxp/imx9/imx93.json", 0, "ethos-u65"),
    ],
)
def test_an_ethos_u_npu_missing_mac_per_cycle_is_refused(
    rel: str, index: int, expected_type: str
) -> None:
    """The defect this gate exists to catch, on all three Ethos-U widths.

    Parametrised across u85/u55/u65 because the rule keys on the `^ethos-u`
    prefix, not on an enumerated list -- a regex accidentally narrowed to one
    width would still pass a single-case test.
    """
    validator = _validator()
    doc = _doc(rel)
    assert doc["npus"][index]["type"] == expected_type, "fixture drifted"
    assert validator.is_valid(doc)

    stripped = copy.deepcopy(doc)
    del stripped["npus"][index]["mac_per_cycle"]
    errors = list(validator.iter_errors(stripped))
    assert errors, f"{expected_type} without mac_per_cycle should be refused"
    assert any("mac_per_cycle" in e.message for e in errors), \
        [e.message for e in errors]


@pytest.mark.parametrize(
    ("rel", "expected_type"),
    [
        ("renesas/rzv2n/n44.json", "drp-ai"),
        ("deepx/dx/m1.json", "deepx-dx-m1"),
    ],
)
def test_a_non_ethos_npu_without_mac_per_cycle_still_validates(
    rel: str, expected_type: str
) -> None:
    """The mutation that proves the requirement is conditional.

    These two documents are committed and carry no `mac_per_cycle`. An
    unconditional `"required": ["mac_per_cycle"]` would refuse both, and every
    other test in this file would still pass -- which is exactly how an
    over-tight rule ships.
    """
    validator = _validator()
    doc = _doc(rel)
    npu = doc["npus"][0]
    assert npu["type"] == expected_type, "fixture drifted"
    assert "mac_per_cycle" not in npu, (
        f"{expected_type} gained a mac_per_cycle -- pick another non-Ethos "
        "fixture, this test is now vacuous")
    assert validator.is_valid(doc)


def test_an_npu_with_no_type_is_still_refused_for_the_right_reason() -> None:
    """The vacuous-match guard.

    `if: { properties: { type: ... } }` matches VACUOUSLY when `type` is
    absent -- `properties` says nothing about a key that is not there. The
    `required: ["type"]` inside the `if` is what stops an entry with no `type`
    from being dragged into the `then` branch and refused for `mac_per_cycle`
    when the real defect is the missing `type`.
    """
    validator = _validator()
    doc = copy.deepcopy(_doc("alif/ensemble/e8.json"))
    doc["npus"][1].pop("type")
    messages = [e.message for e in validator.iter_errors(doc)]
    assert any("'type' is a required property" in m for m in messages), messages
