# Block Physical Data Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give alp-studio enough physical data (footprints, pinouts, passives, block realizations) to emit a carrier netlist + BOM for the AEN801 reference-carrier part set.

**Architecture:** Physical data attaches to the **chip** manifest (`metadata/chips/<part>.yaml`) via an optional `physical:` block, and to a **new** block manifest (`metadata/blocks/<name>.yaml`) via `realizations[]`. Two new JSON schemas (`chip-v1`, `block-v1`) plus semantic cross-checks in `scripts/validate_metadata.py` enforce integrity. A per-part `visibility:` marker carries the public/private split. No EDA engine — the netlist is a downstream render over this data + the studio allocator.

**Tech Stack:** YAML metadata, JSON Schema (draft 2020-12), Python 3.10+ (`jsonschema`, `pyyaml`), pytest.

## Global Constraints

- **Design spec:** `docs/superpowers/specs/2026-07-05-block-physical-layer-design.md` (authoritative).
- **Schema validator:** `jsonschema.Draft202012Validator`; every schema sets `"$schema": "https://json-schema.org/draft/2020-12/schema"`, `additionalProperties: false`, `schema_version` `const: 1`.
- **Filename == id:** a manifest's id field (`chip_id` / `block_id`) must equal its filename stem; ids match `^[a-z][a-z0-9_]*$`.
- **No invented HW values:** footprint pads / pin numbers come from the part's datasheet (each chip manifest already has a `datasheet:` field). Where a value touches SoM-internal design, mark `visibility: internal` and leave the detail-rich body for `alp-sdk-internal`. Never guess a pad number — omit the part from this slice and note it instead.
- **Classification:** `visibility:` is `public | internal`; default `internal` when unsure (public git history is forever). E1M connector pinout is public. SoM `on_module` parts never enter the carrier netlist.
- **Attribution:** headers/prose say "Alp Lab AB"; no AI/Claude attribution in commits or files.
- **Before push:** `py -3 scripts/validate_metadata.py` green + the running-local-ci gates.

## File Structure

- `metadata/schemas/chip-v1.schema.json` — NEW. Validates the 74 chip manifests; `physical:` optional.
- `metadata/schemas/block-v1.schema.json` — NEW. Validates block manifests.
- `metadata/blocks/` — NEW dir. `button_led.yaml`, `pdm_mic.yaml`.
- `metadata/chips/{icm42670,tas2563,ina236}.yaml` — MODIFY (add `signals:` + `physical:`).
- `metadata/chips/{bmi323,bmp581,cam_mux_pi3wvr626,tcal9538}.yaml` — NEW (driver exists, manifest missing).
- `scripts/validate_metadata.py` — MODIFY (wire chip + block schemas; add semantic checks).
- `tests/scripts/test_validate_metadata_physical.py` — NEW.

---

### Task 1: chip-v1 schema + wire chip validation

Retro-validates all 74 existing chip manifests with `physical:` optional, so nothing breaks and the schema is real before any `physical:` block exists.

**Files:**
- Create: `metadata/schemas/chip-v1.schema.json`
- Modify: `scripts/validate_metadata.py` (constants near line 31-42; `main()` near line 436)
- Test: `tests/scripts/test_validate_metadata_physical.py`

**Interfaces:**
- Produces: `CHIP_SCHEMA` path const; chip validation block in `main()` appending to the failure tally. Semantic-check function names for later tasks: `_check_chip_physical(chip_files) -> list`.

- [ ] **Step 1: Write the failing test**

```python
# tests/scripts/test_validate_metadata_physical.py
import json, subprocess, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

def test_chip_schema_exists_and_is_draft2020():
    schema = json.loads((REPO / "metadata/schemas/chip-v1.schema.json").read_text())
    assert schema["$schema"].endswith("2020-12/schema")
    assert schema["additionalProperties"] is False
    assert schema["properties"]["schema_version"]["const"] == 1

def test_validate_metadata_passes_on_real_tree():
    # The full validator must stay green with the new chip pass wired in.
    r = subprocess.run([sys.executable, "scripts/validate_metadata.py"],
                       cwd=REPO, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "metadata/chips/" in r.stdout  # chips are now being checked
```

- [ ] **Step 2: Run test to verify it fails**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -v`
Expected: FAIL — `chip-v1.schema.json` missing / chips not in output.

- [ ] **Step 3: Create the chip schema**

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://github.com/alplabai/alp-sdk/metadata/schemas/chip-v1.schema.json",
  "title": "alp-sdk chip manifest (v1)",
  "description": "Per-chip manifest under metadata/chips/<part>.yaml. Physical block optional; when present it feeds the carrier netlist + BOM (alp-studio#65).",
  "type": "object",
  "additionalProperties": false,
  "required": ["schema_version", "chip_id", "display_name", "vendor", "mpn_population", "datasheet", "bus"],
  "properties": {
    "schema_version": { "type": "integer", "const": 1 },
    "chip_id": { "type": "string", "pattern": "^[a-z][a-z0-9_]*$" },
    "display_name": { "type": "string" },
    "driver_status": { "type": "string" },
    "vendor": { "type": "string" },
    "mpn_population": { "type": "array", "items": { "type": "string" }, "minItems": 1 },
    "datasheet": { "type": "object" },
    "bus": { "type": "string" },
    "i2c": { "type": "object" },
    "spi": { "type": "object" },
    "uart": { "type": "object" },
    "signals": {
      "type": "array",
      "items": {
        "type": "object", "additionalProperties": false,
        "required": ["name", "type"],
        "properties": {
          "name": { "type": "string" },
          "type": { "type": "string", "enum": ["input", "output", "bidir", "power", "ground", "analog"] },
          "scope": { "type": "string" }
        }
      }
    },
    "physical": {
      "type": "object", "additionalProperties": false,
      "required": ["refdes_prefix", "package", "footprint", "pins", "visibility"],
      "properties": {
        "refdes_prefix": { "type": "string", "pattern": "^[A-Z]+$" },
        "package": { "type": "string" },
        "footprint": { "type": "string", "pattern": "^[a-z0-9_]+$" },
        "visibility": { "type": "string", "enum": ["public", "internal"] },
        "pins": {
          "type": "array", "minItems": 1,
          "items": {
            "type": "object", "additionalProperties": false,
            "required": ["pad", "signal"],
            "properties": { "pad": { "type": "string" }, "signal": { "type": "string" } }
          }
        },
        "passives": {
          "type": "array",
          "items": {
            "type": "object", "additionalProperties": false,
            "required": ["role", "value", "net", "refdes_prefix"],
            "properties": {
              "role": { "type": "string", "enum": ["decouple", "pullup", "pulldown", "series", "bulk", "level_shift"] },
              "value": { "type": "string" }, "net": { "type": "string" },
              "refdes_prefix": { "type": "string", "pattern": "^[A-Z]+$" }
            }
          }
        }
      }
    }
  }
}
```

> **Note:** the required list mirrors what all 74 manifests already carry (verify with a quick `grep -L mpn_population metadata/chips/*.yaml` — expect empty). If any manifest lacks a listed key, relax that key to optional rather than editing manifests in this task.

- [ ] **Step 4: Wire chip validation into `scripts/validate_metadata.py`**

Add near the other schema path constants (after line ~42):

```python
CHIP_SCHEMA = REPO / "metadata" / "schemas" / "chip-v1.schema.json"
CHIPS = REPO / "metadata" / "chips"
```

Add in `main()`, mirroring the board-preset block:

```python
    chip_failures: list = []
    chip_files: list = []
    if CHIP_SCHEMA.is_file():
        chip_schema = json.loads(CHIP_SCHEMA.read_text(encoding="utf-8"))
        chip_validator = jsonschema.Draft202012Validator(chip_schema)
        chip_files = sorted(CHIPS.glob("*.yaml"))
        if chip_files:
            print()
            chip_failures = _check_files(
                "YAML", chip_files, chip_validator,
                lambda p: yaml.safe_load(p.read_text(encoding="utf-8")),
                "chip_id",
            )
```

Add `chip_failures` to the final failure tally / exit-code sum exactly like `board_failures` is aggregated (find where `board_failures` is summed and add `chip_failures` beside it).

- [ ] **Step 5: Run tests to verify they pass**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -v && py -3 scripts/validate_metadata.py`
Expected: PASS; validator prints `OK metadata/chips/*.yaml` for all 74 and exits 0.

- [ ] **Step 6: Commit**

```bash
git add metadata/schemas/chip-v1.schema.json scripts/validate_metadata.py tests/scripts/test_validate_metadata_physical.py
git commit -m "feat(metadata): chip-v1 schema + validate 74 chip manifests"
```

---

### Task 2: chip `physical:` semantic checks

Schema shape is not enough: every `pins[].signal` must resolve to a declared `signals[]` name (or a power/ground net), and a footprint pad must appear at most once. Cross-field checks live in Python, mirroring `_check_library_semantics`.

**Files:**
- Modify: `scripts/validate_metadata.py`
- Test: `tests/scripts/test_validate_metadata_physical.py`

**Interfaces:**
- Consumes: `chip_files` list from Task 1.
- Produces: `_check_chip_physical(chip_files) -> list[tuple[Path, list[str]]]`.

- [ ] **Step 1: Write the failing test (fixtures)**

```python
def _write(tmp_path, name, doc):
    import yaml
    p = tmp_path / f"{name}.yaml"; p.write_text(yaml.safe_dump(doc)); return p

def test_pin_signal_must_resolve(tmp_path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    bad = _write(tmp_path, "x", {
        "schema_version": 1, "chip_id": "x", "display_name": "X", "vendor": "v",
        "mpn_population": ["X"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "SDA", "type": "bidir"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public",
                     "pins": [{"pad": "1", "signal": "SCL"}]},  # SCL not in signals
    })
    failures = vm._check_chip_physical([bad])
    assert failures and any("SCL" in m for _, msgs in failures for m in msgs)

def test_duplicate_pad_rejected(tmp_path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm2", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    bad = _write(tmp_path, "y", {
        "schema_version": 1, "chip_id": "y", "display_name": "Y", "vendor": "v",
        "mpn_population": ["Y"], "datasheet": {}, "bus": "i2c",
        "signals": [{"name": "VDD", "type": "power"}],
        "physical": {"refdes_prefix": "U", "package": "P", "footprint": "p",
                     "visibility": "public",
                     "pins": [{"pad": "1", "signal": "VDD"}, {"pad": "1", "signal": "GND"}]},
    })
    failures = vm._check_chip_physical([bad])
    assert failures and any("pad" in m.lower() for _, msgs in failures for m in msgs)
```

- [ ] **Step 2: Run to verify it fails**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -k "pin_signal or duplicate_pad" -v`
Expected: FAIL — `_check_chip_physical` not defined.

- [ ] **Step 3: Implement `_check_chip_physical`**

```python
# Power/ground nets are allowed as pin signals without a signals[] entry.
_POWER_NETS = {"VDD", "VDDIO", "VCC", "GND", "VSS", "AVDD", "DVDD"}

def _check_chip_physical(chip_files) -> list:
    failures: list = []
    for path in chip_files:
        rel = path.relative_to(REPO)
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        if not isinstance(doc, dict):
            continue  # parse/schema pass already reported it
        phys = doc.get("physical")
        if not phys:
            continue
        sig_names = {s["name"] for s in doc.get("signals", []) if isinstance(s, dict) and "name" in s}
        msgs: list = []
        seen_pads: dict = {}
        for pin in phys.get("pins", []):
            sig = pin.get("signal"); pad = pin.get("pad")
            if sig not in sig_names and sig not in _POWER_NETS:
                msgs.append(f"physical.pins pad {pad}: signal '{sig}' not in signals[] or power nets")
            if pad in seen_pads:
                msgs.append(f"physical.pins: pad '{pad}' used more than once")
            seen_pads[pad] = True
        if msgs:
            failures.append((rel, msgs))
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
    return failures
```

Call it in `main()` right after the chip schema block and fold its result into the tally:

```python
    if chip_files:
        chip_failures += _check_chip_physical(chip_files)
```

- [ ] **Step 4: Run to verify pass**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -v && py -3 scripts/validate_metadata.py`
Expected: PASS; real tree still exits 0 (no chip has `physical:` yet, so the check is a no-op there).

- [ ] **Step 5: Commit**

```bash
git add scripts/validate_metadata.py tests/scripts/test_validate_metadata_physical.py
git commit -m "feat(metadata): chip physical pin/pad semantic checks"
```

---

### Task 3: block-v1 schema + `metadata/blocks/` validation wiring

**Files:**
- Create: `metadata/schemas/block-v1.schema.json`
- Create: `metadata/blocks/.gitkeep` (dir must exist for the glob)
- Modify: `scripts/validate_metadata.py`
- Test: `tests/scripts/test_validate_metadata_physical.py`

**Interfaces:**
- Produces: `BLOCK_SCHEMA`, `BLOCKS` consts; `block_files` list; `_check_block_realizations(block_files, chip_files) -> list`.

- [ ] **Step 1: Write the failing test**

```python
def test_block_schema_exists():
    schema = json.loads((REPO / "metadata/schemas/block-v1.schema.json").read_text())
    assert schema["properties"]["block_id"]["pattern"] == "^[a-z][a-z0-9_]*$"
    assert schema["additionalProperties"] is False
```

- [ ] **Step 2: Run to verify it fails**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -k block_schema -v`
Expected: FAIL — file missing.

- [ ] **Step 3: Create the block schema**

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://github.com/alplabai/alp-sdk/metadata/schemas/block-v1.schema.json",
  "title": "alp-sdk block manifest (v1)",
  "description": "Per-block manifest under metadata/blocks/<name>.yaml. Declares the block's abstract interface and one or more physical realizations (discrete chips or a pre-routed module) for the carrier netlist (alp-studio#65).",
  "type": "object",
  "additionalProperties": false,
  "required": ["schema_version", "block_id", "display_name", "kconfig", "interface", "realizations"],
  "properties": {
    "schema_version": { "type": "integer", "const": 1 },
    "block_id": { "type": "string", "pattern": "^[a-z][a-z0-9_]*$" },
    "display_name": { "type": "string" },
    "kconfig": { "type": "string", "pattern": "^ALP_SDK_BLOCK_[A-Z0-9_]+$" },
    "interface": {
      "type": "array", "minItems": 1,
      "items": {
        "type": "object", "additionalProperties": false,
        "required": ["signal", "dir"],
        "properties": {
          "signal": { "type": "string" },
          "dir": { "type": "string", "enum": ["input", "output", "bidir"] },
          "pull": { "type": "string", "enum": ["up", "down", "none"] },
          "rail": { "type": "string" },
          "max_ma": { "type": "number" }
        }
      }
    },
    "realizations": {
      "type": "array", "minItems": 1,
      "items": {
        "type": "object", "additionalProperties": false,
        "required": ["id", "physical_form", "visibility"],
        "properties": {
          "id": { "type": "string", "pattern": "^[a-z][a-z0-9_]*$" },
          "physical_form": { "type": "string", "enum": ["discrete", "module"] },
          "visibility": { "type": "string", "enum": ["public", "internal"] },
          "parts": {
            "type": "array",
            "items": {
              "type": "object", "additionalProperties": false,
              "required": ["chip", "maps"],
              "properties": { "chip": { "type": "string" }, "maps": { "type": "object" } }
            }
          },
          "passives": {
            "type": "array",
            "items": {
              "type": "object", "additionalProperties": false,
              "required": ["role", "value", "net", "refdes_prefix"],
              "properties": {
                "role": { "type": "string" }, "value": { "type": "string" },
                "net": { "type": "string" }, "refdes_prefix": { "type": "string", "pattern": "^[A-Z]+$" }
              }
            }
          },
          "connector": {
            "type": "object", "additionalProperties": false,
            "required": ["footprint", "pins"],
            "properties": {
              "footprint": { "type": "string" },
              "pins": {
                "type": "array",
                "items": {
                  "type": "object", "additionalProperties": false,
                  "required": ["pin", "signal"],
                  "properties": { "pin": { "type": "string" }, "signal": { "type": "string" } }
                }
              }
            }
          }
        }
      }
    }
  }
}
```

- [ ] **Step 4: Wire block validation into `main()`**

```python
BLOCK_SCHEMA = REPO / "metadata" / "schemas" / "block-v1.schema.json"
BLOCKS = REPO / "metadata" / "blocks"
```

```python
    block_failures: list = []
    block_files: list = []
    if BLOCK_SCHEMA.is_file():
        block_schema = json.loads(BLOCK_SCHEMA.read_text(encoding="utf-8"))
        block_validator = jsonschema.Draft202012Validator(block_schema)
        block_files = sorted(BLOCKS.glob("*.yaml"))
        if block_files:
            print()
            block_failures = _check_files(
                "YAML", block_files, block_validator,
                lambda p: yaml.safe_load(p.read_text(encoding="utf-8")),
                "block_id",
            )
```

Add `block_failures` to the tally.

- [ ] **Step 5: Run to verify pass**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -k block_schema -v && py -3 scripts/validate_metadata.py`
Expected: PASS; validator exits 0 (no block manifests yet).

- [ ] **Step 6: Commit**

```bash
git add metadata/schemas/block-v1.schema.json metadata/blocks/.gitkeep scripts/validate_metadata.py tests/scripts/test_validate_metadata_physical.py
git commit -m "feat(metadata): block-v1 schema + metadata/blocks validation"
```

---

### Task 4: block realization semantic checks

Every `realizations[].parts[].chip` must resolve to an existing chip manifest, and every `maps` value must name a signal declared in the block's `interface`.

**Files:**
- Modify: `scripts/validate_metadata.py`
- Test: `tests/scripts/test_validate_metadata_physical.py`

**Interfaces:**
- Consumes: `block_files`, `chip_files`.
- Produces: `_check_block_realizations(block_files, chip_files) -> list`.

- [ ] **Step 1: Write the failing test**

```python
def test_realization_chip_must_exist(tmp_path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("vm3", REPO / "scripts/validate_metadata.py")
    vm = importlib.util.module_from_spec(spec); spec.loader.exec_module(vm)
    blk = _write(tmp_path, "b", {
        "schema_version": 1, "block_id": "b", "display_name": "B",
        "kconfig": "ALP_SDK_BLOCK_B",
        "interface": [{"signal": "LED", "dir": "output"}],
        "realizations": [{"id": "r", "physical_form": "discrete", "visibility": "public",
                          "parts": [{"chip": "does_not_exist", "maps": {"A": "LED"}}]}],
    })
    failures = vm._check_block_realizations([blk], chip_files=[])
    assert failures and any("does_not_exist" in m for _, msgs in failures for m in msgs)
```

- [ ] **Step 2: Run to verify it fails**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -k realization_chip -v`
Expected: FAIL — `_check_block_realizations` not defined.

- [ ] **Step 3: Implement it**

```python
def _check_block_realizations(block_files, chip_files) -> list:
    failures: list = []
    chip_ids = {p.stem for p in chip_files}
    for path in block_files:
        rel = path.relative_to(REPO)
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        if not isinstance(doc, dict):
            continue
        iface = {e["signal"] for e in doc.get("interface", []) if isinstance(e, dict) and "signal" in e}
        msgs: list = []
        for r in doc.get("realizations", []):
            for part in r.get("parts", []):
                if part.get("chip") not in chip_ids:
                    msgs.append(f"realization '{r.get('id')}': part chip '{part.get('chip')}' has no metadata/chips manifest")
                for _pin, sig in (part.get("maps") or {}).items():
                    if sig not in iface:
                        msgs.append(f"realization '{r.get('id')}': maps target '{sig}' not in interface[]")
        if msgs:
            failures.append((rel, msgs))
            print(f"FAIL {rel}")
            for m in msgs:
                print(f"  · {m}")
    return failures
```

Call in `main()` after both chip and block file lists exist:

```python
    if block_files:
        block_failures += _check_block_realizations(block_files, chip_files)
```

- [ ] **Step 4: Run to verify pass**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -v`
Expected: PASS (all tests).

- [ ] **Step 5: Commit**

```bash
git add scripts/validate_metadata.py tests/scripts/test_validate_metadata_physical.py
git commit -m "feat(metadata): block realization chip/interface cross-checks"
```

---

### Task 5: populate button_led + pdm_mic block manifests

The two existing blocks (both discrete). Data source: `blocks/README.md` + the driver `blocks/<name>/*.c` for the abstract interface; the EVK schematic (public reference carrier) for the concrete parts. Where a concrete button/LED/mic MPN is not yet chosen, use a generic footprint and mark that realization `visibility: internal` rather than inventing an MPN.

**Files:**
- Create: `metadata/blocks/button_led.yaml`, `metadata/blocks/pdm_mic.yaml`
- Test: `tests/scripts/test_validate_metadata_physical.py`

- [ ] **Step 1: Write the failing test**

```python
def test_reference_blocks_present_and_valid():
    import subprocess, sys
    for name in ("button_led", "pdm_mic"):
        assert (REPO / f"metadata/blocks/{name}.yaml").is_file()
    r = subprocess.run([sys.executable, "scripts/validate_metadata.py"], cwd=REPO,
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    assert "metadata/blocks/button_led.yaml" in r.stdout
```

- [ ] **Step 2: Run to verify it fails**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -k reference_blocks -v`
Expected: FAIL — manifests missing.

- [ ] **Step 3: Author `button_led.yaml`**

```yaml
# Alp SDK block manifest: generic button + LED helper (blk_button_led).
schema_version: 1
block_id:       button_led
display_name:   "Generic momentary button + indicator LED"
kconfig:        ALP_SDK_BLOCK_BUTTON_LED
interface:
  - { signal: BTN, dir: input,  pull: up }
  - { signal: LED, dir: output, rail: 3V3, max_ma: 10 }
realizations:
  - id:            discrete_generic
    physical_form: discrete
    visibility:    internal        # concrete button/LED MPN is carrier-specific; promote per reference design
    parts: []                      # parts + footprints land with a chosen reference button/LED (do NOT invent an MPN)
    passives:
      - { role: series, value: 330R, net: LED, refdes_prefix: R }
```

> When a reference button/LED part is chosen, add it under `chips/` (Task-7 pattern), reference it in `parts[]` with `maps`, and flip the realization to `public`.

- [ ] **Step 4: Author `pdm_mic.yaml`**

```yaml
# Alp SDK block manifest: generic PDM-microphone surface (blk_pdm_mic).
schema_version: 1
block_id:       pdm_mic
display_name:   "Generic PDM microphone surface"
kconfig:        ALP_SDK_BLOCK_PDM_MIC
interface:
  - { signal: PDM_CLK,  dir: output }
  - { signal: PDM_DATA, dir: input }
realizations:
  - id:            discrete_generic
    physical_form: discrete
    visibility:    internal        # EVK uses 4x PDM MEMS mics; concrete MPN promoted with the reference design
    parts: []
    passives:
      - { role: decouple, value: 100nF, net: VDD, refdes_prefix: C }
```

- [ ] **Step 5: Run to verify pass**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -k reference_blocks -v && py -3 scripts/validate_metadata.py`
Expected: PASS; validator prints `OK metadata/blocks/*.yaml`, exits 0.

- [ ] **Step 6: Commit**

```bash
git add metadata/blocks/button_led.yaml metadata/blocks/pdm_mic.yaml tests/scripts/test_validate_metadata_physical.py
git commit -m "feat(metadata): button_led + pdm_mic block manifests"
```

---

### Task 6: add `signals:` + `physical:` to the 3 existing reference chips

`icm42670`, `tas2563`, `ina236` — all I2C, all with empty `signals[]` today. Add both blocks together. **Data source:** each manifest's `datasheet:` field names the datasheet; take the package + pad-to-pin table from it. Do not invent pads — if the datasheet is not on hand, do that chip in a follow-up and note the omission.

**Files:**
- Modify: `metadata/chips/icm42670.yaml`, `metadata/chips/tas2563.yaml`, `metadata/chips/ina236.yaml`
- Test: `tests/scripts/test_validate_metadata_physical.py`

- [ ] **Step 1: Write the failing test**

```python
def test_reference_chips_have_physical():
    import yaml
    for name in ("icm42670", "tas2563", "ina236"):
        d = yaml.safe_load((REPO / f"metadata/chips/{name}.yaml").read_text())
        assert d.get("signals"), f"{name} needs signals[]"
        assert d.get("physical"), f"{name} needs physical:"
        assert d["physical"]["visibility"] in ("public", "internal")
```

- [ ] **Step 2: Run to verify it fails**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -k reference_chips_have_physical -v`
Expected: FAIL — no `physical:` yet.

- [ ] **Step 3: Add `signals:` + `physical:` to each (template — fill pads from the datasheet)**

Append to `metadata/chips/ina236.yaml` (INA236, VSSOP-10 per TI SBOS…; the manifest's `datasheet:` field is authoritative — transcribe pads from it):

```yaml
signals:
  - { name: SCL, type: input,  scope: "I2C" }
  - { name: SDA, type: bidir,  scope: "I2C" }
  - { name: ALERT, type: output }
  - { name: VS,  type: power }
  - { name: GND, type: ground }
  - { name: IN+, type: analog }
  - { name: IN-, type: analog }
physical:
  refdes_prefix: U
  package:       VSSOP-10
  footprint:     vssop_10_3p0x3p0mm
  visibility:    public
  pins:
    # pad numbers TRANSCRIBED from the INA236 datasheet pinout table — do not guess
    - { pad: "1", signal: "IN+" }
    - { pad: "2", signal: "IN-" }
    # … remaining pads from the datasheet …
  passives:
    - { role: decouple, value: 100nF, net: VS,  refdes_prefix: C }
    - { role: pullup,   value: 4k7,   net: SCL, refdes_prefix: R }
    - { role: pullup,   value: 4k7,   net: SDA, refdes_prefix: R }
```

Repeat the same shape for `icm42670` (2.5×3.0 mm LGA-14, PDM/I2C signals per its datasheet) and `tas2563` (VQFN, I2C + I2S signals per its datasheet). Each `pins[].signal` must equal a `signals[].name` or a power net (Task-2 check enforces it).

- [ ] **Step 4: Run to verify pass**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -v && py -3 scripts/validate_metadata.py`
Expected: PASS; all three chips validate with `physical:`.

- [ ] **Step 5: Commit**

```bash
git add metadata/chips/icm42670.yaml metadata/chips/tas2563.yaml metadata/chips/ina236.yaml tests/scripts/test_validate_metadata_physical.py
git commit -m "feat(metadata): physical layer for icm42670/tas2563/ina236"
```

---

### Task 7: create the 4 missing reference chip manifests

`bmi323`, `bmp581`, `cam_mux_pi3wvr626`, `tcal9538` have drivers (`chips/<slug>/`) but **no `metadata/chips/<slug>.yaml`**. Create each full manifest (mirror `metadata/chips/tmp112.yaml` for required fields) plus `signals:` + `physical:`. **Data source:** the driver's header comment + the part datasheet. Do not invent MPN/pads.

**Files:**
- Create: `metadata/chips/{bmi323,bmp581,cam_mux_pi3wvr626,tcal9538}.yaml`
- Test: `tests/scripts/test_validate_metadata_physical.py`

- [ ] **Step 1: Write the failing test**

```python
def test_reconciled_chips_exist_with_physical():
    import yaml
    for name in ("bmi323", "bmp581", "cam_mux_pi3wvr626", "tcal9538"):
        p = REPO / f"metadata/chips/{name}.yaml"
        assert p.is_file(), f"{name} manifest missing"
        d = yaml.safe_load(p.read_text())
        assert d["chip_id"] == name
        assert d.get("physical")
```

- [ ] **Step 2: Run to verify it fails**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -k reconciled_chips -v`
Expected: FAIL — manifests missing.

- [ ] **Step 3: Author each manifest (template)**

```yaml
# Alp SDK chip manifest: <vendor> <part> <one-line role>.
schema_version:   1
chip_id:          bmi323
driver_status:    complete
display_name:     "Bosch BMI323 6-axis IMU"
vendor:           bosch
mpn_population:
  - "BMI323"                       # confirm exact orderable MPN from the datasheet
datasheet:
  primary:        "Bosch BMI323 datasheet"
bus:              i2c
i2c:
  addresses:
    - { addr_7bit: 0x68, scope: "SDO=GND" }
signals:
  - { name: SDA, type: bidir }
  - { name: SCL, type: input }
  - { name: VDD, type: power }
  - { name: GND, type: ground }
kconfig:
  zephyr:         ALP_SDK_CHIP_BMI323
  baremetal:      ALP_SDK_CHIP_BMI323
families: [aen]
verification:
  hil_silicon:    untested
physical:
  refdes_prefix: U
  package:       LGA-14
  footprint:     bosch_lga_14_2p5x3p0mm
  visibility:    public
  pins:
    # TRANSCRIBED from the BMI323 datasheet pin map
    - { pad: "1", signal: "SCL" }
    # … remaining pads …
  passives:
    - { role: decouple, value: 100nF, net: VDD, refdes_prefix: C }
```

Repeat for `bmp581` (Bosch pressure, WLGA), `cam_mux_pi3wvr626` (Diodes MIPI 2:1 mux — note it's a signal mux, `bus:` may be `none`; check the driver), `tcal9538` (NXP/TI I2C GPIO expander at 0x72). Confirm each `bus`/address/MPN against the driver + datasheet.

- [ ] **Step 4: Run to verify pass**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -v && py -3 scripts/validate_metadata.py`
Expected: PASS; the 4 new manifests validate.

- [ ] **Step 5: Commit**

```bash
git add metadata/chips/bmi323.yaml metadata/chips/bmp581.yaml metadata/chips/cam_mux_pi3wvr626.yaml metadata/chips/tcal9538.yaml tests/scripts/test_validate_metadata_physical.py
git commit -m "feat(metadata): reconcile+physical for bmi323/bmp581/cam_mux/tcal9538"
```

---

### Task 8: AEN801-set completeness gate

A single test proving the netlist is derivable from public SDK data for the whole reference set: every `populated: true` part on `e1m-evk.yaml` resolves to a manifest, and every such chip/block has the physical data a BOM line needs (MPN + footprint, or a `visibility: internal` marker explaining why it's deferred).

**Files:**
- Test: `tests/scripts/test_validate_metadata_physical.py`

- [ ] **Step 1: Write the test**

```python
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
```

- [ ] **Step 2: Run to verify it passes**

Run: `py -3 -m pytest tests/scripts/test_validate_metadata_physical.py -k aen801_reference_set -v`
Expected: PASS (Tasks 5-7 populated every `populated: true` part).

> If it fails, the message lists exactly which part is missing physical data — populate it (Task 6/7 pattern) or set the EVK `populated:` entry to `false` with a comment if the part is genuinely not on the reference carrier.

- [ ] **Step 3: Commit**

```bash
git add tests/scripts/test_validate_metadata_physical.py
git commit -m "test(metadata): AEN801 reference-set BOM-completeness gate"
```

---

## Self-Review

- **Spec coverage:** §1 chip physical → Tasks 1,2,6,7. §2 block manifest → Tasks 3,4,5. §3 classification (`visibility`) → schemas (Tasks 1,3) require it; population tasks set it. §4 validation → Tasks 1-4; derivation proven by Task 8. §5 AEN801 set → Tasks 6,7,8. §5 testing (schema round-trip, negative, golden) → Tasks 1,2,4,8. All covered.
- **Placeholder scan:** the population tasks (6,7) intentionally instruct transcription from named datasheets rather than embedding pad numbers the plan author does not have; this is a data-entry instruction with an enforcing test (Task 2 rejects unresolved signals; Task 8 rejects missing physical), not a "figure it out" placeholder. `parts: []` in Task 5 is deliberate (no MPN invented) and gated `internal`.
- **Type consistency:** `_check_chip_physical`, `_check_block_realizations`, consts `CHIP_SCHEMA/CHIPS/BLOCK_SCHEMA/BLOCKS`, and the `physical:`/`realizations:` field names are used identically across tasks and match the two schemas.
