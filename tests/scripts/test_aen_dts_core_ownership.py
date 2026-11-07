"""AEN DTS core ownership (#2984): storage_partition and the HE-local LP
peripherals belong to the M55-HE; the M55-HP boards must not carry them."""
import re
import unittest
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[2]
BOARDS = REPO / "zephyr" / "boards" / "alp"
DTS = REPO / "zephyr" / "dts" / "alif"
HP_BOARDS = ("e1m_aen401_m55_hp", "e1m_aen601_m55_hp",
             "e1m_aen801_m55_hp", "e1m_aen803_m55_hp")
E8_HP = HP_BOARDS[2:]
E8_HE = ("e1m_aen801_m55_he", "e1m_aen803_m55_he")
HE_NODES = ("lpuart", "lpi2c1", "lpspi0", "lpspi1", "lpi2s", "lppdm", "lpcam",
            "rtc0", "lptimer1", "lptimer2", "lptimer3", "lpcmp0", "bod0",
            "lpi3c0", "dma2", "evtrtr2")


def _dts(board: str) -> str:
    return next((BOARDS / board).glob("*.dts")).read_text(encoding="utf-8")


def _storage_block(text: str) -> str:
    return re.search(r"storage_partition: partition@560000 \{.*?\};", text, re.S).group(0)


class TestAenDtsCoreOwnership(unittest.TestCase):
    def test_hp_storage_partition_disabled(self) -> None:
        for b in HP_BOARDS:
            self.assertIn('status = "disabled"', _storage_block(_dts(b)), b)

    def test_he_storage_partition_enabled(self) -> None:
        for b in E8_HE:
            self.assertNotIn("status", _storage_block(_dts(b)), b)

    def test_hp_e8_lptimer0_disabled(self) -> None:
        for b in E8_HP:
            self.assertRegex(_dts(b), r'&lptimer0 \{\s*status = "disabled";', b)

    def test_he_e8_keeps_lptimer0_and_lp_peripherals(self) -> None:
        for b in E8_HE:
            t = _dts(b)
            self.assertNotIn("&lptimer0", t, b)
            self.assertIn("ensemble_e8_he_peripherals.dtsi", t, b)
        for b in E8_HP:
            self.assertNotIn("ensemble_e8_he_peripherals.dtsi", _dts(b), b)

    def test_he_local_nodes_live_only_in_he_dtsi(self) -> None:
        shared = (DTS / "ensemble_e8_peripherals.dtsi").read_text(encoding="utf-8")
        he = (DTS / "ensemble_e8_he_peripherals.dtsi").read_text(encoding="utf-8")
        for n in HE_NODES:
            self.assertIsNone(re.search(rf"^\t{n}: ", shared, re.M), n)
            self.assertIsNotNone(re.search(rf"^\t{n}: ", he, re.M), n)

    def test_metadata_storage_is_he_only(self) -> None:
        for f in sorted((REPO / "metadata" / "e1m_modules").glob("E1M-AEN*.yaml")):
            doc = yaml.safe_load(f.read_text(encoding="utf-8"))
            mm = doc.get("memory_map") or doc.get("som", {}).get("memory_map") or []
            for r in mm:
                if r.get("name") == "storage":
                    self.assertEqual(r["accessible_from"], ["m55_he"], f.name)


if __name__ == "__main__":
    unittest.main()
