# SPDX-License-Identifier: Apache-2.0
"""scripts/gen_linux_ownership_dt.py: the Linux ownership fragment follows the
metadata (resolved ownership, linux_enable, hw_blocked, SoC linux_dt PFC codes,
CM33-owned clocks)."""
import copy
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import gen_linux_ownership_dt as g  # noqa: E402
from alp_orchestrate import linux_ownership as lo  # noqa: E402
from alp_orchestrate.ownership import resolve_ownership  # noqa: E402

DOC, SOC, LINKS = g._load(REPO)
GEN = str(REPO / "scripts" / "gen_linux_ownership_dt.py")


def _enabled(doc, *insts):
    doc = copy.deepcopy(doc)
    for i in insts:
        doc["assignable"][i]["linux_enable"] = True
        doc["assignable"][i]["linux_evidence"] = "test"
    return doc


def test_committed_fragment_is_in_sync():
    r = subprocess.run([sys.executable, GEN, "--check"], capture_output=True, text=True, encoding="utf-8")
    assert r.returncode == 0, r.stderr


def test_default_fragment_enables_nothing_new():
    """The shipped default must change no Linux behaviour: no node is turned
    on, no pinctrl group is added -- only the CM33 clock hold is emitted."""
    text, labels = lo.render(DOC, SOC, LINKS)
    assert "GENERATED (scripts/gen_linux_ownership_dt.py" in text and "DO NOT EDIT" in text
    assert 'status = "okay"' not in text and "&pinctrl" not in text and "PORT_PINMUX" not in text
    assert labels == {"cpg"}
    assert not any(e.get("linux_enable") for e in DOC["assignable"].values())
    assert "3.3 V tolerant" in text  # rspi0: hw_blocked reason quoted
    assert "left at the vendor status" in text


def test_enabled_uart0_gets_pinctrl_from_the_soc_metadata():
    soc = copy.deepcopy(SOC)
    soc["linux_dt"]["UART0"]["pinmux"] = {"UART0_TXD0": 7, "UART0_RXD0": 7}
    soc["linux_dt"]["UART0"]["label"] = "sciX"
    text, labels = lo.render(_enabled(DOC, "e1m_uart0"), soc, LINKS)
    assert "RZV2N_PORT_PINMUX(5, 0, 7)" in text and "&sciX {" in text and 'status = "okay"' in text
    assert labels == {"sciX", "cpg"}


def test_linux_enable_needs_evidence_and_a_pfc_code_is_never_guessed():
    doc = _enabled(DOC, "e1m_uart1")
    text, _ = lo.render(doc, SOC, LINKS)
    assert "GAP -- no PFC function code" in text and "&sci1 {" not in text
    del doc["assignable"]["e1m_uart1"]["linux_evidence"]
    with pytest.raises(lo.GenError, match="needs linux_evidence"):
        lo.render(doc, SOC, LINKS)


def test_can_channel_and_m33_owner():
    soc = copy.deepcopy(SOC)
    soc["linux_dt"]["CANFD3"]["pinmux"] = {"CANFD3_CRX3": 9, "CANFD3_CTX3": 9}
    text, labels = lo.render(_enabled(DOC, "e1m_can0"), soc, LINKS)
    assert "&canfd {" in text and "channel3 {" in text and "canfd/channel3" in labels
    text, _ = lo.render(_enabled(DOC, "e1m_uart0"), SOC, LINKS, {**resolve_ownership(DOC), "e1m_uart0": "m33"})
    assert "owned by m33; Linux must not claim it" in text
    assert '&sci0 {\n\tstatus = "disabled";' in text


def test_hw_blocked_wins_over_linux_enable():
    doc = _enabled(DOC, "e1m_spi0")
    soc = copy.deepcopy(SOC)
    soc["linux_dt"]["RSPI0"]["pinmux"] = {r["peripheral"]: 1 for r in doc["assignable"]["e1m_spi0"]["rows"]}
    text, _ = lo.render(doc, soc, LINKS)
    assert "&rspi0 {" not in text and "hardware-blocked" in text
    del doc["assignable"]["e1m_spi0"]["hw_blocked"]
    assert "&rspi0 {" in lo.render(doc, soc, LINKS)[0]


def test_unknown_soc_instance_is_an_error():
    doc = copy.deepcopy(DOC)
    doc["assignable"]["e1m_uart0"]["soc_instance"] = "NOPE"
    with pytest.raises(lo.GenError, match="NOPE"):
        lo.render(doc, SOC, LINKS)


def test_vendor_dtsi_check_rejects_a_missing_label(tmp_path):
    v = tmp_path / "r9a09g056.dtsi"
    v.write_text("\t\tsci1: serial@12801000 {\n\t\t};\n", encoding="utf-8")
    assert g.check_vendor({"sci0"}, v) == ["label &sci0 is not defined in r9a09g056.dtsi"]
    v.write_text("\t\tsci0: serial@12800c00 {\n\t\t};\n", encoding="utf-8")
    assert g.check_vendor({"sci0"}, v) == []


def _tree(tmp_path, mutate):
    """A throw-away --root with the generator's inputs, `mutate(doc)` applied
    to core-ownership.yaml."""
    for rel in (lo.SRC, g.LINKS, g.SOC.as_posix()):
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(REPO / rel, tmp_path / rel)
    f = tmp_path / lo.SRC
    doc = yaml.safe_load(f.read_text(encoding="utf-8"))
    mutate(doc)
    f.write_text(yaml.safe_dump(doc), encoding="utf-8")
    return tmp_path


def test_main_emits_the_cm33_clock_hold_for_an_m33_default(tmp_path):
    """What main() writes, not what render() returns: a SoM that defaults
    UART0 to the M33 holds its clocks next to RSCI7 and does not enable it."""
    def mutate(doc):
        e = doc["assignable"]["e1m_uart0"]
        e["candidates"], e["default"] = ["a55", "m33"], "m33"
    root = _tree(tmp_path, mutate)
    (root / g.OUT).parent.mkdir(parents=True, exist_ok=True)
    assert subprocess.run([sys.executable, GEN, "--root", str(root)]).returncode == 0
    text = (root / g.OUT).read_text(encoding="utf-8")
    sci7 = ", ".join(f'"{c}"' for c in SOC["linux_dt"]["SCI7"]["cpg_clocks"])
    uart0 = ", ".join(f'"{c}"' for c in SOC["linux_dt"]["UART0"]["cpg_clocks"])
    assert f"renesas,cm33-owned-clocks = {sci7}, {uart0};" in text
    assert '&sci0 {\n\tstatus = "disabled";' in text and 'status = "okay"' not in text
    # the real default holds the GD32 link only
    real = (REPO / g.OUT).read_text(encoding="utf-8")
    assert f"renesas,cm33-owned-clocks = {sci7};" in real


def test_m33_owned_instance_without_cpg_clocks_is_an_error():
    soc = copy.deepcopy(SOC)
    del soc["linux_dt"]["UART0"]["cpg_clocks"]
    with pytest.raises(lo.GenError, match="no cpg_clocks"):
        lo.cm33_clocks(DOC, soc, LINKS, {**resolve_ownership(DOC), "e1m_uart0": "m33"})


def test_project_emit_cli_and_fragment_verification(tmp_path):
    """alp_project.py --emit linux-ownership-dts writes a fragment that
    --fragment verifies against the vendor dtsi (the bbappend's check)."""
    out = tmp_path / "linux-ownership.dtsi"
    r = subprocess.run([sys.executable, str(REPO / "scripts" / "alp_project.py"),
                        "--input", str(REPO / "examples/multicore/rpmsg-v2n/board.yaml"),
                        "--emit", "linux-ownership-dts", "--output", str(out)],
                       capture_output=True, text=True, encoding="utf-8")
    assert r.returncode == 0, r.stderr
    assert out.read_text(encoding="utf-8") == (REPO / g.OUT).read_text(encoding="utf-8")
    v = tmp_path / "r9a09g056.dtsi"
    v.write_text("\t\tcpg: clock-controller@10420000 {\n\t\t};\n", encoding="utf-8")
    cmd = [sys.executable, GEN, "--fragment", str(out), "--vendor-dtsi", str(v)]
    assert subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8").returncode == 0
    out.write_text(out.read_text(encoding="utf-8") + '&nope {\n\tstatus = "disabled";\n};\n', encoding="utf-8")
    assert subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8").returncode == 1


def _project_manifest(tmp_path, override=None):
    board = REPO / "examples/multicore/rpmsg-v2n/board.yaml"
    b = tmp_path / "board.yaml"
    b.write_text(board.read_text(encoding="utf-8") + (override or ""), encoding="utf-8")
    m = tmp_path / "system-manifest.yaml"
    r = subprocess.run([sys.executable, str(REPO / "scripts" / "alp_project.py"), "--input", str(b),
                        "--emit", "system-manifest", "--output", str(m)], capture_output=True, text=True, encoding="utf-8")
    assert r.returncode == 0, r.stderr
    return m


def test_manifest_render_equals_project_emit_for_an_override_project(tmp_path):
    """The bbappend path (--manifest) and the CLI path (--emit
    linux-ownership-dts) render the same bytes for the same board.yaml."""
    m = _project_manifest(tmp_path, "\nownership:\n  e1m_uart0: a55\n")
    out = tmp_path / "o.dtsi"
    assert subprocess.run([sys.executable, GEN, "--manifest", str(m), "--output", str(out)]).returncode == 0
    emit = tmp_path / "e.dtsi"
    r = subprocess.run([sys.executable, str(REPO / "scripts" / "alp_project.py"),
                        "--input", str(tmp_path / "board.yaml"), "--emit", "linux-ownership-dts",
                        "--output", str(emit)], capture_output=True, text=True, encoding="utf-8")
    assert r.returncode == 0, r.stderr
    assert out.read_bytes() == emit.read_bytes()
    assert out.read_text(encoding="utf-8") == (REPO / g.OUT).read_text(encoding="utf-8")


def test_manifest_with_a_stale_ownership_set_fails(tmp_path):
    m = _project_manifest(tmp_path)
    doc = yaml.safe_load(m.read_text(encoding="utf-8"))
    doc["ownership"].pop("e1m_can1")
    m.write_text(yaml.safe_dump(doc), encoding="utf-8")
    r = subprocess.run([sys.executable, GEN, "--manifest", str(m), "--output", str(tmp_path / "o")],
                       capture_output=True, text=True, encoding="utf-8")
    assert r.returncode == 1 and "stale manifest" in r.stderr


def test_fragment_labels_include_channel_children():
    text = '&canfd {\n\tstatus = "okay";\n\n\tchannel3 {\n\t\tstatus = "okay";\n\t};\n};\n&cpg {\n};\n'
    assert g.fragment_labels(text) == {"canfd", "canfd/channel3", "cpg"}


KERNEL = REPO / "meta-alp-sdk" / "recipes-kernel" / "linux"


def test_default_kernel_build_includes_the_ownership_fragment():
    """A build with no system-manifest must still compile the SoM-default
    ownership dtsi: it carries the cm33-owned-clocks hold that keeps the CM33's
    RSCI7 clocks on through clk_disable_unused, so it can never be skipped or
    turned into a hard failure."""
    bb = (KERNEL / "linux-renesas_%.bbappend").read_text(encoding="utf-8")
    assert "e1m-v2n-ownership.dtsi" in bb.split("do_configure:prepend", 1)[1].split("install", 1)[1].split("/\"", 1)[0]
    # no manifest -> warn and keep the committed fragment, never bbfatal
    no_manifest = bb.split("if [ -f \"${ALP_OWN_M}\" ]", 1)[1].split("    fi", 1)[0].split("    else", 1)[1]
    assert "bbwarn" in no_manifest
    # ...but a stale committed fragment is still fatal on that path
    assert "--check" in no_manifest and "--vendor-dtsi" in no_manifest and "bbfatal" in no_manifest
    assert "ALP_OWNERSHIP_SOM_DEFAULT" not in bb
    # every V2N/V2M board dts includes the fragment exactly once (last), the
    # SoM dtsi not at all, and the fragment holds RSCI7
    som = (KERNEL / "linux-renesas" / "e1m-v2n-som.dtsi").read_text(encoding="utf-8")
    assert "e1m-v2n-ownership.dtsi" not in som
    for dts in ("e1m-v2n101-x-evk.dts", "e1m-v2m101-x-evk.dts"):
        assert (KERNEL / "linux-renesas" / dts).read_text(encoding="utf-8").count('#include "e1m-v2n-ownership.dtsi"') == 1
    frag = (REPO / g.OUT).read_text(encoding="utf-8")
    assert all(f'"{c}"' in frag for c in SOC["linux_dt"]["SCI7"]["cpg_clocks"])


def test_can_netdev_map_is_rank_of_channel_not_bus_id():
    """#2352: E1M CAN0 = CANFD3 -> can1, E1M CAN1 = CANFD2 -> can0."""
    own = resolve_ownership(DOC)
    assert lo.can_netdev_map(DOC, SOC, own) == ["can1", "can0"]
    assert 'alp,e1m-can-netdev = "can1", "can0";' in lo.render(DOC, SOC, LINKS)[0]
    # An M33-owned CAN0 leaves no Linux netdev for it; CAN1 is then the only channel.
    assert lo.can_netdev_map(DOC, SOC, {**own, "e1m_can0": "m33"}) == ["", "can0"]
    # No CAN instances -> no property.
    doc = copy.deepcopy(DOC)
    for k in ("e1m_can0", "e1m_can1"):
        del doc["assignable"][k]
    assert lo.can_netdev_map(doc, SOC, resolve_ownership(doc)) == []
    assert "alp,e1m-can-netdev" not in lo.render(doc, SOC, LINKS)[0]
