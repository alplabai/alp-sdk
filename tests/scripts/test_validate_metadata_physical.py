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

def test_validate_metadata_passes_on_real_tree():
    # The full validator must stay green with the new chip pass wired in.
    r = subprocess.run([sys.executable, "scripts/validate_metadata.py"],
                       cwd=REPO, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "metadata/chips/" in r.stdout  # chips are now being checked
