import json, shutil, subprocess, sys
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

def test_physical_string_does_not_crash_the_gate(tmp_path):
    """`physical:` is schema-typed as an object, but `if not phys: continue`
    does not protect a non-empty scalar (e.g. a bare string, which is
    truthy) -- that used to reach `phys.get("pins", [])` and raise
    `AttributeError`, aborting the whole gate mid-run instead of leaving the
    schema FAIL line (which already flags the type mismatch) to explain the
    real problem."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm_phys_str", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "w.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "w", "display_name": "W", "vendor": "v",
        "mpn_population": ["W"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "SDA", "type": "bidir"}],
        "physical": "not-an-object",
    }))
    failures = vm._check_chip_physical([p])  # must not raise
    assert failures == []

def test_non_object_pin_entry_does_not_crash_the_gate(tmp_path):
    """`physical.pins[]` entries are schema-typed objects, but a non-object
    entry (e.g. a bare string) used to reach `pin.get("signal")` and raise
    `AttributeError` here."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm_pin_str", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "v.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "v", "display_name": "V", "vendor": "v",
        "mpn_population": ["V"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "SDA", "type": "bidir"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public",
                     "pins": ["not-an-object"]},
    }))
    failures = vm._check_chip_physical([p])  # must not raise
    assert failures == []

def test_non_object_passive_entry_does_not_crash_the_gate(tmp_path):
    """`physical.passives[]` entries are schema-typed objects, but a
    non-object entry (e.g. a bare string) used to reach `passive.get("net")`
    and raise `AttributeError` here."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm_passive_str", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "u.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "u", "display_name": "U", "vendor": "v",
        "mpn_population": ["U"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "SDA", "type": "bidir"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public", "provenance": "web_provisional",
                     "pins": [{"pad": "1", "signal": "SDA"}],
                     "passives": ["not-an-object"]},
    }))
    failures = vm._check_chip_physical([p])  # must not raise
    assert failures == []

def test_non_list_pins_and_signals_do_not_crash_the_gate(tmp_path):
    """`physical.pins[]` and top-level `signals[]` are themselves
    schema-typed as arrays, but a malformed chip manifest can carry a
    non-list scalar there (e.g. the bare int `5`, which is truthy) --
    iterating the unfiltered value used to raise `TypeError: 'int' object
    is not iterable`, aborting the whole gate mid-run instead of leaving
    the schema FAIL line (which already flags the type mismatch) to
    explain the real problem."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm_pins_nonlist", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "t.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "t", "display_name": "T", "vendor": "v",
        "mpn_population": ["T"], "datasheet": {}, "bus": "i2c",
        "signals": 5,
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public", "pins": 5, "passives": 5},
    }))
    failures = vm._check_chip_physical([p])  # must not raise
    assert failures == []

def test_non_string_signal_name_does_not_crash_the_gate(tmp_path):
    """`signals[].name` is schema-typed as a string, but a malformed
    manifest can carry a dict/list there -- the unfiltered `sig_names`
    set comprehension used to raise `TypeError: unhashable type: 'dict'`
    building the set."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm_signame_dict", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "sn.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "sn", "display_name": "SN", "vendor": "v",
        "mpn_population": ["SN"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": {"nested": "dict"}}, {"name": "SDA", "type": "bidir"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public",
                     "pins": [{"pad": "1", "signal": "SDA"}]},
    }))
    failures = vm._check_chip_physical([p])  # must not raise
    assert failures == []

def test_non_string_pin_signal_does_not_crash_the_gate(tmp_path):
    """`physical.pins[].signal` is schema-typed as a string, but a
    malformed manifest can carry a dict/list there -- the unfiltered
    `sig not in sig_names` membership test used to raise `TypeError:
    unhashable type: 'dict'`. A non-string `signal` is reported as
    unresolved, not skipped."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm_pinsig_dict", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "ps.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "ps", "display_name": "PS", "vendor": "v",
        "mpn_population": ["PS"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "SDA", "type": "bidir"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public",
                     "pins": [{"pad": "1", "signal": {"nested": "dict"}}]},
    }))
    failures = vm._check_chip_physical([p])  # must not raise
    assert failures  # non-string signal is reported as unresolved

def test_non_string_pin_pad_does_not_crash_the_gate(tmp_path):
    """`physical.pins[].pad` is schema-typed as a string, but a malformed
    manifest can carry a dict/list there -- the unfiltered `pad in
    seen_pads` / `seen_pads[pad] = True` used to raise `TypeError:
    unhashable type: 'dict'`."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm_pinpad_dict", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "pp.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "pp", "display_name": "PP", "vendor": "v",
        "mpn_population": ["PP"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "SDA", "type": "bidir"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public",
                     "pins": [{"pad": {"nested": "dict"}, "signal": "SDA"}]},
    }))
    failures = vm._check_chip_physical([p])  # must not raise
    assert failures == []

def test_non_string_passive_net_does_not_crash_the_gate(tmp_path):
    """`physical.passives[].net` is schema-typed as a string, but a
    malformed manifest can carry a dict/list there -- the unfiltered
    `net not in sig_names` membership test used to raise `TypeError:
    unhashable type: 'dict'`."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm_passivenet_dict", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "pn.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "pn", "display_name": "PN", "vendor": "v",
        "mpn_population": ["PN"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "SDA", "type": "bidir"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public", "provenance": "web_provisional",
                     "pins": [{"pad": "1", "signal": "SDA"}],
                     "passives": [{"role": "pullup", "value": "4k7",
                                   "net": {"nested": "dict"}, "refdes_prefix": "R"}]},
    }))
    failures = vm._check_chip_physical([p])  # must not raise
    assert failures  # non-string net is reported as unresolved

def test_null_pad_duplicate_still_reported(tmp_path):
    """A YAML `null` `pad` is hashable (unlike a dict/list) and safe as a
    `seen_pads` key -- an overbroad `isinstance(pad, str)` gate (rather
    than one scoped to the actual unhashable-type hazard) would silently
    drop the `used more than once` diagnostic for a duplicated `null` pad
    instead of reporting it (the pre-hardening behaviour)."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm_pinpad_null", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "pnull.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "pnull", "display_name": "PNULL", "vendor": "v",
        "mpn_population": ["PNULL"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "VDD", "type": "power"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public",
                     "pins": [{"pad": None, "signal": "VDD"}, {"pad": None, "signal": "GND"}]},
    }))
    failures = vm._check_chip_physical([p])  # must not raise
    assert failures and any("pad 'None' used more than once" in m
                             for _, msgs in failures for m in msgs)

def test_null_pin_signal_reports_diagnostic_not_silently_dropped(tmp_path):
    """Same reasoning as the `pad` case above, for `signal`: a YAML
    `null` is hashable and safe for the `sig_names`/`_POWER_NETS`
    membership tests, and must still surface the `not in signals[] or
    power nets` diagnostic."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm_pinsig_null", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    p = tmp_path / "signull.yaml"; p.write_text(yaml.safe_dump({
        "schema_version": 1, "chip_id": "signull", "display_name": "SIGNULL", "vendor": "v",
        "mpn_population": ["SIGNULL"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "SDA", "type": "bidir"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public",
                     "pins": [{"pad": "1", "signal": None}]},
    }))
    failures = vm._check_chip_physical([p])  # must not raise
    assert failures and any("signal 'None' not in signals[] or power nets" in m
                             for _, msgs in failures for m in msgs)

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

def test_non_object_realization_entry_does_not_crash_the_gate(tmp_path):
    """`realizations[]` entries are schema-typed objects, but a non-object
    entry (e.g. a bare string) used to reach `r.get("parts", [])` and raise
    `AttributeError` here, aborting the whole gate mid-run instead of
    leaving the schema FAIL line (which already flags the type mismatch) to
    explain the real problem."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm6", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    blk = tmp_path / "d.yaml"; blk.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "d", "display_name": "D",
        "kconfig": "ALP_SDK_BLOCK_D",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": ["not-an-object"],
    }))
    failures = vm._check_block_realizations([blk], chip_files=[])  # must not raise
    assert failures == []

def test_non_object_part_entry_does_not_crash_the_gate(tmp_path):
    """`realizations[].parts[]` entries are schema-typed objects, but a
    non-object entry used to reach `part.get("chip")`/`part.get("maps")` and
    raise `AttributeError` here -- the sixth site a previous audit round
    named `r.get("parts", [])` but not the nested `part.get(...)` calls it
    feeds."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm7", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    blk = tmp_path / "e.yaml"; blk.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "e", "display_name": "E",
        "kconfig": "ALP_SDK_BLOCK_E",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": [{"id": "r", "physical_form": "discrete", "visibility": "public",
                          "parts": ["not-an-object"]}],
    }))
    failures = vm._check_block_realizations([blk], chip_files=[])  # must not raise
    assert failures == []

def test_non_object_realization_passive_entry_does_not_crash_the_gate(tmp_path):
    """`realizations[].passives[]` entries are schema-typed objects, but a
    non-object entry used to reach `passive.get("net")` and raise
    `AttributeError` here."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm8", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    blk = tmp_path / "f.yaml"; blk.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "f", "display_name": "F",
        "kconfig": "ALP_SDK_BLOCK_F",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": [{"id": "r", "physical_form": "discrete", "visibility": "public",
                          "parts": [], "passives": ["not-an-object"]}],
    }))
    failures = vm._check_block_realizations([blk], chip_files=[])  # must not raise
    assert failures == []

def test_non_object_maps_does_not_crash_the_gate(tmp_path):
    """`realizations[].parts[].maps` is schema-typed as an object, but a
    non-object value (e.g. a bare string) used to reach `.items()` and raise
    `AttributeError` here."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm9", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    blk = tmp_path / "g.yaml"; blk.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "g", "display_name": "G",
        "kconfig": "ALP_SDK_BLOCK_G",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": [{"id": "r", "physical_form": "discrete", "visibility": "public",
                          "parts": [{"chip": "x", "maps": "not-an-object"}]}],
    }))
    failures = vm._check_block_realizations([blk], chip_files=[])  # must not raise
    # `chip: x` has no manifest either (chip_files=[]), so this still
    # reports a FAIL -- the point is that it reports one instead of raising.
    assert failures

def test_non_list_realizations_and_parts_do_not_crash_the_gate(tmp_path):
    """`realizations[]` and `realizations[].parts[]` are themselves
    schema-typed as arrays, but a malformed block manifest can carry a
    non-list scalar there (e.g. the bare int `5`, which is truthy) --
    iterating the unfiltered value used to raise `TypeError: 'int' object
    is not iterable`, aborting the whole gate mid-run instead of leaving
    the schema FAIL line (which already flags the type mismatch) to
    explain the real problem."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm10", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    blk = tmp_path / "h.yaml"; blk.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "h", "display_name": "H",
        "kconfig": "ALP_SDK_BLOCK_H",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": 5,
    }))
    failures = vm._check_block_realizations([blk], chip_files=[])  # must not raise
    assert failures == []

    blk2 = tmp_path / "i.yaml"; blk2.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "i", "display_name": "I",
        "kconfig": "ALP_SDK_BLOCK_I",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": [{"id": "r", "physical_form": "discrete", "visibility": "public",
                          "parts": 5, "passives": 5}],
    }))
    failures2 = vm._check_block_realizations([blk2], chip_files=[])  # must not raise
    assert failures2 == []

def test_non_string_interface_signal_does_not_crash_the_gate(tmp_path):
    """`interface[].signal` is schema-typed as a string, but a malformed
    manifest can carry a dict/list there -- the unfiltered `iface` set
    comprehension used to raise `TypeError: unhashable type: 'dict'`
    building the set."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm11", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    blk = tmp_path / "j.yaml"; blk.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "j", "display_name": "J",
        "kconfig": "ALP_SDK_BLOCK_J",
        "interface": [{"signal": {"nested": "dict"}, "dir": "output"},
                      {"signal": "LED", "dir": "output"}],
        "realizations": [{"id": "r", "physical_form": "discrete", "visibility": "public",
                          "parts": [], "passives": []}],
    }))
    failures = vm._check_block_realizations([blk], chip_files=[])  # must not raise
    assert failures == []

def test_non_string_realization_part_chip_does_not_crash_the_gate(tmp_path):
    """`realizations[].parts[].chip` is schema-typed as a string, but a
    malformed manifest can carry a dict/list there -- the unfiltered
    `part.get("chip") not in chip_ids` membership test used to raise
    `TypeError: unhashable type: 'dict'`."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm12", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    blk = tmp_path / "k.yaml"; blk.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "k", "display_name": "K",
        "kconfig": "ALP_SDK_BLOCK_K",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": [{"id": "r", "physical_form": "discrete", "visibility": "public",
                          "parts": [{"chip": {"nested": "dict"}, "maps": {}}]}],
    }))
    failures = vm._check_block_realizations([blk], chip_files=[])  # must not raise
    assert failures  # non-string chip is reported as unresolved

def test_non_string_realization_maps_target_does_not_crash_the_gate(tmp_path):
    """`realizations[].parts[].maps` values are schema-typed as strings,
    but a malformed manifest can carry a dict/list value there -- the
    unfiltered `sig not in iface` membership test used to raise
    `TypeError: unhashable type: 'dict'`."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm13", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    blk = tmp_path / "l.yaml"; blk.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "l", "display_name": "L",
        "kconfig": "ALP_SDK_BLOCK_L",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": [{"id": "r", "physical_form": "discrete", "visibility": "public",
                          "parts": [{"chip": "x", "maps": {"A": {"nested": "dict"}}}]}],
    }))
    failures = vm._check_block_realizations([blk], chip_files=[])  # must not raise
    assert failures  # non-string maps target is reported as unresolved (plus the missing chip)

def test_non_string_realization_passive_net_does_not_crash_the_gate(tmp_path):
    """`realizations[].passives[].net` is schema-typed as a string, but a
    malformed manifest can carry a dict/list there -- the unfiltered
    `net not in iface` membership test used to raise `TypeError:
    unhashable type: 'dict'`."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm14", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    import yaml
    blk = tmp_path / "m.yaml"; blk.write_text(yaml.safe_dump({
        "schema_version": 1, "block_id": "m", "display_name": "M",
        "kconfig": "ALP_SDK_BLOCK_M",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": [{"id": "r", "physical_form": "discrete", "visibility": "public",
                          "parts": [],
                          "passives": [{"role": "pullup", "value": "4k7",
                                        "net": {"nested": "dict"}, "refdes_prefix": "R"}]}],
    }))
    failures = vm._check_block_realizations([blk], chip_files=[])  # must not raise
    assert failures  # non-string net is reported as unresolved

def test_validate_metadata_passes_on_real_tree():
    # The full validator must stay green with the new chip pass wired in.
    r = subprocess.run([sys.executable, "scripts/validate_metadata.py"],
                       cwd=REPO, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "metadata/chips/" in r.stdout  # chips are now being checked

def test_emit_pending_warnings_peripherals_unverified(capsys):
    # #936: a listed key present in `peripherals` -> one WARN, no typo WARN.
    import validate_metadata as vm
    vm._emit_pending_warnings(Path("x.json"),
                               {"peripherals": {"pdm": 4}, "peripherals_unverified": ["pdm"]})
    out = capsys.readouterr().out
    assert "peripherals_unverified -> ['pdm']" in out
    assert "likely a typo" not in out

def test_emit_pending_warnings_peripherals_unverified_typo(capsys):
    # A listed key absent from `peripherals` -> the typo WARN fires too.
    import validate_metadata as vm
    vm._emit_pending_warnings(Path("x.json"),
                               {"peripherals": {"pdm": 4}, "peripherals_unverified": ["pmd_lp"]})
    out = capsys.readouterr().out
    assert "references key(s) not present in peripherals: ['pmd_lp'] -- likely a typo" in out

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

def test_non_object_topology_does_not_crash_the_gate(tmp_path):
    """`topology:` is schema-typed as an object, but `doc.get("topology") or
    {}` does not protect a non-empty scalar (e.g. a bare string, which is
    truthy) -- that used to reach `topology.items()` and raise
    `AttributeError`, aborting the whole gate mid-run instead of leaving the
    schema FAIL line (which already flags the type mismatch) to explain the
    real problem."""
    vm = _load_vm("vm_bt4"); import yaml
    p = tmp_path / "E1M-AEN801.yaml"
    p.write_text(yaml.safe_dump({"topology": "not-an-object"}))
    failures = vm._check_board_targets([p])  # must not raise
    assert failures == []

def test_non_object_topology_produces_no_output_line(tmp_path, capsys):
    """A malformed `topology:` scalar must SKIP this check entirely, not
    normalise via `_as_dict` and fall through to `checked == 0` -- that
    used to print a green `OK ... (board targets: 0 Zephyr slice(s)
    resolve)` line for a file the schema pass FAILs in the very same run.
    A file this check has nothing to say about must stay silent (restores
    `dcda807d`'s behaviour)."""
    vm = _load_vm("vm_bt4b"); import yaml
    p = tmp_path / "E1M-AEN801.yaml"
    p.write_text(yaml.safe_dump({"topology": "not-an-object"}))
    failures = vm._check_board_targets([p])
    assert failures == []
    out = capsys.readouterr().out
    assert out == "", f"expected no output for a skipped file, got: {out!r}"


# --- issue #1004: two soft-fail sites migrated onto resolve_soc_path() ---

def test_silicon_capability_restrictions_malformed_silicon_ref(tmp_path, monkeypatch):
    """Pins the soft-fail message #1004's migration promised to keep: a
    malformed `silicon:` ref still reports this diagnostic string, not a
    crash or a silently-skipped check."""
    vm = _load_vm("vm_scr1"); import yaml
    # rel = path.relative_to(REPO) needs the fixture under REPO; SOCS (the
    # metadata/socs root resolve_soc_path() resolves against) is unaffected
    # -- it's a separate module constant already bound to the real tree.
    monkeypatch.setattr(vm, "REPO", tmp_path)
    p = tmp_path / "E1M-TEST.yaml"
    p.write_text(yaml.safe_dump({
        "silicon": "acme:widget",
        "silicon_capabilities": {"unpopulated": ["camera"]},
    }))
    failures = vm._check_silicon_capability_restrictions([p])
    assert failures and any(
        "silicon ref `acme:widget` does not resolve to a metadata/socs/ spec" in m
        for _, msgs in failures for m in msgs)

def test_silicon_kconfig_null_known_silicon_rejected(tmp_path, monkeypatch):
    """A JSON-null `knownSilicon` entry must not crash resolve_soc_path() --
    it's reported as this soft-fail message, pinning the shape #1004's
    migration promised to keep."""
    vm = _load_vm("vm_sk1"); import json
    monkeypatch.setattr(vm, "REPO", tmp_path)
    registry = tmp_path / "silicon-kconfig.json"
    registry.write_text(json.dumps({"socSymbolPrefix": "SOC_ALP_", "knownSilicon": [None]}))
    monkeypatch.setattr(vm, "SILICON_KCONFIG_REGISTRY", registry)
    monkeypatch.setattr(vm, "SILICON_KCONFIG_SCHEMA", tmp_path / "does-not-exist.json")
    failures = vm._check_silicon_kconfig()
    assert failures and any(
        "not a <vendor>:<family>:<part> ref" in m for _, msgs in failures for m in msgs)


# --- second-round crash guards found on top of #1004's fix ---

def test_silicon_kconfig_non_object_top_level_does_not_crash_the_gate(tmp_path, monkeypatch):
    """The registry's top level is schema-typed as an object, but
    `data.get("knownSilicon", [])` ran unconditionally -- before the schema
    pass even had a chance to flag the shape, and regardless of whether it
    would have -- so a registry parsing to a bare JSON list used to raise
    `AttributeError: 'list' object has no attribute 'get'` here, aborting
    the whole gate instead of reporting a clean FAIL."""
    vm = _load_vm("vm_sk2"); import json
    monkeypatch.setattr(vm, "REPO", tmp_path)
    registry = tmp_path / "silicon-kconfig.json"
    registry.write_text(json.dumps([]))
    monkeypatch.setattr(vm, "SILICON_KCONFIG_REGISTRY", registry)
    monkeypatch.setattr(vm, "SILICON_KCONFIG_SCHEMA", tmp_path / "does-not-exist.json")
    failures = vm._check_silicon_kconfig()  # must not raise
    assert failures


def test_silicon_kconfig_non_list_known_silicon_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`knownSilicon` is itself schema-typed as an array, but a malformed
    registry can carry a non-list scalar there (e.g. the bare int `5`,
    which is truthy) -- iterating the unfiltered value used to raise
    `TypeError: 'int' object is not iterable`."""
    vm = _load_vm("vm_sk3"); import json
    monkeypatch.setattr(vm, "REPO", tmp_path)
    registry = tmp_path / "silicon-kconfig.json"
    registry.write_text(json.dumps({"socSymbolPrefix": "SOC_ALP_", "knownSilicon": 5}))
    monkeypatch.setattr(vm, "SILICON_KCONFIG_REGISTRY", registry)
    monkeypatch.setattr(vm, "SILICON_KCONFIG_SCHEMA", tmp_path / "does-not-exist.json")
    failures = vm._check_silicon_kconfig()  # must not raise
    assert not failures  # nothing to check; the schema pass (when present) flags the shape


def test_silicon_kconfig_non_string_known_silicon_entry_does_not_crash_the_gate(tmp_path, monkeypatch):
    """`knownSilicon[]` entries are schema-typed as strings, but a
    malformed registry can carry a non-string entry there (e.g. a nested
    mapping) -- `resolve_soc_path(ref, ...)` used to reach
    `silicon.split(":")` on a truthy non-string value and raise
    `AttributeError: 'dict' object has no attribute 'split'`."""
    vm = _load_vm("vm_sk4"); import json
    monkeypatch.setattr(vm, "REPO", tmp_path)
    registry = tmp_path / "silicon-kconfig.json"
    registry.write_text(json.dumps(
        {"socSymbolPrefix": "SOC_ALP_", "knownSilicon": [{"a": "b"}]}))
    monkeypatch.setattr(vm, "SILICON_KCONFIG_REGISTRY", registry)
    monkeypatch.setattr(vm, "SILICON_KCONFIG_SCHEMA", tmp_path / "does-not-exist.json")
    failures = vm._check_silicon_kconfig()  # must not raise
    assert not failures  # the malformed entry is skipped; nothing left to flag


def test_peripheral_kconfig_non_object_top_level_does_not_crash_the_gate(tmp_path, monkeypatch):
    """Same class of bug as `_check_silicon_kconfig`, one function over:
    `data.get("peripherals", {})` is reached only when `msgs` is empty, and
    stayed safe only BY ACCIDENT (when the schema exists, a non-object top
    level lands in `msgs` first) -- with the schema absent, a registry
    parsing to a bare JSON list used to reach `len(data.get(...))` and raise
    `AttributeError: 'list' object has no attribute 'get'`."""
    vm = _load_vm("vm_pk1"); import json
    monkeypatch.setattr(vm, "REPO", tmp_path)
    registry = tmp_path / "peripheral-kconfig.json"
    registry.write_text(json.dumps([]))
    monkeypatch.setattr(vm, "PERIPHERAL_KCONFIG_REGISTRY", registry)
    monkeypatch.setattr(vm, "PERIPHERAL_KCONFIG_SCHEMA", tmp_path / "does-not-exist.json")
    failures = vm._check_peripheral_kconfig()  # must not raise
    assert failures


def test_peripheral_kconfig_malformed_on_disk_registry_fails_cleanly_via_subprocess(tmp_path):
    """The REAL crash this class of bug is about does not happen inside
    `_check_peripheral_kconfig()` at all -- it happens at IMPORT time.
    `alp_orchestrate/slugs.py` calls `alp_registries.peripheral_kconfig()`
    at MODULE scope, and every import of `validate_metadata.py` reaches
    that transitively: `from alp_orchestrate.sdk_compat import ...` ->
    `alp_orchestrate/__init__.py` -> `.loader` -> `alp_project` ->
    `alp_project_emit` -> `alp_registries.peripheral_kconfig()` (module
    scope there too). That runs, against the REAL on-disk
    `metadata/registries/peripheral-kconfig.json`, well before `main()`
    -- and `_check_peripheral_kconfig()`'s own top-level guard above --
    ever run.

    An in-process monkeypatch (like the test above) can never reach that
    crash: it repoints `vm.PERIPHERAL_KCONFIG_REGISTRY` on an
    ALREADY-IMPORTED `validate_metadata` module, whose import already
    succeeded once, in this process, against the REAL on-disk registry --
    Python does not re-run module-scope import code on a second `import`
    of an already-cached module. Only a fresh subprocess, with a
    malformed registry actually on disk where the import chain reads it,
    reproduces the real failure. `scripts/` modules resolve their own
    `REPO` from `__file__`, so this copies the whole tree (its modules
    import each other by absolute path) into a scratch root and drops a
    malformed registry at the real relative location -- verified against
    the pre-fix `alp_registries.peripheral_kconfig()` to raise a raw
    `AttributeError` there; this asserts the fixed, clean `ValueError`
    shape instead.
    """
    tmp_scripts = tmp_path / "scripts"
    shutil.copytree(REPO / "scripts", tmp_scripts)
    registry = tmp_path / "metadata" / "registries" / "peripheral-kconfig.json"
    registry.parent.mkdir(parents=True)
    registry.write_text(json.dumps(
        {"schemaVersion": "peripheral-kconfig-v1", "peripherals": "not-an-object"}))
    r = subprocess.run(
        [sys.executable, str(tmp_scripts / "validate_metadata.py")],
        cwd=tmp_path, capture_output=True, text=True)
    out = r.stdout + r.stderr
    assert r.returncode != 0, out
    assert "peripheral-kconfig.json" in out
    assert "peripherals" in out
    # The whole point of `alp_registries.peripheral_kconfig()`'s guard: a
    # clean, named `ValueError`, not a raw `AttributeError`/`KeyError`
    # traceback several import-frames from anything this test wrote.
    assert "ValueError" in out
    assert "AttributeError" not in out
    assert "KeyError" not in out


def test_silicon_capability_restrictions_malformed_soc_json_does_not_crash_the_gate(
    tmp_path, monkeypatch
):
    """A BARE `json.loads()` on the referenced SoC file (no try/except) used
    to let a syntactically invalid SoC JSON raise `JSONDecodeError` straight
    out of the gate -- the schema pass over `soc_files` reports that
    failure separately; this cross-check must degrade to a diagnostic
    message instead."""
    vm = _load_vm("vm_scr2"); import yaml
    monkeypatch.setattr(vm, "REPO", tmp_path)
    soc = tmp_path / "soc.json"
    soc.write_text("{ not valid json")
    monkeypatch.setattr(vm, "resolve_soc_path", lambda ref, root: soc)
    p = tmp_path / "E1M-TEST.yaml"
    p.write_text(yaml.safe_dump({
        "silicon": "acme:widget",
        "silicon_capabilities": {"unpopulated": ["camera"]},
    }))
    failures = vm._check_silicon_capability_restrictions([p])  # must not raise
    assert failures and any("fails to parse" in m for _, msgs in failures for m in msgs)


def test_silicon_capability_restrictions_non_dict_capabilities_does_not_crash_the_gate(
    tmp_path, monkeypatch
):
    """`capabilities:` is schema-typed as an object, but a malformed SoC doc
    can carry a non-dict scalar there (e.g. the bare int `5`) --
    `soc_caps.get(name)` / `soc_caps.items()` used to raise `AttributeError`
    on that."""
    vm = _load_vm("vm_scr3"); import yaml, json
    monkeypatch.setattr(vm, "REPO", tmp_path)
    soc = tmp_path / "soc.json"
    soc.write_text(json.dumps({"capabilities": 5}))
    monkeypatch.setattr(vm, "resolve_soc_path", lambda ref, root: soc)
    p = tmp_path / "E1M-TEST.yaml"
    p.write_text(yaml.safe_dump({
        "silicon": "acme:widget",
        "silicon_capabilities": {"unpopulated": ["camera"]},
    }))
    failures = vm._check_silicon_capability_restrictions([p])  # must not raise
    assert failures  # `camera` is not offered by the (normalised-to-empty) capability set


def test_silicon_capability_restrictions_non_string_unpopulated_entry_does_not_crash_the_gate(
    tmp_path, monkeypatch
):
    """`unpopulated[]` entries are schema-typed as strings, but a malformed
    preset can carry a non-string entry there (e.g. a nested mapping) --
    `soc_caps.get(name)` / `name in som_caps` used to raise `TypeError:
    unhashable type: 'dict'` on that, since a dict can't be a dict key or a
    set/dict membership probe."""
    vm = _load_vm("vm_scr4"); import yaml, json
    monkeypatch.setattr(vm, "REPO", tmp_path)
    soc = tmp_path / "soc.json"
    soc.write_text(json.dumps({"capabilities": {"camera": True}}))
    monkeypatch.setattr(vm, "resolve_soc_path", lambda ref, root: soc)
    p = tmp_path / "E1M-TEST.yaml"
    p.write_text(yaml.safe_dump({
        "silicon": "acme:widget",
        "silicon_capabilities": {"unpopulated": [{"a": "b"}]},
    }))
    failures = vm._check_silicon_capability_restrictions([p])  # must not raise
    assert failures == []  # the malformed entry is skipped; nothing left to flag
