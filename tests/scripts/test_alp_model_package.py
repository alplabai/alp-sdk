from alp_model.manifest import Manifest, Target
from alp_model.package import write_package, read_package, MAGIC


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
