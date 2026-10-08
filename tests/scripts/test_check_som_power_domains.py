# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_som_power_domains.py (alp-sdk#2784)."""
from __future__ import annotations

from pathlib import Path

import check_som_power_domains as gate

REPO = Path(__file__).resolve().parents[2]

LINKS = """\
schemaVersion: on-module-links-v2
families: [aen]
on_module_links:
  backlight:
    enable:
      silicon_pad: "P5_5"
power_domains:
  wifi_ble:
    presence: { on_module_key: wifi_ble }
    controls:
      - signal: E_WIFI_NRST
        silicon_pad: "P15_1"
        gpio_node: lpgpio
        gpio_pin: 1
        source: { file: inter-chip.tsv, signal: E_WIFI_NRST }
    default_action: hold_reset
    actions:
      hold_reset: { control: E_WIFI_NRST }
    dependents:
      - { kind: cam_ldo, net: CAM_EN_LDO0, driver_pin: GPIO_0,
          source: { file: inter-chip.tsv, signal: CAM_EN_LDO0 } }
  backlight:
    presence: { family_invariant: true }
    controls:
      - signal: BACKLIGHT_EN
        silicon_pad: "P5_5"
        gpio_node: gpio5
        gpio_pin: 5
        source: { file: on-module-links.yaml, key: backlight.enable }
    default_action: enable_low
    actions:
      enable_low: { control: BACKLIGHT_EN }
"""
INTER_CHIP = ("signal\tcc3501e_role\tcc3501e_pad\talif_role\talif_pad\n"
              "CAM_EN_LDO0\tGPIO_0\tGPIO_0\t\t\n"
              "E_WIFI_NRST\t\t\tWi-Fi reset out\tP15_1_FLEX\n")


def _tree(tmp_path: Path, links: str = LINKS, inter_chip: str = INTER_CHIP) -> Path:
    aen = tmp_path / "metadata" / "e1m_modules" / "aen"
    aen.mkdir(parents=True)
    (aen / "on-module-links.yaml").write_text(links, encoding="utf-8")
    (aen / "inter-chip.tsv").write_text(inter_chip, encoding="utf-8")
    (tmp_path / "metadata" / "e1m_modules" / "E1M-AEN801.yaml").write_text(
        "on_module:\n  wifi_ble: cc3501e\n", encoding="utf-8")
    return tmp_path


def test_clean_tree_passes(tmp_path):
    assert gate.find_problems(_tree(tmp_path)) == []


def test_real_tree_passes():
    assert gate.find_problems(REPO) == []


def test_pad_drifting_from_tsv_fails(tmp_path):
    root = _tree(tmp_path, LINKS.replace('silicon_pad: "P15_1"', 'silicon_pad: "P15_2"')
                 .replace("gpio_pin: 1", "gpio_pin: 2"))
    problems = gate.find_problems(root)
    assert any("silicon_pad P15_2 but inter-chip.tsv row E_WIFI_NRST says P15_1" in p
               for p in problems), problems


def test_gpio_pin_disagreeing_with_pad_fails(tmp_path):
    root = _tree(tmp_path, LINKS.replace("gpio_pin: 1", "gpio_pin: 3"))
    problems = gate.find_problems(root)
    assert any("P15_1 is lpgpio pin 1, metadata says lpgpio pin 3" in p for p in problems)


def test_row_missing_from_tsv_fails(tmp_path):
    root = _tree(tmp_path, inter_chip=INTER_CHIP.replace("E_WIFI_NRST", "E_OTHER"))
    problems = gate.find_problems(root)
    assert any("no row 'E_WIFI_NRST'" in p for p in problems), problems


def test_backlight_pad_must_match_links_key(tmp_path):
    root = _tree(tmp_path, LINKS.replace('enable:\n      silicon_pad: "P5_5"',
                                         'enable:\n      silicon_pad: "P5_4"'))
    problems = gate.find_problems(root)
    assert any("on_module_links.backlight.enable says P5_4" in p for p in problems), problems


def test_unresolved_action_control_fails(tmp_path):
    root = _tree(tmp_path, LINKS.replace("hold_reset: { control: E_WIFI_NRST }",
                                         "hold_reset: { control: NOPE }"))
    assert any("control 'NOPE'" in p for p in gate.find_problems(root))


def test_dependent_driver_pin_must_match_tsv(tmp_path):
    root = _tree(tmp_path, LINKS.replace("driver_pin: GPIO_0", "driver_pin: GPIO_7"))
    problems = gate.find_problems(root)
    assert any("driver_pin 'GPIO_7' but inter-chip.tsv cc3501e_pad is 'GPIO_0'" in p
               for p in problems), problems
