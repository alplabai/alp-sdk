# SPDX-License-Identifier: Apache-2.0
"""The Linux reader and the C header must agree on the record layout."""

import importlib.util
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HDR = (ROOT / "include/alp/protocol/gd32_bridge_results.h").read_text(encoding="utf-8")
_spec = importlib.util.spec_from_file_location(
    "reader", ROOT / "scripts/bench/v2n/read_gd32_results.py")
reader = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(reader)


def _define(name):
    m = re.search(r"#define %s\s+(?:\(1u << (\d+)\)|(0x[0-9A-Fa-f]+|\d+)u)" % name, HDR)
    return 1 << int(m.group(1)) if m.group(1) else int(m.group(2), 0)


def test_constants_match_header():
    assert reader.MAGIC == _define("ALP_GD32_RESULTS_MAGIC")
    assert reader.RESULTS_OFFSET == _define("ALP_GD32_RESULTS_OFFSET")
    assert reader.BEACON_OFFSET == _define("ALP_GD32_RESULTS_BEACON_OFFSET")
    amp = (Path(__file__).resolve().parents[2] / "include/alp/protocol/amp_beacon.h").read_text(encoding="utf-8")
    assert reader.BEACON_MAGIC == int(re.search(r"#define ALP_AMP_BEACON_MAGIC\s+(0x[0-9A-Fa-f]+)u", amp).group(1), 16)
    assert reader.BEACON_KIND == _define("ALP_GD32_RESULTS_BEACON_KIND")
    assert reader.LAYOUT == _define("ALP_GD32_RESULTS_LAYOUT")
    assert reader.WORDS == _define("ALP_GD32_RESULTS_WORDS")
    assert reader.LINK_PENDING == _define("ALP_GD32_RESULTS_STATE_LINK_PENDING")
    assert reader.RESULTS_OFFSET + reader.WORDS * 4 <= reader.BEACON_OFFSET


def test_field_order_matches_struct():
    body = re.search(r"typedef struct \{(.*?)\} alp_gd32_results_t;", HDR, re.S).group(1)
    names = re.findall(r"uint32_t\s+(\w+);", body)
    assert tuple(names) == reader.FIELDS


def test_flag_bits_match_header():
    for bit, name in reader.FLAGS.items():
        assert _define("ALP_GD32_RESULTS_FLAG_" + name.upper()) == bit


def test_decode_round_trip():
    words = [0] * reader.WORDS
    words[0], words[1], words[3], words[4] = reader.MAGIC, reader.LAYOUT, 2, 2
    words[5], words[8], words[9], words[10], words[11] = 7, (0 << 16) | (15 << 8) | 1, 0x1B, 252, 0x18
    rec = reader.decode(struct.pack("<20I", *words), (reader.BEACON_MAGIC, 0x200, 42))
    assert rec["fw"] == "0.15.1" and rec["kind_name"] == "soak" and rec["tests_pass"] == 7
    assert rec["flag_names"] == ["batch_ok", "stream2_ok"] and rec["beacon_heartbeat"] == 42


def test_decode_rejects_stale_or_foreign():
    assert reader.decode(b"\0" * 80) is None
    bad = list(struct.unpack("<20I", struct.pack("<20I", reader.MAGIC, 1, *[0] * 18)))
    assert reader.decode(struct.pack("<20I", *bad)) is None


def test_record_left_by_an_earlier_image_is_stale():
    """Bench, E1M-V2M103 0008: after the idle shim (beacon kind 0x100) replaced the
    soak image, the old soak record was still in place and used to be reported."""
    words = [0] * reader.WORDS
    words[0], words[1], words[3], words[4] = reader.MAGIC, reader.LAYOUT, 2, 2
    raw = struct.pack("<20I", *words)
    assert reader.is_live(reader.decode(raw, (reader.BEACON_MAGIC, 0x200, 5)))
    assert not reader.is_live(reader.decode(raw, (reader.BEACON_MAGIC, 0x100, 5)))
    assert not reader.is_live(reader.decode(raw, (0, 0x200, 5)))
    assert not reader.is_live(reader.decode(raw))


def test_fault_block_matches_header():
    assert reader.FAULT_OFFSET == _define("ALP_GD32_FAULT_OFFSET")
    assert reader.FAULT_MAGIC == _define("ALP_GD32_FAULT_MAGIC")
    assert reader.FAULT_WORDS == _define("ALP_GD32_FAULT_WORDS")
    assert reader.RESULTS_OFFSET + reader.WORDS * 4 <= reader.FAULT_OFFSET
    assert reader.FAULT_OFFSET + reader.FAULT_WORDS * 4 <= reader.BEACON_OFFSET
    body = re.search(r"typedef struct \{([^{}]*)\} alp_gd32_fault_t;", HDR).group(1)
    assert tuple(re.findall(r"uint32_t\s+(\w+);", body)) == reader.FAULT_FIELDS


def test_decode_fault():
    words = [0] * reader.FAULT_WORDS
    words[0], words[1], words[2], words[4] = reader.FAULT_MAGIC, 4, 0x1234, 0x01000003
    words[9] = reader.fnv1a("main")
    f = reader.decode_fault(struct.pack("<12I", *words))
    assert f["thread"] == "main" and f["pc"] == 0x1234 and f["exception"] == 3
    assert reader.decode_fault(b"\0" * 48) is None


def test_fnv1a_known_vector():
    assert reader.fnv1a("") == 0x811C9DC5 and reader.fnv1a("a") == 0xE40C292C


def test_frozen_heartbeat_is_stalled():
    assert reader.heartbeat_stalled(7, 7)
    assert not reader.heartbeat_stalled(7, 8)
    assert not reader.heartbeat_stalled(0xFFFFFFFF, 0)


def test_fail_mask_decodes_to_rows():
    words = [0] * reader.WORDS
    words[0], words[1], words[3], words[19] = reader.MAGIC, reader.LAYOUT, 2, (1 << 0) | (1 << 21)
    rec = reader.decode(struct.pack("<20I", *words))
    assert rec["fail_rows"] == [0, 21]
    assert rec["fail_row_names"] == [
        reader.ROWS["soak"][0], reader.ROWS["soak"][21]]


def test_row_names_match_c_tables():
    ex = ROOT / "examples/v2n"
    soak = (ex / "v2n-gd32-bridge-hil-soak/src/main.c").read_text(encoding="utf-8")
    table = soak.split("} tests[] = {", 1)[1].split("};", 1)[0]
    assert tuple(re.findall(r'\{ \{ "(\w+)"', table)) == reader.ROWS["soak"]
    func = (ex / "v2n-gd32-bridge-functional/src/main.c").read_text(encoding="utf-8")
    rows = re.findall(r"^ \*\s+row (\d+) (\w+)$", func, re.M)
    assert [int(i) for i, _ in rows] == list(range(len(rows)))
    assert tuple(n for _, n in rows) == reader.ROWS["functional"]
