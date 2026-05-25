# Cross-EVK Example Portability — Implementation Plan (Plan 1: infrastructure + pilot)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the board-agnostic `BOARD_*` layer + `<alp/board.h>` facade and prove it end-to-end by converting one example (`dac-waveform`) to build+run on both EVKs, verified on native_sim in CI.

**Architecture:** Each `metadata/boards/<board>.yaml` `e1m_routes:` entry that names a peripheral common to both form factors (e1m-spec §7.2) gains a `board_alias: BOARD_*` field. `gen_board_header.py` emits a `#define BOARD_* <macro>` block into each `alp_<board>_routes.h`. A hand-written `<alp/board.h>` includes the active board's routes header based on an `ALP_BOARD_<SLUG>` compile define. Cross-EVK examples include only `<alp/board.h>` and use only `BOARD_*` names. A pytest asserts both boards declare the identical `BOARD_*` set; twister builds each cross-EVK example once per supported board.

**Tech Stack:** Python 3 (generators + pytest), C (Zephyr/native_sim), JSON Schema, Zephyr twister, YAML board metadata.

**Scope:** Plan 1 = infrastructure + the `dac-waveform` pilot. Plan 2 (separate) migrates the remaining ~20 common-only demos using the pattern proven here. Spec: `docs/superpowers/specs/2026-05-24-cross-evk-example-portability-design.md`.

**Gates (run before every push; see CLAUDE.md / memory):** full twister `native_sim/native/64` (0 errored), full `pytest tests/scripts/`, `clang-format-14` diff-only, generated-files in sync. No `Co-Authored-By: Claude` footer on commits.

---

## File structure

| File | Responsibility | Action |
| --- | --- | --- |
| `scripts/gen_board_header.py` | Emit a `BOARD_*` alias block from `e1m_routes[*].board_alias` | Modify |
| `metadata/boards/e1m-evk.yaml` | Add `board_alias:` to the §7.2-common entries | Modify |
| `metadata/boards/e1m-x-evk.yaml` | Add `board_alias:` to the §7.2-common entries | Modify |
| `metadata/schemas/board.schema.json` | Allow `board_alias` on routes entries; allow top-level `supported_boards` | Modify |
| `include/alp/boards/alp_e1m_evk_routes.h` | Regenerated — gains `BOARD_*` block | Regenerate |
| `include/alp/boards/alp_e1m_x_evk_routes.h` | Regenerated — gains `BOARD_*` block | Regenerate |
| `include/alp/board.h` | Facade: pick routes header from `ALP_BOARD_<SLUG>` | Create |
| `tests/scripts/test_board_alias_parity.py` | Assert both boards define the identical `BOARD_*` set | Create |
| `examples/peripheral-io/dac-waveform/src/main.c` | Pilot: `<alp/board.h>` + `BOARD_DAC0`; fix wrong comments | Modify |
| `examples/peripheral-io/dac-waveform/board.yaml` | Add `supported_boards: [e1m-evk, e1m-x-evk]` | Modify |
| `examples/peripheral-io/dac-waveform/testcase.yaml` | One build_only scenario per supported board | Modify |
| `docs/abi/v0.5-snapshot.json` | Regenerated — additive `BOARD_*` macros | Regenerate |

**`BOARD_*` common set for Plan 1 (the entries that get `board_alias`):** start with the full §7.2-common roles present in *both* board YAMLs — `BOARD_UART_DEBUG`, `BOARD_UART_ARDUINO`, `BOARD_I2C_SENSORS`, `BOARD_SPI_ARDUINO`, `BOARD_ADC_A0`, `BOARD_DAC0`, `BOARD_DAC1`, `BOARD_I2S_AUDIO`, `BOARD_CAN0`, `BOARD_ENC_ROTARY`, `BOARD_PIN_LED_RED`, `BOARD_PIN_LED_GREEN`, `BOARD_PIN_LED_BLUE`, `BOARD_PIN_ENCODER_SW`, `BOARD_PIN_BMI323_INT1`, `BOARD_PWM_ARD0..3`. (PWM-LED + DISP backlight names diverge per board and are *not* aliased; the Arduino ADC channel set diverges beyond A0 and only `BOARD_ADC_A0` is common.)

---

## Task 1: `board_alias` → `BOARD_*` generation + parity test

**Files:**
- Test: `tests/scripts/test_board_alias_parity.py` (create)
- Modify: `scripts/gen_board_header.py` (the `_SECTIONS` loop / new emitter)
- Modify: `metadata/boards/e1m-evk.yaml`, `metadata/boards/e1m-x-evk.yaml`
- Regenerate: `include/alp/boards/alp_e1m_evk_routes.h`, `alp_e1m_x_evk_routes.h`

- [ ] **Step 1: Write the failing parity test**

Create `tests/scripts/test_board_alias_parity.py`:

```python
# SPDX-License-Identifier: Apache-2.0
"""The portable BOARD_* alias set must be identical across every board
routes header — a name present on only one board would let a "both"
example compile on one EVK and break on the other."""
from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ROUTES = REPO / "include" / "alp" / "boards"
_BOARD_DEFINE = re.compile(r"^#define\s+(BOARD_[A-Z0-9_]+)\s", re.MULTILINE)


def _board_aliases(header: Path) -> set[str]:
    return set(_BOARD_DEFINE.findall(header.read_text(encoding="utf-8")))


def test_evk_and_x_evk_define_identical_board_alias_sets() -> None:
    evk = _board_aliases(ROUTES / "alp_e1m_evk_routes.h")
    xevk = _board_aliases(ROUTES / "alp_e1m_x_evk_routes.h")
    assert evk, "no BOARD_* aliases found in alp_e1m_evk_routes.h"
    assert evk == xevk, (
        "BOARD_* alias sets differ between EVKs:\n"
        f"  e1m-evk only:   {sorted(evk - xevk)}\n"
        f"  e1m-x-evk only: {sorted(xevk - evk)}"
    )


def test_board_dac0_is_present() -> None:
    """Pilot anchor: DAC0 is common (e1m-spec §7.2) so BOARD_DAC0 must exist."""
    assert "BOARD_DAC0" in _board_aliases(ROUTES / "alp_e1m_evk_routes.h")
```

- [ ] **Step 2: Run it; verify it fails**

Run: `cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk && PYTHONPATH=$PWD/scripts python3 -m pytest tests/scripts/test_board_alias_parity.py -v`
Expected: FAIL — `no BOARD_* aliases found` (the generator emits none yet).

- [ ] **Step 3: Teach `gen_board_header.py` to emit the alias block**

In `scripts/gen_board_header.py`, add an emitter and call it after the section loop in `emit_board` (after the `for section_key, ... in _SECTIONS` loop, before the closing `extern "C"` block):

```python
def _emit_board_aliases(routes: dict[str, Any]) -> list[str]:
    """Emit portable BOARD_* aliases for every entry carrying a
    `board_alias:` (the e1m-spec §7.2 common roles).  Same BOARD_* name
    on every board -> a board-agnostic example resolves per built board."""
    pairs: list[tuple[str, str]] = []
    for entries in routes.values():
        for entry in entries or []:
            alias = entry.get("board_alias")
            if alias:
                pairs.append((alias, entry["macro"]))
    if not pairs:
        return []
    pairs.sort()
    widest = max(len(a) for a, _ in pairs)
    out = [
        "/* ------------------------------------------------------------------ */",
        "/* Portable cross-EVK aliases (e1m-spec STANDARD.md §7.2 common set). */",
        "/* Same BOARD_* names on every board; include via <alp/board.h>.       */",
        "/* ------------------------------------------------------------------ */",
        "",
    ]
    for alias, macro in pairs:
        out.append(f"#define {alias:<{widest}} {macro}")
    out.append("")
    return out
```

Then, in `emit_board`, immediately after the `for section_key, section_title in _SECTIONS:` loop:

```python
    lines.extend(_emit_board_aliases(routes))
```

- [ ] **Step 4: Add `board_alias` to the common entries in both board YAMLs**

In `metadata/boards/e1m-x-evk.yaml`, add `board_alias:` to the common-role entries (leave X-only entries — I2C2/3, CAN1, PCIe/CTP/LCD/CAM/USB/SDIO muxes, DISP2_BL — untouched). Example edits:

```yaml
  buses:
    - { e1m: E1M_X_I2C0,  macro: XEVK_I2C_BUS_SENSORS,  board_alias: BOARD_I2C_SENSORS,
        doc: "On-board sensor + IO-expander + INA236 bus ..." }
    - { e1m: E1M_X_SPI1,  macro: XEVK_SPI_BUS_ARDUINO,  board_alias: BOARD_SPI_ARDUINO,
        doc: "Arduino UNO header SPI (level-shifted)." }
    - { e1m: E1M_X_UART0, macro: XEVK_UART_PORT_DEBUG,  board_alias: BOARD_UART_DEBUG,
        doc: "Console / debug UART." }
    - { e1m: E1M_X_UART1, macro: XEVK_UART_PORT_ARDUINO, board_alias: BOARD_UART_ARDUINO,
        doc: "Arduino UNO header UART (D0/D1, level-shifted)." }
  adc:
    - { e1m: E1M_X_ADC0, macro: XEVK_ADC_ARDUINO_A0, board_alias: BOARD_ADC_A0,
        doc: "Arduino UNO header A0 analog input (ANA_S0)." }
  dac:
    - { e1m: E1M_X_DAC0, macro: XEVK_DAC0, board_alias: BOARD_DAC0, doc: "DAC0 analog output." }
    - { e1m: E1M_X_DAC1, macro: XEVK_DAC1, board_alias: BOARD_DAC1, doc: "DAC1 analog output." }
  i2s:
    - { e1m: E1M_X_I2S0, macro: XEVK_I2S_AUDIO, board_alias: BOARD_I2S_AUDIO,
        doc: "TAS2563 smart-amp I2S (SCLK / WS / SDI / SDO)." }
  can:
    - { e1m: E1M_X_CAN0, macro: XEVK_CAN_BUS0, board_alias: BOARD_CAN0,
        doc: "CAN0 via TCAN1044 transceiver (U51)." }
  qenc:
    - { e1m: E1M_X_ENC0, macro: XEVK_ENC_ROTARY, board_alias: BOARD_ENC_ROTARY, doc: "..." }
```

Add to the `gpio:` section entries that have a twin on both boards:
`XEVK_PIN_LED_RED` → `board_alias: BOARD_PIN_LED_RED`, `XEVK_PIN_LED_GREEN` → `BOARD_PIN_LED_GREEN`, `XEVK_PIN_LED_BLUE` → `BOARD_PIN_LED_BLUE`, `XEVK_PIN_ENCODER_SW` → `BOARD_PIN_ENCODER_SW`, `XEVK_PIN_BMI323_INT1` → `BOARD_PIN_BMI323_INT1`.
Add to `pwm:` for `XEVK_ARD_PWM0..3` → `BOARD_PWM_ARD0..3`.

In `metadata/boards/e1m-evk.yaml`, add the **same** `board_alias:` values to the matching entries (the `EVK_*` twins): `EVK_UART_PORT_DEBUG`→`BOARD_UART_DEBUG`, `EVK_UART_PORT_ARDUINO`→`BOARD_UART_ARDUINO`, `EVK_I2C_BUS_SENSORS`→`BOARD_I2C_SENSORS`, `EVK_SPI_BUS_ARDUINO`→`BOARD_SPI_ARDUINO`, `EVK_DAC_ARDUINO_DAC0`→`BOARD_DAC0`, `EVK_DAC_AUDIO_LINE_OUT`→`BOARD_DAC1`, `EVK_I2S_AUDIO_CODEC`→`BOARD_I2S_AUDIO`, `EVK_CAN_VEHICLE_BUS`→`BOARD_CAN0`, `EVK_ENC_ROTARY`→`BOARD_ENC_ROTARY`, `EVK_PIN_LED_RED/GREEN/BLUE`→`BOARD_PIN_LED_RED/GREEN/BLUE`, `EVK_PIN_ENCODER_SW`→`BOARD_PIN_ENCODER_SW`, `EVK_PIN_BMI323_INT1`→`BOARD_PIN_BMI323_INT1`, `EVK_ADC_ARDUINO_A1`→**no alias** (E1M EVK has no A0 silkscreen; A0 is X-only here — confirm against the YAML; if E1M EVK exposes A0 use it, else drop `BOARD_ADC_A0` from BOTH boards to keep parity), `EVK_ARD_PWM*`→`BOARD_PWM_ARD0..3` (match the Arduino PWM positions; where the E1M EVK's `EVK_ARD_*` numbering differs, alias by Arduino-header position, not by `E1M_PWM<n>` index).

> Parity rule: a `BOARD_*` name MUST be added to **both** YAMLs or neither. The Task-1 test enforces this; if a role only cleanly exists on one board, do not alias it.

- [ ] **Step 5: Regenerate the headers**

Run: `cd /mnt/c/Users/caner/Documents/GitHub/alp-sdk && python3 scripts/gen_board_header.py`
Expected: `wrote include/alp/boards/alp_e1m_evk_routes.h ...` and `... alp_e1m_x_evk_routes.h ...`. The two headers gain the "Portable cross-EVK aliases" block.

- [ ] **Step 6: Run the parity test; verify it passes**

Run: `PYTHONPATH=$PWD/scripts python3 -m pytest tests/scripts/test_board_alias_parity.py -v`
Expected: PASS (both `test_..._identical...` and `test_board_dac0_is_present`).

- [ ] **Step 7: Commit**

```bash
git add scripts/gen_board_header.py metadata/boards/e1m-evk.yaml \
  metadata/boards/e1m-x-evk.yaml include/alp/boards/alp_e1m_evk_routes.h \
  include/alp/boards/alp_e1m_x_evk_routes.h tests/scripts/test_board_alias_parity.py
git commit -m "feat(boards): emit portable BOARD_* aliases from e1m_routes board_alias"
```

---

## Task 2: `<alp/board.h>` facade

**Files:**
- Create: `include/alp/board.h`
- Test: extend `tests/scripts/test_board_alias_parity.py` (a compile-style include check is impractical in pytest; instead a header-shape unit test)

- [ ] **Step 1: Write the failing test**

Append to `tests/scripts/test_board_alias_parity.py`:

```python
BOARD_H = REPO / "include" / "alp" / "board.h"


def test_board_facade_selects_each_known_board() -> None:
    text = BOARD_H.read_text(encoding="utf-8")
    # Each known board's ALP_BOARD_<SLUG> must select its routes header.
    assert "ALP_BOARD_E1M_X_EVK" in text and "alp/boards/alp_e1m_x_evk_routes.h" in text
    assert "ALP_BOARD_E1M_EVK" in text and "alp/boards/alp_e1m_evk_routes.h" in text
    # No board selected must be a hard error, not a silent miss.
    assert "#error" in text
```

- [ ] **Step 2: Run it; verify it fails**

Run: `PYTHONPATH=$PWD/scripts python3 -m pytest tests/scripts/test_board_alias_parity.py::test_board_facade_selects_each_known_board -v`
Expected: FAIL — `board.h` does not exist (FileNotFoundError).

- [ ] **Step 3: Create the facade**

Create `include/alp/board.h`:

```c
/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @file board.h
 * @brief Board-agnostic facade for cross-EVK examples.
 *
 * Includes the active board's generated routes header (selected by the
 * ALP_BOARD_<SLUG> compile define the build emits from the board.yaml
 * preset) so an example can open pins via the portable BOARD_* aliases
 * and build for whichever EVK is targeted.  The BOARD_* names cover the
 * e1m-spec STANDARD.md §7.2 interfaces common to both form factors.
 *
 * Form-factor-specific examples should NOT use this facade; they include
 * the specific routes header (alp_e1m_evk_routes.h / alp_e1m_x_evk_routes.h)
 * directly and use EVK_*/XEVK_* macros.
 */
#ifndef ALP_BOARD_H
#define ALP_BOARD_H

#if   defined(ALP_BOARD_E1M_X_EVK)
#  include "alp/boards/alp_e1m_x_evk_routes.h"
#elif defined(ALP_BOARD_E1M_EVK)
#  include "alp/boards/alp_e1m_evk_routes.h"
#else
#  error "alp/board.h: no ALP_BOARD_* board selected. Set the example's board.yaml preset (e1m-evk / e1m-x-evk), or pass -DALP_BOARD_E1M_EVK / -DALP_BOARD_E1M_X_EVK."
#endif

#endif /* ALP_BOARD_H */
```

- [ ] **Step 4: Run the test; verify it passes**

Run: `PYTHONPATH=$PWD/scripts python3 -m pytest tests/scripts/test_board_alias_parity.py::test_board_facade_selects_each_known_board -v`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add include/alp/board.h tests/scripts/test_board_alias_parity.py
git commit -m "feat(board): add <alp/board.h> cross-EVK facade"
```

---

## Task 3: schema — allow `board_alias` + `supported_boards`

**Files:**
- Modify: `metadata/schemas/board.schema.json`
- Test: `tests/scripts/test_board_alias_parity.py` (schema-load assertion) or the existing schema-validation test path

- [ ] **Step 1: Write the failing test**

Append to `tests/scripts/test_board_alias_parity.py`:

```python
import json


def test_schema_allows_board_alias_and_supported_boards() -> None:
    schema = json.loads(
        (REPO / "metadata" / "schemas" / "board.schema.json").read_text(encoding="utf-8")
    )
    routes_entry = schema["$defs"]["e1m_routes"]["properties"]
    # Find the per-entry properties object (route sections are arrays of entries).
    # The entry schema lives under the section array's "items".
    sample_section = next(iter(routes_entry.values()))
    entry_props = sample_section["items"]["properties"]
    assert "board_alias" in entry_props
    assert "supported_boards" in schema["properties"]
```

(Adjust the navigation to match `$defs/e1m_routes`'s actual shape after reading the file; the assertion targets are `board_alias` on a route entry and top-level `supported_boards`.)

- [ ] **Step 2: Run it; verify it fails**

Run: `PYTHONPATH=$PWD/scripts python3 -m pytest tests/scripts/test_board_alias_parity.py::test_schema_allows_board_alias_and_supported_boards -v`
Expected: FAIL — `board_alias` not in entry props.

- [ ] **Step 3: Edit the schema**

In `metadata/schemas/board.schema.json`, on the `$defs/e1m_routes` route-entry `properties` (the object that already has `e1m`, `macro`, `doc`, `active_low`), add:

```json
"board_alias": {
  "type": "string",
  "pattern": "^BOARD_[A-Z0-9_]*$",
  "description": "Portable cross-EVK alias (e1m-spec §7.2 common role). gen_board_header.py emits `#define <board_alias> <macro>`. The SAME board_alias MUST be declared on every board that exposes the role (enforced by tests/scripts/test_board_alias_parity.py)."
}
```

And add a top-level property (sibling of `preset`, `e1m_routes`):

```json
"supported_boards": {
  "type": "array",
  "items": { "type": "string" },
  "uniqueItems": true,
  "description": "Board presets this project/example is verified to build for (e.g. [e1m-evk, e1m-x-evk]). Examples only; per ADR-0011 application firmware still targets one SoM family. Twister builds one scenario per entry. Defaults to the single `preset:` when omitted."
}
```

- [ ] **Step 4: Run the test; verify it passes**

Run: `PYTHONPATH=$PWD/scripts python3 -m pytest tests/scripts/test_board_alias_parity.py::test_schema_allows_board_alias_and_supported_boards -v`
Expected: PASS. Also run the existing schema/metadata-validate tests to confirm no regression:
`PYTHONPATH=$PWD/scripts python3 -m pytest tests/scripts/ -k schema -q`

- [ ] **Step 5: Commit**

```bash
git add metadata/schemas/board.schema.json tests/scripts/test_board_alias_parity.py
git commit -m "feat(schema): allow board_alias on routes + top-level supported_boards"
```

---

## Task 4: `dac-waveform` pilot conversion

**Files:**
- Modify: `examples/peripheral-io/dac-waveform/src/main.c`
- Modify: `examples/peripheral-io/dac-waveform/board.yaml`
- Modify: `examples/peripheral-io/dac-waveform/testcase.yaml`

- [ ] **Step 1: Convert the source to the facade + `BOARD_DAC0`**

In `examples/peripheral-io/dac-waveform/src/main.c`:

Replace the include (line ~44):
```c
#include "alp/board.h"             /* BOARD_DAC0 -> the EVK's DAC0 pad */
```
(remove `#include "alp/e1m_x_pinout.h"`).

Replace the channel id (line ~132):
```c
        .channel_id = BOARD_DAC0,
```

Fix the header comment (lines ~5, ~13-18) — the "base E1M doesn't route a DAC" claim is **wrong**; both EVKs route 2 DAC channels (E1M via the Alif Ensemble's native DAC; E1M-X via the on-module GD32 bridge). Rewrite to:
```c
 * dac-waveform -- generate a sine wave on BOARD_DAC0.
 *
 * Runs on both EVKs: the E1M EVK drives the Alif Ensemble's native
 * 2-channel DAC; the E1M-X EVK drives the V2N's two DAC channels
 * through the on-module GD32G553 bridge.  BOARD_DAC0 (from
 * <alp/board.h>) resolves to the selected board's DAC0 pad.
```

Fix the inline comment (lines ~128-130) to drop the E1M-X-only framing:
```c
    /* channel_id = BOARD_DAC0 -> the selected board's DAC0 pad
     * (E1M_DAC0 on E1M / Alif; E1M_X_DAC0 -> GD32 PA4 on E1M-X / V2N). */
```
Update the printf (line ~122) `E1M_X_DAC0` -> `BOARD_DAC0`, and the NULL-branch comment (lines ~139-143) to remove the "AEN DAC isn't routed" claim (it is). Keep the `native_sim returns NULL/NOT_READY` note.

- [ ] **Step 2: Declare both boards in `board.yaml`**

In `examples/peripheral-io/dac-waveform/board.yaml`, add after the `preset:` line:
```yaml
supported_boards:
  - e1m-evk
  - e1m-x-evk
```
(Keep `preset: e1m-x-evk` as the build default.)

- [ ] **Step 3: One twister scenario per board**

Replace `examples/peripheral-io/dac-waveform/testcase.yaml`'s `tests:` block with one build_only scenario per supported board, each forcing the board define on native_sim:

```yaml
tests:
  alp_sdk.example.dac_waveform.e1m_x_evk:
    platform_allow: [native_sim, native_sim/native/64]
    integration_platforms: [native_sim/native/64]
    extra_args: ["CONFIG_COMPILER_OPT=\"-DALP_BOARD_E1M_X_EVK\""]
    tags: [alp-sdk, example, dac, v2n]
    build_only: true
  alp_sdk.example.dac_waveform.e1m_evk:
    platform_allow: [native_sim, native_sim/native/64]
    integration_platforms: [native_sim/native/64]
    extra_args: ["CONFIG_COMPILER_OPT=\"-DALP_BOARD_E1M_EVK\""]
    tags: [alp-sdk, example, dac, aen]
    build_only: true
```

(`CONFIG_COMPILER_OPT` injects the `-D` into the compile; this is the build-time board pick for the native_sim verification. Real-silicon/west builds get `ALP_BOARD_<slug>` from the preset via Task 5.)

- [ ] **Step 4: Build both scenarios on native_sim**

Run the full twister gate (WSL; do NOT pipe through `tail`):
```
wsl -d Ubuntu -- bash -lc 'cd /home/alplab/zephyrproject && export ZEPHYR_BASE=/home/alplab/zephyrproject/zephyr EXTRA_ZEPHYR_MODULES=/mnt/c/Users/caner/Documents/GitHub/alp-sdk ZEPHYR_TOOLCHAIN_VARIANT=host && python3 zephyr/scripts/twister --testsuite-root /mnt/c/Users/caner/Documents/GitHub/alp-sdk/examples -p native_sim/native/64 -s alp_sdk.example.dac_waveform.e1m_x_evk -s alp_sdk.example.dac_waveform.e1m_evk -O /tmp/tw_dac'
```
Expected in `/tmp/tw_dac/twister.json`: both scenarios `passed` (build_only), 0 errored. Both prove `BOARD_DAC0` resolves under each board define.

- [ ] **Step 5: Commit**

```bash
git add examples/peripheral-io/dac-waveform/src/main.c \
  examples/peripheral-io/dac-waveform/board.yaml \
  examples/peripheral-io/dac-waveform/testcase.yaml
git commit -m "refactor(examples): dac-waveform builds on both EVKs via BOARD_DAC0"
```

---

## Task 5: emit `ALP_BOARD_<SLUG>` from the preset (in `alp_orchestrate.py`)

So west / real-silicon / non-twister builds get the board define automatically (no per-testcase `extra_args`).

> **Implementation note (as built):** `alp_project.py`'s `--emit` modes delegate to `scripts/alp_orchestrate.py`; the define is emitted there — `_slice_cmake_args` → `-DALP_BOARD_<slug>`, `_slice_alp_conf` → `CONFIG_COMPILER_OPT="-DALP_BOARD_<slug>"`. "alp_project.py" in the steps below means that delegation chain; the committed change touched `scripts/alp_orchestrate.py` + `tests/scripts/test_alp_project.py`.

**Files:**
- Modify: `scripts/alp_orchestrate.py` (`_slice_cmake_args` + `_slice_alp_conf` — the per-slice emit path that `alp_project.py --emit` delegates to)
- Test: `tests/scripts/test_alp_project.py` (add a case)

- [ ] **Step 1: Locate the emit point + write the failing test**

Read `scripts/alp_project.py` to find where it renders the per-core Kconfig/compile fragment from `board.yaml` (the `--emit zephyr-conf` path). Add a test in `tests/scripts/test_alp_project.py` that runs the emitter on a fixture board.yaml with `preset: e1m-x-evk` and asserts the output defines/sets `ALP_BOARD_E1M_X_EVK` (slug via the same `_board_slug` rule: `e1m-x-evk` → `E1M_X_EVK`). Mirror the existing test style in that file.

- [ ] **Step 2: Run it; verify it fails**

Run: `PYTHONPATH=$PWD/scripts python3 -m pytest tests/scripts/test_alp_project.py -k alp_board -v`
Expected: FAIL — no `ALP_BOARD_*` emitted.

- [ ] **Step 3: Emit the define**

In the emit path, derive the slug from the resolved `preset:` (uppercased, `-`→`_`) and add `-DALP_BOARD_<SLUG>` to the compile definitions (via the same mechanism the emitter already uses for compile options — e.g. append to `CONFIG_COMPILER_OPT` or the generated `app.conf`/CMake defs, matching the file's existing pattern). Keep it emitted for every backend (zephyr-conf, native_sim, plain-cmake).

- [ ] **Step 4: Run the test; verify it passes**

Run: `PYTHONPATH=$PWD/scripts python3 -m pytest tests/scripts/test_alp_project.py -k alp_board -v`
Expected: PASS.

- [ ] **Step 5: Simplify the pilot testcase (optional)**

Once Task 5 lands, `dac-waveform`'s two scenarios can drop the explicit `extra_args` define and instead select the board via the preset per scenario (if the twister harness supports per-scenario preset). If not straightforward, leave the explicit `extra_args` from Task 4 — both are correct.

- [ ] **Step 6: Commit**

```bash
git add scripts/alp_project.py tests/scripts/test_alp_project.py
git commit -m "feat(build): emit ALP_BOARD_<slug> compile define from board preset"
```

---

## Final gates (before any push)

- [ ] Regenerate generated files; confirm in sync:
  `python3 scripts/gen_soc_caps.py && python3 scripts/abi_snapshot.py --version v0.5 --output docs/abi/v0.5-snapshot.json && python3 scripts/gen_board_header.py` then `git diff --exit-code` (ignoring the snapshot `generated:` date line). Commit the additive `BOARD_*` ABI-snapshot delta.
- [ ] Full `pytest tests/scripts/` (whole dir) passes.
- [ ] Full twister `native_sim/native/64` over `tests/zephyr` + `tests/unit` + `examples`: 0 errored.
- [ ] `clang-format-14` diff-only vs `origin/main`: clean (the generated routes headers are `clang-format off`; `board.h` is hand-written — keep it formatted).

---

## Self-review

**Spec coverage:**
- Common contract (§7.2) → Task 1 `board_alias` set + parity test. ✓
- `BOARD_*` alias block generated → Task 1. ✓
- `<alp/board.h>` facade → Task 2. ✓
- `ALP_BOARD_<SLUG>` build define → Task 5 (auto) + Task 4 (`extra_args` for the pilot). ✓
- `supported_boards` schema + per-example class → Task 3 + Task 4 (`board.yaml`). ✓
- Audit-don't-trust (dac-waveform reclassified, comment fixed) → Task 4. ✓
- native_sim per-board twister verification → Task 4 (two scenarios). ✓
- Additive ABI → final gates regenerate the snapshot. ✓
- Bulk migration of the other ~20 examples → **Plan 2** (out of scope here, by design). ✓

**Placeholder scan:** Two steps intentionally require reading a file before the exact edit — Task 3 Step 3 (navigate `$defs/e1m_routes`'s exact shape) and Task 5 (locate `alp_project.py`'s emit point). Both give the precise change + assertion target; they are not open-ended. All code-producing steps contain the code.

**Type/name consistency:** `BOARD_DAC0` (Task 1 YAML) = used in Task 4 source = asserted in Task 1/2 tests. `ALP_BOARD_E1M_X_EVK` / `ALP_BOARD_E1M_EVK` consistent across the facade (Task 2), the pilot testcase (Task 4), and the emit (Task 5). Slug rule (`_board_slug`: lower, `-`→`_`; uppercased for the define) reused from `gen_board_header.py`.
