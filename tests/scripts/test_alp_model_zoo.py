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


def test_load_and_filter_by_sku():
    from alp_model.zoo import load_zoo, filter_by_sku
    entries = load_zoo(_META)
    assert any(e.id == "example-tiny" for e in entries)
    hit = filter_by_sku(entries, "E1M-AEN801")
    assert any(e.id == "example-tiny" for e in hit)
    miss = filter_by_sku(entries, "E1M-NOPE")
    assert all(e.id != "example-tiny" for e in miss)


def test_fetch_bundled_copies_into_dest(tmp_path):
    from alp_model.zoo import load_zoo, fetch_source
    entry = next(e for e in load_zoo(_META) if e.id == "example-tiny")
    out = fetch_source(entry, tmp_path, metadata_root=_META)
    assert out.is_file() and out.parent == tmp_path
    assert out.read_bytes() == (_META / "model_zoo" / "starters" / "example-tiny.tflite").read_bytes()


def test_fetch_url_verifies_sha(tmp_path):
    import hashlib
    from alp_model.zoo import ZooEntry, ZooError, fetch_source
    src = tmp_path / "src.tflite"
    src.write_bytes(b"hello-model")
    good = hashlib.sha256(b"hello-model").hexdigest()
    url = src.resolve().as_uri()  # file:// URL — hermetic
    ok_entry = ZooEntry(id="u", task="t", description="d", license="MIT",
                        source={"url": url, "sha256": good},
                        validated_soms=["E1M-AEN801"], compile=None, raw={})
    out = fetch_source(ok_entry, tmp_path / "dest", metadata_root=_META)
    assert out.read_bytes() == b"hello-model"
    bad_entry = ZooEntry(id="u", task="t", description="d", license="MIT",
                         source={"url": url, "sha256": "0" * 64},
                         validated_soms=["E1M-AEN801"], compile=None, raw={})
    with pytest.raises(ZooError):
        fetch_source(bad_entry, tmp_path / "dest2", metadata_root=_META)
