# tests/scripts/test_board_models_schema.py
"""board.yaml `models:` block schema validation (isolated to the models subschema)."""
import json
from pathlib import Path
import jsonschema
import pytest

_ROOT = Path(__file__).resolve().parents[2]
_SCHEMA = json.loads((_ROOT / "metadata/schemas/board.schema.json").read_text(encoding="utf-8"))
# Validate the `models` array against its own subschema, so the test is isolated
# from unrelated top-level board constraints (e.g. `cores`).
_MODELS_SCHEMA = _SCHEMA["properties"]["models"]


def test_valid_models_block_passes():
    jsonschema.validate([{"name": "person_detect", "source": "models/p.tflite"}], _MODELS_SCHEMA)


def test_model_entry_requires_name_and_source():
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate([{"name": "p"}], _MODELS_SCHEMA)   # missing source


def test_model_entry_rejects_backend_field():
    # silicon-determined: a customer must NOT pin a backend in board.yaml
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate([{"name": "p", "source": "m.tflite", "backend": "ethos_u"}], _MODELS_SCHEMA)


def test_models_compile_block_validates():
    jsonschema.validate([{
        "name": "person_detect", "source": "models/p.onnx",
        "compile": {
            "deepx_dxm1": {"config": "models/p.deepx.json", "calibration": "models/calib/"},
            "drpai": {"spec": "models/p.drpai.yaml"},
        },
    }], _MODELS_SCHEMA)


def test_models_compile_rejects_unknown_backend_key():
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate([{"name": "p", "source": "m.onnx",
                              "compile": {"vela": {"x": 1}}}], _MODELS_SCHEMA)


def test_models_compile_deepx_requires_config_and_calibration():
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.validate([{"name": "p", "source": "m.onnx",
                              "compile": {"deepx_dxm1": {"config": "c.json"}}}], _MODELS_SCHEMA)
