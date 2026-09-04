# tests/scripts/test_alpmodel_fixture_self_consistency.py
"""Self-consistency guard for the three committed `.alpmodel` C-test fixtures.

ADR-0028 moved the `.alpmodel` WRITER (`tan.model.package` / `_gen_fixture`)
out of this repo into tan-cli. Before that move, a single generator produced
`tests/fixtures/alpmodel/minimal.alpmodel`,
`tests/unit/alpmodel_reader/src/fixture.h` and
`tests/yocto/onnx_cpu_fixture.h` together, so the three could never drift from
each other by construction. Nothing in *this* repo enforces that any more --
the only cross-repo check is tan-cli's `python/tests/model/test_package.py`,
which needs `ALP_SDK_ROOT` bound to a checkout of the very PR being tested to
be meaningful, and tan-cli CI's `parity.yml` binds it to a frozen
`PINNED_SDK_TAG` instead. A PR here that edits these fixtures (by hand, by a
partial regen, by a bad merge) can pass every alp-sdk gate while silently
breaking that cross-repo contract.

This test is the cheap half of the mitigation: it does NOT reimplement tan's
writer (that would be exactly the drift ADR-0028 exists to kill) -- it only
decodes the container using the wire layout `src/common/alp_model.c`'s
`alp_model_parse()` already implements (24-byte header, CBOR manifest, 8-byte
blob-table entries -- all alp-sdk-owned, since the on-device *reader* stayed
here per ADR-0028 Decision-3), and checks:

  1. `fixture.h`'s C byte array and `minimal.alpmodel`'s raw bytes are the
     SAME bytes -- these are two committed encodings of one logical object
     and must never disagree with each other, independent of what tan's
     generator currently emits.
  2. Both fixtures are structurally well-formed containers (`ALPM` magic,
     `container_v` matching `include/alp/model.h`'s `ALP_MODEL_CONTAINER_V`,
     a CBOR manifest that decodes and names the targets/blob_format each
     fixture's own C test depends on).

What this does NOT catch: two fixtures edited consistently with each other,
by hand or by a stale/buggy generator invocation, into bytes that no longer
match what tan's CANONICAL `tan.model.package.write_package()` /
`to_c_header()` would actually produce for the same manifest. That residual
gap needs a real cross-repo run (`ALP_SDK_ROOT` bound to THIS PR, not a
frozen tag) -- see changelog.d/1471.md for the current mitigation
(a documented `PINNED_SDK_TAG` bump as a tan-cli release step) until/unless
tan-cli's parity workflow binds `ALP_SDK_ROOT` to the PR under test instead.
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
        "regenerate with `python -m tan.model._gen_fixture --root <this checkout>` "
        "(a tan-cli checkout)"
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
        "regenerate both together with "
        "`python -m tan.model._gen_fixture --root <this checkout>` (a tan-cli checkout)"
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
