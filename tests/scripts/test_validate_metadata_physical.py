import json, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

def test_chip_schema_exists_and_is_draft2020():
    schema = json.loads((REPO / "metadata/schemas/chip-v1.schema.json").read_text())
    assert schema["$schema"].endswith("2020-12/schema")
    assert schema["additionalProperties"] is False
    assert schema["properties"]["schema_version"]["const"] == 1

def test_pin_signal_must_resolve(tmp_path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "x.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "x", "display_name": "X", "vendor": "v",
        "mpn_population": ["X"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "SDA", "type": "bidir"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public",
                     "pins": [{"pad": "1", "signal": "SCL"}]},  # SCL not in signals
    }))
    failures = vm._check_chip_physical([p])
    assert failures and any("SCL" in m for _, msgs in failures for m in msgs)

def test_duplicate_pad_rejected(tmp_path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm2", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "y.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "y", "display_name": "Y", "vendor": "v",
        "mpn_population": ["Y"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "VDD", "type": "power"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public",
                     "pins": [{"pad": "1", "signal": "VDD"}, {"pad": "1", "signal": "GND"}]},
    }))
    failures = vm._check_chip_physical([p])
    assert failures and any("pad" in m.lower() for _, msgs in failures for m in msgs)

def test_passive_net_must_resolve(tmp_path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm5", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "z.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "z", "display_name": "Z", "vendor": "v",
        "mpn_population": ["Z"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "SCL", "type": "bidir"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public", "provenance": "web_provisional",
                     "pins": [{"pad": "1", "signal": "SCL"}],
                     "passives": [{"role": "pullup", "value": "4k7", "net": "SCLL",
                                   "refdes_prefix": "R"}]},  # SCLL not in signals
    }))
    failures = vm._check_chip_physical([p])
    assert failures and any("SCLL" in m for _, msgs in failures for m in msgs)

def test_block_schema_exists():
    schema = json.loads((REPO / "metadata/schemas/block-v1.schema.json").read_text())
    assert schema["properties"]["block_id"]["pattern"] == "^[a-z][a-z0-9_]*$"
    assert schema["additionalProperties"] is False

def test_realization_chip_must_exist(tmp_path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm3", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    blk = tmp_path / "b.yaml"; blk.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "b", "display_name": "B",
        "kconfig": "ALP_SDK_BLOCK_B",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": [{"id": "r", "physical_form": "discrete", "visibility": "public",
                          "parts": [{"chip": "does_not_exist", "maps": {"A": "LED"}}]}],
    }))
    failures = vm._check_block_realizations([blk], chip_files=[])
    assert failures and any("does_not_exist" in m for _, msgs in failures for m in msgs)

def test_realization_maps_must_be_in_interface(tmp_path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm4", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    blk = tmp_path / "c.yaml"; blk.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "c", "display_name": "C",
        "kconfig": "ALP_SDK_BLOCK_C",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": [{"id": "r", "physical_form": "discrete", "visibility": "public",
                          "parts": [{"chip": "x", "maps": {"A": "NOT_IN_INTERFACE"}}]}],
    }))
    failures = vm._check_block_realizations([blk], chip_files=[])
    assert failures and any("NOT_IN_INTERFACE" in m for _, msgs in failures for m in msgs)

def test_validate_metadata_passes_on_real_tree():
    # The full validator must stay green with the new chip pass wired in.
    r = subprocess.run([sys.executable, "scripts/validate_metadata.py"],
                       cwd=REPO, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "metadata/chips/" in r.stdout  # chips are now being checked

def test_reference_blocks_present_and_valid():
    for name in ("button_led", "pdm_mic"):
        assert (REPO / f"metadata/blocks/{name}.yaml").is_file()
    r = subprocess.run([sys.executable, "scripts/validate_metadata.py"], cwd=REPO,
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "metadata/blocks/button_led.yaml" in r.stdout

def test_reference_chips_have_physical():
    import yaml
    for name in ("icm42670", "tas2563", "ina236"):
        d = yaml.safe_load((REPO / f"metadata/chips/{name}.yaml").read_text())
        assert d.get("signals"), f"{name} needs signals[]"
        assert d.get("physical"), f"{name} needs physical:"
        assert d["physical"]["visibility"] in ("public", "internal")

def test_reconciled_chips_exist_with_physical():
    import yaml
    for name in ("bmi323", "bmp581", "cam_mux_pi3wvr626", "tcal9538"):
        p = REPO / f"metadata/chips/{name}.yaml"
        assert p.is_file(), f"{name} manifest missing"
        d = yaml.safe_load(p.read_text())
        assert d["chip_id"] == name
        assert d.get("physical")

def test_aen801_reference_set_is_bom_complete():
    import yaml
    evk = yaml.safe_load((REPO / "metadata/boards/e1m-evk.yaml").read_text())
    populated = [k for k, v in evk["populated"].items() if v is True]
    unresolved = []
    for slug in populated:
        chip = REPO / f"metadata/chips/{slug}.yaml"
        block = REPO / f"metadata/blocks/{slug}.yaml"
        if chip.is_file():
            d = yaml.safe_load(chip.read_text())
            if not d.get("physical"):
                unresolved.append(f"{slug}: chip has no physical:")
        elif block.is_file():
            d = yaml.safe_load(block.read_text())
            if not d.get("realizations"):
                unresolved.append(f"{slug}: block has no realizations")
        else:
            unresolved.append(f"{slug}: no chip or block manifest")
    assert not unresolved, "\n".join(unresolved)


# --- issue #720: topology.board <-> generated Zephyr board tree cross-check ---

def _load_vm(name):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    return vm

def test_board_bare_when_tree_is_qualified_rejected(tmp_path):
    """A board whose generated tree needs a qualifier but is spelled bare is
    the #720 bug -- west build -b cannot resolve it."""
    vm = _load_vm("vm_bt1"); import yaml
    p = tmp_path / "E1M-AEN801.yaml"
    p.write_text(yaml.safe_dump({"topology": {
        "m55_he": {"board": "alp_e1m_aen801_m55_he"}}}))  # bare, tree is qualified
    failures = vm._check_board_targets([p])
    assert failures and any("fully-qualified" in m for _, msgs in failures for m in msgs)

def test_board_qualified_without_tree_rejected(tmp_path):
    """Qualifying a board before its tree is generated (V2N102-style drift)
    must fail: the string points at a board Zephyr cannot find."""
    vm = _load_vm("vm_bt2"); import yaml
    p = tmp_path / "E1M-V2N102.yaml"
    p.write_text(yaml.safe_dump({"topology": {
        "m33_sm": {"board": "alp_e1m_v2n102_m33_sm/r9a09g056n48gbg/cm33"}}}))
    failures = vm._check_board_targets([p])
    assert failures and any("no generated board tree" in m for _, msgs in failures for m in msgs)

def test_board_qualified_matching_tree_accepted(tmp_path):
    """The correct qualified identifier matching the generated tree passes."""
    vm = _load_vm("vm_bt3"); import yaml
    p = tmp_path / "E1M-AEN801.yaml"
    p.write_text(yaml.safe_dump({"topology": {
        "m55_he": {"board": "alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he"},
        "a32_cluster": {"machine": "e1m-aen801-a32"}}}))  # yocto slice: no board, skipped
    assert vm._check_board_targets([p]) == []
