import pytest
from pathlib import Path
from alp_model.manifest import Manifest, Target
from alp_model.package import write_package, read_package, MAGIC
from alp_model._gen_fixture import build_fixture_bytes, to_c_header


def _mft() -> Manifest:
    return Manifest(
        name="m", src_sha=bytes(32),
        targets=[Target("ethos_u", "alif:ensemble:e8", "vela_tflite", "ethos-u85-256",
                        524288, {"sram_kib": 512, "op_features": []}, 0),
                 Target("cpu", "*", "tflite", "", 786432, {"sram_kib": 0, "op_features": []}, 1)],
    )


def test_package_roundtrips_manifest_and_blobs():
    blobs = [b"VELA-BLOB-BYTES", b"TFLITE-BLOB"]
    raw = write_package(_mft(), blobs)
    assert raw[:4] == MAGIC
    mft, got_blobs = read_package(raw)
    assert mft == _mft()
    assert got_blobs == blobs                    # retrieved by table order == blob index


_ROOT = Path(__file__).resolve().parents[2]


def test_committed_fixture_matches_generator():
    raw = build_fixture_bytes()
    on_disk = (_ROOT / "tests/fixtures/alpmodel/minimal.alpmodel").read_bytes()
    assert raw == on_disk, "regenerate: python -m alp_model._gen_fixture"
    header = (_ROOT / "tests/unit/alpmodel_reader/src/fixture.h").read_text()
    assert to_c_header(raw) == header, "regenerate: python -m alp_model._gen_fixture"


def test_bad_magic_rejected():
    raw = bytearray(write_package(_mft(), [b"x", b"y"]))
    raw[0] = ord("Z")
    with pytest.raises(ValueError, match="bad magic"):
        read_package(bytes(raw))


def test_unsupported_version_rejected():
    raw = bytearray(write_package(_mft(), [b"x", b"y"]))
    raw[4] = 99                                   # container_v low byte
    with pytest.raises(ValueError, match="unsupported container version"):
        read_package(bytes(raw))
