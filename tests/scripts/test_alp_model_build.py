# tests/scripts/test_alp_model_build.py
"""build_model: resolve targets -> run available adapters -> write .alpmodel."""
from pathlib import Path

import pytest

from alp_model.adapters import CompilerAdapter
from alp_model.adapters.cpu import CpuAdapter
from alp_model.build import build_model
from alp_model.package import read_package

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"


def test_build_model_writes_alpmodel_with_cpu_blob_and_coverage(tmp_path):
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-DUMMY")
    # Inject only the CPU adapter so the result is independent of which compiler
    # toolchains happen to be installed on the build host.
    out = build_model(sku="E1M-AEN701", name="demo", source=src, out_dir=tmp_path,
                      metadata_root=_META, adapters=[CpuAdapter()])
    assert out == tmp_path / "demo.alpmodel"
    mft, blobs = read_package(out.read_bytes())
    assert mft.name == "demo"
    cpu = [t for t in mft.targets if t.backend == "cpu"]
    assert len(cpu) == 1
    assert blobs[cpu[0].blob] == b"TFL3-DUMMY"
    # Ethos-U has no injected adapter -> recorded as coverage skips (both variants).
    ethos_u_skips = [c for c in mft.coverage if c.backend == "ethos_u" and c.status == "skipped"]
    assert len(ethos_u_skips) == 2
    assert {c.accel_config for c in ethos_u_skips} == {"ethos-u55-256", "ethos-u55-128"}


def test_build_model_errors_when_no_blob_compiled(tmp_path):
    # Unsupported source format: CpuAdapter rejects .pt, no other adapter -> no blob.
    src = tmp_path / "m.pt"
    src.write_bytes(b"PYTORCH")
    with pytest.raises(ValueError, match="no blob compiled"):
        build_model(sku="E1M-AEN701", name="demo", source=src, out_dir=tmp_path,
                    metadata_root=_META, adapters=[CpuAdapter()])


def test_build_model_records_unavailable_tool_as_skip(tmp_path):
    # An adapter exists for ethos_u but its tool is "not installed" -> coverage skip,
    # and its compile() must never be called.
    class _Unavail(CompilerAdapter):
        backend = "ethos_u"

        def is_available(self):
            return False

        def accepts(self, src_format):
            return src_format == "tflite"

        def compile(self, source, *, accel_config, out_dir):
            raise AssertionError("compile() must not run for an unavailable adapter")

    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-DUMMY")
    out = build_model(sku="E1M-AEN701", name="demo", source=src, out_dir=tmp_path,
                      metadata_root=_META, adapters=[CpuAdapter(), _Unavail()])
    mft, _ = read_package(out.read_bytes())
    ethos_u_skips = [c for c in mft.coverage
                     if c.backend == "ethos_u" and c.status == "skipped"]
    assert len(ethos_u_skips) == 2                      # both u55 accel-config variants
    assert all("not installed" in c.reason for c in ethos_u_skips)


def test_build_model_v2m101_records_drpai_and_deepx_skips(tmp_path, monkeypatch):
    # With the default registry, V2M101 has drpai (host) + deepx_dxm1 (on-module) targets;
    # both proprietary tools are absent -> coverage skips; cpu still compiles.
    monkeypatch.delenv("ALP_DRPAI_TVM_HOME", raising=False)
    monkeypatch.delenv("ALP_DEEPX_SDK_HOME", raising=False)
    monkeypatch.setattr("alp_model.adapters.deepx.shutil.which", lambda n: None)
    src = tmp_path / "m.tflite"
    src.write_bytes(b"TFL3-DUMMY")
    out = build_model(sku="E1M-V2M101", name="demo", source=src,
                      out_dir=tmp_path, metadata_root=_META)   # default registry
    mft, _ = read_package(out.read_bytes())
    skipped = {c.backend for c in mft.coverage if c.status == "skipped"}
    assert "drpai" in skipped
    assert "deepx_dxm1" in skipped              # resolver folded it in (Task 1)
    assert any(t.backend == "cpu" for t in mft.targets)
