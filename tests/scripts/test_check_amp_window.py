# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_amp_window.py and scripts/gen_amp_window.py."""
from __future__ import annotations

import copy
import json
import re
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_amp_window as gate  # noqa: E402
import gen_amp_window as gen  # noqa: E402
import gen_zephyr_board as zb  # noqa: E402

FILES = [str(gen.SOC_JSON), str(gen.OUT), gate.LINUX_DTSI, *gate.BOARD_DTS, gate.MAGIC_HOME]


def _tree(tmp_path: Path) -> Path:
    for rel in FILES:
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, tmp_path / rel)
    return tmp_path


def _edit(path: Path, old: str, new: str) -> None:
    text = path.read_text(encoding="utf-8")
    assert old in text, old
    path.write_text(text.replace(old, new), encoding="utf-8")


def test_real_tree_is_clean():
    assert gate.find_problems(REPO) == []


def test_clean_copy_passes(tmp_path):
    assert gate.find_problems(_tree(tmp_path)) == []


def test_header_carries_every_region():
    c = gen.load_carveout()
    h = (REPO / gen.OUT).read_text(encoding="utf-8")
    for name, (addr, size) in gen.regions(c).items():
        m = name.upper().replace("-", "_")
        assert f"ALP_AMP_{m}_A55_BASE" in h and f"{addr:#x}u" in h
        assert f"ALP_AMP_{m}_SIZE" in h and f'"{addr:x}.{name}"' in h


def test_generators_follow_a_moved_soc_spec():
    """Moving cm33_ns_base / a region in a COPY of the spec moves every generated output."""
    soc = json.loads((REPO / gen.SOC_JSON).read_text(encoding="utf-8"))
    soc["openamp_carveout"]["cm33_ns_base"] += 0x100000
    soc["openamp_carveout"]["regions"]["rsctbl"]["size"] = 0x2000
    soc["openamp_carveout"]["regions"]["mhu-shm"]["offset"] = 0x2000
    c = soc["openamp_carveout"]

    header = gen.render(c)
    assert "ALP_AMP_RSCTBL_SIZE" in header and "0x2000u" in header
    assert "ALP_AMP_A55_TO_CM33_NS_OFFSET 0x50100000u" in header
    assert '"4f702000.mhu-shm"' in header

    dts = "\n".join(zb._openamp_subst(zb._V2N_OPENAMP_TAIL, soc))
    assert "rsctbl: memory@9f800000" in dts and "reg = <0x9f800000 0x2000>;" in dts
    assert "mhu1_shm: memory@9f802000" in dts
    assert "9f700000" not in dts
    assert "alp,cm33-ns-to-a55-offset = <0x50100000>;" in dts
    assert not re.search(r"@[a-z0-9-]+\.[a-z0-9]+@", dts)


def test_moved_window_in_metadata_is_caught_everywhere(tmp_path):
    root = _tree(tmp_path)
    _edit(root / gen.SOC_JSON, '"a55_base": 1332740096', '"a55_base": 1332744192')
    msgs = "\n".join(gate.find_problems(root))
    assert "alp_amp_window.h: stale" in msgs
    assert "reserved-memory node" in msgs
    assert "rsctbl uio node" in msgs
    assert "vring-shm1 uio node" in msgs
    assert "alp,cm33-ns-to-a55-offset" in msgs


def test_rsctbl_smaller_than_the_beacon_is_caught(tmp_path):
    root = _tree(tmp_path)
    soc = json.loads((root / gen.SOC_JSON).read_text(encoding="utf-8"))
    soc["openamp_carveout"]["regions"]["rsctbl"]["size"] = 8
    (root / gen.SOC_JSON).write_text(json.dumps(soc), encoding="utf-8")
    assert "rsctbl size 0x8 < beacon" in "\n".join(gate.find_problems(root))


def test_moved_region_in_metadata_is_caught_in_dtsi_and_board_dts(tmp_path):
    root = _tree(tmp_path)
    soc = json.loads((root / gen.SOC_JSON).read_text(encoding="utf-8"))
    soc["openamp_carveout"]["regions"]["vring-ctl0"]["size"] = 0x40000
    (root / gen.SOC_JSON).write_text(json.dumps(soc), encoding="utf-8")
    msgs = "\n".join(gate.find_problems(root))
    assert "vring-ctl0 uio node" in msgs
    assert "vring_ctrl0 reg" in msgs
    assert "alp_amp_window.h: stale" in msgs


def test_drifted_linux_dt_and_board_dts_are_caught(tmp_path):
    root = _tree(tmp_path)
    _edit(root / gate.LINUX_DTSI, "0x0 0x900000", "0x0 0x800000")
    _edit(root / gate.LINUX_DTSI, "reg = <0x0 0x4f850000 0x0 0x50000>;", "reg = <0x0 0x4f850000 0x0 0x40000>;")
    _edit(root / gate.BOARD_DTS[0], "<0x9f700000 0x900000>", "<0x9f700000 0x800000>")
    _edit(root / gate.BOARD_DTS[0], "<0x9fc00000 0x300000>", "<0x9fc00000 0x200000>")
    msgs = gate.find_problems(root)
    assert any("e1m-v2n-som.dtsi: reserved-memory node" in m for m in msgs)
    assert any("vring-ctl1 uio node" in m for m in msgs)
    assert any("openamp_shm reg" in m and "v2n101" in m for m in msgs)
    assert any("vring_shm1 reg" in m and "v2n101" in m for m in msgs)


def test_region_outside_the_carveout_is_caught(tmp_path):
    root = _tree(tmp_path)
    soc = json.loads((root / gen.SOC_JSON).read_text(encoding="utf-8"))
    soc["openamp_carveout"]["regions"]["vring-shm1"]["size"] = 0x500000
    (root / gen.SOC_JSON).write_text(json.dumps(soc), encoding="utf-8")
    assert any("runs past the carveout" in m for m in gate.find_problems(root))


def test_second_copy_of_the_magic_or_beacon_address_is_caught(tmp_path):
    root = _tree(tmp_path)
    (root / "src").mkdir(exist_ok=True)
    (root / "src" / "dup.c").write_text("#define M 0xA10D0683u\n", encoding="utf-8")
    (root / "firmware").mkdir()
    (root / "firmware" / "dup.h").write_text("#define A 0x4f700ff8u\n", encoding="utf-8")
    msgs = "\n".join(gate.find_problems(root))
    assert "src/dup.c: beacon magic/address restated" in msgs
    assert "firmware/dup.h: beacon magic/address restated" in msgs


def test_hil_yaml_may_quote_the_beacon_but_only_correctly(tmp_path):
    root = _tree(tmp_path)
    spec = root / "tests" / "hil" / "board" / "spec.yaml"
    spec.parent.mkdir(parents=True)
    spec.write_text("ssh_command: |-\n  devmem 0x4f700ff0 32 | grep -q 0xa10d0683\n", encoding="utf-8")
    assert gate.find_problems(root) == []
    spec.write_text("ssh_command: |-\n  devmem 0x4f700fe0 32 | grep -q 0xa10d0684\n", encoding="utf-8")
    msgs = "\n".join(gate.find_problems(root))
    assert "devmem 0x4f700fe0 is not a beacon word" in msgs
    assert "magic 0xa10d0684" in msgs
