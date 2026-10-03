"""The on-module xSPI NOR is a multi-source class (#2660): presets declare the class, the
Linux DT layout fits the smallest approved part, and nothing names one MPN."""
import re
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
V2_PRESETS = sorted((REPO / "metadata" / "e1m_modules").glob("E1M-V2[NM]10?.yaml"))
DTSI = REPO / "meta-alp-sdk" / "recipes-kernel" / "linux" / "linux-renesas" / "e1m-v2n-som.dtsi"


@pytest.mark.parametrize("path", V2_PRESETS, ids=lambda p: p.stem)
def test_preset_declares_the_flash_class(path):
    cls = yaml.safe_load(path.read_text(encoding="utf-8"))["on_module"]["nor_flash_class"]
    assert cls == {"interface": "xspi", "data_width": 4, "voltage_v": 1.8,
                   "min_size_bytes": 32 << 20, "jedec_detect_required": True}


def _partitions():
    flash = DTSI.read_text(encoding="utf-8").split("&xspi {")[1].split("\n};")[0]
    assert re.search(r'compatible = "jedec,spi-nor";', flash)
    return [(m[1], int(m[2], 16), int(m[3], 16)) for m in
            re.finditer(r'label = "(\w+)";\s*reg = <(0x[0-9a-f]+) (0x[0-9a-f]+)>', flash)]


def test_dt_layout_fits_min_size_and_user_ends_at_it():
    min_b = yaml.safe_load(V2_PRESETS[0].read_text(encoding="utf-8"))["on_module"]["nor_flash_class"]["min_size_bytes"]
    parts = _partitions()
    assert [p[0] for p in parts] == ["bl2", "fip", "user"]
    end = 0
    for label, off, size in parts:
        assert off == end, f"{label}: gap/overlap at {off:#x}"
        end = off + size
    assert end == min_b
    fip = parts[1]
    assert fip[1] + fip[2] <= 16 << 20                       # boot content stays below the 16 MiB xSPI read limit
    assert fip[1] + 0x1A0000 + 0x30000 <= fip[1] + fip[2]    # CM33 slot (mtd1+0x1a0000, 0x30000) is inside fip


def test_dt_names_no_part():
    assert not re.search(r"MX25|GD25|W25Q|IS25", DTSI.read_text(encoding="utf-8"))


def test_steps_reads_the_min_size_from_the_preset():
    import sys
    sys.path.insert(0, str(REPO / "scripts"))
    from provision import steps
    preset = yaml.safe_load(V2_PRESETS[0].read_text(encoding="utf-8"))
    assert steps.xspi_min_bytes(preset) == 32 << 20
    assert steps.xspi_min_bytes({"on_module": {}}) == 0
