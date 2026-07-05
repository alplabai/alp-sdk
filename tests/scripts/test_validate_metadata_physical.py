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
