# SPDX-License-Identifier: Apache-2.0
"""Deterministic generator for synthetic_capture.jsonl (run: python3 gen_synthetic.py).

Known truth, 500 us period, 2 rails:
  idle window 1.0 s, marker low
  then 20 pulses: marker high 10 ms, low 40 ms
  rail1 ina236@0x4A 20 mOhm, fine: raw 100 idle / 300 active -> 100 mW / 300 mW (1 mW LSB)
        energy per inference 2.0 mJ net, 3.0 mJ gross
  rail2 ina236@0x40 50 mOhm, wide: raw 50 constant -> 80 mW (1.6 mW LSB)
        energy per inference 0 mJ net, 0.8 mJ gross
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts"))
from alp_power.capture import write_jsonl  # noqa: E402

PERIOD, T0 = 500, 1_000_000
header = {"type": "header", "version": 1, "protocol": 2, "period_us": PERIOD, "idle_s": 1.0,
          "i2c_hz": 1000000, "marker": "TARGET_DONE",
          "monitors": [
              {"name": "5V", "part": "ina236", "addr": "0x4A", "shunt": 0.02, "range": "fine"},
              {"name": "3V3", "part": "ina236", "addr": "0x40", "shunt": 0.05, "range": "wide"}]}
rows = []
for i in range(4000):
    t = i * PERIOD
    high = t >= 1_000_000 and (t - 1_000_000) % 50_000 < 10_000
    raw1 = 300 if high else 100
    rows.append((T0 + t, 1 if high else 0, raw1.to_bytes(2, "big") + (50).to_bytes(2, "big")))
write_jsonl(Path(__file__).with_name("synthetic_capture.jsonl"), header, rows, 0)
