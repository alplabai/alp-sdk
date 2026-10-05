# SPDX-License-Identifier: Apache-2.0
"""The Linux reader and the C header must agree on the record layout."""

import importlib.util
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HDR = (ROOT / "include/alp/protocol/gd32_bridge_results.h").read_text()
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
    assert reader.BEACON_MAGIC == _define("ALP_GD32_RESULTS_BEACON_MAGIC")
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
    words[0], words[1], words[3], words[4] = reader.MAGIC, 1, 2, 2
    words[5], words[8], words[9], words[10], words[11] = 7, (0 << 16) | (15 << 8) | 1, 0x1B, 252, 0x18
    rec = reader.decode(struct.pack("<20I", *words), (reader.BEACON_MAGIC, 0x200, 42))
    assert rec["fw"] == "0.15.1" and rec["kind_name"] == "soak" and rec["tests_pass"] == 7
    assert rec["flag_names"] == ["batch_ok", "stream2_ok"] and rec["beacon_heartbeat"] == 42


def test_decode_rejects_stale_or_foreign():
    assert reader.decode(b"\0" * 80) is None
    bad = list(struct.unpack("<20I", struct.pack("<20I", reader.MAGIC, 2, *[0] * 18)))
    assert reader.decode(struct.pack("<20I", *bad)) is None
