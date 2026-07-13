# SPDX-License-Identifier: Apache-2.0
import json, sys
from pathlib import Path
import jsonschema

REPO = Path(__file__).resolve().parents[2]
SCHEMA = REPO / "metadata/schemas/build-receipt-v1.schema.json"
sys.path.insert(0, str(REPO / "scripts"))
import build_receipt  # noqa: E402


def test_schema_closed_draft2020():
    s = json.loads(SCHEMA.read_text(encoding="utf-8"))
    assert s["$schema"].endswith("2020-12/schema")
    assert s["additionalProperties"] is False
    assert s["properties"]["schemaVersion"]["const"] == 1
    jsonschema.Draft202012Validator.check_schema(s)


def _fixture(tmp_path):
    bp = tmp_path / "build-plan.json"
    bp.write_text(json.dumps({"schemaVersion": 1, "sku": "E1M-AEN801",
                              "boardYaml": "board.yaml"}))
    img = tmp_path / "app.bin"; img.write_bytes(b"\x01\x02\x03")
    board = tmp_path / "board.yaml"; board.write_text("som:\n  sku: E1M-AEN801\n")
    return bp, img, board


def test_compose_validates_and_hashes(tmp_path):
    bp, img, board = _fixture(tmp_path)
    r = build_receipt.build_receipt(
        tmp_path, bp, [("m55_hp", img)], board,
        rev_resolver=lambda root: ("deadbeef", False))
    jsonschema.Draft202012Validator(
        json.loads(SCHEMA.read_text(encoding="utf-8"))).validate(r)
    assert r["images"][0]["sizeBytes"] == 3
    assert r["images"][0]["sha256"].startswith("sha256:")
    assert r["source"]["sdkRevision"] == "deadbeef"
    assert "timestamp" not in json.dumps(r).lower()


def test_deterministic(tmp_path):
    bp, img, board = _fixture(tmp_path)
    args = (tmp_path, bp, [("m55_hp", img)], board)
    rr = lambda root: ("deadbeef", False)
    a = build_receipt.build_receipt(*args, rev_resolver=rr)
    b = build_receipt.build_receipt(*args, rev_resolver=rr)
    assert build_receipt.digest_json(a) == build_receipt.digest_json(b)


def test_missing_image_raises(tmp_path):
    bp, _img, board = _fixture(tmp_path)
    import pytest
    with pytest.raises(build_receipt.MissingInputError):
        build_receipt.build_receipt(tmp_path, bp, [("m55_hp", tmp_path / "nope.bin")],
                                    board, rev_resolver=lambda r: ("x", False))


def test_dirty_flag(tmp_path):
    bp, img, board = _fixture(tmp_path)
    r = build_receipt.build_receipt(tmp_path, bp, [("m55_hp", img)], board,
                                    rev_resolver=lambda root: ("x", True))
    assert r["source"]["sdkDirty"] is True
