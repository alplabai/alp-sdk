"""alp_model.zoo — model-zoo machinery."""
import sys
from pathlib import Path

import pytest
import yaml

_ROOT = Path(__file__).resolve().parents[2]
_META = _ROOT / "metadata"
sys.path.insert(0, str(_ROOT / "scripts"))


def test_example_entry_shape():
    entry = yaml.safe_load((_META / "model_zoo" / "example-tiny.yaml").read_text("utf-8"))
    assert entry["id"] == "example-tiny"
    assert entry["task"] and entry["description"] and entry["license"]
    assert "bundled" in entry["source"]
    assert isinstance(entry["validated_soms"], list) and entry["validated_soms"]
    # the bundled starter file must exist, relative to metadata/model_zoo/
    starter = _META / "model_zoo" / entry["source"]["bundled"]
    assert starter.is_file(), starter
