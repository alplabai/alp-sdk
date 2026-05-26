"""Tests for alp_model.manifest round-trip serialisation."""
import json
import cbor2
from alp_model.manifest import Tensor, Target, Coverage, Manifest


def _sample() -> Manifest:
    return Manifest(
        name="person_detect",
        src_sha=bytes(range(32)),
        inputs=[Tensor(dtype="int8", rank=4, shape=[1, 224, 224, 3], scale=0.0078, zp=-1)],
        outputs=[Tensor(dtype="int8", rank=2, shape=[1, 1000], scale=0.004, zp=12)],
        targets=[
            Target(backend="ethos_u", silicon_ref="alif:ensemble:e8",
                   blob_format="vela_tflite", accel_config="ethos-u85-256",
                   arena=524288, requires={"sram_kib": 512, "op_features": ["transformer"]},
                   blob=0),
            Target(backend="cpu", silicon_ref="*", blob_format="tflite",
                   accel_config="", arena=786432, requires={"sram_kib": 0, "op_features": []},
                   blob=1),
        ],
        coverage=[Coverage(backend="deepx_dxm1", accel_config="",
                           status="skipped", reason="dx-compiler not found")],
    )


def test_manifest_roundtrips_through_dict():
    m = _sample()
    assert Manifest.from_dict(m.to_dict()) == m
    assert m.to_dict()["targets"][0]["backend"] == "ethos_u"


def test_manifest_json_is_human_readable_and_roundtrips():
    m = _sample()
    text = m.to_json()
    doc = json.loads(text)                       # valid JSON
    assert doc["name"] == "person_detect"
    assert doc["src_sha"] == "00010203" + "0405060708090a0b0c0d0e0f" + \
        "101112131415161718191a1b1c1d1e1f"       # hex-encoded bytes
    assert Manifest.from_json(text) == m


def test_manifest_cbor_roundtrips():
    m = _sample()
    blob = m.to_cbor()
    assert isinstance(blob, (bytes, bytearray))
    assert Manifest.from_cbor(blob) == m


def test_cbor_decode_tolerates_unknown_keys():
    # extensibility: a future writer adds a key the current reader ignores
    m = _sample()
    doc = cbor2.loads(m.to_cbor())
    doc["future_field"] = 123
    doc["targets"][0]["future_target_field"] = "x"
    extended = cbor2.dumps(doc)
    assert Manifest.from_cbor(extended) == m      # unknown keys dropped, rest intact
