# tests/scripts/test_vela_yolo_internal.py
"""Real-model proof: compile a real int8 model with Vela for the E8 Ethos-U85.

The fixture lives in the PRIVATE alp-sdk-internal repo, not the public tree --
real production models carry licensing + repo-bloat concerns (mirrors
test_deepx_yolo_internal.py).  This test runs only when that sibling repo is
checked out AND `vela` (ethos-u-vela) is installed -- i.e. on a maintainer's
box, never in cloud CI.  It is the SoM-credibility proof that the alp-sdk
Ethos-U/Vela pipeline compiles a real production model for the AEN801 (E8)
accelerator configs, not just the tiny hermetic fixture in
test_alp_model_adapters.py.

Drop a real int8 .tflite (e.g. a quantized yolo / mobilenet detector) at:
    $ALP_SDK_INTERNAL/vendors/alif-ethos-u/sample-models/<name>_int8.tflite

Run (with alp-sdk-internal beside alp-sdk, vela on PATH):
    ALP_SDK_INTERNAL=../alp-sdk-internal \\
      python -m pytest tests/scripts/test_vela_yolo_internal.py
"""
import os
import shutil
from pathlib import Path

import pytest

from alp_model.adapters.ethos_u import VelaAdapter

_ROOT = Path(__file__).resolve().parents[2]


def _internal_root() -> Path:
    """alp-sdk-internal location: $ALP_SDK_INTERNAL, else the sibling default."""
    env = os.environ.get("ALP_SDK_INTERNAL")
    return Path(env) if env else _ROOT.parent / "alp-sdk-internal"


_MODELS_DIR = _internal_root() / "vendors/alif-ethos-u/sample-models"


def _real_int8_models() -> list[Path]:
    return sorted(_MODELS_DIR.glob("*_int8.tflite")) if _MODELS_DIR.is_dir() else []


@pytest.mark.skipif(shutil.which("vela") is None, reason="vela (ethos-u-vela) not installed")
@pytest.mark.skipif(not _real_int8_models(),
                    reason="alp-sdk-internal Ethos-U sample model absent (set ALP_SDK_INTERNAL)")
@pytest.mark.parametrize("accel_config", ["ethos-u85-256", "ethos-u55-256"])
def test_vela_compiles_real_model_for_e8(tmp_path, accel_config):
    """A real int8 .tflite -> a vela_tflite blob for the E8 accel configs.

    Proves op-support + a real arena/SRAM footprint on a production-scale model
    (NOT on-device correctness, which is silicon + Ethos-U HAL gated).
    """
    model = _real_int8_models()[0]
    src = tmp_path / model.name
    shutil.copy(model, src)
    blob = VelaAdapter().compile(src, accel_config=accel_config, out_dir=tmp_path)
    assert blob.format == "vela_tflite"
    assert blob.payload[4:8] == b"TFL3"
    # A real (non-toy) model yields a real reported footprint.
    assert blob.arena_bytes > 0 or blob.req_sram_kib > 0
