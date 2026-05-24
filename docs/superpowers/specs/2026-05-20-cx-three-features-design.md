# CX Improvements: `alp` CLI, rich diagnostics, capability API

**Date:** 2026-05-20
**Status:** Draft — pending implementation
**Owner:** alpCaner

## Motivation

Three CX gaps surfaced when reviewing the new-customer first-run path:

1. No one-command "hello world" — customers must learn YAML schema, Zephyr workflow, and CMake plumbing on day one before seeing anything blink.
2. Schema failures from `scripts/alp_project.py` surface as one-line `sys.exit` errors with no file/line/column — a customer who mistypes a pad name gets a Python traceback at best.
3. Application code that wants to branch on hardware features (NPU present? hardware FFT?) must `#ifdef` on SoM SKU tokens; there's no portable capability surface, even though the SoC JSON already carries the data.

The three features in this spec close each gap. They share generators and validation infrastructure, so they ship together.

## Non-goals

- No new `os: ubuntu` backend work (deferred past v1.0 per project memory).
- No board.yaml schema changes beyond what's required for diagnostics.
- No interactive flashing or hardware detection in `alp run` v1 (`--board <name>` does a regular `west build`; flashing stays manual).
- No coverage of internal metadata files (SoM presets, SoC JSON) in the diagnostics rewrite — board.yaml only.

---

## Feature 1 — `alp` Python CLI with `init` and `run`

### Packaging

- New `pyproject.toml` at repo root with a `console_scripts` entry:
  `alp = alp_cli.main:cli`.
- Source under `scripts/alp_cli/` (Python package). Existing `scripts/alp_project.py` keeps working standalone; the `alp` CLI imports its loader/validator/emitter internals from the package.
- CLI framework: `click` (well-tested, integrates cleanly with `questionary` for prompts; no heavy new deps).
- Customer install: `pip install -e .` once after clone — added to top-level README quickstart.

### `alp init <name>` — interactive wizard

1. Validate `<name>` is a fresh directory; bail with a friendly one-liner if it exists.
2. Prompt 1 — SoM SKU: enumerated from `metadata/e1m_modules/*.yaml` (questionary `select`, searchable).
3. Prompt 2 — board preset: enumerated from `metadata/boards/*.yaml`, filtered to those whose `hosts_som_families:` list contains the chosen SoM's family.
4. Prompt 3 — starter peripherals (multi-select, may be empty): `uart`, `gpio`, `i2c`, `spi`, `pwm`. Each maps onto a stub `peripherals:` entry in the generated `board.yaml`.
5. Scaffold: copy `examples/peripheral-io/hello-world/` → `./<name>/`, rewrite `board.yaml` with wizard answers, add a one-line `README.md` pointing to the docs.
6. Non-interactive flags: `--som`, `--preset`, `--peripherals` skip the corresponding prompt (so CI scripts can use `alp init` headless).

### `alp run` — build + run on native_sim

1. Walk up from `cwd` to find the nearest `board.yaml`; bail if none.
2. Default path: rebuild for `native_sim` (re-using each example's existing `native_sim.conf` convention from the v0.6 sweep — see `project_native_sim_conf_overlay_convention`), then exec `build/zephyr/zephyr.exe`. Streams stdout through; exit code mirrors the simulated app.
3. `--board <name>` runs a normal `west build -b <name>` and stops after build.
4. `--flash` (with `--board`) chains into `west flash`.

### Error surface

Friendly one-liners for expected errors:

- `alp init: '<name>' already exists`
- `alp run: no board.yaml found in this directory or any parent`

`--debug` flips to full Python tracebacks. No silent failures — every error path either emits a one-liner and exits non-zero, or escalates to the debug traceback.

### Testing

`tests/scripts/test_alp_cli.py` (pytest + `click.testing.CliRunner`):

- `init` creates the expected files (board.yaml, src/main.c, CMakeLists.txt, README.md).
- `init` refuses an existing dir.
- Non-interactive `init` with all flags emits a deterministic board.yaml.
- `run` finds the nearest board.yaml when invoked from a subdirectory.
- `run` reports missing project with the friendly one-liner.
- Wizard prompts mocked via `questionary`'s `unsafe_prompt` for deterministic UI tests.

---

## Feature 2 — Rich diagnostics for `board.yaml`

### Position-aware YAML loader

New module `scripts/alp_cli/yaml_pos.py`:

- Subclass `yaml.SafeLoader`; override `construct_mapping` and `construct_sequence` to attach `__line__`, `__column__`, `__end_line__`, `__end_column__` to each constructed dict/list.
- Scalar leaves expose position via a thin wrapper preserving `start_mark`/`end_mark`.
- Standard PyYAML trick, no new dependency.
- Shared by the `alp_project` validator and any other YAML-consuming script that wants positions.

### Diagnostic data type

`scripts/alp_cli/diagnostic.py`:

```python
@dataclass
class Diagnostic:
    severity: Literal["error", "warning", "note"]
    path: Path
    line: int          # 1-based
    col: int           # 1-based
    span: int          # caret length in columns
    code: str          # e.g. "ALP-B001" — stable id for docs
    message: str       # one-line summary
    hint: str | None   # "did you mean X?" or "add Y to fix"
    doc_url: str | None  # defaults to docs/diagnostics/<code>.md; overridable via ALP_DIAG_BASE_URL
```

`render(diag) -> str` emits the Rust-style block:

```
error[ALP-B005]: pad 'P21' not present on E1M-AEN701
  --> board.yaml:14:11
   |
14 |     - { pad: P21, signal: I2C0_SCL }
   |           ^^^ pad not in this SoM's pad_routes
   |
   = hint: did you mean 'P20'? (closest match, distance 1)
   = see: docs/diagnostics/ALP-B005.md
```

`doc_url` defaults to the in-tree `docs/diagnostics/<code>.md` path so it works offline. An optional env var `ALP_DIAG_BASE_URL` can override the prefix once a hosted docs site exists. ANSI colours via `colorama`; disabled when `NO_COLOR=1` is set or stdout is not a TTY.

### Validation passes

Three passes, each emitting `Diagnostic`s into a shared collector instead of `sys.exit`-ing on first error:

1. **Schema pass** — wraps `jsonschema.Draft7Validator(...).iter_errors(data)`. Walks each `ValidationError`'s `absolute_path` back to the saved `__line__`/`__column__`. Maps common schema errors to friendly codes:
   - `ALP-B001`: required key missing → caret at parent mapping.
   - `ALP-B002`: unknown key → caret at the key, "did you mean…" via `difflib.get_close_matches` against the schema's allowed keys.
   - `ALP-B003`: enum / pattern violation → caret at value, "expected one of {…}" or "value must match `^E1M-…$`".
   - `ALP-B004`: type mismatch → caret at value.
2. **Cross-reference pass** — semantic checks the JSON Schema can't express:
   - `som.sku` resolves to a real SoM file under `metadata/e1m_modules/`.
   - `preset:` resolves to a real preset under `metadata/boards/`.
   - Each `pad:` in inline routes exists on the chosen SoM's `pad_routes`.
   - Codes `ALP-B005…`.
3. **Compatibility pass** — `peripherals:` entries that the chosen SoC doesn't have (e.g. CAN on a no-CAN part). Cross-references the same SoC JSON that `gen_soc_caps.py` consumes (so the source of truth aligns with Feature 3). Codes `ALP-B010…`.

After all passes run, the collector prints every diagnostic. Exits non-zero if any are errors; warnings keep going.

### Doc landing pages

`docs/diagnostics/<code>.md` — one stub per error code (title + reserved URL). Empty bodies initially; populating prose is out of scope here.

### Integration

`alp validate` subcommand: thin wrapper that runs the validator and exits. The existing `alp_project.py --emit zephyr-conf` path runs the same validator first; prints diagnostics and exits non-zero on errors before emitting anything.

### Testing

`tests/scripts/test_board_yaml_diagnostics.py`:

- For each error code, a fixture under `tests/fixtures/board_yaml_bad/<code>.yaml` plus an assertion that running the validator yields the expected `code`, `line`, and `col`.
- One "happy path" fixture asserts zero diagnostics.
- Snapshot-style rendering tests via `pytest-snapshot` to catch unintended format regressions.

---

## Feature 3 — Capability API (`ALP_HAS` / `alp_has`)

### Compile-time layer

Extend `scripts/gen_soc_caps.py` to append, after the existing per-SoC blocks, a shared capability layer:

```c
/* Capability layer — portable, SoM-agnostic. Derived from the active
 * CONFIG_ALP_SOC_* selection via the macros above. */
#define ALP_CAP_HW_I2C       (ALP_SOC_I2C_COUNT > 0)
#define ALP_CAP_HW_SPI       (ALP_SOC_SPI_COUNT > 0)
#define ALP_CAP_HW_UART      (ALP_SOC_UART_COUNT > 0)
#define ALP_CAP_HW_CAN       (ALP_SOC_CAN_COUNT > 0)
#define ALP_CAP_HW_CAN_FD    (ALP_SOC_CAN_FD_SUPPORTED)
#define ALP_CAP_NPU_DRPAI    (ALP_SOC_DRP_AI)
#define ALP_CAP_HELIUM_MVE   (ALP_SOC_HELIUM_MVE)
#define ALP_CAP_NEON         (ALP_SOC_NEON)
#define ALP_CAP_GPU2D        (ALP_SOC_GPU2D)
/* …one entry per existing ALP_SOC_* macro… */

#define ALP_HAS(cap) (ALP_CAP_##cap)
```

Counts and booleans both collapse to `0`/`1` for `ALP_HAS()`, so it is safe inside `#if` / `static_assert`. The mapping is held in `gen_soc_caps.py` as a Python dict `_CAP_MAP`, with one row per `ALP_SOC_*` field — automatically generated from the SoC JSON keys so future SoC fields get a matching `ALP_CAP_*` partner without further hand-editing.

### Runtime layer

New header `include/alp/cap.h`:

```c
typedef enum {
    ALP_CAP_ID_HW_I2C,
    ALP_CAP_ID_HW_SPI,
    /* …same set as the macros, in stable order… */
    ALP_CAP_ID_COUNT
} alp_cap_id_t;

/**
 * @brief Test whether the active SoC offers a hardware capability.
 * @param cap  Capability id from @ref alp_cap_id_t.
 * @return true if the capability is present, false otherwise.
 */
bool alp_has(alp_cap_id_t cap);

/**
 * @brief Return the symbolic name of a capability (e.g. "HW_I2C").
 * @param cap  Capability id; ALP_CAP_ID_COUNT or out-of-range returns NULL.
 * @return Pointer to a static string, or NULL.
 */
const char *alp_cap_name(alp_cap_id_t cap);
```

`src/cap.c` carries a static `const bool _cap_table[ALP_CAP_ID_COUNT]` initialised from the same `ALP_CAP_*` macros at compile time. `alp_has` is a bounds-checked array index; the table sits in `.rodata`. No per-SoC code path — the active SoC is already pinned by `CONFIG_ALP_SOC_*` at preprocessor time.

### Single source of truth

Both files (`include/alp/cap.h` and `src/cap.c`) are now emitted by `gen_soc_caps.py`. Both carry the existing "AUTO-GENERATED — DO NOT EDIT" banner and the `[ABI-STABLE]` marker (per `docs/abi-markers.md`).

### Board.yaml impact

None. The capability set is determined entirely by the active SoC (selected by `som.sku`). No schema changes, no new validation.

### Testing

`tests/unit/test_cap.c` (Ztest under twister, runs once per SoC):

- For every `ALP_CAP_ID_*`, `alp_has(id) == (bool)ALP_HAS(<name>)`.
- `alp_cap_name(id)` returns the expected string for each id; returns NULL for `ALP_CAP_ID_COUNT`.
- `alp_has(ALP_CAP_ID_COUNT)` returns false (out-of-bounds safety).

`tests/scripts/test_gen_soc_caps_emits_cap_layer.py`:

- Every `ALP_SOC_*` field in every SoC JSON produces a matching `ALP_CAP_*` macro in the generated header.
- The enum order in `include/alp/cap.h` matches the macro order in `soc_caps.h`.

### Example update

`examples/peripheral-io/hello-world/src/main.c` gets a small teaching block:

```c
#if ALP_HAS(NPU_DRPAI)
    /* …compile-time path that wires DRP-AI… */
#endif

    if (alp_has(ALP_CAP_ID_HW_I2C)) {
        /* …runtime branch that probes the I²C bus… */
    }
```

Comment ratio held at ~50%, per the `feedback_examples_are_documentation` convention.

---

## Cross-cutting concerns

### Shared infrastructure

- `scripts/alp_cli/` package becomes the import target for both the new CLI (#1) and the new validator (#2). `scripts/alp_project.py` is kept as a backwards-compatible thin wrapper.
- The SoC JSON is the single source of truth shared by `gen_soc_caps.py` (Feature 3) and the diagnostics compatibility pass (Feature 2's `ALP-B010…` codes).

### Dependencies added

- `click` (CLI framework, MIT) — was previously absent.
- `questionary` (interactive prompts on top of `prompt_toolkit`, MIT).
- `colorama` (Windows ANSI shim, MIT).
- `pytest-snapshot` (test-time only, MIT).

All four are widely used and have stable APIs. Listed in `pyproject.toml` under `[project.optional-dependencies] dev`.

### Documentation

- README: top-level quickstart updated to mention `pip install -e .`, then `alp init`, then `alp run`.
- `docs/diagnostics/` directory: stub pages for each error code.
- `docs/abi/`: new `cap.h` entry under the ABI-stable inventory.

### Build / CI

- New twister test for `tests/unit/test_cap.c` covering one representative SoC per family.
- Existing scripts pytest job picks up `tests/scripts/test_alp_cli.py` and `tests/scripts/test_board_yaml_diagnostics.py` automatically.
- No new GitHub Actions workflows.

## Build order (high-level — refined in the implementation plan)

1. `scripts/alp_cli/` package skeleton (empty subcommands) + `pyproject.toml`.
2. Position-aware YAML loader + diagnostic data type (shared infra for #1 validate + #2).
3. Feature 2 — validation passes (schema, cross-ref, compat) + fixtures + snapshot tests.
4. Feature 3 — extend `gen_soc_caps.py` to emit `cap.h` + `cap.c`; add unit + generator tests.
5. Feature 1 — wire `alp init` + `alp run` + CLI tests; update README.
6. Update `examples/peripheral-io/hello-world/src/main.c` with the capability-API teaching block.

The three features are mostly independent after step 2, so parallel-agent dispatch is viable from step 3 onward.
