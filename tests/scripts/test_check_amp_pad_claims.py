# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_amp_pad_claims.py (issue #1142).

The gate exists to catch ONE historical regression shape: a Linux
devicetree pad claim landing on a pad the metadata attributes to the
CM33 -- the errata E3 `P9.6`/`SCK7` clobber
(errata E3 in `docs/errata-e1m-x-v2n.md`).  So the seeded-violation test
reproduces that exact hog rather than a synthetic one: a gate never shown
to go red against the known-bad tree is decoration.

Run locally:

    python -m pytest tests/scripts/test_check_amp_pad_claims.py -q
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))
import check_amp_pad_claims as gate  # noqa: E402

# The errata E3 hog as it read before the 2026-06-12 revision made it
# PB.1-only.  P9.6 is the CM33's SCK7.
_P96_HOG = """\
&pinctrl {
	usb-ovc-disable-hog {
		gpio-hog;
		gpios = <RZV2N_GPIO(9, 6) GPIO_ACTIVE_HIGH>, <RZV2N_GPIO(B, 1) GPIO_ACTIVE_HIGH>;
		input;
		line-name = "usb_ovc_disable";
	};
};
"""

_PINCTRL_GROUP = "&pinctrl { g { pinmux = <RZV2N_PORT_PINMUX(0, 7, 1)>; }; };\n"


def _pad(peripheral: str, pad: str, core: str | None = None) -> dict:
    row = {"owner": "renesas", "silicon_peripheral": peripheral,
           "silicon_pad": pad}
    if core:
        row["core"] = core
    return row


def _root(tmp_path: Path, dt_text: str, pads: list[dict],
          with_exempt: bool = True) -> Path:
    rows = list(pads)
    if with_exempt:
        # Keep gate.EXEMPT matched, or every fixture also trips the
        # stale-exemption check and drowns the case under test.
        rows += [_pad(peripheral, pad, "m33") for peripheral, pad in gate.EXEMPT]
    (tmp_path / gate.PINMUX).parent.mkdir(parents=True, exist_ok=True)
    (tmp_path / gate.PINMUX).write_text(
        yaml.safe_dump({"pads": rows}), encoding="utf-8")
    dt_dir = tmp_path / gate.LINUX_DT_DIR / "linux-renesas"
    dt_dir.mkdir(parents=True, exist_ok=True)
    (dt_dir / "e1m-x-evk.dtsi").write_text(dt_text, encoding="utf-8")
    return tmp_path


def test_real_tree_is_clean() -> None:
    """The shipped tree passes -- the P9.6 half of the hog is gone."""
    assert gate.find_problems(REPO) == []


def test_linux_claim_on_m33_pad_fails(tmp_path: Path) -> None:
    """The known-bad tree: the pre-2026-06-12 P9.6 hog goes red."""
    root = _root(tmp_path, _P96_HOG,
                 [_pad("GD32_SPI.SCLK", "P96", "m33"),
                  _pad("WIFI_SDIO.D0", "PB1")])
    problems = gate.find_problems(root)
    assert len(problems) == 1
    assert "e1m-x-evk.dtsi:4" in problems[0]
    assert "P96" in problems[0] and "GD32_SPI.SCLK" in problems[0]


def test_pinmux_group_claim_on_m33_pad_fails(
        tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """A pinctrl group claim counts too, not just a gpio-hog.

    `gate.EXEMPT` is empty on the real tree now (RIIC8/BRD_I2C moved to
    `core: "a55"`, feat/v2m-deepx-rail-uboot 2026-09-24 -- see
    `test_real_tree_is_clean`), so the "exemption's own named peripheral
    is allowed" half of this test synthesises a throwaway exemption
    rather than resting on a real (peripheral, pad) fact that can drift
    out from under it again."""
    monkeypatch.setattr(gate, "EXEMPT", {("RIIC8_SCL8", "P07"): "test-only"})
    # P07 under the peripheral its exemption names: allowed.
    assert gate.find_problems(
        _root(tmp_path, _PINCTRL_GROUP,
              [_pad("RIIC8_SCL8", "P07", "m33")])) == []
    # The same pad under a peripheral the exemption does not name: not.
    problems = gate.find_problems(
        _root(tmp_path / "b", _PINCTRL_GROUP,
              [_pad("SOMETHING_ELSE", "P07", "m33")], with_exempt=False))
    assert any("P07" in p and "SOMETHING_ELSE" in p for p in problems)


def test_unattributed_pad_is_not_a_claim(tmp_path: Path) -> None:
    """A pad with no `core` is 'not attributed' -- never a violation."""
    root = _root(tmp_path,
                 "&pinctrl { h { gpios = <RZV2N_GPIO(A, 0) 0>; }; };\n",
                 [_pad("eMMC_V_SEL", "PA0")])
    assert gate.find_problems(root) == []


def test_stale_exemption_fails(tmp_path: Path) -> None:
    """An exemption excusing an attribution that no longer exists is an error."""
    root = _root(tmp_path, "/* no pad claims */\n",
                 [_pad("GD32_SPI.SCLK", "P96", "m33")], with_exempt=False)
    problems = gate.find_problems(root)
    assert len(problems) == len(gate.EXEMPT)
    assert all("matches no" in p for p in problems)


# --- both directions + assignable defaults ---------------------------------

def _ownership(root: Path, default: str) -> None:
    f = root / "metadata" / "e1m_modules" / "v2n" / "core-ownership.yaml"
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_text(yaml.safe_dump({"core_ownership": [], "assignable": {"e1m_uart0": {
        "default": default, "candidates": ["a55", "m33"],
        "rows": [{"peripheral": "UART0_TXD0", "pad": "P50"}]}}}), encoding="utf-8")


def _cm33_board(root: Path, status: str) -> None:
    d = root / gate.CM33_BOARDS / "e1m_v2n101_m33_sm"
    d.mkdir(parents=True, exist_ok=True)
    (d / "b-pinctrl.dtsi").write_text(
        "&pinctrl {\n\tsci0_pins: sci0 {\n\t\tsci0-pinmux {\n"
        "\t\t\tpinmux = <RZV_PINMUX(PORT_05, 0, 1)>;\n\t\t};\n\t};\n};\n", encoding="utf-8")
    (d / "b.dts").write_text(
        f'&sci0 {{\n\tpinctrl-0 = <&sci0_pins>;\n\tstatus = "{status}";\n}};\n', encoding="utf-8")


def test_real_cm33_board_claims_only_m33_pads() -> None:
    pads = {pad for _, _, pad in gate._cm33_claims(REPO)}
    assert {"P76", "P77", "P96"} <= pads  # the GD32 SCI7 link is a real, enabled claim
    assert not pads & set(gate._pads_by_core(REPO, "a55"))


def test_cm33_enabled_claim_on_a55_default_pad_fails(tmp_path: Path) -> None:
    root = _root(tmp_path, "", [_pad("UART0_TXD0", "P50")])
    _ownership(root, "a55")
    _cm33_board(root, "okay")
    problems = gate.find_problems(root)
    assert len(problems) == 1
    assert "b.dts:1" in problems[0] and "P50" in problems[0] and "A55" in problems[0]


def test_cm33_disabled_node_is_not_a_claim(tmp_path: Path) -> None:
    root = _root(tmp_path, "", [_pad("UART0_TXD0", "P50")])
    _ownership(root, "a55")
    _cm33_board(root, "disabled")
    assert gate.find_problems(root) == []


def test_linux_claim_on_assignable_pad_defaulting_to_m33_fails(tmp_path: Path) -> None:
    root = _root(tmp_path, "&pinctrl { g { pinmux = <RZV2N_PORT_PINMUX(5, 0, 1)>; }; };\n",
                 [_pad("UART0_TXD0", "P50")])
    _ownership(root, "m33")
    problems = gate.find_problems(root)
    assert len(problems) == 1 and "P50" in problems[0] and "CM33" in problems[0]
    _ownership(root, "a55")  # same claim, a55 default: clean
    assert gate.find_problems(root) == []


def test_project_override_makes_a_carrier_claim_fail(tmp_path: Path) -> None:
    """A carrier dtsi claims UART0's pad (P50).  Clean at the SoM default
    (a55); a board.yaml `ownership: {e1m_uart0: m33}` moves the pad to the CM33,
    so the real pass over carrier dtsi + generated fragment must fail.  No
    patching: the project fragment comes from the real emitter."""
    import shutil
    from alp_orchestrate import load_board_yaml
    root = tmp_path / "metadata"
    shutil.copytree(REPO / "metadata", root)
    f = root / "e1m_modules" / "v2n" / "core-ownership.yaml"
    doc = yaml.safe_load(f.read_text(encoding="utf-8"))
    doc["assignable"]["e1m_uart0"].update(
        candidates=["a55", "m33"], m33={"dt_label": "sci0", "alias": "alp-uart9"})
    f.write_text(yaml.safe_dump(doc), encoding="utf-8")
    dt = tmp_path / gate.LINUX_DT_DIR / "linux-renesas"
    dt.mkdir(parents=True)
    (dt / "e1m-x-evk.dtsi").write_text(
        "&pinctrl { g { pinmux = <RZV2N_PORT_PINMUX(5, 0, 1)>; }; };\n", encoding="utf-8")
    shutil.copytree(REPO / "examples" / "multicore" / "rpmsg-v2n", tmp_path / "p")
    b = tmp_path / "p" / "board.yaml"
    base = b.read_text(encoding="utf-8")
    assert gate.find_project_problems(load_board_yaml(b, metadata_root=root)) == []
    b.write_text(base + "\nownership:\n  e1m_uart0: m33\n", encoding="utf-8")
    proj = load_board_yaml(b, metadata_root=root)
    assert "P50" in gate._pads_by_core(tmp_path, "m33", proj.ownership)
    problems = gate.find_project_problems(proj)
    assert len(problems) == 1 and "e1m-x-evk.dtsi:1" in problems[0] and "P50" in problems[0]
