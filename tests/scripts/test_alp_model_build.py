# tests/scripts/test_alp_model_build.py
"""build_model: resolve targets -> run available adapters -> write .alpmodel."""
from pathlib import Path
from alp_model.build import build_model
from alp_model.package import read_package

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"


def test_build_model_writes_alpmodel_with_cpu_blob_and_coverage(tmp_path):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-DUMMY")
    out = build_model(sku="E1M-AEN701", name="demo", source=src,
                      out_dir=tmp_path, metadata_root=_META)
    assert out == tmp_path / "demo.alpmodel"
    mft, blobs = read_package(out.read_bytes())
    assert mft.name == "demo"
    cpu = [t for t in mft.targets if t.backend == "cpu"]
    assert len(cpu) == 1
    assert blobs[cpu[0].blob] == b"TFL3-DUMMY"
    # Ethos-U targets recorded as coverage skips (no vela adapter in 1b-i).
    ethos_u_skips = [c for c in mft.coverage if c.backend == "ethos_u" and c.status == "skipped"]
    assert len(ethos_u_skips) == 2
    assert {c.accel_config for c in ethos_u_skips} == {"ethos-u55-256", "ethos-u55-128"}
