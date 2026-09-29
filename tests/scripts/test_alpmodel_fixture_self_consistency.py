# tests/scripts/test_alpmodel_fixture_self_consistency.py
"""Self-consistency guard for the three committed `.alpmodel` C-test fixtures.

The `.alpmodel` WRITER (`scripts/alp_model/package.py` / `_gen_fixture.py`)
is still IN this repo today -- ADR-0028 (`docs/adr/0028-tan-owns-the-model-
engine.md`, Status: Proposed) proposes moving it to tan-cli's `tan.model`,
but that migration has not been enacted. One generator produces
`tests/fixtures/alpmodel/minimal.alpmodel`,
`tests/unit/alpmodel_reader/src/fixture.h` and
`tests/yocto/onnx_cpu_fixture.h` together (`python -m alp_model._gen_fixture`),
so the three should never drift from each other by construction -- but
nothing runs that generator as part of this repo's own gates, so a fixture
edited by hand, by a partial regen, or by a bad merge can still pass every
other alp-sdk gate while silently disagreeing with itself.

This test is that missing check. It does NOT reimplement the generator --
it only decodes the container using the wire layout `src/common/alp_model.c`'s
`alp_model_parse()` already implements (24-byte header, CBOR manifest, 8-byte
blob-table entries), and checks:

  1. `fixture.h`'s C byte array and `minimal.alpmodel`'s raw bytes are the
     SAME bytes -- these are two committed encodings of one logical object
     and must never disagree with each other, independent of what the
     generator currently emits.
  2. Both fixtures are structurally well-formed containers (`ALPM` magic,
     `container_v` matching `include/alp/model.h`'s `ALP_MODEL_CONTAINER_V`,
     a CBOR manifest that decodes and names the targets/blob_format each
     fixture's own C test depends on).

What this does NOT catch: two fixtures edited consistently with each other,
by hand or by a stale/buggy generator invocation, into bytes that no longer
match what `scripts/alp_model/package.py`'s `write_package()` / `to_c_header()`
would actually produce for the same manifest. Running the real generator and
diffing its output is the only thing that closes that residual gap.
"""
from __future__ import annotations

import re
import struct
from pathlib import Path

import cbor2

_ROOT = Path(__file__).resolve().parents[2]
_MODEL_H = _ROOT / "include/alp/model.h"
_MINIMAL_BIN = _ROOT / "tests/fixtures/alpmodel/minimal.alpmodel"
_READER_HDR = _ROOT / "tests/unit/alpmodel_reader/src/fixture.h"
_ONNX_HDR = _ROOT / "tests/yocto/onnx_cpu_fixture.h"

_HDR_STRUCT = struct.Struct("<4sHHIIII")  # magic, container_v, flags, mft_off, mft_len, tbl_off, blob_count


def _container_version() -> int:
    """ALP_MODEL_CONTAINER_V, read from the header that actually defines it --
    never hardcoded, so a real container-version bump makes this test notice
    instead of silently comparing against a stale expectation."""
    m = re.search(r"#define\s+ALP_MODEL_CONTAINER_V\s+(\d+)u", _MODEL_H.read_text(encoding="utf-8"))
    assert m, "ALP_MODEL_CONTAINER_V not found in include/alp/model.h"
    return int(m.group(1))


def _parse_c_byte_array(path: Path, array_name: str) -> bytes:
    """Extract a `static const uint8_t <array_name>[] = { 0x.., ... };`
    literal's bytes -- these headers are GENERATED (clang-format off, one
    hex byte at a time), so a plain hex-token regex is exact, not a guess."""
    text = path.read_text(encoding="utf-8")
    m = re.search(re.escape(array_name) + r"\[\]\s*=\s*\{(.*?)\};", text, re.S)
    assert m, f"{array_name}[] not found in {path}"
    return bytes(int(tok, 16) for tok in re.findall(r"0x[0-9a-fA-F]{2}", m.group(1)))


def _decode_container(data: bytes) -> dict:
    """Structural decode mirroring alp_model_parse()'s own bounds checks --
    proves the bytes are a container this repo's real reader can parse, and
    returns the CBOR manifest for content assertions."""
    assert len(data) >= _HDR_STRUCT.size, "container shorter than the 24-byte header"
    magic, container_v, _flags, mft_off, mft_len, tbl_off, blob_count = _HDR_STRUCT.unpack_from(data, 0)
    assert magic == b"ALPM", f"bad magic {magic!r}"
    want_v = _container_version()
    assert container_v == want_v, (
        f"container_v {container_v} != ALP_MODEL_CONTAINER_V {want_v} -- "
        "regenerate with `python -m alp_model._gen_fixture`"
    )
    assert mft_off <= len(data) and mft_len <= len(data) - mft_off, "manifest offset/length out of bounds"
    manifest = cbor2.loads(data[mft_off:mft_off + mft_len])
    assert tbl_off <= len(data) and blob_count <= (len(data) - tbl_off) // 8, "blob table out of bounds"
    for i in range(blob_count):
        boff, blen = struct.unpack_from("<II", data, tbl_off + i * 8)
        assert boff <= len(data) and blen <= len(data) - boff, f"blob[{i}] out of bounds"
    return manifest


def test_fixture_h_matches_committed_binary():
    """The C unit-test header and the raw .alpmodel binary must be byte-for-
    byte the same object -- nothing else in this repo cross-checks them."""
    header_bytes = _parse_c_byte_array(_READER_HDR, "alp_model_fixture")
    binary_bytes = _MINIMAL_BIN.read_bytes()
    assert header_bytes == binary_bytes, (
        "tests/unit/alpmodel_reader/src/fixture.h and "
        "tests/fixtures/alpmodel/minimal.alpmodel have drifted apart -- "
        "regenerate both together with `python -m alp_model._gen_fixture`"
    )


def test_minimal_alpmodel_is_well_formed():
    manifest = _decode_container(_MINIMAL_BIN.read_bytes())
    assert manifest["name"] == "minimal"
    assert len(manifest["src_sha"]) == 32
    assert manifest["targets"], "minimal.alpmodel must declare at least one target"


def test_onnx_cpu_fixture_h_is_well_formed():
    """tests/yocto/onnx_cpu_fixture.h (issue #1254's regression fixture) has
    no separate committed .alpmodel binary to diff against, so this checks it
    structurally instead: a well-formed container whose target list actually
    carries the "onnx"/"cpu" pairing tests/yocto/alpmodel_onnx_cpu.c relies on
    to reach the ONNX Runtime CPU backend."""
    header_bytes = _parse_c_byte_array(_ONNX_HDR, "k_onnx_cpu_alpmodel")
    manifest = _decode_container(header_bytes)
    targets = manifest["targets"]
    assert any(t["backend"] == "cpu" and t["blob_format"] == "onnx" for t in targets), (
        "tests/yocto/onnx_cpu_fixture.h no longer declares a cpu/onnx target"
    )
